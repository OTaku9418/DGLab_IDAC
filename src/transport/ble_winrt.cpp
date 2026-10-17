// ---------------------------------------------------------------------------
// DGLab_IDAC — BLE(WinRT)传输实现 —— Coyote 3.0 / DG-LAB V3
//
// 为什么用原始 WRL + 原始 WinRT ABI 头, 而不是 C++/WinRT:
//   宿主是 UE4SS.dll(16 MB, 4067 个导出, 没有一个是 winrt*)。C++/WinRT 会在
//   我们的 TU 里塞进大量模板实例, 还可能拖进 cppwinrt.dll。用 <wrl/*> 配合
//   MIDL 生成的原始 ABI 头(<windows.devices.bluetooth*.h>)可以保证零符号冲突、
//   零额外运行时依赖。
//
//   ⚠ 任务书里提到的 <bleapidef.h> 在 SDK 10.0.26100 里**不存在**; 真正存在的
//     是 <bluetoothleapis.h>(Win32 蓝牙 LE API)。本文件走 WinRT 那条路, 不调用
//     它的任何函数, 保留这个 include 只是为了让两套蓝牙头确认能共存。
//
// 线程模型(重要):
//   * 游戏线程只调 submit() / stop() / status() —— 只碰互斥量, 绝不碰 COM。
//   * 所有 WinRT 调用都在我们自己的工作线程上, 且**只在那条线程上**
//     RoInitialize(RO_INIT_MULTITHREADED)。绝不在游戏主线程初始化套间。
//   * WinRT 事件回调跑在线程池线程上, 所以回调只碰 SharedState
//     (shared_ptr 持有, 生命周期长于任何回调), 绝不碰 BleWinRtTransport 本身。
//     这样 stop() 之后即使还有回调在飞, 也不会访问已析构的对象。
//
// 安全要点:
//   * BF 软限位是最后一道闸, 每次(重)连接后、第一条 B0 之前必须先发。
//   * 每次(重)连接都把"已知实际强度"置为未知, 于是第一帧必然用
//     StrengthMode::Absolute 把设备拉到当前期望值 —— 设备是电池供电的,
//     上次会话残留的强度必须被主动覆盖, 不能靠 Discard 蒙混过去。
//   * 静默帧的 20 个字节全部显式赋值, 不存在未初始化字节上线的可能。
// ---------------------------------------------------------------------------

#ifndef NOMINMAX
#define NOMINMAX
#endif

// asyncinfo.h 在 C++ 下会往全局命名空间塞 Started/Completed/Canceled/Error
// 四个 static const。我们用不到(一律写 AsyncStatus::Xxx), 关掉以免撞名。
#ifndef _HIDE_GLOBAL_ASYNC_STATUS
#define _HIDE_GLOBAL_ASYNC_STATUS
#endif

#include <windows.h>

#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/event.h>

#include <bluetoothleapis.h>

// WinRT 原始 ABI 头。注意: 它们不是 C++/WinRT 投影, 只依赖 <windows.h>/<ole2.h>。
#include <objbase.h>    // IIDFromString
#include <roapi.h>      // RoInitialize / RoGetActivationFactory
#include <hstring.h>
#include <robuffer.h>   // IBufferByteAccess
#include <windows.foundation.h>
#include <windows.foundation.collections.h>
#include <windows.storage.streams.h>
#include <windows.devices.bluetooth.h>
#include <windows.devices.bluetooth.advertisement.h>
#include <windows.devices.bluetooth.genericattributeprofile.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "logger.hpp"
#include "transport/transport.hpp"

// 不用 #pragma comment(lib, ...) —— 链接行由 CMake 显式给出
// (windowsapp.lib runtimeobject.lib ole32.lib)。

