// ---------------------------------------------------------------------------
// DGLab_IDAC — 配置实现
//
// 用 Win32 的 GetPrivateProfileString 读 INI —— 不引第三方 parser, 而且
// 和 segatools.ini 的处理方式一致。
// ---------------------------------------------------------------------------
#include "config.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "logger.hpp"

namespace dglab_idac {

namespace {

// ---------------------------------------------------------------------------
// INI 行尾归一化: 把裸 LF 全部转成 CRLF。
//
// Win32 的 INI API 只认 CRLF。一个 LF-only 的 INI 会被读成"什么都没有" ——
// 每个键都回落到默认值, 而且不报任何错。这个坑真的发生过(见 load_config
// 里的长注释), 所以放在这里做成一个不可绕过的动作。
//
// 返回 true 表示确实改写了文件。
// ---------------------------------------------------------------------------
bool normalize_ini_line_endings(const std::string& path) {
    HANDLE h = ::CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (16 * 1024 * 1024)) {
        ::CloseHandle(h);
        return false;
    }

    std::string data(static_cast<size_t>(sz.QuadPart), '\0');
    DWORD got = 0;
    const BOOL ok = ::ReadFile(h, &data[0], static_cast<DWORD>(data.size()), &got, nullptr);
    ::CloseHandle(h);
    if (!ok) return false;
    data.resize(got);

    // 统计裸 LF(前面不是 CR 的 LF)
    size_t bare = 0;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) ++bare;
    }
    if (bare == 0) return false;   // 已经是 CRLF, 不动

    std::string out;
    out.reserve(data.size() + bare);
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\n' && (i == 0 || data[i - 1] != '\r')) out.push_back('\r');
        out.push_back(data[i]);
    }

    // ★ 原子写入 (T13d) ★
    // 以前直接 CREATE_ALWAYS 覆盖原文件: 如果写到一半失败(磁盘满/被杀进程),
    // 用户配置就变成一个残缺的 INI —— 而下次启动会把它当"合法配置"读, 每项
    // 都可能回落到内置默认值(其中 base_strength 的默认值比用户配的大得多)。
    // 所以先写 <path>.tmp, 再用 MoveFileEx 替换; 任何一步失败都保留原文件。
    const std::string tmp = path + ".tmp";
    HANDLE w = ::CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (w == INVALID_HANDLE_VALUE) {
        log_warn("配置 %s 是 LF 行尾(Win32 INI API 会读不到内容), 但临时文件建不出来 —— "
                 "本次仍按默认值运行, 原文件未改动", path.c_str());
        return false;
    }
    DWORD wrote = 0;
    const BOOL wok = ::WriteFile(w, out.data(), static_cast<DWORD>(out.size()), &wrote, nullptr);
    ::FlushFileBuffers(w);
    ::CloseHandle(w);

    if (!wok || wrote != out.size()) {
        ::DeleteFileA(tmp.c_str());
        log_error("配置 %s 行尾归一化写入不完整 (%lu/%zu), 已放弃改写, 原文件未改动", path.c_str(),
                  static_cast<unsigned long>(wrote), out.size());
        return false;
    }
    if (!::MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD err = ::GetLastError();
        ::DeleteFileA(tmp.c_str());
        log_warn("配置 %s 行尾归一化没能替换原文件 (GetLastError=%lu), 原文件未改动 —— "
                 "本次仍按默认值运行", path.c_str(), static_cast<unsigned long>(err));
        return false;
    }

    log_warn("配置 %s 原本是 LF 行尾 —— Win32 的 INI API 只认 CRLF, 那样会让**所有**配置项"
             "静默回落成内置默认值。已自动改写成 CRLF(%zu 处)。",
             path.c_str(), bare);
    return true;
}

// ---------------------------------------------------------------------------
// 大小写不敏感的字符串比较(两个来源都是我们自己的字面量/INI 值)
// ---------------------------------------------------------------------------
bool ieq(const std::string& a, const char* b) {
    if (!b) return false;
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return i == a.size() && b[i] == '\0';
}

std::string read_str(const std::string& file, const char* section, const char* key,
                     const std::string& def) {
    char buf[1024]{};
    ::GetPrivateProfileStringA(section, key, def.c_str(), buf, sizeof(buf), file.c_str());
    return std::string(buf);
}

int read_int(const std::string& file, const char* section, const char* key, int def) {
    return static_cast<int>(::GetPrivateProfileIntA(section, key, def, file.c_str()));
}

double read_double(const std::string& file, const char* section, const char* key, double def) {
    const std::string s = read_str(file, section, key, {});
    if (s.empty()) return def;
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) {
        log_warn("配置 [%s] %s=\"%s\" 不是合法数字, 用默认值", section, key, s.c_str());
        return def;
    }
    return v;
}

