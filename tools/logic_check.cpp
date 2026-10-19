// ---------------------------------------------------------------------------
// logic_check —— 纯逻辑自检(不进游戏、不连设备、不碰 WinRT)
//
// 为什么要有这个文件:
//   protocol_check 只管"字节编码对不对", 它管不了"什么时候该发静默帧"。
//   而看门狗 ([safety] watchdog_ms) 恰恰是本项目最危险的一类代码:
//   它在**出故障的时候**才生效, 平时跑一万次也看不出问题。等真出事
//   (游戏卡死 2 秒)才发现看门狗根本没接上, 代价是设备在错误强度上一直输出。
//
//   所以看门狗的判定逻辑必须能被离线测试: 时间由调用方传进去, 不依赖
//   真实时钟 —— FrameMailbox 的接口就是这么设计的。
//
// 覆盖范围:
//   1. 信箱基本语义(覆盖、新鲜度、重复发送计数)
//   2. 看门狗触发 / 只触发一次 / 恢复
//   3. 静默帧的逐字段内容(强度 0、脉冲 0、频率合法)
//   4. 静默帧经 encode_b0 之后真的是"绝对设置 0", 不会变成"保持不变"
//   5. 管线自身的停顿检测(停顿后强度归零、重新预热)
//
// 用法: logic_check.exe        全部通过返回 0, 有失败返回 1
// ---------------------------------------------------------------------------

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "config.hpp"
#include "ffb_gap_stats.hpp"
#include "ffb_types.hpp"
#include "pipeline.hpp"
#include "transport/transport.hpp"

using namespace dglab_idac;

// ---------------------------------------------------------------------------
namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_group = "";

void group(const char* name) {
    g_group = name;
    std::printf("\n== %s ==\n", name);
}

void check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        std::printf("  [ok]   %s\n", what);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s   <-- %s\n", what, g_group);
    }
}

// 拼接出来的说明(防漂移测试要把字段差异打出来)直接传字符串。
void check(bool ok, const std::string& what) { check(ok, what.c_str()); }

void check_eq(long long got, long long want, const char* what) {
    if (got == want) {
        ++g_pass;
        std::printf("  [ok]   %s = %lld\n", what, got);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s: 得到 %lld, 期望 %lld   <-- %s\n", what, got, want, g_group);
    }
}

// ---------------------------------------------------------------------------
// 一个"有内容"的帧: 用来验证信箱确实把内容传出去了(而不是永远返回静默帧)。
Frame make_loud_frame() {
    Frame f;
    f.servo_on = true;
    f.muted = false;
    f.mute_reason = "";
    f.a.strength = 15;
    f.b.strength = 10;
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        f.a.slots[i].pulse_intensity = 42;
        f.a.slots[i].pulse_frequency = 30;
        f.b.slots[i].pulse_intensity = 7;
        f.b.slots[i].pulse_frequency = 60;
    }
    return f;
}

bool is_silent(const Frame& f) {
    if (f.a.strength != 0 || f.b.strength != 0) return false;
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        if (f.a.slots[i].pulse_intensity != 0) return false;
        if (f.b.slots[i].pulse_intensity != 0) return false;
        // 频率必须是合法值, 不能是未初始化的 0 —— 0 在设备上属于越界输入
        if (f.a.slots[i].pulse_frequency < 10) return false;
        if (f.b.slots[i].pulse_frequency < 10) return false;
    }
    return true;
}

// 管线测试用的一份确定配置。刻意不用模板值: 模板是给用户看的,
// 这里要的是**结构体默认值**那一档, 两者混起来测试会互相误导。
Config make_test_config() {
    Config c;
    c.frame_ms = 100;
    c.watchdog_ms = 500;
    c.slew_per_second = 40.0;
    c.max_strength = 40;
    c.clamp_strength = 40;
    c.warmup_ms = 0;          // 预热会掩盖其他逻辑, 单独测
    c.auto_gain = false;      // 关掉动态增益, 让映射结果可预测
    c.zero_when_servo_off = true;
    // T22b: 单测一次只验一个机制, 所以这里把"保持超时"两级都关掉; 要测它们
    // 的用例自己打开。默认值本身由 T23 的"默认值与模板不漂移"用例钉住。
    c.torque_hold_ms = 0;
    c.vibration_hold_ms = 0;
    c.silent_mute_ms = 0;

    c.channel_a.enable = true;
    c.channel_a.source = "torque";
    c.channel_a.mode = MapMode::AbsLinear;
    c.channel_a.in_min = 0;
    c.channel_a.in_max = 127;
    c.channel_a.level = 1.0;
    c.channel_a.base_strength = 15;
    c.channel_a.strength_source = "static";

    c.channel_b.enable = true;
    c.channel_b.source = "viscosity";
    c.channel_b.mode = MapMode::AbsLinear;
    c.channel_b.in_min = 0;
    c.channel_b.in_max = 127;
    c.channel_b.level = 1.0;
    c.channel_b.base_strength = 10;
    c.channel_b.strength_source = "static";
    return c;
}

FfbState make_ffb() {
    FfbState s;
    s.servo_on = true;
    s.ever_updated = true;
    s.torque_magnitude = 64;
    s.torque_dir = 1;
    s.viscosity_a = 1;
    s.viscosity_b = 32;
    s.vibration_a = 0;
    s.vibration_b = 0;
    return s;
}

// ---------------------------------------------------------------------------
// 1. 信箱基本语义
// ---------------------------------------------------------------------------
void test_mailbox_basic() {
    group("信箱: 基本语义(覆盖、新鲜度)");

    FrameMailbox mb;

    // 从没 put 过 —— 必须返回静默帧, 而不是未初始化内存。
    MailboxTake t0 = mb.take(1000, 500);
    check(is_silent(t0.frame), "从未提交过 => 取出的是静默帧");
    check(!t0.stale, "从未提交过 => 不算看门狗触发(这是启动, 不是故障)");
    check(!t0.fresh, "从未提交过 => fresh=false");

    // put 一个真帧, 立刻 take
    mb.put(make_loud_frame(), 1000);
    MailboxTake t1 = mb.take(1000, 500);
    check(t1.fresh, "新帧 => fresh=true");
    check(!t1.stale, "没超时 => 不是 stale");
    check_eq(t1.frame.a.strength, 15, "A 强度原样传出");
    check_eq(t1.frame.b.strength, 10, "B 强度原样传出");
    check_eq(t1.frame.a.slots[0].pulse_intensity, 42, "A 脉冲强度原样传出");
    check_eq(t1.frame.b.slots[0].pulse_frequency, 60, "B 脉冲频率原样传出");
    check(!t1.frame.muted, "真帧的 muted 是 false");

    // 紧接着再 take: 没有新帧, 必须重复上一帧(而不是变静默)
    MailboxTake t2 = mb.take(1010, 500);
    check(!t2.fresh, "没有新帧 => fresh=false");
    check(!t2.stale, "仍然没超时 => 不是 stale");
    check_eq(t2.frame.a.strength, 15, "重复发送时强度保持不变(不能突然归零)");
}

// ---------------------------------------------------------------------------
// 2. 覆盖计数
// ---------------------------------------------------------------------------
void test_mailbox_overwrite() {
    group("信箱: 深度 1, 新帧覆盖旧帧");

    FrameMailbox mb;
    // 这个用例测的是"传输线程跟不上导致丢帧", 所以先让消费端就绪
    // —— 未就绪时覆盖记进 over_pre(T13c), 那是另一栏。
    mb.set_consumer_ready(true);
    mb.put(make_loud_frame(), 1000);

    Frame f2 = make_loud_frame();
    f2.a.strength = 20;
    mb.put(f2, 1010);   // 上一帧还没被取走

    MailboxTake t = mb.take(1010, 500);
    check_eq(t.frame.a.strength, 20, "取到的是**最新**那一帧, 不是排队的旧帧");
    const MailboxStats s = mb.stats();
    check_eq(static_cast<long long>(s.submitted), 2, "submitted 计数");
    check_eq(static_cast<long long>(s.overwritten), 1, "overwritten 计数(旧帧被丢弃)");
}