namespace dglab_idac {
namespace {

// ---------------------------------------------------------------------------
// 协议常量(来自 docs/research-dglab-transport.md, 已核对)
// ---------------------------------------------------------------------------
constexpr wchar_t kDeviceNameDefault[] = L"47L121000";
constexpr wchar_t kDeviceNameAlt[] = L"47L1210J0";  // 同一批硬件的另一个广播名

constexpr char kServiceUuidDefault[] = "0000180C-0000-1000-8000-00805F9B34FB";
constexpr char kWriteUuidDefault[] = "0000150A-0000-1000-8000-00805F9B34FB";
constexpr char kNotifyUuidDefault[] = "0000150B-0000-1000-8000-00805F9B34FB";

// B0 里通道强度设定值的合法域
constexpr int kStrengthMin = 0;
constexpr int kStrengthMax = 200;

// start() 等待工作线程完成 WinRT 激活的上限
constexpr DWORD kStartupTimeoutMs = 4000;

// 强度变更发出后, 最多等这么多帧还没等到匹配的 B1 就放弃本次握手并重试。
// 没有这个上限的话, 一次丢包会让状态机永久卡死 —— 那比多试一次危险得多。
constexpr int kAckGiveUpFrames = 20;

// 设备返回的应答首字节
constexpr uint8_t kNotifyB1 = 0xB1;

// ---------------------------------------------------------------------------
// 显式 using 声明(而不是 using namespace), 避免 ABI 命名空间之间的歧义
// ---------------------------------------------------------------------------
using ABI::Windows::Devices::Bluetooth::BluetoothCacheMode;
using ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus;
using ABI::Windows::Devices::Bluetooth::BluetoothLEDevice;
using ABI::Windows::Devices::Bluetooth::IBluetoothLEDevice;
using ABI::Windows::Devices::Bluetooth::IBluetoothLEDevice3;
using ABI::Windows::Devices::Bluetooth::IBluetoothLEDeviceStatics;

using ABI::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementReceivedEventArgs;
using ABI::Windows::Devices::Bluetooth::Advertisement::BluetoothLEAdvertisementWatcher;
using ABI::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode;
using ABI::Windows::Devices::Bluetooth::Advertisement::IBluetoothLEAdvertisement;
using ABI::Windows::Devices::Bluetooth::Advertisement::IBluetoothLEAdvertisementReceivedEventArgs;
using ABI::Windows::Devices::Bluetooth::Advertisement::IBluetoothLEAdvertisementWatcher;
using ABI::Windows::Devices::Bluetooth::Advertisement::IBluetoothLEAdvertisementWatcher2;

using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristic;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCharacteristicsResult;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattClientCharacteristicConfigurationDescriptorValue;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceService;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattDeviceServicesResult;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattValueChangedEventArgs;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattWriteResult;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattCharacteristic;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattCharacteristic3;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattCharacteristicsResult;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattDeviceService;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattDeviceService3;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattDeviceServicesResult;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattValueChangedEventArgs;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::IGattWriteResult;

using ABI::Windows::Foundation::AsyncStatus;
using ABI::Windows::Foundation::IAsyncOperation;
using ABI::Windows::Foundation::IAsyncOperationCompletedHandler;
using ABI::Windows::Foundation::ITypedEventHandler;

using ABI::Windows::Foundation::Collections::IVectorView;
using ABI::Windows::Storage::Streams::IBuffer;
using ABI::Windows::Storage::Streams::IBufferFactory;

// 枚举值拼写注意: 这批 MIDL 生成的原始 ABI 头用 <枚举名>_<成员名> 的命名
// (BluetoothCacheMode_Uncached), 不是 C++/WinRT 投影里的 BluetoothCacheMode::Uncached。
using ABI::Windows::Devices::Bluetooth::Advertisement::BluetoothLEScanningMode_Active;
using ABI::Windows::Devices::Bluetooth::BluetoothCacheMode_Uncached;
using ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Connected;
using ABI::Windows::Devices::Bluetooth::BluetoothConnectionStatus_Disconnected;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus_Success;
using ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::GattCommunicationStatus_Unreachable;

// IBufferByteAccess 不在 ABI:: 命名空间下, 是 robuffer.h 直接声明的
// COM 接口(全局 Windows::Storage::Streams)。
using ByteAccessBuffer = ::Windows::Storage::Streams::IBufferByteAccess;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

std::string hresult_hex(HRESULT hr) {
    char buf[16]{};
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(buf);
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();

    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {
        // 配置里塞了非 UTF-8 字节时退回 ANSI, 而不是直接失败
        cp = CP_ACP;
        n = ::MultiByteToWideChar(cp, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        if (n <= 0) return std::wstring();
    }

    std::wstring w(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(cp, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// 用 ole32 的 IIDFromString 解析 GUID —— 不引入 IGuidHelperStatics, 少一个
// 需要激活的运行时类。
bool parse_guid(const std::string& s, GUID* out) {
    if (s.empty() || out == nullptr) return false;
    std::wstring w = utf8_to_wide(s);
    if (w.empty()) return false;

    // IIDFromString 要求串带花括号: 不带时它返回 E_INVALIDARG(0x80070057),
    // 而不是像大家以为的那样容忍裸 UUID。我们的协议默认值都是裸的, 所以这里
    // 自己补一对 —— 两种写法都接受。
    // (踩过: 这一条让 create_ble_transport 从写下来那天起就一直返回 nullptr,
    //  BLE 从来没真正跑起来过, 而且报错文案把锅推给了"配置里的 UUID 不合法"。)
    if (w.front() != L'{') {
        w.insert(w.begin(), L'{');
        w.push_back(L'}');
    }
    return SUCCEEDED(::IIDFromString(w.c_str(), out));
}

// RuntimeClass_* 在 SDK 里声明为 `WCHAR[]` 常量数组, 而 RoGetActivationFactory
// 第一个参数要的是 HSTRING, 所以在这里现包一层。激活成功后立刻释放字符串。
HRESULT activate_factory(const wchar_t* runtime_class, REFIID iid, void** out) {
    if (runtime_class == nullptr || out == nullptr) return E_POINTER;
    *out = nullptr;

    HSTRING h = nullptr;
    HRESULT hr = ::WindowsCreateString(runtime_class, static_cast<UINT32>(std::wcslen(runtime_class)), &h);
    if (FAILED(hr) || h == nullptr) return FAILED(hr) ? hr : E_FAIL;

    hr = ::RoGetActivationFactory(h, iid, out);
    ::WindowsDeleteString(h);
    return hr;
}

// 激活一个普通实例类(有构造函数), 然后 QI 到目标接口。
//
// 为什么需要这个而不是 activate_factory: 对
// BluetoothLEAdvertisementWatcher 这类"普通实例类", RoGetActivationFactory
// 直接要实例接口会返回 E_NOINTERFACE(0x80004002) —— 实测如此, 即使该类在
// 本机完全可用、RoActivateInstance 一秒就成功。原因是激活工厂那层只保证
// 暴露静态接口, 实例接口要通过实例去 QI。
//
// 踩过: 可用性探测(probe_winrt_ble_once)当初就卡在这里, 于是本机明明有蓝牙、
// Watcher 明明能建, 却被判成"本机 WinRT BLE 不可用", 传输层直接不给创建。
HRESULT activate_instance_and_qi(const wchar_t* runtime_class, REFIID iid, void** out) {
    if (runtime_class == nullptr || out == nullptr) return E_POINTER;
    *out = nullptr;

    HSTRING h = nullptr;
    HRESULT hr = ::WindowsCreateString(runtime_class, static_cast<UINT32>(std::wcslen(runtime_class)), &h);
    if (FAILED(hr) || h == nullptr) return FAILED(hr) ? hr : E_FAIL;

    IInspectable* inst = nullptr;
    hr = ::RoActivateInstance(h, &inst);
    ::WindowsDeleteString(h);
    if (FAILED(hr) || inst == nullptr) return FAILED(hr) ? hr : E_FAIL;

    hr = inst->QueryInterface(iid, out);
    inst->Release();
    return hr;
}
// (静默帧构造已上移到 transport.cpp 的 make_idle_frame(const char*), 便于离线测试)

// ---------------------------------------------------------------------------
// 事件回调与工作线程共享的状态。
//
// 用 shared_ptr 持有, 且句柄的关闭放在析构里: 任何还在线程池上跑的回调都持有
// 一份引用, 所以它 SetEvent 的时候句柄一定还有效。
// ---------------------------------------------------------------------------
struct SharedState {
    SharedState() = default;
    ~SharedState() {
        if (scan_event != nullptr) ::CloseHandle(scan_event);
        if (wake_event != nullptr) ::CloseHandle(wake_event);
        if (startup_event != nullptr) ::CloseHandle(startup_event);
    }

    SharedState(const SharedState&) = delete;
    SharedState& operator=(const SharedState&) = delete;

    // 构造后只读
    std::wstring target_name;
    std::wstring alt_name;

    // 句柄只在工作线程上使用(回调也可能 SetEvent 醒它)
    HANDLE scan_event = nullptr;
    HANDLE wake_event = nullptr;
    HANDLE startup_event = nullptr;

    // start() 与工作线程之间的激活结果握手
    HRESULT startup_hr = E_PENDING;

    mutable std::mutex mu;

    // ---- 扫描 ----
    bool found = false;
    uint64_t address = 0;
    int rssi = 0;
    uint64_t adverts_seen = 0;

    // ---- B1 应答 ----
    uint64_t b1_count = 0;
    uint8_t b1_last_seq = 0;
    int b1_last_a = -1;
    int b1_last_b = -1;
    uint16_t acked_mask = 0;  // 位 i = 收到过 seq==i 的应答
    int acked_a = -1;
    int acked_b = -1;
    // 收到过、但不像 B1 的通知条数。用来区分"设备没应"和"设备应了我们没认出来" ——
    // 只看 B1 计数的话这两种情况在日志里长得一模一样。
    uint32_t other_notify_count = 0;

    // ---- 链路 ----
    std::atomic<bool> link_down{false};
};

// ---------------------------------------------------------------------------
// 异步操作 -> 阻塞等待。
//
// 用 put_Completed + CreateEventW + WaitForSingleObject, 不碰 winrt:: 辅助设施。
// 完成后在等待线程上直接 GetResults(), 这样 TResultAbiPtr 既可以是接口指针
// (IBluetoothLEDevice*), 也可以是值类型(GattCommunicationStatus)。
// ---------------------------------------------------------------------------
template <typename TResultLogical, typename TResultAbiPtr>
HRESULT await_operation(IAsyncOperation<TResultLogical>* op, DWORD timeout_ms, TResultAbiPtr* result) {
    if (op == nullptr || result == nullptr) return E_POINTER;
    *result = TResultAbiPtr{};

    HANDLE done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (done == nullptr) return HRESULT_FROM_WIN32(::GetLastError());

    HRESULT hr = S_OK;
    {
        // 事件句柄在 lambda 里不带所有权, 由下面的 RAII 负责关闭
        Microsoft::WRL::ComPtr<IAsyncOperationCompletedHandler<TResultLogical>> handler =
            Microsoft::WRL::Callback<IAsyncOperationCompletedHandler<TResultLogical>>(
                [done](IAsyncOperation<TResultLogical>* /*async_info*/, AsyncStatus /*status*/) -> HRESULT {
                    ::SetEvent(done);
                    return S_OK;
                });

        if (!handler) {
            ::CloseHandle(done);
            return E_OUTOFMEMORY;
        }

        hr = op->put_Completed(handler.Get());
        if (SUCCEEDED(hr)) {
            const DWORD w = ::WaitForSingleObject(done, timeout_ms);
            if (w == WAIT_OBJECT_0) {
                hr = op->GetResults(result);
            } else if (w == WAIT_TIMEOUT) {
                hr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            } else {
                hr = HRESULT_FROM_WIN32(::GetLastError());
            }
        }
    }
    ::CloseHandle(done);
    return hr;
}

// 同步写一次 CCCD(0x2902), 也就是真正意义上的"订阅通知"。
//
// 为什么单独抽出来: notify_char_->add_ValueChanged() 只挂本地回调, 设备侧
// 一无所知。少了这一步, B0 全部写得成功、强度也真的会变, 但 B1 一条都收不到。
//
// 为什么要同步等: 异步发出去就走, 会在 CCCD 尚未写入时开始发 B0。CCCD 没生效
// 的那几十毫秒里设备不会回 B1, 于是握手从第一帧就错位。
HRESULT write_cccd_sync(IGattCharacteristic* ch, GattClientCharacteristicConfigurationDescriptorValue v) {
    if (ch == nullptr) return E_POINTER;

    Microsoft::WRL::ComPtr<IAsyncOperation<GattCommunicationStatus>> op;
    const HRESULT hr = ch->WriteClientCharacteristicConfigurationDescriptorAsync(
        v, op.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !op) return FAILED(hr) ? hr : E_FAIL;

    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;
    const HRESULT hr_wait = await_operation(op.Get(), 5000, &status);
    if (FAILED(hr_wait)) return hr_wait;
    if (status != GattCommunicationStatus_Success) {
        // Unreachable / ProtocolError / AccessDenied 都归成一个 HRESULT,
        // 让调用方能靠 HRESULT 判断并降级到 Indicate。
        return E_FAIL;
    }
    return S_OK;
}

// 一次性的"本机有没有 WinRT BLE"探测。必须在自己起的线程上做 ——
// 绝不能在调用方(游戏)线程上初始化套间。
HRESULT probe_winrt_ble_once() {
    HRESULT hr_init = ::RoInitialize(RO_INIT_MULTITHREADED);
    bool need_uninit = false;

    if (hr_init == S_OK) {
        need_uninit = true;
    } else if (hr_init == S_FALSE) {
        need_uninit = false;  // 这条线程本来就初始化过
    } else if (hr_init == RPC_E_CHANGED_MODE) {
        const HRESULT hr_co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr_co)) return hr_co;
        need_uninit = (hr_co == S_OK);
    } else {
        return hr_init;
    }

    Microsoft::WRL::ComPtr<IBluetoothLEAdvertisementWatcher> watcher;
    const HRESULT hr = activate_instance_and_qi(
        RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher,
        IID_PPV_ARGS(watcher.ReleaseAndGetAddressOf()));
    watcher.Reset();

    if (need_uninit) ::RoUninitialize();
    return hr;
}

}  // namespace

// ---------------------------------------------------------------------------
// BleWinRtTransport
// ---------------------------------------------------------------------------
class BleWinRtTransport final : public ITransport {
public:
    explicit BleWinRtTransport(const Config& cfg);
    ~BleWinRtTransport() override;

    BleWinRtTransport(const BleWinRtTransport&) = delete;
    BleWinRtTransport& operator=(const BleWinRtTransport&) = delete;

    const char* name() const override { return "ble-winrt"; }

    bool start() override;
    void stop() override;
    void submit(const Frame& f) override;
    std::string status() const override;

private:
    void worker_entry();
    bool init_apartment();
    bool activate_watcher();
    bool scan_for_device();
    bool connect_and_setup();
    bool subscribe_notify_and_conn();
    bool send_bf();
    void send_loop();
    void teardown_link();
    void wait_ms(DWORD ms) const;
    void set_state(const char* s, bool log_it);

    HRESULT write_bytes(const uint8_t* data, uint32_t len, DWORD timeout_ms);
    HRESULT get_buffer(uint32_t len, IBuffer** out);

    // 期望强度 -> 实际可写强度(受 [safety] 两道限位约束)
    int clamp_strength(int want) const;
    int bf_limit() const;

    // 只读, 构造后不变
    Config cfg_;
    GUID service_uuid_{};
    GUID write_uuid_{};
    GUID notify_uuid_{};

    // 线程
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::shared_ptr<SharedState> shared_;

    // 信箱: 深度 1, 覆盖式, 内含 [safety] watchdog_ms 的看门狗。
    FrameMailbox mailbox_;

    // 诊断(互斥量保护, status() 可能从游戏线程调用)
    mutable std::mutex stat_mu_;
    std::string state_ = "idle";
    int last_rssi_ = 0;
    uint64_t writes_ = 0;
    uint64_t errors_ = 0;
    uint64_t reconnects_ = 0;

    // ---- 以下成员只在工作线程上访问 ----
    Microsoft::WRL::ComPtr<IBluetoothLEAdvertisementWatcher> watcher_;
    EventRegistrationToken watcher_token_{};
    bool watcher_token_valid_ = false;

    Microsoft::WRL::ComPtr<IBluetoothLEDevice> device_;
    EventRegistrationToken conn_token_{};
    bool conn_token_valid_ = false;

    Microsoft::WRL::ComPtr<IGattDeviceService> service_;
    Microsoft::WRL::ComPtr<IGattCharacteristic> write_char_;
    Microsoft::WRL::ComPtr<IGattCharacteristic3> write_char3_;
    Microsoft::WRL::ComPtr<IGattCharacteristic> notify_char_;
    EventRegistrationToken notify_token_{};
    bool notify_token_valid_ = false;

    Microsoft::WRL::ComPtr<IBufferFactory> buffer_factory_;

    uint8_t next_seq_ = 1;       // 1..15, 永不为 0
    uint8_t inflight_seq_ = 0;   // 0 = 没有在飞的强度变更
    int inflight_a_ = -1;
    int inflight_b_ = -1;
    int inflight_frames_ = 0;
    int actual_a_ = -1;          // -1 = 未知
    int actual_b_ = -1;
    uint64_t seen_b1_ = 0;
    bool bf_sent_ = false;
    bool mode_semantics_logged_ = false;   // 一次性线上语义自证, 见发送循环里的说明
    bool first_frame_dumped_ = false;      // 一次性把首条改强度的 B0 原始字节打出来
};

// ---------------------------------------------------------------------------
// 构造 / 析构
// ---------------------------------------------------------------------------
BleWinRtTransport::BleWinRtTransport(const Config& cfg) : cfg_(cfg) {
    // UUID 一律走协议默认值; 配置里非空才覆盖。
    const std::string svc = cfg_.ble_service_uuid.empty() ? std::string(kServiceUuidDefault) : cfg_.ble_service_uuid;
    const std::string wr = cfg_.ble_write_uuid.empty() ? std::string(kWriteUuidDefault) : cfg_.ble_write_uuid;
    const std::string nt = cfg_.ble_notify_uuid.empty() ? std::string(kNotifyUuidDefault) : cfg_.ble_notify_uuid;

    parse_guid(svc, &service_uuid_);
    parse_guid(wr, &write_uuid_);
    parse_guid(nt, &notify_uuid_);
}

BleWinRtTransport::~BleWinRtTransport() { stop(); }

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------
bool BleWinRtTransport::start() {
    if (running_.load(std::memory_order_acquire)) {
        log_warn("BLE: start() 被重复调用, 忽略");
        return true;
    }
    if (worker_.joinable()) {
        // 上次 stop() 之后线程对象没被回收(理论上不会), 收一下
        worker_.join();
    }

    auto st = std::make_shared<SharedState>();
    st->target_name = cfg_.ble_device_name.empty() ? std::wstring(kDeviceNameDefault)
                                                   : utf8_to_wide(cfg_.ble_device_name);
    st->alt_name = std::wstring(kDeviceNameAlt);

    st->scan_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);      // 自动复位
    st->wake_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);      // 自动复位
    st->startup_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);   // 自动复位
    if (st->scan_event == nullptr || st->wake_event == nullptr || st->startup_event == nullptr) {
        const std::string e = hresult_hex(HRESULT_FROM_WIN32(::GetLastError()));
        log_error("BLE: 创建事件对象失败 (HRESULT=%s)", e.c_str());
        return false;
    }

