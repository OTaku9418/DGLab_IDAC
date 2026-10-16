// ---------------------------------------------------------------------------
// DGLab_IDAC — 日志
//
// 同时写三个地方, 因为三处的用途不同:
//   * 独立文件 DGLab_IDAC.log —— 探针阶段主要读这个, 格式稳定好解析
//   * OutputDebugStringA      —— UE4SS 会捕获, 落到 UE4SS.log
//   * stdout                  —— 控制台模式(dwmapi 代理 / 调试器)下可见
//
// 探针阶段每秒最多几十行, 直接在游戏线程同步写就够; 不做异步队列,
// 免得引入自己的竞态。
// ---------------------------------------------------------------------------
#pragma once

#include <string>

namespace dglab_idac {

// 初始化。path 为空时日志文件落在 DLL 自身所在目录。
// 返回实际使用的日志文件路径(可能为空 = 文件写入被禁用)。
std::string log_init(const std::string& path = {});

// 关闭并 flush
void log_shutdown();

// printf 风格。内部截断到 2048 字节。
void log_line(const char* fmt, ...);

// 级别前缀版本, 便于过滤
void log_info(const char* fmt, ...);
void log_warn(const char* fmt, ...);
void log_error(const char* fmt, ...);

// 是否把 log_line 也复制到 UE4SS 日志(OutputDebugString)。默认开。
void log_set_debug_output(bool on);

// 当前日志文件路径(未初始化时为空)
const char* log_path();

}  // namespace dglab_idac