bool read_bool(const std::string& file, const char* section, const char* key, bool def) {
    const std::string s = read_str(file, section, key, def ? "1" : "0");
    return s == "1" || s == "true" || s == "True" || s == "TRUE" || s == "yes" || s == "on";
}

// 判断 INI 里到底有没有这个键。
// 为什么需要: read_int/read_bool 传进去的是默认值, 分不清"键不存在"和
// "键值恰好等于默认值"。对 dry_run 这种决定要不要真的通电的字段, 这两种
// 情况必须区分开 —— 缺键要告警。用一个不可能出现的哨兵值探一次即可。
bool has_key(const std::string& file, const char* section, const char* key) {
    static const char* const kSentinel = "__dglab_missing_key__";
    char buf[64]{};
    ::GetPrivateProfileStringA(section, key, kSentinel, buf, sizeof(buf), file.c_str());
    return std::strcmp(buf, kSentinel) != 0;
}

MapMode parse_map_mode(const std::string& s, MapMode def) {
    if (ieq(s, "linear")) return MapMode::Linear;
    if (ieq(s, "abs_linear") || ieq(s, "abslinear")) return MapMode::AbsLinear;
    if (ieq(s, "sqrt")) return MapMode::Sqrt;
    if (ieq(s, "square")) return MapMode::Square;
    if (ieq(s, "threshold")) return MapMode::Threshold;
    if (ieq(s, "constant")) return MapMode::Constant;
    if (!s.empty()) log_warn("未知的 map_mode \"%s\", 用默认值", s.c_str());
    return def;
}

void read_channel(const std::string& file, const char* section, ChannelMapping& c) {
    // 段名形如 [channel_a], 键名不带前缀
    c.enable        = read_bool(file, section, "enable", c.enable);
    c.source        = read_str(file, section, "source", c.source);
    c.mode          = parse_map_mode(read_str(file, section, "map_mode", ""), c.mode);
    c.in_min        = read_int(file, section, "in_min", c.in_min);
    c.in_max        = read_int(file, section, "in_max", c.in_max);
    c.level         = read_double(file, section, "level", c.level);
    c.base_strength = read_int(file, section, "base_strength", c.base_strength);
    c.strength_source = read_str(file, section, "strength_source", c.strength_source);
    c.strength_margin = read_double(file, section, "strength_margin", c.strength_margin);

    if (c.strength_source != "static" && c.strength_source != "level") {
        log_warn("[%s] strength_source=\"%s\" 不是 static/level, 按 static 处理",
                 section, c.strength_source.c_str());
        c.strength_source = "static";
    }
    if (c.strength_margin < 0.0 || c.strength_margin > 1.0) {
        log_warn("[%s] strength_margin=%.3f 超出 [0,1], 已钳制", section, c.strength_margin);
        c.strength_margin = (c.strength_margin < 0.0) ? 0.0 : 1.0;
    }

    if (c.in_max <= c.in_min) {
        log_warn("[%s] in_max(%d) <= in_min(%d), 该通道映射会退化为恒定最大值",
                 section, c.in_max, c.in_min);
    }
}

}  // namespace

const char* to_string(LinkMode m) {
    switch (m) {
        case LinkMode::WebSocket: return "websocket";
        case LinkMode::Ble:       return "ble";
        case LinkMode::Disabled:  return "disabled";
    }
    return "?";
}

const char* to_string(ProtocolVersion v) {
    switch (v) {
        case ProtocolVersion::V2: return "v2";
        case ProtocolVersion::V3: return "v3";
    }
    return "?";
}

const char* to_string(MapMode m) {
    switch (m) {
        case MapMode::Linear:    return "linear";
        case MapMode::AbsLinear: return "abs_linear";
        case MapMode::Sqrt:      return "sqrt";
        case MapMode::Square:    return "square";
        case MapMode::Threshold: return "threshold";
        case MapMode::Constant:  return "constant";
    }
    return "?";
}

bool is_held_risk_source(const std::string& src) {
    return ieq(src, "viscosity") || ieq(src, "viscosity_a");
}

int effective_frame_ms(int requested_ms, LinkMode link) {
    // 只有 BLE 直连这条链路的节拍是硬的: V3 的 B0 每帧覆盖 4×25 ms = 100 ms,
    // 设备也按这个窗口回 B1。改成 50 不会让设备变快, 只会让"管线提交节拍"
    // 与"设备消费节拍"错位, 信箱覆盖率和看门狗阈值跟着一起算错。
    // 空输出/WebSocket 没有这个约束, 原样返回。
    if (link != LinkMode::Ble) return requested_ms;
    if (requested_ms == 100) return 100;
    log_warn("[output] frame_ms=%d: BLE 链路的帧节拍固定为 100 ms, 已按 100 处理 (配置值被忽略)",
             requested_ms);
    return 100;
}