// ---------------------------------------------------------------------------
// 3. 看门狗触发
// ---------------------------------------------------------------------------
void test_watchdog_trip() {
    group("看门狗: 超时 => 静默帧");

    FrameMailbox mb;
    mb.put(make_loud_frame(), 1000);

    // 499 ms 时还不该触发
    MailboxTake t1 = mb.take(1499, 500);
    check(!t1.stale, "间隔 499ms < watchdog 500ms => 不触发");
    check_eq(t1.frame.a.strength, 15, "不触发时强度不变");

    // 501 ms 必须触发
    MailboxTake t2 = mb.take(1501, 500);
    check(t2.stale, "间隔 501ms > watchdog 500ms => 触发");
    check(is_silent(t2.frame), "触发时取到的是静默帧(全 0 强度)");
    check_eq(static_cast<long long>(mb.stats().watchdog_trips), 1, "trips 计数 = 1");

    // 继续停顿: 仍然静默, 但 trips 不能每帧 +1(否则日志会被刷爆)
    MailboxTake t3 = mb.take(2000, 500);
    check(t3.stale, "停顿持续 => 仍然 stale");
    check(is_silent(t3.frame), "停顿持续 => 仍然静默");
    check_eq(static_cast<long long>(mb.stats().watchdog_trips), 1, "持续停顿只记一次 trips");
}

// ---------------------------------------------------------------------------
// 4. 看门狗恢复
// ---------------------------------------------------------------------------
void test_watchdog_recover() {
    group("看门狗: 恢复后重新用真帧");

    FrameMailbox mb;
    mb.put(make_loud_frame(), 1000);
    mb.take(1000, 500);          // 正常取走
    mb.take(2000, 500);          // 停顿 1 秒 => 触发

    // 管线重新开始提交
    mb.put(make_loud_frame(), 2100);
    MailboxTake t = mb.take(2100, 500);
    check(!t.stale, "重新提交后 => 不再 stale");
    check_eq(t.frame.a.strength, 15, "恢复后强度回到真帧的值");
    check(t.fresh, "恢复后第一次取到的是新帧 => fresh=true");

    // 恢复之后再停顿, 应该能再次触发(不是"一辈子只能触发一次")
    mb.take(3000, 500);
    check_eq(static_cast<long long>(mb.stats().watchdog_trips), 2, "再次停顿 => trips 累加到 2");
}

// ---------------------------------------------------------------------------
// 5. 静默帧的字段与编码
// ---------------------------------------------------------------------------
void test_idle_frame_encoding() {
    group("静默帧: 字段内容 + 编码结果");

    const Frame f = make_idle_frame("watchdog");
    check(f.muted, "muted=true");
    check(!f.servo_on, "servo_on=false");
    check(std::strcmp(f.mute_reason, "watchdog") == 0, "mute_reason 记录来源");
    check(is_silent(f), "所有强度为 0、所有频率 >= 10");

    // 关键: 静默帧经过编码后必须是"绝对设置 0"。
    // 如果编成 Unchanged, 设备会**保持旧强度** —— 这就是历史上那个"设备不动"
    // 反向 bug 的同类问题: 看起来发了静默帧, 实际什么都没关掉。
    bool va = false;
    bool vb = false;
    const B0 b0 = encode_b0(f, StrengthMode::Absolute, StrengthMode::Absolute, 1, &va, &vb);
    std::printf("  静默帧编码: %s\n", b0.to_hex().c_str());
    check_eq(b0.bytes[0], 0xB0, "byte[0] = 0xB0");
    check_eq(b0.bytes[2], 0, "byte[2] = A 强度 0");
    check_eq(b0.bytes[3], 0, "byte[3] = B 强度 0");
    for (int i = 0; i < kSlotsPerFrame; ++i) {
        check_eq(b0.bytes[8 + i], 0, "A 槽脉冲强度为 0");
        check_eq(b0.bytes[16 + i], 0, "B 槽脉冲强度为 0");
    }
    check(b0.bytes[4] >= 10 && b0.bytes[4] <= 240, "A 槽频率落在 10..240");
    check(b0.bytes[12] >= 10 && b0.bytes[12] <= 240, "B 槽频率落在 10..240");

    // 停止归零帧(BleWinRtTransport::send_shutdown_idle)走的是 Unchanged 路径。
    // 它必须在编码里被提升成"绝对设置 0": 真按 Unchanged 编出去的话设备会
    // **保持旧强度**, 结果就是"程序已经关了、电极还在输出"。
    bool va2 = false;
    bool vb2 = false;
    const B0 shut = encode_b0(f, StrengthMode::Unchanged, StrengthMode::Unchanged, 0, &va2, &vb2);
    check_eq((shut.bytes[1] >> 2) & 0x03, 3, "停止归零: 强度 0 + Unchanged => A 提升为绝对设置");
    check_eq(shut.bytes[1] & 0x03, 3, "停止归零: 强度 0 + Unchanged => B 提升为绝对设置");
    check_eq(shut.bytes[2], 0, "停止归零: A 强度字节 = 0");
    check_eq(shut.bytes[3], 0, "停止归零: B 强度字节 = 0");
    check_eq(shut.seq(), 0, "停止归零: seq=0 (不等 B1 应答)");
}

// ---------------------------------------------------------------------------
// 6. 管线: 正常 tick 输出真帧(对照组)
// ---------------------------------------------------------------------------
void test_pipeline_normal() {
    group("管线: 正常 tick(不触发停顿)");

    const Config cfg = make_test_config();
    Pipeline p(cfg);
    FfbState ffb = make_ffb();

    Frame out;
    bool any_loud = false;
    // 10 Hz 跑 1 秒。斜率限制 40/s => 从 0 爬到 15 需要约 375 ms。
    for (int i = 0; i < 12; ++i) {
        const double now = 1.0 + static_cast<double>(i) * 0.1;
        p.tick(now, ffb, out);
        if (out.a.strength > 0) any_loud = true;
    }
    check(any_loud, "持续有数据时, 强度能爬上去");
    check_eq(out.a.strength, 15, "最终 A 强度 = base_strength 15");
    check_eq(out.b.strength, 10, "最终 B 强度 = base_strength 10");
    check(!p.safety().watchdog_tripped, "没有停顿 => watchdog_tripped=false");
}

// ---------------------------------------------------------------------------
// 7. 管线: 停顿检测
// ---------------------------------------------------------------------------
void test_pipeline_stall() {
    group("管线: 自己停顿超过 watchdog_ms");

    const Config cfg = make_test_config();
    Pipeline p(cfg);
    FfbState ffb = make_ffb();

    Frame out;
    // 先跑到满强度
    for (int i = 0; i < 12; ++i) {
        p.tick(1.0 + static_cast<double>(i) * 0.1, ffb, out);
    }
    check_eq(out.a.strength, 15, "停顿前 A 强度已达 15");

    // 停顿 0.8 秒(> watchdog_ms=500)。上一 tick 在 2.1s, 所以这里 dt=0.8s。
    p.tick(2.9, ffb, out);
    check(p.safety().watchdog_tripped, "停顿超过 watchdog_ms => watchdog_tripped=true");
    check_eq(out.a.strength, 0, "停顿后的这一帧 A 强度归零");
    check_eq(out.b.strength, 0, "停顿后的这一帧 B 强度归零");

    // 停顿后紧接着正常 tick: 强度必须从 0 慢慢爬, 不能一步跳满
    p.tick(3.0, ffb, out);
    check(!p.safety().watchdog_tripped, "恢复后 watchdog_tripped 复位");
    check(out.a.strength <= 4, "恢复后第一帧最多爬 slew 允许的量(40/s * 0.1s = 4)");

    // 停顿没超阈值时不能误触发
    Pipeline q(cfg);
    Frame out2;
    for (int i = 0; i < 12; ++i) {
        q.tick(1.0 + static_cast<double>(i) * 0.1, ffb, out2);
    }
    q.tick(2.25, ffb, out2);   // 间隔 250 ms < 500 ms
    check(!q.safety().watchdog_tripped, "间隔 250ms < watchdog_ms => 不误触发");
    check(out2.a.strength > 0, "不误触发时强度保持");
}

