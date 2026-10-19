// ---------------------------------------------------------------------------
// DGLab_IDAC — [ffbgap] 诊断统计 (T9)
//
// 为什么要单独一个头文件: 这段代码只被 dllmain 的主循环喂数据, 但它得出的
// 结论会被用来决定"要不要给力反馈加一个保持超时"。而它唯一的验证机会是
// 用户开车的那十几分钟 —— 分档算错了, 数据就白收了, 还会把方向带偏。
// 所以把它放在头文件里, 让 tools/logic_check.cpp 能用假时间戳离线测一遍。
//
// 它只统计, 不改变任何输出行为。
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace dglab_idac {

// ---------------------------------------------------------------------------
// 要回答的问题: 存不存在"游戏卡死、FfbState 冻结在非零值、而管线线程还在跑"
// 这种 T1 的看门狗抓不到的场景?
//
// 那类场景的特征是"扭矩一直是正的, 但再也没来新的回调"。所以在**扭矩为正
// 期间**统计相邻两次回调的间隔分布就能看出来。
//
// ★ 已用实测数据回答过一轮 (2026-10-18, 见 README 5.3 节) ★
// 结论是"整进程一起冻结": 卡顿时挂钩、主循环、BLE 发送线程同时停, 所以既没有
// 新的回调, 也没有新的帧 —— 这已经落在 T1 看门狗的覆盖范围里, 设备在冻结期间
// 保持最后一帧强度直到进程被恢复。真正抓不到的是"进程还活着、但游戏停止投喂
// FFB"这类场景, 那需要的是"源数据陈旧"判定(读 latest 的时间戳), 不是本统计。
// 本统计保留, 因为它是唯一能拿到这种间隔分布的离线可验证工具; 但不要把它
// 当成"卡顿能被软件归零"的证据。
//
// 为什么只在正扭矩区间里统计: 方向盘回正、力为 0 时"很久没有回调"是正常
// 现象(回调是边沿触发的), 混进来会把结论完全污染。
// 采样精度就是主循环的 frame_ms, 分档粒度远大于它。
// ---------------------------------------------------------------------------
struct FfbGapStats {
    // 分档与计划一致: <0.5 s / 0.5-2 s / 2-5 s / >5 s
    uint64_t n_lt_05 = 0;
    uint64_t n_05_2  = 0;
    uint64_t n_2_5   = 0;
    uint64_t n_gt_5  = 0;
    double   max_gap = 0.0;   // 正扭矩期间最长的一次间隔(秒)
    uint64_t stops   = 0;     // 正扭矩区间因"力降到 0"而结束的次数

    uint64_t total() const { return n_lt_05 + n_05_2 + n_2_5 + n_gt_5; }

    // 每收到一次**新的**扭矩回调就调一次(用 torque_calls 的增量判断)。
    // 不要每个 tick 都调 —— 那样采样频率就成了主循环频率, 统计失去意义。
    void on_torque(double t, int32_t magnitude) {
        if (magnitude > 0) {
            if (armed_) {
                const double gap = t - last_seen_;
                if (gap < 0.5)      ++n_lt_05;
                else if (gap < 2.0) ++n_05_2;
                else if (gap < 5.0) ++n_2_5;
                else                ++n_gt_5;
                if (gap > max_gap) max_gap = gap;
            }
            armed_     = true;
            last_seen_ = t;
        } else if (armed_) {
            // 力已归零: 这一段"正扭矩区间"结束。下次再来正值重新起算,
            // 否则回正期间的空白会被记成一个巨大的间隔。
            armed_ = false;
            ++stops;
        }
    }

    // 是否正处在"正扭矩区间"内(离线测试要用; 运行期只用于判断)
    bool armed() const { return armed_; }

    // 当前这个"还没结束的间隔"已经有多长(秒)。不在正扭矩区间内时是 0。
    //
    // 为什么要这个东西 (T18): 统计按 60 s 窗口打印, 而"菜单里停更"那段间隔很
    // 可能整个跨过窗口边界 —— 只清计数的话, 这段间隔要等到**下一个回调**才会
    // 落进某一档, 而那可能是一分钟以后的事。有了 open, 每一行都能看到"到打印
    // 这一刻为止, 已经有多久没来新回调了", 停更期间的下一条日志就能直接指认。
    double open_seconds(double now) const {
        if (!armed_) return 0.0;
        const double d = now - last_seen_;
        return (d > 0.0) ? d : 0.0;
    }

    // 清计数但**保留**上一次回调的时刻与区间状态 (T18)。
    //
    // 旧版这里是整体重来, 结果是: 一条跨过 60 s 边界的间隔会被整段丢掉 ——
    // 第二轮那局里 viscosity 在菜单里停了 52.5 s (298.0 -> 350.5), 而报告里
    // 只看到 9.50 s, 因为 300 s 那次打印把起点抹平了。跨窗口的间隔恰恰是最
    // 想抓的那一类, 丢掉它等于统计对自己最关心的现象视而不见。
    void reset_counts() {
        n_lt_05 = n_05_2 = n_2_5 = n_gt_5 = 0;
        max_gap = 0.0;
        stops   = 0;
    }

    // 整体重来(离线测试用; 运行期请用 reset_counts)
    void reset() { *this = FfbGapStats{}; }

private:
    double last_seen_ = 0.0;   // 上一次正值回调的时刻
    bool   armed_     = false; // 是否处在正扭矩区间内
};

// ---------------------------------------------------------------------------
// [ffbhold] 非零值"保持不变"了多久 (T22a)
//
// 与上面 FfbGapStats 的区别 (计划第 0 节第 3 条说的盲区):
//   FfbGapStats.on_torque() 收到 0 时只结束区间并 ++stops, **不记录**最后一个
//   非零值保持了多久。比如 m=5 停 3 s 后变成 0, 这 3 s 不会进任何一档。
//   而定"保持超时"的默认值需要的恰恰是"每个非零值保持不变了多久"。
//
// 为什么只需要"回调之间的间隔": 回调是边沿触发的(值不变不回调), 所以
// "上一个值保持不变"的时长 == "距上次回调"的时长。因此这里不逐 tick 采样,
// 每次**新回调**时把上一次的间隔记下来即可, 精度就是回调本身的时刻。
//
// ★ 只统计, 不改变任何输出行为 ★
// ---------------------------------------------------------------------------
struct HoldStats {
    // 分档: <0.5 / 0.5-1 / 1-2 / 2-3 / 3-5 / >5 秒
    uint64_t n_lt_05 = 0;
    uint64_t n_05_1  = 0;
    uint64_t n_1_2   = 0;
    uint64_t n_2_3   = 0;
    uint64_t n_3_5   = 0;
    uint64_t n_gt_5  = 0;
    double   max_hold = 0.0;   // 最长的一次"保持不变"
    int32_t  max_val  = 0;     // 那一次保持的值
    uint64_t suppressed = 0;   // 因为明细满 10 条而没打印的条数

    static constexpr int kMaxLong = 10;

    struct Long {
        double  end   = 0.0;   // 这次保持结束(即新回调到来)的时刻
        double  held  = 0.0;   // 保持了多久
        int32_t value = 0;     // 保持期间的值
    };
    Long longs[kMaxLong];
    int  long_n = 0;

    uint64_t total() const { return n_lt_05 + n_05_1 + n_1_2 + n_2_3 + n_3_5 + n_gt_5; }

    // 每收到一次**新的**回调就调一次(用 *_calls 的增量判断), t 用 *_at。
    // 注意: 记录的是**上一次**那个值保持了多久, 不管新值是不是 0。
    void on_update(double t, int32_t value) {
        if (has_) {
            const double held = t - prev_t_;
            if (prev_val_ != 0 && held > 0.0) {
                if (held < 0.5)      ++n_lt_05;
                else if (held < 1.0) ++n_05_1;
                else if (held < 2.0) ++n_1_2;
                else if (held < 3.0) ++n_2_3;
                else if (held < 5.0) ++n_3_5;
                else                 ++n_gt_5;

                if (held > max_hold) { max_hold = held; max_val = prev_val_; }

                // 1 s 以上的保持单独留一份明细: 定超时默认值时要看的就是它们
                // 具体发生在什么值上(1 和 19 是两回事)。
                if (held >= 1.0) {
                    if (long_n < kMaxLong) {
                        longs[long_n].end   = t;
                        longs[long_n].held  = held;
                        longs[long_n].value = prev_val_;
                        ++long_n;
                    } else {
                        ++suppressed;
                    }
                }
            }
        }
        has_     = true;
        prev_t_  = t;
        prev_val_ = value;
    }

    // 当前这个非零值已经保持了多久(秒)。当前值是 0 或还没有样本时返回 0。
    double open_seconds(double now) const {
        if (!has_ || prev_val_ == 0) return 0.0;
        const double d = now - prev_t_;
        return (d > 0.0) ? d : 0.0;
    }

    int32_t value() const { return prev_val_; }

    // 清计数与明细, **保留**上一次回调的时刻与值 (与 FfbGapStats 一致)。
    void reset_counts() {
        n_lt_05 = n_05_1 = n_1_2 = n_2_3 = n_3_5 = n_gt_5 = 0;
        max_hold = 0.0;
        max_val  = 0;
        long_n   = 0;
        suppressed = 0;
    }

    // 整体重来(离线测试用)
    void reset() { *this = HoldStats{}; }

private:
    double  prev_t_  = 0.0;
    int32_t prev_val_ = 0;
    bool    has_      = false;
};

}  // namespace dglab_idac