int effective_viscosity_hold_ms(int requested_ms) {
    // viscosity 的实测间隔几乎全 <0.5 s, 所以它能承受更严的上限; 用同一个
    // 通用折算函数, 只是把上限传成 10000。
    return effective_source_hold_ms(requested_ms, "viscosity_hold_ms", 10000);
}

int effective_source_hold_ms(int requested_ms, const char* key_name, int hi_ms) {
    // 0 是唯一表示"关闭"的值 —— 这是用户可选的取舍(有人就想要停住的阻尼),
    // 所以不把它当成"配错"抬到下限。
    if (requested_ms == 0) return 0;
    if (requested_ms < 500) {
        log_warn("[safety] %s(%d) < 500, 会把正常行驶中的短暂间隙误判为停更, 已抬到 500",
                 key_name, requested_ms);
        return 500;
    }
    if (requested_ms > hi_ms) {
        log_warn("[safety] %s(%d) > %d, 残值会先被感知到才归零, 已降到 %d",
                 key_name, requested_ms, hi_ms, hi_ms);
        return hi_ms;
    }
    return requested_ms;
}

int effective_silent_mute_ms(int requested_ms) {
    if (requested_ms == 0) return 0;   // 关闭
    if (requested_ms < 200) {
        log_warn("[safety] silent_mute_ms(%d) < 200, 会被帧节拍抖动误触发, 已抬到 200",
                 requested_ms);
        return 200;
    }
    if (requested_ms > 10000) {
        log_warn("[safety] silent_mute_ms(%d) > 10000, 超过它就等于退化成单源超时, 已降到 10000",
                 requested_ms);
        return 10000;
    }
    return requested_ms;
}

int effective_watchdog_ms(int requested_ms, int frame_ms) {
    const int fm = (frame_ms < 20) ? 20 : frame_ms;
    const int wd_floor = fm * 3;
    // 上限 2000, 但若 3*frame_ms 已经超过 2000(frame_ms > 666), 约束自相矛盾 ——
    // 让下限优先, 因为"低于帧周期三倍"的看门狗会一直误触发。
    const int wd_cap = (wd_floor > 2000) ? wd_floor : 2000;

    int v = requested_ms;
    if (v < 50) {
        log_warn("[safety] watchdog_ms(%d) < 50, 已抬到 50", v);
        v = 50;
    }
    if (v > wd_cap) {
        log_warn("[safety] watchdog_ms(%d) 超过上限 %d ms, 已降到 %d —— 看门狗再长就失去兜底意义",
                 v, wd_cap, wd_cap);
        v = wd_cap;
    }
    if (v < wd_floor) {
        log_warn("[safety] watchdog_ms(%d) < 3*frame_ms(%d), 会误触发, 已抬到 %d", v, wd_floor,
                 wd_floor);
        v = wd_floor;
    }
    return v;
}

