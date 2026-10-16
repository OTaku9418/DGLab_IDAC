// ---------------------------------------------------------------------------
// DGLab_IDAC — 力反馈语义层与配置契约
//
// 这个头文件是"游戏侧力反馈"与"郊狼输出"之间的唯一契约。
// IndRun.dll 的 hook 负责产出 FfbState, 管线消费它。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

namespace dglab_idac {

// ---------------------------------------------------------------------------
// nsFeedbackDefine::eTrqueDirection 的可观测取值
//
// 注意: 枚举的**成员名**无法静态恢复 —— IndRun.dll 里既没有该枚举的字符串
// 也没有 RTTI, 只有 mangled 类型名 `W4eTrqueDirection@nsFeedbackDefine@@`。
// 所以下面的名字是我们自己起的, 只有**数值**是权威的。
//
// 已证实 (IndRun.dll RVA 0x2BCD1, 扭矩 > 0 时):
//     mov ecx, 0xffffffff      ; direction = -1
// 已证实 (RVA 0x6D19, FFB 拆除路径 `torque(-1, 0)`):
//     同一常量 -1 用于把扭矩归零
//
// 强推断 (未证明): +1, 来自 `mov ecx, r13d` (RVA 0x2BCD8), 而 r13d 在
// 整个 100 KB 巨函数里只被写过 3 次, 全是 `mov r13d,1`。因为对巨函数 CFG 的
// 前向数据流分析因 3 个无法解析的间接跳转表而不完整, 所以这一点**没有证明**。
//
// 因此 hook 代码必须把 direction 当作一般的有符号 int 处理, 不能假设只有 ±1。
// ---------------------------------------------------------------------------
enum class TorqueDirection : int32_t {
    Zero   = -1,   // 已证实: 常量 0xFFFFFFFF。出现于 torque>0 以及 FFB 拆除
    Plus   =  1,   // 强推断(未证明)
    Other  =  0,   // 其它任何值都不该被假设成上面两者
};

inline const char* to_string(TorqueDirection d) {
    switch (d) {
        case TorqueDirection::Zero:  return "-1";
        case TorqueDirection::Plus:  return "+1";
        case TorqueDirection::Other: return "0/other";
        default:                     return "other";
    }
}

// ---------------------------------------------------------------------------
// 一帧力反馈状态。
//
// 量纲(来自 IndRun.dll 实测):
//   torque_magnitude  = abs(signed_torque), 且 signed_torque 被 clamp 到 ±127
//                       (RVA 0x2BC46 mov r9d,-127 / 0x2BC4C mov r8d,0x7f)
//                       -> 实际范围 0..127
//   torque_dir        = 上面那个 signed int 的**符号**, 单独用一个 enum 传
//   viscosity / vibration = 2 个 int 参数, 其中第 1 个参数复用同一个 ±1 符号
//                       (RVA 0x2BFC1 `mov ecx,r13d`)
//
// 两个来源都是**边沿触发 + 变化缓存**: 只有当值变化时才会调一次回调。
//   torque    变化缓存 [manager+0x163C]
//   vibration 变化缓存 [manager+0x1640], 且被 master FFB 使能字节
//   viscosity 变化缓存 [manager+0x1644]   [manager+0x5812] 把关
//
// 官方"关闭"编码 (RVA 0x6C7E-0x6D48): viscosity(0,0) -> vibration(0,0)
//   -> torque(-1, 0)
//
// ⚠ 未验证的关键点: 每 tick 的 FFB 增益 [manager+0x4E4] 由 wrapper 0xEF15-
//   0xEFDF 从 .data 里 5 个**零初始化(BSS)**浮点 0x24113C/0x241140/0x241144/
//   0x241148/0x24114C 之一算出。这些地址在文件里不存在(.data VS=0x7000 但
//   raw 只有 0x2800), 所以初值为 0.0f。如果有任何东西没去写它们, 那么
//   增益恒为 0 -> 每个 magnitude 都是 0, 力反馈全灭。
//   => 必须在运行时实测 magnitude 是否非零。这就是 verbose 日志要记录原始值的原因。
// ---------------------------------------------------------------------------
struct FfbState {
    // --- servo ---
    bool     servo_on           = false;
    double   servo_changed_at   = 0.0;   // servo 状态最后一次变化的时刻(steady_clock 秒)

    // --- 扭矩 (槽 g+0x108) ---
    int32_t  torque_dir         = 0;     // 原始有符号 direction, 不要假设只有 ±1
    int32_t  torque_magnitude   = 0;     // 0..127, abs(torque)
    double   torque_at          = 0.0;   // 该值被更新的时刻

    // --- 粘度/阻尼 (槽 g+0x148) ---
    int32_t  viscosity_a        = 0;     // 第 1 参数(word 1) —— 与扭矩同源的 ±1 符号
    int32_t  viscosity_b        = 0;     // 第 2 参数
    double   viscosity_at       = 0.0;

    // --- 振动 (槽 g+0x188) ---
    int32_t  vibration_a        = 0;
    int32_t  vibration_b        = 0;
    double   vibration_at       = 0.0;

    // 诊断计数: 每个回调被实际调用的次数(边沿触发, 所以数值不大)
    uint64_t torque_calls       = 0;
    uint64_t viscosity_calls    = 0;
    uint64_t vibration_calls    = 0;
    uint64_t servo_on_calls     = 0;
    uint64_t servo_off_calls    = 0;

    // 观测到的原始值极值, 用来在不知道量纲时自标定
    int32_t  torque_mag_min     = INT32_MAX;
    int32_t  torque_mag_max     = INT32_MIN;
    int32_t  torque_dir_min     = INT32_MAX;
    int32_t  torque_dir_max     = INT32_MIN;
    int32_t  vibration_b_min    = INT32_MAX;
    int32_t  vibration_b_max    = INT32_MIN;
    int32_t  viscosity_b_min    = INT32_MAX;
    int32_t  viscosity_b_max    = INT32_MIN;

    bool     ever_updated       = false;

    double latest_at() const {
        double t = 0.0;
        if (torque_at    > t) t = torque_at;
        if (viscosity_at > t) t = viscosity_at;
        if (vibration_at > t) t = vibration_at;
        return t;
    }
};

// ---------------------------------------------------------------------------
// 力反馈数据源接口。
// 实现: IndRunHookSource (真机) / MockSource (离线测试)
// ---------------------------------------------------------------------------
class IFfbSource {
public:
    virtual ~IFfbSource() = default;

    // 返回 false 表示挂载失败(比如 IndRun.dll 没加载 / 特征码对不上)
    virtual bool install() = 0;
    virtual void uninstall() = 0;

    // 线程安全: 可能从游戏线程调用, 也可能从 hook 内部调用
    virtual FfbState snapshot() const = 0;

    virtual const char* name() const = 0;
};

}  // namespace dglab_idac
