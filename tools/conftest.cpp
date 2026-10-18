// ---------------------------------------------------------------------------
// conftest — 离线校验 DGLab_IDAC.ini 能不能被正确解析。
//
// 为什么需要它: 配置文件是"人写的", 而它读错的表现是**沉默的**。实测里就
// 因为 dry_run 被解析成 1, 整条 BLE 链路一行代码都没跑到, 而日志上一切正常
// (mod 正常加载、钩子正常安装、帧数正常增长)。光读日志看不出"少做了什么"。
//
// 用法: conftest.exe <DGLab_IDAC.ini>
// 只为把关键字段回显出来, 不打进游戏, 不需要 UE4SS。
//
// 构建: tools\build_conftest.cmd
//   cl /nologo /std:c++17 /EHsc /O2 /W3 /utf-8 /I..\src ^
//      conftest.cpp ..\src\config.cpp /Fe:conftest.exe
//   (/utf-8 不能省 —— 源码里有中文注释和字符串, 少了会按 ANSI 解析报一堆
//    C2001/C2051, 主工程 CMakeLists.txt:216 也是为这个加的)
// ---------------------------------------------------------------------------
#include <cstdio>
#include <string>

#include "config.hpp"
#include "logger.hpp"

// ---- logger.cpp 的最小替身: config.cpp 只需要这几个符号存在 ----------------
namespace dglab_idac {
std::string log_init(const std::string&) { return {}; }
void log_shutdown() {}
void log_line(const char*, ...) {}
void log_info(const char*, ...) {}
void log_warn(const char*, ...) {}
void log_error(const char*, ...) {}
void log_set_debug_output(bool) {}
const char* log_path() { return ""; }
}  // namespace dglab_idac

using namespace dglab_idac;

