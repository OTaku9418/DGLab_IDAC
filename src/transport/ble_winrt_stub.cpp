// ---------------------------------------------------------------------------
// DGLab_IDAC — BLE/WinRT 传输
//
// ⚠ 临时占位实现。真正的 WinRT 版本正在单独编写中(src/transport/ble_winrt.cpp
//   的完整实现)。这个文件的存在只是为了在 BLE 实现落地之前让整个工程能编译
//   通过 —— 这样力反馈 hook 与管线可以先行验证, 不必等传输层。
//
// 它刻意**不**尝试任何 BLE 操作, 直接返回 nullptr, 于是 create_transport()
// 会记录一条明确的错误并降级到 NullTransport(干跑)。这是正确的降级行为,
// 不是静默失败。
// ---------------------------------------------------------------------------
#include "ble_winrt.hpp"

#include "logger.hpp"

namespace dglab_idac {

ITransport* create_ble_transport(const Config& cfg) {
    (void)cfg;
    log_error("BLE 传输尚未构建进本二进制(ble_winrt.cpp 目前是占位实现)。"
              "探针阶段不影响使用 —— [link] mode 保持 disabled 即可。");
    return nullptr;
}

}  // namespace dglab_idac