// ---------------------------------------------------------------------------
// 8. 管线: 预热与静音
// ---------------------------------------------------------------------------
void test_pipeline_warmup() {
    group("管线: 预热期内不输出");

    Config cfg = make_test_config();
    cfg.warmup_ms = 1000;
    Pipeline p(cfg);
    FfbState ffb = make_ffb();

    Frame out;
    p.tick(10.0, ffb, out);
    check(out.muted, "预热期内 muted=true");
    check(std::strcmp(out.mute_reason, "warmup") == 0, "静音原因 = warmup");
    check_eq(out.a.strength, 0, "预热期内强度 0");

    for (int i = 0; i < 12; ++i) {
        p.tick(10.0 + static_cast<double>(i + 1) * 0.1, ffb, out);
    }
    check(!out.muted, "预热结束后不再静音");
    check(out.a.strength > 0, "预热结束后强度能上来");
}

// ---------------------------------------------------------------------------
// 9. [ffbgap] 诊断统计 (T9)
//
// 这段代码唯一的真实验证机会是用户开车的那十几分钟, 分档算错了数据就白收,
// 所以这里用假时间戳把边界逐个钉住。
// ---------------------------------------------------------------------------
void test_ffb_gap_stats() {
    group("[ffbgap]: 正扭矩区间的间隔分档");

    FfbGapStats g;
    g.on_torque(10.0, 5);        // 区间开始, 第一次不计间隔
    check_eq(static_cast<int>(g.total()), 0, "第一次回调不计间隔");
    check(g.max_gap == 0.0, "此时最大间隔是 0");

    g.on_torque(10.3, 7);        // 0.3 s -> <0.5
    check_eq(static_cast<int>(g.n_lt_05), 1, "0.3 s 落进 <0.5s 档");

    g.on_torque(11.3, 7);        // 1.0 s -> 0.5-2
    check_eq(static_cast<int>(g.n_05_2), 1, "1.0 s 落进 0.5-2s 档");

    g.on_torque(14.3, 7);        // 3.0 s -> 2-5
    check_eq(static_cast<int>(g.n_2_5), 1, "3.0 s 落进 2-5s 档");

    g.on_torque(20.3, 7);        // 6.0 s -> >5
    check_eq(static_cast<int>(g.n_gt_5), 1, "6.0 s 落进 >5s 档");
    check(g.max_gap > 5.9 && g.max_gap < 6.1, "max_gap 记下 6.0 s");

    g.on_torque(20.4, 0);        // 力归零 -> 区间结束
    check_eq(static_cast<int>(g.stops), 1, "力归零时记一次 stops");
    check_eq(static_cast<int>(g.total()), 4, "归零本身不产生间隔");

    // 回正期间的空白**不能**被算成一个巨大间隔: 重新起算。
    g.on_torque(60.0, 9);
    check_eq(static_cast<int>(g.total()), 4, "归零后重新来正值, 仍不计间隔");
    g.on_torque(60.5, 9);
    check_eq(static_cast<int>(g.n_lt_05), 1, "边界值 0.5 s 不进 <0.5s 档");
    check_eq(static_cast<int>(g.n_05_2), 2, "边界值 0.5 s 进 0.5-2s 档");

    g.reset();
    check_eq(static_cast<int>(g.total()), 0, "reset 清空计数");
    check(g.max_gap == 0.0 && g.stops == 0 && !g.armed(),
          "reset 也清掉 max_gap / stops 与区间状态");

    // ---- T18: 60 s 窗口打印后只清计数, 不抹掉"上一次回调的时刻" ----
    //
    // 这一段复现第二轮那局里真实发生的事: viscosity 在菜单里停了 52.5 s,
    // 起点 298.0 s, 而 300 s 那次打印恰好落在停更中间。旧实现把起点抹平,
    // 于是这段间隔在恢复后的下一档里彻底消失 —— 报告里只看到 9.50 s。
    {
        FfbGapStats w;
        w.on_torque(298.0, 10);                       // 停更前最后一次回调
        check(w.open_seconds(300.0) > 1.9 && w.open_seconds(300.0) < 2.1,
              "停更中 open 随时间增长 (2.0 s)");

        w.reset_counts();                             // 300 s 那次打印
        check_eq(static_cast<int>(w.total()), 0, "reset_counts 清掉计数");
        check(w.armed(), "reset_counts 保留区间状态");
        check(w.open_seconds(350.5) > 52.4 && w.open_seconds(350.5) < 52.6,
              "打印之后 open 继续累积到 52.5 s");

        w.on_torque(350.5, 19);                        // 停更结束, 第一个新回调
        check_eq(static_cast<int>(w.n_gt_5), 1,
                 "52.5 s 的间隔完整落进 >5s 档 (跨窗口也不丢)");
        check(w.max_gap > 52.4 && w.max_gap < 52.6, "max_gap 记下 52.5 s");

        // 反例: 整体重来会把这段丢掉 —— 用它说明两种清法的差别。
        FfbGapStats r;
        r.on_torque(298.0, 10);
        r.reset();
        r.on_torque(350.5, 19);
        check_eq(static_cast<int>(r.total()), 0,
                 "整体 reset 之后同样的 52.5 s 会消失 (旧实现的缺陷)");
        check(r.open_seconds(350.5) == 0.0, "整体 reset 之后 open 也是 0");
    }

    // ---- T18: 没有新回调的窗口照样打印 ----
    //
    // 旧实现"只在有新回调时打印"会把停更期间的行整段吞掉(第二轮 t=360 s
    // 那一行 src=visc 直接缺失), 于是最需要看的那段时间恰好没有日志。
    {
        FfbGapStats quiet;
        quiet.on_torque(400.0, 5);
        quiet.reset_counts();
        const uint64_t total = quiet.n_lt_05 + quiet.n_05_2 + quiet.n_2_5 + quiet.n_gt_5;
        check_eq(static_cast<int>(total), 0, "没有新回调时计数全 0 (但仍要打印)");
        check(quiet.open_seconds(460.0) > 59.9 && quiet.open_seconds(460.0) < 60.1,
              "60 s 没有回调时 open=60 s, 这一行能把停更指认出来");
        check(quiet.open_seconds(399.0) == 0.0, "早于最后一次回调的时间不给负 open");
    }
}