Config load_config(const std::string& ini_path) {
    Config cfg{};

    // 默认 channel_a = 扭矩, channel_b = 振动。
    // 这两个赋值必须在读取之前, 因为 read_channel 用它们当默认值。
    cfg.channel_a.source = "torque";
    cfg.channel_b.source = "vibration";

    if (ini_path.empty()) {
        log_warn("未找到配置文件, 全部使用内置默认值");
        return cfg;
    }

    const DWORD attr = ::GetFileAttributesA(ini_path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        log_warn("配置文件不存在: %s —— 全部使用内置默认值", ini_path.c_str());
        return cfg;
    }

    // -----------------------------------------------------------------------
    // ★ 行尾归一化: 纯 LF 的 INI 会被 Win32 的 GetPrivateProfileString 整个读空 ★
    //
    // 这不是假设, 是踩过的坑: 部署目录里的 INI 被写成了 LF 行尾, 于是**每一个**
    // 配置项都静默回落到内置默认值。表现是"代码全绿、日志无错、设备却毫无反应",
    // 而默认值里 base_strength=60 会让强度比配置值高好几倍 —— 既是功能 bug
    // 也是安全问题。GetPrivateProfileStringA 对 LF-only 文件返回的与"文件不
    // 存在"完全一致, 所以调用方根本分不出这两种情况。
    //
    // 这里直接就地改写成 CRLF。用户手写的 LF 文件在第一次加载时被自动修正,
    // 之后一切正常。只在确实存在裸 LF 时才重写, 避免无谓改动 mtime。
    // -----------------------------------------------------------------------
    normalize_ini_line_endings(ini_path);

    // ---- [general] ----
    cfg.enabled  = read_bool(ini_path, "general", "enable", cfg.enabled);
    cfg.verbose  = read_bool(ini_path, "general", "verbose", cfg.verbose);
    cfg.log_file = read_str(ini_path, "general", "log_file", cfg.log_file);

    // ---- [link] ----
    // 缺 mode 键时给一条明确提示: 以前的默认值是 WebSocket(未实现), 缺键
    // 等于"什么都不连却看起来正常"; 现在是 Disabled, 也是不做任何事, 但至少
    // 用户能在日志里看到原因。
    if (!has_key(ini_path, "link", "mode")) {
        log_warn("未配置 [link] mode, 按 %s 处理(不会连接任何设备)", to_string(cfg.link));
    }
    {
        const std::string s = read_str(ini_path, "link", "mode", to_string(cfg.link));
        if (ieq(s, "ble") || ieq(s, "bluetooth")) {
            cfg.link = LinkMode::Ble;
        } else if (ieq(s, "websocket") || ieq(s, "ws")) {
            // 本地协议仓库里没有 WS 规范(见 docs/research-dglab-transport.md §1),
            // 这条路径是为将来接手机 APP 留的。实现之前会选择 Disabled。
            cfg.link = LinkMode::WebSocket;
        } else if (ieq(s, "disabled") || ieq(s, "none")) {
            cfg.link = LinkMode::Disabled;
        } else {
            log_warn("[link] mode=\"%s\" 无法识别, 用默认值 %s", s.c_str(), to_string(cfg.link));
        }
    }
    {
        const std::string s = read_str(ini_path, "link", "protocol", to_string(cfg.protocol));
        if (ieq(s, "v2")) {
            cfg.protocol = ProtocolVersion::V2;
        } else if (ieq(s, "v3")) {
            cfg.protocol = ProtocolVersion::V3;
        } else {
            log_warn("[link] protocol=\"%s\" 无法识别, 用默认值 %s", s.c_str(), to_string(cfg.protocol));
        }
    }

    // ---- [ble] ----
    cfg.ble_device_name        = read_str(ini_path, "ble", "device_name", cfg.ble_device_name);
    cfg.ble_service_uuid       = read_str(ini_path, "ble", "service_uuid", cfg.ble_service_uuid);
    cfg.ble_write_uuid         = read_str(ini_path, "ble", "write_uuid", cfg.ble_write_uuid);
    cfg.ble_notify_uuid        = read_str(ini_path, "ble", "notify_uuid", cfg.ble_notify_uuid);
    cfg.ble_reconnect_ms       = read_int(ini_path, "ble", "reconnect_ms", cfg.ble_reconnect_ms);
    cfg.ble_connect_timeout_ms = read_int(ini_path, "ble", "connect_timeout_ms",
                                          cfg.ble_connect_timeout_ms);

    // ---- [websocket] (旧设计遗留, 解析保留但无人消费) ----
    // 将来实现 Socket 输出时这些键会被重新定义, 不要基于 bind/port 的
    // "PC 做服务端" 语义去写新代码。见 docs/research-dglab-transport.md 第 1.2 节。
    cfg.ws_bind_address = read_str(ini_path, "websocket", "bind", cfg.ws_bind_address);
    cfg.ws_port         = static_cast<uint16_t>(read_int(ini_path, "websocket", "port", cfg.ws_port));
    cfg.ws_path         = read_str(ini_path, "websocket", "path", cfg.ws_path);
    cfg.ws_token        = read_str(ini_path, "websocket", "token", cfg.ws_token);
    cfg.ws_heartbeat_ms = read_int(ini_path, "websocket", "heartbeat_ms", cfg.ws_heartbeat_ms);

    // ---- [output] ----
    cfg.frame_ms         = read_int(ini_path, "output", "frame_ms", cfg.frame_ms);
    cfg.pulse_freq_base  = read_int(ini_path, "output", "pulse_freq_base", cfg.pulse_freq_base);
    cfg.pulse_freq_min   = read_int(ini_path, "output", "pulse_freq_min", cfg.pulse_freq_min);
    cfg.pulse_freq_max   = read_int(ini_path, "output", "pulse_freq_max", cfg.pulse_freq_max);
    cfg.pulse_freq_track = read_double(ini_path, "output", "pulse_freq_track", cfg.pulse_freq_track);

    // ---- [output] 自动增益 ----
    cfg.auto_gain           = read_bool(ini_path, "output", "auto_gain", cfg.auto_gain);
    cfg.gain_attack_sec     = read_double(ini_path, "output", "gain_attack_sec", cfg.gain_attack_sec);
    cfg.gain_release_sec    = read_double(ini_path, "output", "gain_release_sec", cfg.gain_release_sec);
    cfg.gain_floor          = read_int(ini_path, "output", "gain_floor", cfg.gain_floor);
    cfg.gain_ceiling        = read_int(ini_path, "output", "gain_ceiling", cfg.gain_ceiling);
    cfg.gain_idle_reset_sec = read_double(ini_path, "output", "gain_idle_reset_sec",
                                          cfg.gain_idle_reset_sec);

    read_channel(ini_path, "channel_a", cfg.channel_a);
    read_channel(ini_path, "channel_b", cfg.channel_b);

    // ★ 已知风险源告警 ★
    // viscosity 的回调是"值变了才回调", 而强制退出比赛回到菜单时游戏会把阻尼
    // 停在一个可感的值上却不再回调 —— 本程序没有"源数据陈旧"判定, 那个值会被
    // 一直输出。默认配置不用它, 但用户有权选, 所以只在"启用 + 真会输出"时提醒
    // 一次(不阻止)。判定逻辑本身在 is_held_risk_source(), 有离线自检。
    if (cfg.channel_a.enable && cfg.channel_a.base_strength > 0 &&
        is_held_risk_source(cfg.channel_a.source)) {
        log_warn("[channel_a] source=%s: 强制退出比赛回到菜单后阻尼可能停在可感的值上并持续输出, "
                 "不推荐作为输出源 (见 README 已知风险)", cfg.channel_a.source.c_str());
    }
    if (cfg.channel_b.enable && cfg.channel_b.base_strength > 0 &&
        is_held_risk_source(cfg.channel_b.source)) {
        log_warn("[channel_b] source=%s: 强制退出比赛回到菜单后阻尼可能停在可感的值上并持续输出, "
                 "不推荐作为输出源 (见 README 已知风险)", cfg.channel_b.source.c_str());
    }

    // ---- [safety] ----
    cfg.max_strength        = read_int(ini_path, "safety", "max_strength", cfg.max_strength);
    cfg.clamp_strength      = read_int(ini_path, "safety", "clamp_strength", cfg.clamp_strength);
    cfg.slew_per_second     = read_double(ini_path, "safety", "slew_per_second", cfg.slew_per_second);
    cfg.watchdog_ms         = read_int(ini_path, "safety", "watchdog_ms", cfg.watchdog_ms);
    cfg.zero_when_servo_off = read_bool(ini_path, "safety", "zero_when_servo_off",
                                        cfg.zero_when_servo_off);
    cfg.warmup_ms           = read_int(ini_path, "safety", "warmup_ms", cfg.warmup_ms);
    cfg.viscosity_hold_ms   = read_int(ini_path, "safety", "viscosity_hold_ms",
                                        cfg.viscosity_hold_ms);
    cfg.torque_hold_ms      = read_int(ini_path, "safety", "torque_hold_ms",
                                        cfg.torque_hold_ms);
    cfg.vibration_hold_ms   = read_int(ini_path, "safety", "vibration_hold_ms",
                                        cfg.vibration_hold_ms);
    cfg.silent_mute_ms      = read_int(ini_path, "safety", "silent_mute_ms",
                                        cfg.silent_mute_ms);
    const bool has_dry_run_key = has_key(ini_path, "safety", "dry_run");
    cfg.dry_run             = read_bool(ini_path, "safety", "dry_run", cfg.dry_run);
    // dry_run 缺键必须说清楚: 它是"要不要真的通电"的总开关, 静默取默认值
    // 是最不该发生的。旧 INI 没有这个键 -> 现在是干跑(安全方向)。
    if (!has_dry_run_key) {
        log_warn("未配置 [safety] dry_run, 按 %d(干跑) 处理 —— 要真实输出请显式写 dry_run=0",
                 cfg.dry_run ? 1 : 0);
    }

    // ---- [ranges] ----
    cfg.torque_negative_is_positive =
        read_bool(ini_path, "ranges", "torque_negative_is_positive",
                  cfg.torque_negative_is_positive);

    // ---- [debug] ----
    cfg.hook = read_bool(ini_path, "debug", "hook", cfg.hook);

    // ---- 一致性钳制: 安全相关的值先自纠, 免得把非法值送进协议编码 ----
    auto clamp_int = [](int& v, int lo, int hi, const char* what) {
        if (v < lo || v > hi) {
            log_warn("%s = %d 超出 [%d,%d], 已钳制", what, v, lo, hi);
            v = (v < lo) ? lo : hi;
        }
    };
    clamp_int(cfg.max_strength, 0, 200, "[safety] max_strength");
    clamp_int(cfg.clamp_strength, 0, 200, "[safety] clamp_strength");
    clamp_int(cfg.channel_a.base_strength, 0, 200, "[channel_a] base_strength");
    clamp_int(cfg.channel_b.base_strength, 0, 200, "[channel_b] base_strength");
    clamp_int(cfg.frame_ms, 20, 1000, "[output] frame_ms");
    // T19: BLE 链路的节拍固定 100 ms, 在配置层就固定下来(不改 encode_b0),
    // 必须在 effective_watchdog_ms() 之前 —— 看门狗的 3*frame_ms 用的是实际节拍。
    cfg.frame_ms = effective_frame_ms(cfg.frame_ms, cfg.link);
    clamp_int(cfg.pulse_freq_min, 10, 240, "[output] pulse_freq_min");
    clamp_int(cfg.pulse_freq_max, 10, 240, "[output] pulse_freq_max");
    clamp_int(cfg.warmup_ms, 0, 60000, "[safety] warmup_ms");
    // watchdog_ms 的取舍逻辑在 effective_watchdog_ms() 里 —— 它有上下限两条
    // 与 frame_ms 绑定的约束, 且必须能被离线用例覆盖(T13a), 所以不再用
    // clamp_int + 就地抬 floor 的写法。
    cfg.watchdog_ms = effective_watchdog_ms(cfg.watchdog_ms, cfg.frame_ms);
    // T17: 0 = 关闭, 其他值钳到 500..10000(理由见 config.hpp 里那个字段)。
    cfg.viscosity_hold_ms = effective_viscosity_hold_ms(cfg.viscosity_hold_ms);
    // T22b: torque/vibration 的上限放宽到 20000 —— 实测正常驾驶中 torque 的
    // 真实保持能到 7 s, 上限 10 s 会逼着用户把保护关掉。
    cfg.torque_hold_ms    = effective_source_hold_ms(cfg.torque_hold_ms,
                                                     "torque_hold_ms", 20000);
    cfg.vibration_hold_ms = effective_source_hold_ms(cfg.vibration_hold_ms,
                                                     "vibration_hold_ms", 20000);
    cfg.silent_mute_ms    = effective_silent_mute_ms(cfg.silent_mute_ms);
    // 自动增益: floor 不能超过 ceiling, 否则归一化区间是负的, 会整体归零。
    clamp_int(cfg.gain_floor, 1, 200, "[output] gain_floor");
    clamp_int(cfg.gain_ceiling, 1, 200, "[output] gain_ceiling");
    if (cfg.gain_ceiling < cfg.gain_floor) {
        log_warn("[output] gain_ceiling(%d) < gain_floor(%d), 已把 ceiling 抬到 floor",
                 cfg.gain_ceiling, cfg.gain_floor);
        cfg.gain_ceiling = cfg.gain_floor;
    }
    // 静态标定比 ceiling 还大时, 以静态标定为准 —— 那说明用户知道满量程更大。
    // 自动增益只会在这个上限之内浮动, 绝不会凭空放大到超过物理满量程。
    if (cfg.channel_a.in_max > cfg.gain_ceiling) {
        log_info("[output] channel_a.in_max(%d) > gain_ceiling(%d), 自动增益上限抬到 %d",
                 cfg.channel_a.in_max, cfg.gain_ceiling, cfg.channel_a.in_max);
        cfg.gain_ceiling = cfg.channel_a.in_max;
    }
    if (cfg.channel_b.in_max > cfg.gain_ceiling) {
        log_info("[output] channel_b.in_max(%d) > gain_ceiling(%d), 自动增益上限抬到 %d",
                 cfg.channel_b.in_max, cfg.gain_ceiling, cfg.channel_b.in_max);
        cfg.gain_ceiling = cfg.channel_b.in_max;
    }
    if (cfg.gain_attack_sec < 0.0) cfg.gain_attack_sec = 0.0;
    if (cfg.gain_release_sec < 0.0) cfg.gain_release_sec = 0.0;
    if (cfg.gain_idle_reset_sec < 0.0) cfg.gain_idle_reset_sec = 0.0;
    if (cfg.pulse_freq_min > cfg.pulse_freq_max) {
        log_warn("[output] pulse_freq_min(%d) > pulse_freq_max(%d), 已交换",
                 cfg.pulse_freq_min, cfg.pulse_freq_max);
        const int t = cfg.pulse_freq_min;
        cfg.pulse_freq_min = cfg.pulse_freq_max;
        cfg.pulse_freq_max = t;
    }

    // ★ 最重要的一条安全约束 ★
    // 软件钳制绝不能高于设备软限位 —— 否则软件以为自己在保护, 实际没有。
    if (cfg.clamp_strength > cfg.max_strength) {
        log_warn("[safety] clamp_strength(%d) > max_strength(%d): 软件钳制比设备软限位还宽松, "
                 "已把 clamp_strength 降到 max_strength",
                 cfg.clamp_strength, cfg.max_strength);
        cfg.clamp_strength = cfg.max_strength;
    }

    // -----------------------------------------------------------------------
    // ★ 无条件回显"实际生效"的关键值 ★
    //
    // 上面那个 LF 事故里, 配置整份被忽略、全部用内置默认值, 日志里**一条错误
    // 都没有** —— 唯一能看出问题的方法就是"把生效值和 INI 里写的对一遍"。
    // 所以这一行不是调试信息, 是判据: 每次加载都把生效值原样打出来, 任何人
    // 拿它跟 INI 比一下就知道配置有没有被吃进去。
    // -----------------------------------------------------------------------
    log_info("配置生效值: %s | link=%s protocol=%s dry_run=%d hook=%d | "
             "A(str=%d src=%s) B(str=%d src=%s) | clamp=%d max=%d frame_ms=%d",
             ini_path.c_str(), to_string(cfg.link), to_string(cfg.protocol), cfg.dry_run ? 1 : 0,
             cfg.hook ? 1 : 0, cfg.channel_a.base_strength, cfg.channel_a.strength_source.c_str(),
             cfg.channel_b.base_strength, cfg.channel_b.strength_source.c_str(), cfg.clamp_strength,
             cfg.max_strength, cfg.frame_ms);
    if (!cfg.enabled) {
        log_warn("[general] enable=0 —— mod 已按配置停用, 后面什么都不会做");
    }
    return cfg;
}