    shared_ = st;
    running_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lk(stat_mu_);
        state_ = "startup";
    }

    worker_ = std::thread(&BleWinRtTransport::worker_entry, this);

    // 等激活结果。套间初始化与运行时类激活都在工作线程上做, 这里只是收结果。
    const DWORD w = ::WaitForSingleObject(st->startup_event, kStartupTimeoutMs);
    if (w != WAIT_OBJECT_0) {
        log_error("BLE: 等待 WinRT 激活超时(%lu ms), 放弃启动", static_cast<unsigned long>(kStartupTimeoutMs));
        stop();
        return false;
    }
    if (FAILED(st->startup_hr)) {
        const std::string e = hresult_hex(st->startup_hr);
        log_error("BLE: WinRT 激活失败 (HRESULT=%s), 本机 BLE 不可用", e.c_str());
        stop();
        return false;
    }

    log_info("BLE: 工作线程已启动 (设备名 \"%s\", 帧节拍 %d ms)",
             cfg_.ble_device_name.empty() ? "47L121000" : cfg_.ble_device_name.c_str(),
             cfg_.frame_ms > 0 ? cfg_.frame_ms : 100);
    return true;
}

void BleWinRtTransport::stop() {
    running_.store(false, std::memory_order_release);

    if (worker_.joinable()) {
        if (worker_.get_id() == std::this_thread::get_id()) {
            // 从工作线程自己调 stop() —— join 自己会死锁, 只置标志让外层循环退出
            log_error("BLE: stop() 被工作线程自己调用, 只置退出标志(不 join)");
            if (shared_ && shared_->wake_event != nullptr) ::SetEvent(shared_->wake_event);
            return;
        }
        if (shared_ && shared_->wake_event != nullptr) ::SetEvent(shared_->wake_event);
        worker_.join();
    }

    shared_.reset();

    std::lock_guard<std::mutex> lk(stat_mu_);
    state_ = "stopped";
}

void BleWinRtTransport::submit(const Frame& f) {
    mailbox_.put(f, steady_now_ms());
}