// ---------------------------------------------------------------------------
// 10b. [ffbhold]: 非零值保持了多久 (T22a)
//
// 这份统计的唯一用途是"给保持超时定默认值", 所以它必须能正确回答两个问题:
//   1) 非零值保持不变时, 这段时长有没有被记下来(包括最后降到 0 的那一段);
//   2) 长保持具体发生在什么值上 —— 1 和 19 是两回事, 超时设小了会砍掉真手感。
// 分档边界与 max/open 的语义逐条钉住, 因为默认值就是照这几档读出来的。
// ---------------------------------------------------------------------------
void test_hold_stats() {
    group("[ffbhold]: 非零值保持了多久");

    HoldStats h;
    h.on_update(10.0, 5);              // 第一个样本, 没有"上一个值"
    check_eq(static_cast<int>(h.total()), 0, "第一个样本不计保持");

    h.on_update(10.3, 5);              // 值 5 保持了 0.3 s
    check_eq(static_cast<int>(h.n_lt_05), 1, "0.3 s 进 <0.5 档");

    h.on_update(11.8, 0);              // 值 5 保持了 1.5 s, 这次降到了 0
    check_eq(static_cast<int>(h.n_1_2), 1, "1.5 s 进 1-2 档 (降到 0 也要记)");
    check(h.max_hold > 1.49 && h.max_hold < 1.51, "max_hold 记下 1.5 s");
    check_eq(static_cast<int>(h.max_val), 5, "max_val 记下那一次的值 5");

    h.on_update(20.0, 0);              // 上一个值是 0: 不记
    check_eq(static_cast<int>(h.total()), 2, "0 值保持不计数");

    h.on_update(20.0, 7);              // 0 -> 非零: 起点, 不记
    check_eq(static_cast<int>(h.total()), 2, "0 -> 非零是起点, 不计保持");

    // 分档边界: 1.0 / 2.0 / 3.0 / 5.0
    h.on_update(21.0, 7);              // 1.0 -> 1-2
    h.on_update(23.0, 7);              // 2.0 -> 2-3
    h.on_update(26.0, 7);              // 3.0 -> 3-5
    h.on_update(31.0, 7);              // 5.0 -> >5
    check_eq(static_cast<int>(h.n_1_2), 2, "边界 1.0 s 进 1-2 档");
    check_eq(static_cast<int>(h.n_2_3), 1, "边界 2.0 s 进 2-3 档");
    check_eq(static_cast<int>(h.n_3_5), 1, "边界 3.0 s 进 3-5 档");
    check_eq(static_cast<int>(h.n_gt_5), 1, "边界 5.0 s 进 >5 档");

    // open: 当前这个非零值已经保持了多久
    check(h.open_seconds(34.0) > 2.9 && h.open_seconds(34.0) < 3.1,
          "open 从最后一次回调算起 (3.0 s)");
    check(h.open_seconds(30.0) == 0.0, "早于最后一次回调的时间不给负 open");

    HoldStats z;
    z.on_update(1.0, 0);
    check(z.open_seconds(50.0) == 0.0, "当前值是 0 时 open 恒为 0");

    // 明细: 只留 >=1 s, 且最多 10 条, 超出的计 suppressed
    HoldStats d;
    d.on_update(0.0, 9);
    for (int i = 1; i <= 12; ++i) d.on_update(i * 2.0, 9);   // 每次 2.0 s
    check_eq(d.long_n, 10, "明细最多 10 条");
    check_eq(static_cast<int>(d.suppressed), 2, "超出的 2 条计 suppressed");
    check(d.longs[0].value == 9 && d.longs[0].held > 1.99 && d.longs[0].held < 2.01,
          "明细记下值与时长");
    d.on_update(100.0, 0);                                     // 0.5 s 的保持
    check_eq(d.long_n, 10, "<1 s 的保持不进明细");

    // reset_counts 只清计数, 保留"上一次回调的时刻" —— 跨 60 s 窗口的长保持不能丢
    HoldStats w;
    w.on_update(400.0, 19);
    w.reset_counts();
    check(w.value() == 19, "reset_counts 保留当前值");
    check(w.open_seconds(403.0) > 2.9 && w.open_seconds(403.0) < 3.1,
          "reset_counts 之后 open 继续累积");
    w.on_update(406.0, 0);                                    // 19 保持了 6.0 s
    check_eq(static_cast<int>(w.n_gt_5), 1, "跨窗口的 6.0 s 完整落进 >5 档");
    check(w.long_n == 1 && w.longs[0].value == 19, "跨窗口的明细也在");

    w.reset();
    check_eq(static_cast<int>(w.total()), 0, "reset 清空计数");
    check(w.long_n == 0 && w.open_seconds(500.0) == 0.0, "reset 也清明细与 open");
}

// ---------------------------------------------------------------------------
// 10. 已知风险源判定 (T11b)
//
// 这个函数决定配置加载时要不要打"viscosity 不推荐作为输出源"的告警。名单写错
// 的两种后果都不轻: 漏掉 viscosity 等于把 README 里那条风险提示变成空话;
// 把 torque 也列进去则会让告警变成常态噪声, 真出问题时反而被忽略。
// ---------------------------------------------------------------------------
void test_held_risk_source() {
    group("[已知风险] 只有 viscosity 系算");

    check(is_held_risk_source("viscosity"), "viscosity => true");
    check(is_held_risk_source("viscosity_a"), "viscosity_a => true");
    check(is_held_risk_source("VISCOSITY"), "大小写不敏感");

    check(!is_held_risk_source("torque"), "torque => false");
    check(!is_held_risk_source("torque_signed"), "torque_signed => false");
    check(!is_held_risk_source("vibration"), "vibration => false");
    check(!is_held_risk_source("vibration_a"), "vibration_a => false");
    check(!is_held_risk_source("const"), "const => false");
    check(!is_held_risk_source("servo"), "servo => false");
    check(!is_held_risk_source(""), "空串 => false");
}

// ---------------------------------------------------------------------------
// 11. watchdog_ms 的上下限折算 (T13a)
//
// 上下限是两条会互相打架的约束: 下限 3*frame_ms 保证不误触发, 上限 2000 ms
// 保证"配得太大也不至于失去兜底意义"。frame_ms 一大, 两条就交叉了, 这里把
// 边界逐个钉住 —— 否则改一次参数就可能悄悄把上限顶掉。
// ---------------------------------------------------------------------------
void test_effective_watchdog_ms() {
    group("[watchdog] 上下限折算");

    check_eq(effective_watchdog_ms(2500, 100), 2000, "2500 被压到上限 2000");
    check_eq(effective_watchdog_ms(2000, 100), 2000, "正好 2000 不动");
    check_eq(effective_watchdog_ms(500, 100), 500, "500 在区间内, 不动");
    check_eq(effective_watchdog_ms(50, 100), 300, "50 < 3*frame_ms(300) => 抬到 300");
    check_eq(effective_watchdog_ms(10, 100), 300, "低于 50 也最终落到 floor 300");
    check_eq(effective_watchdog_ms(0, 100), 300, "0 不表示关闭, 仍然落到 floor");
    check_eq(effective_watchdog_ms(2000, 20), 2000, "frame_ms=20 时 floor=60, 上限仍是 2000");
    // frame_ms=1000(max) 时 3*frame_ms=3000 已经超过 2000: 让下限优先,
    // 否则看门狗会低于帧周期的三倍而一直误触发。
    check_eq(effective_watchdog_ms(3000, 1000), 3000, "floor 3000 > 上限 2000 时下限优先");
    check_eq(effective_watchdog_ms(100, 1000), 3000, "frame_ms=1000 时低于 floor 仍抬到 3000");
}