std::string resolve_config_path() {
    // 优先用 DLL 同目录下的 DGLab_IDAC.ini —— UE4SS 的 mod 一般在
    // <mod>/dlls/main.dll, 所以先看 dlls\ 再看上一级, 覆盖两种摆法。
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&resolve_config_path), &self)) {
        return {};
    }
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};

    std::wstring dir(buf, n);
    const auto slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir.resize(slash);

    std::wstring parent = dir;
    const auto slash2 = parent.find_last_of(L"\\/");
    if (slash2 != std::wstring::npos) parent.resize(slash2);

    // 窄化: GetPrivateProfileStringA 需要 ANSI 路径。游戏装在 ASCII 路径下,
    // 这里显式 static_cast 避免 C4244; 真遇到非 ASCII 路径也只是找不到文件,
    // 上层会退回默认配置, 不会崩。
    auto narrow = [](const std::wstring& w) {
        std::string s;
        s.reserve(w.size());
        for (wchar_t c : w) s.push_back(static_cast<char>(c & 0x7F));
        return s;
    };

    const wchar_t* names[] = {L"\\DGLab_IDAC.ini", L"\\config.ini"};
    for (const wchar_t* nm : names) {
        std::wstring p = dir + nm;
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return narrow(p);
        }
    }
    for (const wchar_t* nm : names) {
        std::wstring p = parent + nm;
        if (::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return narrow(p);
        }
    }
    // 都不存在 —— 返回 dlls\ 下那个路径, 让上层决定要不要铺一份默认配置
    return narrow(dir + L"\\DGLab_IDAC.ini");
}