std::string BleWinRtTransport::status() const {
    std::lock_guard<std::mutex> lk(stat_mu_);
    char buf[448]{};
    // 把 B1 计数放进 status: "设备到底会不会发 B1" 是判链路语义的关键问题,
    // 而它只能靠这个数字回答 —— 只写日志的话得等跑完翻文件才能看见。
    const uint64_t b1c = shared_ ? shared_->b1_count : 0;
    const int b1a = shared_ ? shared_->b1_last_a : -1;
    const int b1b = shared_ ? shared_->b1_last_b : -1;
    // 看门狗与信箱计数: trips>0 说明管线侧真的断过, over>0 说明传输跟不上。
    // 这两个数字是判断"到底是谁卡住了"的唯一凭据。
    const MailboxStats ms = mailbox_.stats();
    std::snprintf(buf, sizeof(buf),
                  "BLE state=%s rssi=%d writes=%llu errors=%llu b1=%llu lastA=%d lastB=%d "
                  "wd=%d trips=%llu dup=%llu over=%llu idle=%llu",
                  state_.c_str(), last_rssi_, static_cast<unsigned long long>(writes_),
                  static_cast<unsigned long long>(errors_), static_cast<unsigned long long>(b1c), b1a, b1b,
                  ms.watchdog_trips > 0 ? 1 : 0, static_cast<unsigned long long>(ms.watchdog_trips),
                  static_cast<unsigned long long>(ms.repeated), static_cast<unsigned long long>(ms.overwritten),
                  static_cast<unsigned long long>(ms.idle_sent));
    return std::string(buf);
}

void BleWinRtTransport::set_state(const char* s, bool log_it) {
    {
        std::lock_guard<std::mutex> lk(stat_mu_);
        state_ = s;
    }
    if (log_it) log_info("BLE: 状态 -> %s", s);
}

int BleWinRtTransport::clamp_strength(int want) const {
    int lim = cfg_.clamp_strength;
    if (lim > cfg_.max_strength) lim = cfg_.max_strength;  // 两道限位取严
    if (lim > kStrengthMax) lim = kStrengthMax;
    if (lim < kStrengthMin) lim = kStrengthMin;

    if (want < kStrengthMin) want = kStrengthMin;
    if (want > lim) want = lim;
    return want;
}

int BleWinRtTransport::bf_limit() const {
    int lim = cfg_.max_strength;
    if (lim > kStrengthMax) lim = kStrengthMax;
    if (lim < kStrengthMin) lim = kStrengthMin;
    return lim;
}

void BleWinRtTransport::wait_ms(DWORD ms) const {
    // 用 wake_event 而不是 Sleep, 这样 stop() 能立刻打断重连等待
    if (shared_ && shared_->wake_event != nullptr) {
        ::WaitForSingleObject(shared_->wake_event, ms);
    } else {
        ::Sleep(ms);
    }
}

// ---------------------------------------------------------------------------
// 工作线程
// ---------------------------------------------------------------------------
void BleWinRtTransport::worker_entry() {
#if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0A00)
    ::SetThreadDescription(::GetCurrentThread(), L"DGLab-IDAC-BLE");
#endif

    try {
        if (!init_apartment()) {
            shared_->startup_hr = E_FAIL;
            ::SetEvent(shared_->startup_event);
            return;
        }

        // 帧节拍: 用绝对时刻推进, 不让每次发送的耗时累积成漂移
        const int frame_ms_cfg = cfg_.frame_ms > 0 ? cfg_.frame_ms : 100;
        const auto frame_dur = std::chrono::milliseconds(frame_ms_cfg);

        bool first_activation = true;

        while (running_.load(std::memory_order_acquire)) {
            // --- (a) 激活 ---
            const bool ok = activate_watcher();
            if (first_activation) {
                shared_->startup_hr = ok ? S_OK : E_FAIL;
                ::SetEvent(shared_->startup_event);
                first_activation = false;
            }
            if (!ok) {
                if (!running_.load(std::memory_order_acquire)) break;
                set_state("activate-failed", true);
                wait_ms(static_cast<DWORD>(cfg_.ble_reconnect_ms > 0 ? cfg_.ble_reconnect_ms : 3000));
                continue;
            }

            // --- (b) 扫描 ---
            set_state("scanning", true);
            if (!scan_for_device()) {
                if (!running_.load(std::memory_order_acquire)) break;
                set_state("scan-timeout", true);
                teardown_link();
                wait_ms(static_cast<DWORD>(cfg_.ble_reconnect_ms > 0 ? cfg_.ble_reconnect_ms : 3000));
                continue;
            }

            // --- (c)(d)(e) 连接 / GATT / 订阅 ---
            set_state("connecting", true);
            if (!connect_and_setup()) {
                if (!running_.load(std::memory_order_acquire)) break;
                set_state("connect-failed", true);
                teardown_link();
                wait_ms(static_cast<DWORD>(cfg_.ble_reconnect_ms > 0 ? cfg_.ble_reconnect_ms : 3000));
                continue;
            }

            set_state("connected", true);
            log_info("BLE: 链路就绪 (A/B 期望强度上限 %d)", bf_limit());

            // --- (f) 发送循环 ---
            {
                auto next_deadline = std::chrono::steady_clock::now() + frame_dur;

                while (running_.load(std::memory_order_acquire)) {
                    if (shared_->link_down.load(std::memory_order_acquire)) {
                        log_warn("BLE: 设备的连接状态变为断开, 回到重连流程");
                        break;
                    }

                    // 取最新一帧。从没提交过、或管线超过 watchdog_ms 没提交,
                    // 信箱内部会自动给静默帧 —— 所以这里拿到的永远是可以安全
                    // 下发的帧, 不需要在这里判空。
                    const MailboxTake tk = mailbox_.take(steady_now_ms(), cfg_.watchdog_ms);
                    const Frame& f = tk.frame;

                    const int want_a = clamp_strength(f.a.strength);
                    const int want_b = clamp_strength(f.b.strength);

                    // 先消费应答, 再决定这一帧要不要发起新的强度变更
                    {
                        std::lock_guard<std::mutex> lk(shared_->mu);
                        if (shared_->b1_count != seen_b1_) {
                            seen_b1_ = shared_->b1_count;
                            if (inflight_seq_ != 0) {
                                const uint16_t bit = static_cast<uint16_t>(1u << inflight_seq_);
                                if ((shared_->acked_mask & bit) != 0) {
                                    shared_->acked_mask = static_cast<uint16_t>(shared_->acked_mask & ~bit);
                                    actual_a_ = shared_->acked_a;
                                    actual_b_ = shared_->acked_b;
                                    log_info("BLE: 强度已确认 seq=%u A=%d B=%d", static_cast<unsigned>(inflight_seq_),
                                             actual_a_, actual_b_);
                                    inflight_seq_ = 0;
                                    inflight_frames_ = 0;
                                }
                            } else {
                                // 没有在飞变更时, 任何 B1 都是当前真实值
                                actual_a_ = shared_->b1_last_a;
                                actual_b_ = shared_->b1_last_b;
                            }
                            if (cfg_.verbose) {
                                log_info("BLE: 收到 B1 seq=%u A=%d B=%d (累计 %llu)",
                                         static_cast<unsigned>(shared_->b1_last_seq), shared_->b1_last_a,
                                         shared_->b1_last_b, static_cast<unsigned long long>(shared_->b1_count));
                            }
                        }
                    }

                    // 在飞太久就当丢了, 放弃本次握手好让下一帧重试
                    if (inflight_seq_ != 0 && ++inflight_frames_ > kAckGiveUpFrames) {
                        log_warn("BLE: 强度变更 seq=%u 等了 %d 帧仍无 B1 应答, 放弃并重试",
                                 static_cast<unsigned>(inflight_seq_), inflight_frames_ - 1);
                        inflight_seq_ = 0;
                        inflight_frames_ = 0;
                    }

                    // 默认不改强度(0b00): 本帧只是想刷脉冲波形, 不想动设备强度
                    StrengthMode mode_a = StrengthMode::Unchanged;
                    StrengthMode mode_b = StrengthMode::Unchanged;
                    uint8_t seq = 0;
                    uint8_t sent_a = static_cast<uint8_t>(-1);
                    uint8_t sent_b = static_cast<uint8_t>(-1);

                    if (inflight_seq_ == 0 && (want_a != actual_a_ || want_b != actual_b_)) {
                        seq = next_seq_;
                        next_seq_ = static_cast<uint8_t>(next_seq_ >= 15 ? 1 : next_seq_ + 1);
                        mode_a = StrengthMode::Absolute;
                        mode_b = StrengthMode::Absolute;
                        inflight_seq_ = seq;
                        inflight_a_ = want_a;
                        inflight_b_ = want_b;
                        inflight_frames_ = 0;
                        sent_a = static_cast<uint8_t>(want_a);
                        sent_b = static_cast<uint8_t>(want_b);
                    }

                    bool valid_a = false;
                    bool valid_b = false;
                    const B0 cmd = encode_b0(f, mode_a, mode_b, seq, &valid_a, &valid_b);

                    // --- 一次性"线上语义自证" ---
                    // 强度模式的位语义曾经整个反过来(Absolute 写成 0b00), 后果是
                    // 设备完全不动, 而本地一切正常、日志全绿 —— 没有任何一处会报错。
                    // 所以这里在**第一条真正改强度的 B0 上线前**把解出来的模式位打进
                    // 日志: 以后拿到日志的人能直接看出"绝对设置"到底编成了几。
                    if (seq != 0 && !mode_semantics_logged_) {
                        mode_semantics_logged_ = true;
                        const int ma = (cmd.bytes[1] >> 2) & 0x03;
                        const int mb = cmd.bytes[1] & 0x03;
                        log_info("BLE: 线上语义核对 byte[1]=0x%02X -> A模式=0b%02d%s B模式=0b%02d%s | "
                                 "绝对设置应为 0b11",
                                 static_cast<unsigned>(cmd.bytes[1]), ma,
                                 ma == 0x03 ? "(绝对设置)" : "(非绝对!)", mb,
                                 mb == 0x03 ? "(绝对设置)" : "(非绝对!)");
                        if (ma != 0x03 && mb != 0x03) {
                            log_error("BLE: ★ 两条通道都不是绝对设置模式 —— 设备会忽略强度值, "
                                      "强度永远不会变。检查 StrengthMode 的枚举取值是否与 "
                                      "re/reference/dglab-bluetooth-protocol/coyote-v3-README.md "
                                      "一致 (跑 protocol_check.exe)");
                        }
                    }

                    if (seq != 0 && !valid_a && !valid_b) {
                        // 两个通道都没被真正编码进 B0, 设备不会应答 —— 别干等
                        log_warn("BLE: 第 %u 号强度变更未通过编码校验, 本帧不发强度(A=%d B=%d)",
                                 static_cast<unsigned>(seq), want_a, want_b);
                        inflight_seq_ = 0;
                        inflight_frames_ = 0;
                    } else if (seq != 0) {
                        log_info("BLE: 请求强度 seq=%u A=%d B=%d (当前已知 A=%d B=%d)", static_cast<unsigned>(seq),
                                 sent_a, sent_b, actual_a_, actual_b_);
                    }

                    // --- 一次性把真正上线的 20 字节打出来 ---
                    // "设备报了 A=0 B=0" 这种症状光看强度值查不出来: 得看字节。
                    // 官方 v3 文档里的示例帧是
                    //   0xB0 0b0000 0b0100 5 0 {10,10,10,10} {0,10,20,30} {0,0,0,0} {0,0,0,101}
                    // 对着它逐字节比, 比对着"模式=0b11"这种二手结论有用得多。
                    if (seq != 0 && !first_frame_dumped_) {
                        first_frame_dumped_ = true;
                        log_info("BLE: 首帧强度 B0 原始字节 = %s", cmd.to_hex().c_str());
                    }

                    const DWORD write_timeout = static_cast<DWORD>(frame_ms_cfg * 2);
                    const HRESULT hr = write_bytes(cmd.bytes, static_cast<uint32_t>(kB0Size), write_timeout);
                    if (FAILED(hr)) {
                        {
                            std::lock_guard<std::mutex> lk(stat_mu_);
                            ++errors_;
                        }
                        const std::string e = hresult_hex(hr);
                        log_error("BLE: 写 B0 失败 (HRESULT=%s), 判定链路已死", e.c_str());
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lk(stat_mu_);
                        ++writes_;
                    }

                    next_deadline += frame_dur;
                    const auto now = std::chrono::steady_clock::now();
                    if (next_deadline > now) {
                        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(next_deadline - now);
                        wait_ms(static_cast<DWORD>(remain.count()));
                    } else {
                        // 落后太多就重新对表, 免得追赶式连发
                        next_deadline = now + frame_dur;
                    }
                }
            }

            // --- (g) 重连 ---
            teardown_link();
            if (!running_.load(std::memory_order_acquire)) break;

            {
                std::lock_guard<std::mutex> lk(stat_mu_);
                ++reconnects_;
            }
            set_state("reconnecting", true);
            const DWORD back = static_cast<DWORD>(cfg_.ble_reconnect_ms > 0 ? cfg_.ble_reconnect_ms : 3000);
            log_info("BLE: %lu ms 后重连", static_cast<unsigned long>(back));
            wait_ms(back);
        }
    } catch (const std::exception& e) {
        log_error("BLE: 工作线程抛出异常: %s", e.what());
    } catch (...) {
        log_error("BLE: 工作线程抛出未知异常");
    }

    // 收尾: 在线程内(且线程仍在 MTA 里)释放所有 COM 引用
    teardown_link();
    shared_.reset();
    log_info("BLE: 工作线程退出");
}