static void dump_channel(const char* name, const ChannelMapping& m) {
    std::printf("[%s]\n", name);
    std::printf("  enable                 = %d\n", m.enable ? 1 : 0);
    std::printf("  source                 = %s\n", m.source.c_str());
    std::printf("  map_mode               = %s\n", to_string(m.mode));
    std::printf("  in_min / in_max        = %d / %d\n", m.in_min, m.in_max);
    std::printf("  level                  = %.3f\n", m.level);
    std::printf("  base_strength          = %d   <-- 0 = 设备静音\n", m.base_strength);
    std::printf("  strength_source        = %s\n", m.strength_source.c_str());
    std::printf("  strength_margin        = %.3f\n", m.strength_margin);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("用法: conftest.exe <DGLab_IDAC.ini>\n");
        return 2;
    }

    const Config c = load_config(argv[1]);

    std::printf("配置文件: %s\n\n", argv[1]);

    std::printf("[general]\n");
    std::printf("  enable                 = %d\n", c.enabled ? 1 : 0);
    std::printf("  verbose                = %d\n", c.verbose ? 1 : 0);
    std::printf("  log_file               = \"%s\"\n", c.log_file.c_str());

    std::printf("[link]\n");
    std::printf("  mode                   = %s\n", to_string(c.link));
    std::printf("  protocol               = %s\n", to_string(c.protocol));

    std::printf("[ble]\n");
    std::printf("  device_name            = %s\n", c.ble_device_name.c_str());
    std::printf("  service_uuid           = \"%s\"\n", c.ble_service_uuid.c_str());
    std::printf("  write_uuid             = \"%s\"\n", c.ble_write_uuid.c_str());
    std::printf("  notify_uuid            = \"%s\"\n", c.ble_notify_uuid.c_str());
    std::printf("  reconnect_ms           = %d\n", c.ble_reconnect_ms);
    std::printf("  connect_timeout_ms     = %d\n", c.ble_connect_timeout_ms);

    std::printf("[output]\n");
    std::printf("  frame_ms               = %d\n", c.frame_ms);
    std::printf("  pulse_freq_base        = %d\n", c.pulse_freq_base);
    std::printf("  pulse_freq_min         = %d\n", c.pulse_freq_min);
    std::printf("  pulse_freq_max         = %d\n", c.pulse_freq_max);
    std::printf("  pulse_freq_track       = %.3f\n", c.pulse_freq_track);
    std::printf("  auto_gain              = %d\n", c.auto_gain ? 1 : 0);
    std::printf("  gain_attack_sec        = %.3f\n", c.gain_attack_sec);
    std::printf("  gain_release_sec       = %.3f\n", c.gain_release_sec);
    std::printf("  gain_floor             = %d\n", c.gain_floor);
    std::printf("  gain_ceiling           = %d\n", c.gain_ceiling);
    std::printf("  gain_idle_reset_sec    = %.3f\n", c.gain_idle_reset_sec);

    dump_channel("channel_a", c.channel_a);
    dump_channel("channel_b", c.channel_b);

    std::printf("[safety]\n");
    std::printf("  max_strength           = %d\n", c.max_strength);
    std::printf("  clamp_strength         = %d\n", c.clamp_strength);
    std::printf("  slew_per_second        = %.1f\n", c.slew_per_second);
    std::printf("  watchdog_ms            = %d\n", c.watchdog_ms);
    std::printf("  zero_when_servo_off    = %d\n", c.zero_when_servo_off ? 1 : 0);
    std::printf("  warmup_ms              = %d\n", c.warmup_ms);
    std::printf("  dry_run                = %d   <-- 0 = 允许真实输出\n", c.dry_run ? 1 : 0);

    std::printf("[debug]\n");
    std::printf("  hook                   = %d\n", c.hook ? 1 : 0);

    // ---- 结论: 把"这轮到底会不会碰设备"直接判定出来.
    // 这一栏才是这个工具存在的意义 —— 上一轮所有字段看起来都对, 唯独
    // dry_run 把真实输出拦掉了, 而日志里没有任何"失败"可看.
    const bool real_link = (c.link == LinkMode::Ble || c.link == LinkMode::WebSocket);
    const bool will_touch_device = c.enabled && !c.dry_run && real_link;
    const bool silent = (c.channel_a.base_strength == 0 && c.channel_b.base_strength == 0);

    std::printf("\n================ 判定 ================\n");
    std::printf("  会连真实设备吗 : %s\n", will_touch_device ? "是" : "否");
    if (!will_touch_device) {
        if (!c.enabled)       std::printf("  原因           : [general] enable=0\n");
        else if (c.dry_run)   std::printf("  原因           : [safety] dry_run=1 硬开关\n");
        else                  std::printf("  原因           : [link] mode 没指向真实链路\n");
        std::printf("  设备会有刺激吗 : 否 (不连设备)\n");
    } else {
        std::printf("  设备会有刺激吗 : %s\n",
                    silent ? "否 (通道强度=0, 链路照跑)" : "★ 会 ★");
        if (!silent) {
            std::printf("  通道 A 强度     : %d\n", c.channel_a.base_strength);
            std::printf("  通道 B 强度     : %d\n", c.channel_b.base_strength);
        }
    }
    // 持值风险 (T11b): 只提示, 不改变上面的判定 —— 用 viscosity 不是"错",
    // 只是"退出比赛回菜单后可能留着一段可感的输出"。放在这里是因为它是
    // 上线前唯一能在设备之外发现的途径: load_config 的 WARN 只写进日志。
    {
        const char* risky_a = (c.channel_a.enable && c.channel_a.base_strength > 0 &&
                               is_held_risk_source(c.channel_a.source))
                                  ? "A"
                                  : "";
        const char* risky_b = (c.channel_b.enable && c.channel_b.base_strength > 0 &&
                               is_held_risk_source(c.channel_b.source))
                                  ? "B"
                                  : "";
        if (risky_a[0] != '\0' || risky_b[0] != '\0') {
            std::printf("  持值风险        : 通道 %s%s 用了 viscosity —— 强制退出比赛回菜单后\n",
                        risky_a, risky_b);
            std::printf("                    阻尼可能停在可感的值上并持续输出 (见 README 5.1)\n");
        } else {
            std::printf("  持值风险        : 无 (没有通道用 viscosity)\n");
        }
    }
    std::printf("======================================\n");
    return 0;
}