// ---------------------------------------------------------------------------
// 13. BLE 模式下的帧节拍 (T19)
//
// 配 frame_ms=50 不会让设备变快: 一帧仍然覆盖 4x25ms, 只是管线提交频率翻倍,
// 信箱覆盖率与看门狗阈值全部跟着算错。所以在配置层固定 100, 并让非 BLE 链路
// 保持原值(空输出没有节拍约束, 离线测试要靠它跑得快)。
// ---------------------------------------------------------------------------
void test_effective_frame_ms() {
    group("[output] BLE 帧节拍固定 100");

    check_eq(effective_frame_ms(100, LinkMode::Ble), 100, "BLE 下 100 不动");
    check_eq(effective_frame_ms(50, LinkMode::Ble), 100, "BLE 下 50 => 100");
    check_eq(effective_frame_ms(200, LinkMode::Ble), 100, "BLE 下 200 => 100");
    check_eq(effective_frame_ms(20, LinkMode::Ble), 100, "BLE 下最小值 20 => 100");
    check_eq(effective_frame_ms(1000, LinkMode::Ble), 100, "BLE 下最大值 1000 => 100");
    check_eq(effective_frame_ms(50, LinkMode::Disabled), 50, "空输出不约束, 原样返回");
    check_eq(effective_frame_ms(33, LinkMode::WebSocket), 33, "WebSocket 不约束, 原样返回");
}

// ---------------------------------------------------------------------------
// 12. 覆盖计数分栏 (T13c)
//
// 链路建立前 BLE 线程不取帧, 管线照样 10 Hz 提交 —— 那些覆盖不是"传输跟不上"。
// 混在一起会让每局开头的 20 多次覆盖把 over/submitted 的比例整体拉高。
// ---------------------------------------------------------------------------
void test_mailbox_consumer_ready() {
    group("[信箱] 就绪前的覆盖单独计数");

    FrameMailbox mb;
    const Frame f = make_idle_frame("test");
    mb.put(f, 1000);
    mb.put(f, 1100);   // 未就绪: 覆盖第 1 次
    mb.put(f, 1200);   // 未就绪: 覆盖第 2 次
    MailboxStats s = mb.stats();
    check_eq(static_cast<int>(s.submitted), 3, "未就绪时也照常记 submitted");
    check_eq(static_cast<int>(s.overwritten), 0, "未就绪 => over 保持 0");
    check_eq(static_cast<int>(s.overwritten_pre), 2, "未就绪 => over_pre=2");

    mb.set_consumer_ready(true);
    check(mb.consumer_ready(), "set_consumer_ready(true) 生效");
    mb.put(f, 1300);   // 就绪: 覆盖第 1 次(上一帧仍未被取走)
    mb.put(f, 1400);   // 就绪: 覆盖第 2 次
    s = mb.stats();
    check_eq(static_cast<int>(s.overwritten), 2, "就绪后覆盖计入 over");
    check_eq(static_cast<int>(s.overwritten_pre), 2, "over_pre 不再增长");

    mb.set_consumer_ready(false);
    mb.put(f, 1500);
    s = mb.stats();
    check_eq(static_cast<int>(s.overwritten), 2, "就绪标志撤回后 over 不再增长");
    check_eq(static_cast<int>(s.overwritten_pre), 3, "撤回后覆盖回到 over_pre");
}

// ---------------------------------------------------------------------------
// 14. viscosity 保持超时 (T17)
//
// 场景就是实测到的那一段: 退出比赛回菜单后粘度源不再回调, 最后的值被当成
// "当前状态"一直输出(实测脉冲 42/100, 指示灯亮着)。保护做在"取原始值"那一层,
// 所以要看的是脉冲强度, 不是通道强度 —— static 模式下通道强度是用户设定的音量,
// 归零靠脉冲振幅为 0。
// ---------------------------------------------------------------------------
void test_viscosity_hold() {
    group("[T17] viscosity 保持超时");

    Config cfg = make_test_config();
    cfg.viscosity_hold_ms = 2000;
    cfg.channel_a.source = "torque";
    cfg.channel_b.source = "viscosity";

    Pipeline p(cfg);
    FfbState ffb = make_ffb();
    ffb.torque_magnitude = 64;
    ffb.torque_at = 1.0;
    ffb.viscosity_b = 32;
    ffb.viscosity_at = 1.0;   // 1.0 s 时刚更新过

    Frame out;
    int pulse_19 = -1;   // now=2.9s, 停更 1.9s
    int pulse_21 = -1;   // now=3.1s, 停更 2.1s
    int a_pulse_21 = -1;
    int strength_21 = -1;
    // 10 Hz 推进, dt 保持 0.1 s —— 否则会撞上管线自己的停顿判定, 干扰结果。
    for (int i = 0; i <= 21; ++i) {
        const double now = 1.0 + static_cast<double>(i) * 0.1;
        p.tick(now, ffb, out);
        if (i == 0) check(out.b.slots[0].pulse_intensity > 0, "刚更新过 => B 有脉冲");
        if (i == 19) pulse_19 = out.b.slots[0].pulse_intensity;
        if (i == 21) {
            pulse_21 = out.b.slots[0].pulse_intensity;
            a_pulse_21 = out.a.slots[0].pulse_intensity;
            strength_21 = out.b.strength;
        }
    }
    check(pulse_19 > 0, "停更 1.9s < 2000ms => 仍然输出");
    check_eq(pulse_21, 0, "停更 2.1s > 2000ms => 脉冲归零");
    check_eq(strength_21, 10, "归零发生在脉冲层, static 的通道强度保持");
    check(a_pulse_21 > 0, "另一通道(torque)不受影响");

    // 源一更新就立刻恢复 —— 不做渐变(保持超时是"没数据", 不是"要淡出")。
    ffb.viscosity_at = 3.2;
    p.tick(3.2, ffb, out);
    check(out.b.slots[0].pulse_intensity > 0, "源更新 => 下一帧就恢复");

    // 0 = 关闭: 一直输出停住的值(这是显式选择, 不是 bug)
    Config off_cfg = cfg;
    off_cfg.viscosity_hold_ms = 0;
    Pipeline p_off(off_cfg);
    FfbState stale = ffb;
    stale.viscosity_at = 1.0;
    for (int i = 0; i <= 100; ++i) {
        p_off.tick(1.0 + static_cast<double>(i) * 0.1, stale, out);
    }
    check(out.b.slots[0].pulse_intensity > 0, "viscosity_hold_ms=0 => 不判定, 照旧输出");

    // 非 viscosity 源不受影响(vibration 在默认配置里是 B 的源)
    Config vib_cfg = cfg;
    vib_cfg.channel_b.source = "vibration";
    Pipeline p_vib(vib_cfg);
    FfbState v = make_ffb();
    v.vibration_b = 40;
    v.vibration_at = 1.0;
    for (int i = 0; i <= 100; ++i) {
        p_vib.tick(1.0 + static_cast<double>(i) * 0.1, v, out);
    }
    check(out.b.slots[0].pulse_intensity > 0,
          "vibration 源在 vibration_hold_ms=0 时不判定(要测它自己的阈值见 T22b 用例)");

    // 纯函数折算: 0 关闭, 其余钳到 500..10000
    check_eq(effective_viscosity_hold_ms(0), 0, "0 = 关闭, 原样");
    check_eq(effective_viscosity_hold_ms(2000), 2000, "区间内不动");
    check_eq(effective_viscosity_hold_ms(500), 500, "恰好 500 不动");
    check_eq(effective_viscosity_hold_ms(10000), 10000, "恰好 10000 不动");
    check_eq(effective_viscosity_hold_ms(100), 500, "<500 抬到 500");
    check_eq(effective_viscosity_hold_ms(50000), 10000, ">10000 降到 10000");
}

