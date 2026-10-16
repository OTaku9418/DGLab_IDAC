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

    // ---- [general] ----
    cfg.enabled  = read_bool(ini_path, "general", "enable", cfg.enabled);
    cfg.verbose  = read_bool(ini_path, "general", "verbose", cfg.verbose);
    cfg.log_file = read_str(ini_path, "general", "log_file", cfg.log_file);

    // ---- [link] ----
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

    // ---- [websocket] (预留) ----
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

    read_channel(ini_path, "channel_a", cfg.channel_a);
    read_channel(ini_path, "channel_b", cfg.channel_b);

    // ---- [safety] ----
    cfg.max_strength        = read_int(ini_path, "safety", "max_strength", cfg.max_strength);
    cfg.clamp_strength      = read_int(ini_path, "safety", "clamp_strength", cfg.clamp_strength);
    cfg.slew_per_second     = read_double(ini_path, "safety", "slew_per_second", cfg.slew_per_second);
    cfg.watchdog_ms         = read_int(ini_path, "safety", "watchdog_ms", cfg.watchdog_ms);
    cfg.zero_when_servo_off = read_bool(ini_path, "safety", "zero_when_servo_off",
                                        cfg.zero_when_servo_off);
    cfg.warmup_ms           = read_int(ini_path, "safety", "warmup_ms", cfg.warmup_ms);
    cfg.dry_run             = read_bool(ini_path, "safety", "dry_run", cfg.dry_run);

    // ---- [ranges] ----
    cfg.torque_negative_is_positive =
        read_bool(ini_path, "ranges", "torque_negative_is_positive",
                  cfg.torque_negative_is_positive);

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
    clamp_int(cfg.pulse_freq_min, 10, 240, "[output] pulse_freq_min");
    clamp_int(cfg.pulse_freq_max, 10, 240, "[output] pulse_freq_max");
    clamp_int(cfg.warmup_ms, 0, 60000, "[safety] warmup_ms");
    clamp_int(cfg.watchdog_ms, 50, 60000, "[safety] watchdog_ms");
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
    // 是 Windows 认的。真正带注释的模板由 tools/ 部署脚本铺。
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
    w("channel_a", "enable", "1");
    w("channel_a", "source", "torque");
    w("channel_a", "map_mode", "abs_linear");
    w("channel_a", "in_min", "0");
    w("channel_a", "in_max", "127");
    w("channel_a", "level", "1.0");
    w("channel_a", "base_strength", "40");
    w("channel_b", "enable", "1");
    w("channel_b", "source", "vibration");
    w("channel_b", "map_mode", "abs_linear");
    w("channel_b", "in_min", "0");
    w("channel_b", "in_max", "127");
    w("channel_b", "level", "0.6");
    w("channel_b", "base_strength", "30");
    w("safety", "max_strength", "40");
    w("safety", "clamp_strength", "40");
    w("safety", "slew_per_second", "40");
    w("safety", "watchdog_ms", "500");
    w("safety", "zero_when_servo_off", "1");
    w("safety", "warmup_ms", "1500");
    w("safety", "dry_run", "1");

    log_info("已写出默认配置: %s", ini_path.c_str());
    return true;
}

}  // namespace dglab_idac
