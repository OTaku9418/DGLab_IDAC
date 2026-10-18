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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

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

    std::printf("\n-------------------------------------------\n");
    std::printf("通过 %d, 失败 %d\n", g_pass, g_fail);
    if (g_fail == 0) {
        std::printf("结论: 看门狗与静默帧逻辑自洽。\n");
        return 0;
    }
    std::printf("结论: 有 %d 项失败, 见上面的 [FAIL]。\n", g_fail);
    return 1;
}