bool write_default_config(const std::string& ini_path) {
    if (ini_path.empty()) return false;
    if (::GetFileAttributesA(ini_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return true;   // 已存在, 不覆盖用户改过的配置
    }

    // 用 WritePrivateProfileStringA 逐键写 —— 这样注释虽然会丢, 但格式一定
    // 是 Windows 认的。带注释的权威模板在 tools/DGLab_IDAC.ini.template
    // (含通道对应关系与每个数值的含义), 要改配置请从它拷。
    // 这里只保证"能跑起来"。
    auto w = [&](const char* sec, const char* key, const char* val) {
        ::WritePrivateProfileStringA(sec, key, val, ini_path.c_str());
    };

    w("general", "enable", "1");
    w("general", "verbose", "1");
    w("link", "mode", "disabled");
    w("link", "protocol", "v3");
    w("ble", "device_name", "47L121000");
    w("ble", "reconnect_ms", "3000");
    w("ble", "connect_timeout_ms", "15000");
    w("output", "frame_ms", "100");
    w("output", "pulse_freq_base", "30");
    w("output", "pulse_freq_min", "10");
    w("output", "pulse_freq_max", "60");
    // 自动增益: 力反馈原始值的实际范围随车/赛道/工况变化很大, 用动态峰值
    // 当归一化上界, 幅值才能铺满 0..1。关闭时退回各通道自己的 in_max。
    w("output", "auto_gain", "1");
    w("output", "gain_attack_sec", "0.7");
    w("output", "gain_release_sec", "8.0");
    w("output", "gain_floor", "24");
    w("output", "gain_ceiling", "127");
    w("output", "gain_idle_reset_sec", "3.0");
    // 通道对应关系: A = 郊狼主机 A 通道(左侧电极), B = B 通道(右侧电极)。
    // 每个通道的 source 决定它接哪个游戏数据:
    //   torque    方向盘回正扭矩的幅值(= abs(游戏扭矩), 域 0..127), 实测最活跃。
    //             左转与右转都输出力, 但取的是幅值, 本程序不区分左右。
    //   viscosity 转向阻尼, 几乎全程有值
    //             ⚠ 不推荐作为输出源: 强制退出比赛回到菜单后, 游戏可能把阻尼停在
    //             一个可感的值上; 这个回调只在值变化时触发, 停住的值会一直输出,
    //             直到下一次变化。
    //   vibration 振动/路肩, 只在事件发生时给一次
    //   static    恒定输出, 不看游戏数据
    // 另有 torque_signed(按 direction 恢复符号): 逆向证实该参数是齿轮/档位索引
    // (域 0..255), 不是左右方向, 当前不要用 —— 见 re/indrun_hook_mechanics.md 4.1/5。
    // 建议 A 用 torque。
    // 字段范围: enable 0/1; map_mode {abs_linear,linear,abs_sqrt,sqrt};
    // in_min 0~200; in_max 1~200(auto_gain=1 时只当启动初值);
    // level 0.0~1.0; base_strength 0~200(再受 [safety] max_strength 钳制);
    // strength_source {static,level}; level 模式下 strength_margin 为 0.0~1.0。
    w("channel_a", "enable", "1");
    w("channel_a", "source", "torque");
    w("channel_a", "map_mode", "abs_linear");
    w("channel_a", "in_min", "0");
    w("channel_a", "in_max", "127");
    w("channel_a", "level", "1.0");
    w("channel_a", "base_strength", "40");
    // static = 强度恒为 base_strength, 游戏动态全部体现在脉冲振幅上(推荐);
    // level  = 强度跟着映射值走(旧行为, 做 A/B 用)。
    w("channel_a", "strength_source", "static");
    w("channel_b", "enable", "1");
    // B 当前接 vibration, 它是事件型信号: 只在压路肩/碰撞等判定发生时给一次
    // 短促输出, 其余时间安静。不建议改成 viscosity(原因见上面的来源表)。
    // 想分辨"是哪个通道在动", 让两个通道 base_strength 不同即可。
    w("channel_b", "source", "vibration");
    w("channel_b", "map_mode", "abs_linear");
    w("channel_b", "in_min", "0");
    w("channel_b", "in_max", "127");
    w("channel_b", "level", "0.6");
    w("channel_b", "base_strength", "30");
    w("channel_b", "strength_source", "static");
    w("safety", "max_strength", "40");
    w("safety", "clamp_strength", "40");
    w("safety", "slew_per_second", "40");
    // watchdog_ms: 管线超过这么多毫秒没提交新帧, 传输层改发静默帧。
    // 判据是"管线有没有在提交帧", 不是"力反馈回调有没有来"(回调是边沿触发的)。
    w("safety", "watchdog_ms", "500");
    w("safety", "zero_when_servo_off", "1");
    w("safety", "warmup_ms", "1500");
    // viscosity_hold_ms: viscosity 源停更多久就按 0 处理(T17)。0 = 关闭。
    // 副作用: 发车倒计时等原地不动的时段也会被归零 —— 这是它的工作原理, 不是 bug。
    w("safety", "viscosity_hold_ms", "2000");
    // torque_hold_ms / vibration_hold_ms: 单个源停更多久就按 0 处理(T22b)。
    // 默认 10 s 而不是 2 s: 实测正常驾驶中"方向盘稳住"会让 torque 停 3~7 s,
    // 2 s 会在真实过弯里砍掉手感。0 = 关闭。
    w("safety", "torque_hold_ms", "10000");
    w("safety", "vibration_hold_ms", "10000");
    // silent_mute_ms: 三个源"同时"停更超过这个毫秒数, 所有通道的原始值都按 0
    // 处理(T22b 一级)。它抓的是"游戏不再喂任何力反馈"(退出比赛/匹配打断计时赛),
    // 实测这类齐停最短 48 s, 所以 1 s 足够快且不会误伤。0 = 关闭。
    w("safety", "silent_mute_ms", "1000");
    // dry_run=1 是硬开关: 不管 [link] mode 写什么, 一律走空输出, 绝不碰设备。
    // 想真实输出必须显式改成 0 —— 别把"干跑"和"接真机"混在一次改动里。
    w("safety", "dry_run", "1");
    // [debug] hook=0 是排障开关: 关掉 IndRun.dll 挂钩, 只留日志与管线。
    // 崩溃时先试这个 —— 不崩就说明问题在挂钩, 还崩就说明在别处。
    w("debug", "hook", "1");
    log_info("已写出默认配置: %s", ini_path.c_str());
    return true;
}

}  // namespace dglab_idac
