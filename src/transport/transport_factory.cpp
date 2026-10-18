// ---------------------------------------------------------------------------
// DGLab_IDAC — 传输工厂
//
// 用户要求"连接方式通过配置文件切换"。目前**只有 BLE 一条真路径**。
//
// WebSocket 尚未实现。计划基于官方 DG-LAB V4 协议(dglab-websocket-server 的
// v4-server.ts + dglab-kit 的 device.op / clearOperate), 但那条路**仍然需要
// 手机 APP 做蓝牙中转**:
//     third-party controller(本程序) <-> WebSocket 中继 <-> 多个 DG-LAB 4 APP
// 中继服务器不解析设备命令, 只转发 data 字段; 真正的 BLE 链路在手机上。
// 对逐帧变化的力反馈来说, 它比 BLE 直连多一跳网络 + 一套任务队列语义, 所以
// 主线继续走 BLE。详见 docs/research-dglab-transport.md 第 1.2 节。
//
// 分派规则:
//   dry_run = 1       -> Null, 且**优先于 link** (不管 link 写的是什么)
//   link = ble        -> BleWinRtTransport (失败时降级到 Null 并告警)
//   link = websocket  -> 未实现, 降级到 Null 并明确告知
//   link = disabled   -> Null
//
// dry_run 放在最前面是有意的: 它是"绝对不要输出到设备"的硬开关。曾经它只
// 出现在一行日志里, 于是 [link] mode=ble + dry_run=1 会真的去电人 —— 配置项
// 写着"干跑"却在输出, 这种坑不能留。要真实输出必须显式把 dry_run 改成 0。
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

    if (cfg.dry_run) {
        log_warn("[safety] dry_run=1 —— 硬开关生效, 不会向任何设备输出"
                 "(即使 [link] mode=%s 也走空输出)。要真实输出请改成 dry_run=0。",
                 to_string(cfg.link));
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
            log_error("[link] mode=websocket 尚未实现。计划基于官方 V4 协议: "
                      "本程序作为控制器连 dglab-websocket-server 的 v4 中继, "
                      "DG-LAB 4 APP 用 ?tid=<控制器 clientId> 连进来, "
                      "设备命令走 dglab-kit 的 device.op / device.op.clear。"
                      "注意中继服务器不做 BLE —— 手机 APP 仍是蓝牙中枢, "
                      "所以它解决不了\"PC 直连\"的目标, 只多了一跳网络与任务队列语义。"
                      "详见 docs/research-dglab-transport.md 第 1.2 节。");
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