// ---------------------------------------------------------------------------
// 12b. T22b: torque / vibration 的单源保持超时(二级)
//
// 这一级唯一的作用是"某个源彻底不更新时兜底", 所以阈值必须大于实测的真实
// 保持(torque 最长 7.0 s), 默认 10 s。用例同时钉住"只作用于原始值, 强度字节
// 不动"和"源一更新立刻恢复(不做渐变)"。
// ---------------------------------------------------------------------------
void test_source_hold() {
    group("[T22b] torque/vibration 保持超时");

    Config cfg = make_test_config();
    cfg.channel_b.enable = false;      // 只测 A; B 的判定由下面的齐停用例覆盖
    cfg.torque_hold_ms = 2000;

    Pipeline p(cfg);
    FfbState ffb = make_ffb();
    ffb.torque_at = 1.0;
    Frame out;
    int p19 = -1, p21 = -1, strength_21 = -1;
    for (int i = 0; i <= 21; ++i) {
        const double now = 1.0 + static_cast<double>(i) * 0.1;
        p.tick(now, ffb, out);
        if (i == 0) check(out.a.slots[0].pulse_intensity > 0, "刚更新过 => A 有脉冲");
        if (i == 19) p19 = out.a.slots[0].pulse_intensity;
        if (i == 21) {
            p21 = out.a.slots[0].pulse_intensity;
            strength_21 = out.a.strength;
        }
    }
    check(p19 > 0, "停更 1.9 s < 2000 ms => 仍然输出");
    check_eq(p21, 0, "停更 2.1 s > 2000 ms => 脉冲归零");
    check_eq(strength_21, 15, "强度字节不动(只作用于原始值)");

    ffb.torque_at = 2.2;
    p.tick(2.2, ffb, out);
    check(out.a.slots[0].pulse_intensity > 0, "源一更新 => 下一帧就恢复");

    // vibration 走自己的阈值(默认配置里 B 就是用它的)
    Config vcfg = make_test_config();
    vcfg.channel_b.enable = false;
    vcfg.channel_a.source = "vibration";
    vcfg.vibration_hold_ms = 2000;     // 实测 vibration 源读 b 参数
    Pipeline pv(vcfg);
    FfbState v = make_ffb();
    v.vibration_b = 40;
    v.vibration_at = 1.0;
    int v19 = -1, v21 = -1;
    for (int i = 0; i <= 21; ++i) {
        const double now = 1.0 + static_cast<double>(i) * 0.1;
        pv.tick(now, v, out);
        if (i == 19) v19 = out.a.slots[0].pulse_intensity;
        if (i == 21) v21 = out.a.slots[0].pulse_intensity;
    }
    check(v19 > 0, "vibration 停更 1.9 s => 仍输出");
    check_eq(v21, 0, "vibration 停更 2.1 s => 归零");

    // 0 = 关闭
    Config off = cfg;
    off.torque_hold_ms = 0;
    Pipeline po(off);
    FfbState stale = ffb;
    stale.torque_at = 1.0;
    for (int i = 0; i <= 100; ++i) {
        po.tick(1.0 + static_cast<double>(i) * 0.1, stale, out);
    }
    check(out.a.slots[0].pulse_intensity > 0, "torque_hold_ms=0 => 不判定");

    // 单源超时只影响用这个源的通道: B 用 constant 模式, 不受 A 的 torque 超时影响
    Config mix = cfg;
    mix.channel_b.enable = true;
    mix.channel_b.mode = MapMode::Constant;
    mix.channel_b.level = 1.0;
    mix.channel_b.source = "viscosity";
    mix.viscosity_hold_ms = 0;         // 让 B 的源不受二级影响
    Pipeline pm(mix);
    FfbState m = make_ffb();
    m.torque_at = 1.0;
    for (int i = 0; i <= 25; ++i) {
        pm.tick(1.0 + static_cast<double>(i) * 0.1, m, out);
    }
    check_eq(out.a.slots[0].pulse_intensity, 0, "A 的 torque 停更 2.5 s => 归零");
    check_eq(out.b.slots[0].pulse_intensity, 100, "B 不受影响(仍满脉冲)");

    // 纯函数折算: 上限由调用方给
    check_eq(effective_source_hold_ms(0, "torque_hold_ms", 20000), 0, "0 = 关闭, 原样");
    check_eq(effective_source_hold_ms(10000, "torque_hold_ms", 20000), 10000,
             "默认值不被改动");
    check_eq(effective_source_hold_ms(15000, "vibration_hold_ms", 20000), 15000,
             "20000 上限内不动");
    check_eq(effective_source_hold_ms(30000, "torque_hold_ms", 20000), 20000,
             "超上限降到 20000");
    check_eq(effective_source_hold_ms(100, "torque_hold_ms", 20000), 500, "<500 抬到 500");
    check_eq(effective_source_hold_ms(1, "torque_hold_ms", 20000), 500, "边界 1 抬到 500");
    check_eq(effective_source_hold_ms(499, "torque_hold_ms", 20000), 500, "边界 499 抬到 500");
    check_eq(effective_source_hold_ms(500, "torque_hold_ms", 20000), 500, "边界 500 不动");
    check_eq(effective_source_hold_ms(10001, "torque_hold_ms", 20000), 10001,
             "10001 仍在 20000 上限内");
    check_eq(effective_source_hold_ms(-500, "torque_hold_ms", 20000), 500,
             "负数按 <500 处理, 抬到 500");

    // T22b: hold_trips 只数"停住的值不是 0"的进入次数
    {
        Config t = make_test_config();
        t.channel_b.enable = false;
        t.torque_hold_ms = 2000;
        Pipeline pt(t);
        FfbState f = make_ffb();
        f.torque_at = 1.0;
        for (int i = 0; i <= 25; ++i) pt.tick(1.0 + static_cast<double>(i) * 0.1, f, out);
        check_eq(static_cast<int>(pt.hold_trips_a()), 1,
                 "停住的值是 64(非 0) => 记一次 hold_trips");
        check_eq(static_cast<int>(pt.hold_trips_b()), 0, "B 没启用, 不记");

        // 停住的本来就是 0: 不记(否则 vibration 这种源每次都刷一条)
        Pipeline pz(t);
        FfbState z = make_ffb();
        z.torque_magnitude = 0;
        z.torque_at = 1.0;
        for (int i = 0; i <= 25; ++i) pz.tick(1.0 + static_cast<double>(i) * 0.1, z, out);
        check_eq(static_cast<int>(pz.hold_trips_a()), 0,
                 "停住的值是 0 => 不计 hold_trips");

        // 增益包络也必须看到 0: 归零后按 release 曲线回落到 floor
        Config g = t;
        g.auto_gain = true;
        g.gain_floor = 24;
        g.gain_ceiling = 127;
        g.gain_release_sec = 1.0;
        Pipeline pg(g);
        FfbState gf = make_ffb();
        gf.torque_at = 1.0;
        for (int i = 0; i <= 10; ++i) pg.tick(1.0 + static_cast<double>(i) * 0.1, gf, out);
        for (int i = 0; i <= 300; ++i) pg.tick(2.0 + static_cast<double>(i) * 0.1, gf, out);
        check(pg.gain_summary().find("gainA=24") != std::string::npos,
              "超时期间增益回落到 floor —— 包络看到的也是 0");
    }
}