bool BleWinRtTransport::init_apartment() {
    const HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
    if (hr == S_OK) return true;
    if (hr == S_FALSE) return true;  // 这条线程本来就初始化过, 不配对 RoUninitialize

    if (hr == RPC_E_CHANGED_MODE) {
        log_warn("BLE: RoInitialize 返回 RPC_E_CHANGED_MODE(线程已在 STA), 退回 CoInitializeEx(MTA)");
        const HRESULT hr_co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(hr_co)) return true;
        log_error("BLE: CoInitializeEx(MTA) 也失败 (HRESULT=0x%08lX), 放弃工作线程",
                  static_cast<unsigned long>(hr_co));
        return false;
    }

    log_error("BLE: RoInitialize 失败 (HRESULT=0x%08lX), 放弃工作线程", static_cast<unsigned long>(hr));
    return false;
}

bool BleWinRtTransport::activate_watcher() {
    if (watcher_) return true;

    // 实例类走 RoActivateInstance + QI; 直接用 RoGetActivationFactory 要实例
    // 接口会拿到 E_NOINTERFACE(实测)。万一这条路径也失败, 退回工厂方式再试
    // 一次 —— 不同 Windows 版本行为不一致, 两条都留着更稳。
    HRESULT hr = activate_instance_and_qi(
        RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher,
        IID_PPV_ARGS(watcher_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !watcher_) {
        log_warn("BLE: RoActivateInstance 建 Watcher 失败 (HRESULT=0x%08lX), 改试激活工厂",
                 static_cast<unsigned long>(hr));
        hr = activate_factory(
            RuntimeClass_Windows_Devices_Bluetooth_Advertisement_BluetoothLEAdvertisementWatcher,
            IID_PPV_ARGS(watcher_.ReleaseAndGetAddressOf()));
    }
    if (FAILED(hr) || !watcher_) {
        log_error("BLE: 激活 IBluetoothLEAdvertisementWatcher 失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    const HRESULT hr_mode = watcher_->put_ScanningMode(BluetoothLEScanningMode_Active);
    if (FAILED(hr_mode)) {
        log_warn("BLE: 设置 ScanningMode=Active 失败 (HRESULT=0x%08lX), 继续用被动扫描",
                 static_cast<unsigned long>(hr_mode));
    }

    // IBluetoothLEAdvertisementWatcher2 不是所有版本都有 —— 可选查, 缺了就降级。
    Microsoft::WRL::ComPtr<IBluetoothLEAdvertisementWatcher2> watcher2;
    const HRESULT hr2 = watcher_.As(&watcher2);
    if (SUCCEEDED(hr2) && watcher2) {
        const HRESULT hr_ext = watcher2->put_AllowExtendedAdvertisements(static_cast<boolean>(1));
        if (FAILED(hr_ext)) {
            log_warn("BLE: 打开扩展广播失败 (HRESULT=0x%08lX), 只用传统广播",
                     static_cast<unsigned long>(hr_ext));
        }
    } else {
        log_warn("BLE: 本机没有 IBluetoothLEAdvertisementWatcher2, 跳过扩展广播设置");
    }

    return true;
}

bool BleWinRtTransport::scan_for_device() {
    {
        std::lock_guard<std::mutex> lk(shared_->mu);
        shared_->found = false;
        shared_->address = 0;
        shared_->rssi = 0;
    }

    std::shared_ptr<SharedState> st = shared_;

    Microsoft::WRL::ComPtr<ITypedEventHandler<BluetoothLEAdvertisementWatcher*,
                                              BluetoothLEAdvertisementReceivedEventArgs*>>
        handler = Microsoft::WRL::Callback<ITypedEventHandler<BluetoothLEAdvertisementWatcher*,
                                                              BluetoothLEAdvertisementReceivedEventArgs*>>(
            [st](IBluetoothLEAdvertisementWatcher* /*sender*/,
                 IBluetoothLEAdvertisementReceivedEventArgs* args) -> HRESULT {
                if (args == nullptr) return S_OK;

                {
                    std::lock_guard<std::mutex> lk(st->mu);
                    ++st->adverts_seen;
                    if (st->found) return S_OK;  // 已经找到了, 别做重复劳动
                }

                Microsoft::WRL::ComPtr<IBluetoothLEAdvertisement> adv;
                if (FAILED(args->get_Advertisement(&adv)) || !adv) return S_OK;

                HSTRING hname = nullptr;
                if (FAILED(adv->get_LocalName(&hname)) || hname == nullptr) return S_OK;

                bool match = false;
                UINT32 len = 0;
                const wchar_t* raw = ::WindowsGetStringRawBuffer(hname, &len);
                if (raw != nullptr && len > 0) {
                    match = (::_wcsicmp(raw, st->target_name.c_str()) == 0) ||
                            (::_wcsicmp(raw, st->alt_name.c_str()) == 0);
                }
                ::WindowsDeleteString(hname);
                if (!match) return S_OK;

                UINT64 addr = 0;
                if (FAILED(args->get_BluetoothAddress(&addr))) return S_OK;

                INT16 rssi = 0;
                (void)args->get_RawSignalStrengthInDBm(&rssi);

                {
                    std::lock_guard<std::mutex> lk(st->mu);
                    if (st->found) return S_OK;
                    st->found = true;
                    st->address = addr;
                    st->rssi = static_cast<int>(rssi);
                }

                // 停止扫描交给工作线程做: 在事件回调里同步调 Stop() 有死锁风险
                ::SetEvent(st->scan_event);
                return S_OK;
            });

    if (!handler) {
        log_error("BLE: 创建广播回调失败");
        return false;
    }

    const HRESULT hr_add = watcher_->add_Received(handler.Get(), &watcher_token_);
    if (FAILED(hr_add)) {
        log_error("BLE: 注册广播回调失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr_add));
        return false;
    }
    watcher_token_valid_ = true;

    const HRESULT hr_start = watcher_->Start();
    if (FAILED(hr_start)) {
        log_error("BLE: 启动扫描失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr_start));
        return false;
    }

    const DWORD timeout = static_cast<DWORD>(cfg_.ble_connect_timeout_ms > 0 ? cfg_.ble_connect_timeout_ms : 15000);
    const DWORD w = ::WaitForSingleObject(shared_->scan_event, timeout);

    // 无论结果如何都先停扫描
    if (watcher_token_valid_) {
        (void)watcher_->remove_Received(watcher_token_);
        watcher_token_valid_ = false;
    }
    (void)watcher_->Stop();

    if (w != WAIT_OBJECT_0) {
        uint64_t seen = 0;
        {
            std::lock_guard<std::mutex> lk(shared_->mu);
            seen = shared_->adverts_seen;
        }
        if (!running_.load(std::memory_order_acquire)) return false;
        log_warn("BLE: %lu ms 内没找到广播名 \"%s\"(共看到 %llu 条广播)",
                 static_cast<unsigned long>(timeout), cfg_.ble_device_name.c_str(),
                 static_cast<unsigned long long>(seen));
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(shared_->mu);
        last_rssi_ = shared_->rssi;
    }
    log_info("BLE: 找到设备 address=%012llX rssi=%d dBm",
             static_cast<unsigned long long>(shared_->address), shared_->rssi);
    return true;
}

bool BleWinRtTransport::connect_and_setup() {
    uint64_t address = 0;
    {
        std::lock_guard<std::mutex> lk(shared_->mu);
        address = shared_->address;
    }

    Microsoft::WRL::ComPtr<IBluetoothLEDeviceStatics> device_statics;
    HRESULT hr = activate_factory(RuntimeClass_Windows_Devices_Bluetooth_BluetoothLEDevice,
                                          IID_PPV_ARGS(device_statics.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !device_statics) {
        log_error("BLE: 激活 IBluetoothLEDeviceStatics 失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IAsyncOperation<BluetoothLEDevice*>> op;
    hr = device_statics->FromBluetoothAddressAsync(address, op.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !op) {
        log_error("BLE: FromBluetoothAddressAsync 调用失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IBluetoothLEDevice> device;
    const DWORD connect_timeout =
        static_cast<DWORD>(cfg_.ble_connect_timeout_ms > 0 ? cfg_.ble_connect_timeout_ms : 15000);
    hr = await_operation(op.Get(), connect_timeout, device.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !device) {
        if (hr == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
            log_error("BLE: 连接设备超时(%lu ms)", static_cast<unsigned long>(connect_timeout));
        } else {
            log_error("BLE: 连接设备失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        }
        return false;
    }
    device_ = device;

    {
        BluetoothConnectionStatus cs = BluetoothConnectionStatus_Disconnected;
        if (SUCCEEDED(device_->get_ConnectionStatus(&cs))) {
            log_info("BLE: 设备已连接 (ConnectionStatus=%d)", static_cast<int>(cs));
        }
    }

    // 设备信息接口是有版本差别的, 只做可选查询, 用于日志
    Microsoft::WRL::ComPtr<IBluetoothLEDevice3> device3;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(device3.ReleaseAndGetAddressOf()))) || !device3) {
        log_error("BLE: 设备不支持 IBluetoothLEDevice3, 无法用 Uncached 模式取 GATT");
        return false;
    }

    // --- 服务 ---
    Microsoft::WRL::ComPtr<IAsyncOperation<GattDeviceServicesResult*>> svc_op;
    hr = device3->GetGattServicesForUuidWithCacheModeAsync(service_uuid_, BluetoothCacheMode_Uncached,
                                                           svc_op.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !svc_op) {
        log_error("BLE: GetGattServicesForUuidAsync 调用失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IGattDeviceServicesResult> svc_res;
    hr = await_operation(svc_op.Get(), connect_timeout, svc_res.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !svc_res) {
        log_error("BLE: 取服务结果失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;
    (void)svc_res->get_Status(&status);
    if (status != GattCommunicationStatus_Success) {
        log_error("BLE: 按 UUID 找不到服务 (GattCommunicationStatus=%d)", static_cast<int>(status));
        return false;
    }

    Microsoft::WRL::ComPtr<IVectorView<GattDeviceService*>> services;
    hr = svc_res->get_Services(services.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !services) {
        log_error("BLE: 读取服务列表失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    UINT32 nsvc = 0;
    (void)services->get_Size(&nsvc);
    if (nsvc == 0) {
        log_error("BLE: 服务列表为空");
        return false;
    }
    hr = services->GetAt(0, service_.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !service_) {
        log_error("BLE: 取第 0 个服务失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }

    Microsoft::WRL::ComPtr<IGattDeviceService3> service3;
    if (FAILED(service_->QueryInterface(IID_PPV_ARGS(service3.ReleaseAndGetAddressOf()))) || !service3) {
        log_error("BLE: 服务不支持 IGattDeviceService3, 无法用 Uncached 模式取特征");
        return false;
    }

    // --- 写特征 ---
    if (![&]() -> bool {
            Microsoft::WRL::ComPtr<IAsyncOperation<GattCharacteristicsResult*>> ch_op;
            const HRESULT h = service3->GetCharacteristicsForUuidWithCacheModeAsync(
                write_uuid_, BluetoothCacheMode_Uncached, ch_op.ReleaseAndGetAddressOf());
            if (FAILED(h) || !ch_op) {
                log_error("BLE: 取写特征(0x%08lX) 调用失败", static_cast<unsigned long>(h));
                return false;
            }
            Microsoft::WRL::ComPtr<IGattCharacteristicsResult> ch_res;
            const HRESULT h2 = await_operation(ch_op.Get(), connect_timeout, ch_res.ReleaseAndGetAddressOf());
            if (FAILED(h2) || !ch_res) {
                log_error("BLE: 取写特征结果失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(h2));
                return false;
            }
            GattCommunicationStatus s = GattCommunicationStatus_Unreachable;
            (void)ch_res->get_Status(&s);
            if (s != GattCommunicationStatus_Success) {
                log_error("BLE: 找不到写特征 (GattCommunicationStatus=%d)", static_cast<int>(s));
                return false;
            }
            Microsoft::WRL::ComPtr<IVectorView<GattCharacteristic*>> chars;
            if (FAILED(ch_res->get_Characteristics(chars.ReleaseAndGetAddressOf())) || !chars) {
                log_error("BLE: 读取写特征列表失败");
                return false;
            }
            UINT32 n = 0;
            (void)chars->get_Size(&n);
            if (n == 0) {
                log_error("BLE: 写特征列表为空");
                return false;
            }
            if (FAILED(chars->GetAt(0, write_char_.ReleaseAndGetAddressOf())) || !write_char_) {
                log_error("BLE: 取第 0 个写特征失败");
                return false;
            }
            return true;
        }()) {
        return false;
    }

    // --- 通知特征 ---
    if (![&]() -> bool {
            Microsoft::WRL::ComPtr<IAsyncOperation<GattCharacteristicsResult*>> ch_op;
            const HRESULT h = service3->GetCharacteristicsForUuidWithCacheModeAsync(
                notify_uuid_, BluetoothCacheMode_Uncached, ch_op.ReleaseAndGetAddressOf());
            if (FAILED(h) || !ch_op) {
                log_error("BLE: 取通知特征调用失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(h));
                return false;
            }
            Microsoft::WRL::ComPtr<IGattCharacteristicsResult> ch_res;
            const HRESULT h2 = await_operation(ch_op.Get(), connect_timeout, ch_res.ReleaseAndGetAddressOf());
            if (FAILED(h2) || !ch_res) {
                log_error("BLE: 取通知特征结果失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(h2));
                return false;
            }
            GattCommunicationStatus s = GattCommunicationStatus_Unreachable;
            (void)ch_res->get_Status(&s);
            if (s != GattCommunicationStatus_Success) {
                log_error("BLE: 找不到通知特征 (GattCommunicationStatus=%d)", static_cast<int>(s));
                return false;
            }
            Microsoft::WRL::ComPtr<IVectorView<GattCharacteristic*>> chars;
            if (FAILED(ch_res->get_Characteristics(chars.ReleaseAndGetAddressOf())) || !chars) {
                log_error("BLE: 读取通知特征列表失败");
                return false;
            }
            UINT32 n = 0;
            (void)chars->get_Size(&n);
            if (n == 0) {
                log_error("BLE: 通知特征列表为空");
                return false;
            }
            if (FAILED(chars->GetAt(0, notify_char_.ReleaseAndGetAddressOf())) || !notify_char_) {
                log_error("BLE: 取第 0 个通知特征失败");
                return false;
            }
            return true;
        }()) {
        return false;
    }

    // IGattCharacteristic3 提供带结果的写(能拿到 GattCommunicationStatus),
    // 拿不到就退回 IGattCharacteristic::WriteValueAsync。
    if (FAILED(write_char_->QueryInterface(IID_PPV_ARGS(write_char3_.ReleaseAndGetAddressOf()))) || !write_char3_) {
        write_char3_.Reset();
        log_warn("BLE: 写特征不支持 IGattCharacteristic3, 退回 WriteValueAsync");
    }

    // 一次性的 IBuffer 工厂
    if (!buffer_factory_) {
        hr = activate_factory(RuntimeClass_Windows_Storage_Streams_Buffer,
                                      IID_PPV_ARGS(buffer_factory_.ReleaseAndGetAddressOf()));
        if (FAILED(hr) || !buffer_factory_) {
            log_error("BLE: 激活 IBufferFactory 失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
            return false;
        }
    }

    // --- (e) 订阅 ---
    return subscribe_notify_and_conn();
}

bool BleWinRtTransport::subscribe_notify_and_conn() {
    std::shared_ptr<SharedState> st = shared_;

    // --- 通知特征: 解析 B1 ---
    Microsoft::WRL::ComPtr<ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*>> notify_handler =
        Microsoft::WRL::Callback<ITypedEventHandler<GattCharacteristic*, GattValueChangedEventArgs*>>(
            [st](IGattCharacteristic* /*sender*/, IGattValueChangedEventArgs* args) -> HRESULT {
                if (args == nullptr) return S_OK;

                Microsoft::WRL::ComPtr<IBuffer> buf;
                if (FAILED(args->get_CharacteristicValue(buf.ReleaseAndGetAddressOf())) || !buf) return S_OK;

                UINT32 len = 0;
                if (FAILED(buf->get_Length(&len)) || len == 0) return S_OK;

                Microsoft::WRL::ComPtr<ByteAccessBuffer> bytes_access;
                if (FAILED(buf.As(&bytes_access)) || !bytes_access) return S_OK;

                byte* raw = nullptr;
                if (FAILED(bytes_access->Buffer(&raw)) || raw == nullptr) return S_OK;

                // 需要 4 个字节: [0]=0xB1 [1]=seq [2]=A 实际 [3]=B 实际
                if (len < 4 || raw[0] != kNotifyB1) {
                    // 不是 B1 —— 但绝不静默丢弃。设备如果发的是别的东西,
                    // 或者干脆什么都没发, 这两种情况必须能区分开, 否则
                    // "等不到 B1" 这句话既可能是设备没应, 也可能是我们没认出来。
                    std::lock_guard<std::mutex> lk(st->mu);
                    ++st->other_notify_count;
                    if (st->other_notify_count <= 4) {
                        // 前几条原样记下来, 字节级
                        char hex[3 * 32 + 1]{};
                        const UINT32 n = len < 32 ? len : 32;
                        for (UINT32 i = 0; i < n; ++i) {
                            std::snprintf(hex + i * 3, 4, "%02X ", raw[i]);
                        }
                        log_info("BLE: 收到非 B1 通知 (len=%u): %s", len, hex);
                    }
                    return S_OK;
                }

                const uint8_t seq = raw[1];
                const int a = static_cast<int>(raw[2]);
                const int b = static_cast<int>(raw[3]);

                // 前 5 条 B1 把原始字节打出来。"设备报了 A=0" 有两种可能:
                // 设备真给了 0, 或者我们把 4 个字节认错了位置。不打出字节就分不开。
                if (st->b1_count < 5) {
                    log_info("BLE: B1 原始字节 (len=%u): %02X %02X %02X %02X -> seq=%u A=%d B=%d", len, raw[0],
                             raw[1], raw[2], raw[3], static_cast<unsigned>(seq), a, b);
                }

                std::lock_guard<std::mutex> lk(st->mu);
                ++st->b1_count;
                st->b1_last_seq = seq;
                st->b1_last_a = a;
                st->b1_last_b = b;
                if (seq != 0) {
                    st->acked_mask = static_cast<uint16_t>(st->acked_mask | (1u << seq));
                    st->acked_a = a;
                    st->acked_b = b;
                }
                return S_OK;
            });

    if (!notify_handler) {
        log_error("BLE: 创建通知回调失败");
        return false;
    }

    HRESULT hr = notify_char_->add_ValueChanged(notify_handler.Get(), &notify_token_);
    if (FAILED(hr)) {
        log_error("BLE: 订阅通知特征失败 (HRESULT=0x%08lX)", static_cast<unsigned long>(hr));
        return false;
    }
    notify_token_valid_ = true;

    // ★ 注册本地回调**不等于**订阅。必须再写一次 CCCD(0x2902), 否则设备根本
    //   不知道要发通知 —— B0 写会成功、强度也会真的变, 但 B1 永远收不到,
    //   日志上表现成"等了 20 帧仍无 B1 应答", 看上去像设备不应答。
    //
    //   踩过: 这一段以前只有 add_ValueChanged, 所以从写下来那天起就没收到过
    //   任何通知。官方 v3 文档: "Subscribe to this characteristic after
    //   connecting." —— 那个 subscribe 指的就是写 CCCD。
    {
        HRESULT hr_cccd = write_cccd_sync(
            notify_char_.Get(),
            ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::
                GattClientCharacteristicConfigurationDescriptorValue_Notify);
        if (FAILED(hr_cccd)) {
            // 有些设备只声明 Indicate。两者都能收到 B1, 差别只在链路层是否要确认。
            log_warn("BLE: 写 CCCD(Notify) 失败 (HRESULT=0x%08lX), 改试 Indicate",
                     static_cast<unsigned long>(hr_cccd));
            hr_cccd = write_cccd_sync(
                notify_char_.Get(),
                ABI::Windows::Devices::Bluetooth::GenericAttributeProfile::
                    GattClientCharacteristicConfigurationDescriptorValue_Indicate);
        }
        if (FAILED(hr_cccd)) {
            log_error("BLE: 订阅 CCCD 失败 (HRESULT=0x%08lX) —— 设备不会发任何通知",
                      static_cast<unsigned long>(hr_cccd));
            return false;
        }
        log_info("BLE: 已写入 CCCD, 设备侧通知已开启");
    }
    log_info("BLE: 已订阅通知特征");

    // --- 设备连接状态: 掉了就断链 ---
    Microsoft::WRL::ComPtr<ITypedEventHandler<BluetoothLEDevice*, IInspectable*>> conn_handler =
        Microsoft::WRL::Callback<ITypedEventHandler<BluetoothLEDevice*, IInspectable*>>(
            [st](IBluetoothLEDevice* sender, IInspectable* /*args*/) -> HRESULT {
                if (sender == nullptr) return S_OK;
                BluetoothConnectionStatus cs = BluetoothConnectionStatus_Disconnected;
                if (FAILED(sender->get_ConnectionStatus(&cs))) return S_OK;
                if (cs != BluetoothConnectionStatus_Connected) {
                    st->link_down.store(true, std::memory_order_release);
                    ::SetEvent(st->wake_event);
                }
                return S_OK;
            });

    if (conn_handler) {
        hr = device_->add_ConnectionStatusChanged(conn_handler.Get(), &conn_token_);
        if (FAILED(hr)) {
            log_warn("BLE: 订阅连接状态变化失败 (HRESULT=0x%08lX), 仅靠写失败检测断链",
                     static_cast<unsigned long>(hr));
        } else {
            conn_token_valid_ = true;
        }
    }

    // --- BF 软限位: 必须在第一条 B0 之前发出去 ---
    if (!send_bf()) return false;

    // 新链路: 已知实际强度作废, 于是第一帧必然用 Absolute 主动对齐
    inflight_seq_ = 0;
    inflight_frames_ = 0;
    actual_a_ = -1;
    actual_b_ = -1;
    next_seq_ = 1;
    bf_sent_ = true;

    return true;
}

bool BleWinRtTransport::send_bf() {
    const int lim = bf_limit();
    const B0 bf = make_bf(lim, lim);

    const DWORD write_timeout = static_cast<DWORD>((cfg_.frame_ms > 0 ? cfg_.frame_ms : 100) * 2);
    const HRESULT hr = write_bytes(bf.bytes, static_cast<uint32_t>(kBfSize), write_timeout);
    if (FAILED(hr)) {
        const std::string e = hresult_hex(hr);
        log_error("BLE: 发送 BF 软限位失败 (HRESULT=%s), 拒绝在无软限位保护下继续", e.c_str());
        {
            std::lock_guard<std::mutex> lk(stat_mu_);
            ++errors_;
        }
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stat_mu_);
        ++writes_;
    }
    log_info("BLE: 已发送 BF 软限位 A=B=%d", lim);
    return true;
}

HRESULT BleWinRtTransport::get_buffer(uint32_t len, IBuffer** out) {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (!buffer_factory_) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<IBuffer> buf;
    const HRESULT hr = buffer_factory_->Create(len, buf.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !buf) return FAILED(hr) ? hr : E_FAIL;
    *out = buf.Detach();
    return S_OK;
}

HRESULT BleWinRtTransport::write_bytes(const uint8_t* data, uint32_t len, DWORD timeout_ms) {
    if (data == nullptr || len == 0) return E_INVALIDARG;
    if (!write_char_) return E_UNEXPECTED;

    Microsoft::WRL::ComPtr<IBuffer> buf;
    HRESULT hr = get_buffer(len, buf.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !buf) return FAILED(hr) ? hr : E_FAIL;

    // 往 IBuffer 里灌字节: IBufferByteAccess::Buffer 给出原始指针
    Microsoft::WRL::ComPtr<ByteAccessBuffer> bytes_access;
    hr = buf.As(&bytes_access);
    if (FAILED(hr) || !bytes_access) return hr;

    byte* raw = nullptr;
    hr = bytes_access->Buffer(&raw);
    if (FAILED(hr) || raw == nullptr) return FAILED(hr) ? hr : E_POINTER;

    std::memcpy(raw, data, len);
    hr = buf->put_Length(len);
    if (FAILED(hr)) return hr;

    GattCommunicationStatus status = GattCommunicationStatus_Unreachable;

    if (write_char3_) {
        Microsoft::WRL::ComPtr<IAsyncOperation<GattWriteResult*>> op;
        hr = write_char3_->WriteValueWithResultAsync(buf.Get(), op.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !op) return FAILED(hr) ? hr : E_FAIL;

        Microsoft::WRL::ComPtr<IGattWriteResult> res;
        hr = await_operation(op.Get(), timeout_ms, res.ReleaseAndGetAddressOf());
        if (FAILED(hr)) return hr;
        if (!res) return E_FAIL;
        (void)res->get_Status(&status);
    } else {
        Microsoft::WRL::ComPtr<IAsyncOperation<GattCommunicationStatus>> op;
        hr = write_char_->WriteValueAsync(buf.Get(), op.ReleaseAndGetAddressOf());
        if (FAILED(hr) || !op) return FAILED(hr) ? hr : E_FAIL;

        hr = await_operation(op.Get(), timeout_ms, &status);
        if (FAILED(hr)) return hr;
    }

    if (status != GattCommunicationStatus_Success) {
        // 设备侧拒绝/链路不可达 —— 统一按"这条链路已经不可用"上报
        return HRESULT_FROM_WIN32(ERROR_BAD_COMMAND);
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// 拆链路: 撤订阅 -> 停扫描 -> 释放所有 COM 引用。只在工作线程上调用。
// ---------------------------------------------------------------------------
void BleWinRtTransport::teardown_link() {
    if (notify_token_valid_ && notify_char_) {
        (void)notify_char_->remove_ValueChanged(notify_token_);
        notify_token_valid_ = false;
    }
    if (conn_token_valid_ && device_) {
        (void)device_->remove_ConnectionStatusChanged(conn_token_);
        conn_token_valid_ = false;
    }
    if (watcher_token_valid_ && watcher_) {
        (void)watcher_->remove_Received(watcher_token_);
        watcher_token_valid_ = false;
    }
    if (watcher_) {
        (void)watcher_->Stop();
    }

    write_char3_.Reset();
    write_char_.Reset();
    notify_char_.Reset();
    service_.Reset();
    device_.Reset();
    watcher_.Reset();

    if (shared_) {
        shared_->link_down.store(false, std::memory_order_release);
    }
    bf_sent_ = false;
}

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------
ITransport* create_ble_transport(const Config& cfg) {
    // 1) 配置校验: UUID 解析不了就没必要起线程了。
    const std::string svc = cfg.ble_service_uuid.empty() ? std::string(kServiceUuidDefault) : cfg.ble_service_uuid;
    const std::string wr = cfg.ble_write_uuid.empty() ? std::string(kWriteUuidDefault) : cfg.ble_write_uuid;
    const std::string nt = cfg.ble_notify_uuid.empty() ? std::string(kNotifyUuidDefault) : cfg.ble_notify_uuid;

    //    三条都要能定位到具体是哪一个 —— "service_uuid 不合法" 和
    //    "write_uuid 不合法" 指向完全不同的配置错误。但三个默认值是同一族
    //    GUID(只有第二段不同), 所以文案还必须把值本身打出来, 否则光看键名
    //    仍然分不清是"配置写错了"还是"默认值本身解析不了"。
    GUID g{};
    struct UuidCheck {
        const char* key;
        const std::string& value;
    };
    const UuidCheck checks[3] = {{"service_uuid", svc}, {"write_uuid", wr}, {"notify_uuid", nt}};
    for (const UuidCheck& c : checks) {
        if (!parse_guid(c.value, &g)) {
            log_error("BLE: [ble] %s 不是合法 GUID: \"%s\"", c.key, c.value.c_str());
            return nullptr;
        }
    }

    // 2) 本机可用性探测。必须在我们自己起的线程上做 —— 调用方是游戏线程,
    //    按约束绝不能在那里初始化套间。
    HRESULT probe = E_FAIL;
    try {
        std::thread t([&probe]() { probe = probe_winrt_ble_once(); });
        t.join();
    } catch (const std::exception& e) {
        log_error("BLE: 可用性探测线程创建失败: %s", e.what());
        return nullptr;
    }

    if (FAILED(probe)) {
        const std::string e = hresult_hex(probe);
        log_error("BLE: 本机 WinRT BLE 不可用 (HRESULT=%s)", e.c_str());
        return nullptr;
    }

    return new BleWinRtTransport(cfg);
}

}  // namespace dglab_idac
