// ---------------------------------------------------------------------------
// DGLab_IDAC — 传输工厂
//
// 用户要求"连接方式通过配置文件切换"。目前**只有 BLE 一条真路径** ——
// 本地协议仓库(dglab-bluetooth-protocol)里没有 WebSocket 规范, 那部分是
// 从网上查到的手机 APP 方案(见 docs/research-coyote-protocol.md), 需要
// 手机参与, 与"PC 直连"的目标不符, 所以没有实现。
//
// 分派规则:
//   link = ble        -> BleWinRtTransport (失败时降级到 Null 并告警)
//   link = websocket  -> 未实现, 降级到 Null 并明确告知
//   link = disabled   -> Null
// ---------------------------------------------------------------------------
#include <string>

#include "ble_winrt.hpp"
#include "logger.hpp"
#include "transport/transport.hpp"

namespace dglab_idac {

ITransport* create_transport(const Config& cfg) {
    if (!cfg.enabled) {
        log_warn("[general] enable=0 —— mod 被配置为禁用, 使用空输出");
        return create_null_transport(cfg);
    }

    switch (cfg.link) {
        case LinkMode::Ble: {
            log_info("连接方式: BLE 直连 (设备名 \"%s\", 协议 %s)",
                     cfg.ble_device_name.c_str(), to_string(cfg.protocol));
            if (cfg.protocol != ProtocolVersion::V3) {
                // V2 的波形路径是 3 个 3 字节特性、没有 20 字节 B0、还有小端
                // 字节序陷阱(见 docs/research-coyote-protocol.md)。这里没有实现,
                // 不要假装能用。
                log_error("[link] protocol=%s 尚未实现 —— BLE 传输只支持 v3。"
                          "请把 protocol 改成 v3, 或先切到 disabled 干跑。",
                          to_string(cfg.protocol));
                return create_null_transport(cfg);
            }
            ITransport* t = create_ble_transport(cfg);
            if (!t) {
                log_error("BLE 传输初始化失败(本机可能没有蓝牙, 或 WinRT 不可用), 降级到空输出");
                return create_null_transport(cfg);
            }
            return t;
        }

        case LinkMode::WebSocket:
            log_error("[link] mode=websocket 尚未实现。本地协议仓库里没有 WS 规范, "
                      "那条路需要手机 APP 参与(见 docs/research-coyote-protocol.md 路径 B)。"
                      "先降级到空输出。");
            return create_null_transport(cfg);

        case LinkMode::Disabled:
            log_info("连接方式: disabled(干跑)");
            return create_null_transport(cfg);
    }

    log_error("未知的 link 模式, 降级到空输出");
    return create_null_transport(cfg);
}

void destroy_transport(ITransport* t) {
    if (!t) return;
    t->stop();
    delete t;
}

}  // namespace dglab_idac