// ---------------------------------------------------------------------------
// 12c. T22b: 全源静默(一级)
//
// 这一级的判据是"三个源同时停更", 抓的是"游戏不再喂任何力反馈数据"(退出比赛、
// 对战匹配打断计时赛)。实测这种齐停最短 48 s, 正常驾驶中 0 次, 所以 1 s 阈值
// 不会误伤; 而只要有一个源还在动, 就必须解除 —— 否则真实过弯(粘度每秒更新、
// torque 稳住 7 s)会被误判。
// ---------------------------------------------------------------------------
void test_all_sources_silent() {
    group("[T22b] 全源静默(一级)");

    Config cfg = make_test_config();
    cfg.silent_mute_ms = 1000;
    Pipeline p(cfg);                    // A=torque, B=viscosity
    FfbState ffb = make_ffb();
    ffb.torque_at = 1.0;
    ffb.viscosity_at = 1.0;
    ffb.vibration_at = 0.0;             // 整局没调过 vibration: 不该拖住判定
    Frame out;

    p.tick(1.9, ffb, out);
    check(out.a.slots[0].pulse_intensity > 0 && out.b.slots[0].pulse_intensity > 0,
          "齐停 0.9 s < 1000 ms => 两路都还输出");

    p.tick(2.1, ffb, out);
    check_eq(out.a.slots[0].pulse_intensity, 0, "齐停 1.1 s => A 脉冲归零");
    check_eq(out.b.slots[0].pulse_intensity, 0, "齐停 1.1 s => B 脉冲归零");
    check(out.a.strength > 0 && out.b.strength > 0, "强度字节不变(只作用于原始值)");

    // 只有粘度恢复: 一级立刻解除, 而 torque 才停 1.2 s(远不到二级的 10 s)
    ffb.viscosity_at = 2.2;
    p.tick(2.2, ffb, out);
    check(out.a.slots[0].pulse_intensity > 0, "只要有一个源活着 => 一级解除");
    check(out.b.slots[0].pulse_intensity > 0, "恢复的那个通道也回来");

    // 恒定输出(map_mode=constant)也受一级影响 —— 齐停时没有任何通道该继续输出
    Config c2 = cfg;
    c2.channel_b.source = "viscosity";
    c2.channel_b.mode = MapMode::Constant;
    c2.channel_b.level = 1.0;
    Pipeline pc(c2);
    FfbState g = make_ffb();
    g.torque_at = 1.0;
    g.viscosity_at = 1.0;
    pc.tick(1.9, g, out);
    check_eq(out.b.slots[0].pulse_intensity, 100, "Constant 模式平时是满脉冲");
    pc.tick(2.1, g, out);
    check_eq(out.b.slots[0].pulse_intensity, 0, "全源静默 => Constant 通道也静音");

    // 0 = 关闭
    Config off = cfg;
    off.silent_mute_ms = 0;
    Pipeline po(off);
    FfbState s = make_ffb();
    s.torque_at = 1.0;
    s.viscosity_at = 1.0;
    for (int i = 0; i <= 30; ++i) {
        po.tick(1.0 + static_cast<double>(i) * 0.1, s, out);
    }
    check(out.a.slots[0].pulse_intensity > 0, "silent_mute_ms=0 => 不判齐停");

    // 一个样本都没有时这一级不判定(交给 evaluate_safety 的 no_ffb_data_yet)
    Config early = cfg;
    Pipeline pe(early);
    FfbState none;                       // 所有 *_at 都是 0
    none.servo_on = true;
    none.ever_updated = false;
    pe.tick(100.0, none, out);
    check_eq(out.a.strength, 0, "没有样本时由 no_ffb_data_yet 静音, 不是这一级");

    // 纯函数折算: 0 关闭, 其余钳到 200..10000
    check_eq(effective_silent_mute_ms(0), 0, "0 = 关闭, 原样");
    check_eq(effective_silent_mute_ms(1000), 1000, "默认值不被改动");
    check_eq(effective_silent_mute_ms(100), 200, "<200 抬到 200");
    check_eq(effective_silent_mute_ms(20000), 10000, ">10000 降到 10000");
}

// ---------------------------------------------------------------------------
// 13. T23: 三处默认值防漂移
//
// 同一个配置有三份来源: 发布包模板(tools/DGLab_IDAC.ini.template)、
// INI 缺失时自动生成的 write_default_config()、以及 config.hpp 里"缺键时"
// 的结构体默认值。三份里任何一份单独改动, 用户看到的行为就会因"INI 在不在"
// 而不同 —— v0.2.0 就发生过(模板 15/10, 生成 40/30, 缺键 60/60)。
//
// 所以这里把三者钉在一起: 模板与生成文件逐字段相同(除两个故意不同的开关),
// 而"只写两个开关"的极简 INI 必须和模板完全一致 —— 后者检查的是结构体默认值。
// 找不到模板算失败, 不许跳过: 跳过等于这条防线不存在。
// ---------------------------------------------------------------------------
namespace {

std::string read_text_file(const std::string& path, bool* ok) {
    std::string out;
    if (ok != nullptr) *ok = false;
    FILE* f = nullptr;
    if (::fopen_s(&f, path.c_str(), "rb") != 0 || f == nullptr) return out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    if (ok != nullptr) *ok = true;
    return out;
}

std::string ascii_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// 取出所有 "section.key"(小写), 忽略注释与空白。
std::vector<std::string> ini_keys(const std::string& text) {
    std::vector<std::string> keys;
    std::string sec;
    std::string line;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size() && text[i] != '\n') {
            line.push_back(text[i]);
            continue;
        }
        std::string s = line;
        line.clear();
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
        const size_t b = s.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        s = s.substr(b);
        if (s[0] == ';' || s[0] == '#') continue;
        if (s[0] == '[') {
            const size_t r = s.find(']');
            if (r != std::string::npos) sec = ascii_lower(s.substr(1, r - 1));
            continue;
        }
        std::string key = (s.find('=') == std::string::npos) ? s : s.substr(0, s.find('='));
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (!key.empty() && !sec.empty()) keys.push_back(sec + "." + ascii_lower(key));
    }
    return keys;
}

bool has_key(const std::vector<std::string>& keys, const std::string& k) {
    for (const auto& x : keys) {
        if (x == k) return true;
    }
    return false;
}

// 逐字段比较。新增配置字段时必须在这里补一行 —— 这是有意的: 让"忘记同步
// 三处默认值"变成编译期之后的第一道红灯, 而不是用户手感变了才发现。
std::string diff_configs(const Config& a, const Config& b, bool include_switches) {
    std::string d;
    auto I = [&](int x, int y, const char* n) {
        if (x != y) { d += n; d += "(" + std::to_string(x) + "!=" + std::to_string(y) + ") "; }
    };
    auto D = [&](double x, double y, const char* n) {
        if (!(std::fabs(x - y) < 1e-9)) {
            d += n; d += "(" + std::to_string(x) + "!=" + std::to_string(y) + ") ";
        }
    };
    auto B = [&](bool x, bool y, const char* n) {
        if (x != y) { d += n; d += "(" + std::string(x ? "1" : "0") + "!=" + (y ? "1" : "0") + ") "; }
    };
    auto S = [&](const std::string& x, const std::string& y, const char* n) {
        if (x != y) { d += n; d += "(" + x + "!=" + y + ") "; }
    };

    if (include_switches) {
        I(static_cast<int>(a.link), static_cast<int>(b.link), "link");
        B(a.dry_run, b.dry_run, "dry_run");
    }
    B(a.enabled, b.enabled, "enabled");
    B(a.verbose, b.verbose, "verbose");
    I(static_cast<int>(a.protocol), static_cast<int>(b.protocol), "protocol");
    S(a.ble_device_name, b.ble_device_name, "ble_device_name");
    I(a.ble_reconnect_ms, b.ble_reconnect_ms, "ble_reconnect_ms");
    I(a.ble_connect_timeout_ms, b.ble_connect_timeout_ms, "ble_connect_timeout_ms");
    I(a.frame_ms, b.frame_ms, "frame_ms");
    I(a.pulse_freq_base, b.pulse_freq_base, "pulse_freq_base");
    I(a.pulse_freq_min, b.pulse_freq_min, "pulse_freq_min");
    I(a.pulse_freq_max, b.pulse_freq_max, "pulse_freq_max");
    D(a.pulse_freq_track, b.pulse_freq_track, "pulse_freq_track");
    B(a.auto_gain, b.auto_gain, "auto_gain");
    D(a.gain_attack_sec, b.gain_attack_sec, "gain_attack_sec");
    D(a.gain_release_sec, b.gain_release_sec, "gain_release_sec");
    I(a.gain_floor, b.gain_floor, "gain_floor");
    I(a.gain_ceiling, b.gain_ceiling, "gain_ceiling");
    D(a.gain_idle_reset_sec, b.gain_idle_reset_sec, "gain_idle_reset_sec");
    I(a.max_strength, b.max_strength, "max_strength");
    I(a.clamp_strength, b.clamp_strength, "clamp_strength");
    D(a.slew_per_second, b.slew_per_second, "slew_per_second");
    I(a.watchdog_ms, b.watchdog_ms, "watchdog_ms");
    B(a.zero_when_servo_off, b.zero_when_servo_off, "zero_when_servo_off");
    I(a.viscosity_hold_ms, b.viscosity_hold_ms, "viscosity_hold_ms");
    I(a.torque_hold_ms, b.torque_hold_ms, "torque_hold_ms");
    I(a.vibration_hold_ms, b.vibration_hold_ms, "vibration_hold_ms");
    I(a.silent_mute_ms, b.silent_mute_ms, "silent_mute_ms");
    I(a.warmup_ms, b.warmup_ms, "warmup_ms");
    B(a.torque_negative_is_positive, b.torque_negative_is_positive,
      "torque_negative_is_positive");
    B(a.hook, b.hook, "hook");

    const char* ch[2] = {"a", "b"};
    const ChannelMapping* ca[2] = {&a.channel_a, &a.channel_b};
    const ChannelMapping* cb[2] = {&b.channel_a, &b.channel_b};
    for (int i = 0; i < 2; ++i) {
        const std::string p = std::string(ch[i]) + ".";
        B(ca[i]->enable, cb[i]->enable, (p + "enable").c_str());
        I(static_cast<int>(ca[i]->mode), static_cast<int>(cb[i]->mode), (p + "mode").c_str());
        I(ca[i]->in_min, cb[i]->in_min, (p + "in_min").c_str());
        I(ca[i]->in_max, cb[i]->in_max, (p + "in_max").c_str());
        D(ca[i]->level, cb[i]->level, (p + "level").c_str());
        I(ca[i]->base_strength, cb[i]->base_strength, (p + "base_strength").c_str());
        S(ca[i]->strength_source, cb[i]->strength_source, (p + "strength_source").c_str());
        D(ca[i]->strength_margin, cb[i]->strength_margin, (p + "strength_margin").c_str());
        S(ca[i]->source, cb[i]->source, (p + "source").c_str());
    }
    return d;
}

bool write_text_file(const std::string& path, const std::string& text) {
    FILE* f = nullptr;
    if (::fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) return false;
    const size_t n = std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return n == text.size();
}

// ★ 必须用绝对路径 ★
// WritePrivateProfileString / GetPrivateProfileString 收到**相对路径**时, 会先
// 去 Windows 目录找同名文件, 而不是相对当前工作目录 —— 测试里用相对路径会
// 让"写进去"和"读出来"落在两个不同地方, 表现成的假象是"生成文件里一个键都
// 没有, 而两份配置逐字段相同"。
std::string absolute_path(const std::string& p) {
    char buf[4096] = {0};
    if (::_fullpath(buf, p.c_str(), sizeof buf) == nullptr) return p;
    return std::string(buf);
}

}  // namespace

void test_default_drift() {
    group("[T23] 三处默认值不漂移");

    bool ok = false;
    const std::string tpl_text = read_text_file(DGLAB_TEMPLATE_FILE, &ok);
    check(ok, std::string("模板文件必须存在: ") + DGLAB_TEMPLATE_FILE);
    if (!ok) return;   // 找不到模板就是失败, 不跳过

    // load_config 会就地做 LF->CRLF 归一化, 所以一律读临时副本。
    const std::string tpl_copy = absolute_path("dglab_drift_tpl.ini");
    const std::string gen_path = absolute_path("dglab_drift_gen.ini");
    const std::string min_path = absolute_path("dglab_drift_min.ini");
    std::remove(tpl_copy.c_str());
    std::remove(gen_path.c_str());
    std::remove(min_path.c_str());

    check(write_text_file(tpl_copy, tpl_text), "模板副本写出成功");
    check(write_default_config(gen_path), "write_default_config 写出文件");
    check(write_text_file(min_path, "[link]\nmode=ble\n[safety]\ndry_run=0\n"),
          "极简 INI 写出成功");

    const std::string gen_text = read_text_file(gen_path, &ok);
    check(ok, "能读回生成的文件");

    // 1) 模板里的每一个键, 生成文件里都要有
    const std::vector<std::string> tpl_keys = ini_keys(tpl_text);
    const std::vector<std::string> gen_keys = ini_keys(gen_text);
    check(tpl_keys.size() > 20, "模板里解析出了键(防止解析器本身失效)");
    std::string missing;
    for (const auto& k : tpl_keys) {
        if (!has_key(gen_keys, k)) { missing += k; missing += " "; }
    }
    check(missing.empty(), "生成文件缺少模板里的键: " +
                               (missing.empty() ? std::string("(无)") : missing));

    // 2) 逐字段比较, 除 link 与 dry_run
    const Config ct = load_config(tpl_copy);
    const Config cg = load_config(gen_path);
    const std::string d = diff_configs(ct, cg, false);
    check(d.empty(), "模板与生成文件逐字段相同(除 link/dry_run): " +
                         (d.empty() ? std::string("(无)") : d));

    // 3) 缺键默认值: 只写两个开关, 结果必须与模板完全一致
    const Config cm = load_config(min_path);
    const std::string d2 = diff_configs(ct, cm, true);
    check(d2.empty(), "只写两个开关时, 其余全部来自缺键默认值且与模板一致: " +
                          (d2.empty() ? std::string("(无)") : d2));

    // 两个开关本身确实是故意不同的(否则上面两条比较会失去意义)
    check(cg.link == LinkMode::Disabled && ct.link == LinkMode::Ble,
          "生成文件故意用 link=disabled, 模板是 ble");
    check(cg.dry_run && !ct.dry_run, "生成文件故意用 dry_run=1, 模板是 0");
    check(ct.channel_a.base_strength == 15 && ct.channel_b.base_strength == 10,
          "两路 base_strength 不同(A=15, B=10), 且缺键默认值与之一致");
    check(std::fabs(ct.channel_b.level - 0.6) < 1e-9, "B 的 level 是 0.6");

    std::remove(tpl_copy.c_str());
    std::remove(gen_path.c_str());
    std::remove(min_path.c_str());
}

}  // namespace

// ---------------------------------------------------------------------------
int main() {
    std::printf("logic_check —— 信箱/看门狗/静默帧/管线的离线逻辑自检\n");

    test_mailbox_basic();
    test_mailbox_overwrite();
    test_watchdog_trip();
    test_watchdog_recover();
    test_idle_frame_encoding();
    test_pipeline_normal();
    test_pipeline_stall();
    test_pipeline_warmup();
    test_ffb_gap_stats();
    test_held_risk_source();
    test_hold_stats();
    test_effective_watchdog_ms();
    test_effective_frame_ms();
    test_viscosity_hold();
    test_source_hold();
    test_all_sources_silent();
    test_default_drift();
    test_mailbox_consumer_ready();

    std::printf("\n-------------------------------------------\n");
    std::printf("通过 %d, 失败 %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        std::printf("结论: 看门狗与静默帧逻辑自洽。\n");
        return 0;
    }
    std::printf("结论: 有 %d 项失败, 见上面的 [FAIL]。\n", g_fail);
    return 1;
}
