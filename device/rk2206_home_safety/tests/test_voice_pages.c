/* 语音帧解析的主机侧测试：不需要板子、不需要 SDK，`cc` 编完就跑。
 *
 * 只验证 su_03t.c 的「收帧 → 干什么」这一段，重点覆盖两类容易改错的地方：
 *   1. 查询连播（0x14 05/06）有没有真的错开发送；
 *   2. 翻页（0x15 01~09）页号映射对不对，相对翻页给的是不是哨兵。
 *
 * 其余模块（safety/living/sntp）用下面的桩顶替，只回固定值。
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "su_03t.h"
#include "safety_runtime.h"
#include "living.h"
#include "sntp_client.h"
#include "iot_uart.h"
#include "los_tick.h"

/* ---------------- 桩 ---------------- */
static uint64_t g_tick = 1000;

uint64_t LOS_TickCountGet(void) { return g_tick; }

/* su03t_init() 会建线程；主机侧不建，按成功返回即可 */
unsigned int LOS_TaskCreate(unsigned int *task_id, void *param)
{
    (void)task_id; (void)param;
    return 0;   /* LOS_OK */
}
void LOS_Msleep(unsigned int ms) { g_tick += ms; }

static safety_state_t g_state;
static safety_config_t g_cfg;
static int g_last_cmd = -1;

void safety_lock(void) {}
void safety_unlock(void) {}

void safety_state_get(safety_state_t *state) { *state = g_state; }
void safety_config_get(safety_config_t *out) { *out = g_cfg; }
void safety_state_quality(const safety_state_t *state, unsigned int uptime, safety_quality_t *out)
{
    (void)state; (void)uptime;
    memset(out, 0, sizeof(*out));
    out->env_available = 1;
}
int safety_command_submit(int code, const char *source, unsigned int *id)
{
    (void)source;
    g_last_cmd = code;
    if (id) *id = 1;
    return 0;
}

void living_get(living_status_t *out) { memset(out, 0, sizeof(*out)); }
int living_slot_enabled(const living_config_t *c, int slot) { (void)c; (void)slot; return 0; }
int living_confirm_next(unsigned epoch, int *slot)
{
    (void)epoch;
    if (slot) *slot = 0;
    return 0;
}
void living_light_override(int mode) { (void)mode; }
int living_execute(int code, const char *json, unsigned epoch)
{
    (void)code; (void)json; (void)epoch;
    return 0;
}

unsigned int sntp_get_epoch(void) { return 1700000000U; }

/* 串口桩：把发出去的字节省下来，测试里逐帧比对 */
#define MAX_FRAMES 32
static uint8_t g_frames[MAX_FRAMES][16];
static int g_frame_len[MAX_FRAMES];
static int g_frames_n;

unsigned int IoTUartInit(unsigned int id, IotUartAttribute *attr) { (void)id; (void)attr; return 0; }
unsigned int IoTUartDeinit(unsigned int id) { (void)id; return 0; }
int IoTUartRead(unsigned int id, uint8_t *data, unsigned int len) { (void)id; (void)data; (void)len; return 0; }
int IoTUartWrite(unsigned int id, const uint8_t *data, unsigned int len)
{
    (void)id;
    if (g_frames_n < MAX_FRAMES && len <= sizeof(g_frames[0])) {
        memcpy(g_frames[g_frames_n], data, len);
        g_frame_len[g_frames_n] = (int)len;
        g_frames_n++;
    }
    return (int)len;
}

static void frames_reset(void) { g_frames_n = 0; }

/* 推进时间并让队列里到点的帧发出去 */
static void tick_advance(uint64_t ms)
{
    g_tick += ms;
    su03t_receive_bytes((const uint8_t *)"", 0);
}

static void send_frame(uint8_t prefix, uint8_t param)
{
    uint8_t cmd[2] = {prefix, param};
    su03t_receive_bytes(cmd, sizeof(cmd));
}

/* ---------------- 用例 ---------------- */
static void setup(void)
{
    memset(&g_state, 0, sizeof(g_state));
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_state.temp = 26.4;
    g_state.humi = 53.0;
    g_state.light = 248.0;
    g_state.gas = 100.0;
    g_cfg.gas_limit = 1000.0;
    g_tick = 1000;
    frames_reset();
    su03t_init();
    /* 开关打开时会排一句上电自检（23 号「语音已就绪」），先把这条放掉再清计数，
     * 否则后面按序号取帧全都会错位。 */
    tick_advance(2000);
    frames_reset();
    su03t_take_page();
}

/* 0x14 06：温度、湿度、光照三句，各隔 1.4 秒 */
static void test_chain_three(void)
{
    setup();
    g_state.light = 248.0;
    send_frame(SU03T_PFX_QUERY, 6);
    assert(g_frames_n == 1 && g_frame_len[0] == 13 && g_frames[0][2] == SU03T_VAL_IDX_TEMP);

    tick_advance(SU03T_CHAIN_STEP_MS - 100);
    assert(g_frames_n == 1);                 /* 还没到第二句 */

    tick_advance(200);
    assert(g_frames_n == 2 && g_frames[1][2] == SU03T_VAL_IDX_HUMI);

    tick_advance(SU03T_CHAIN_STEP_MS);
    assert(g_frames_n == 3 && g_frames[2][2] == SU03T_VAL_IDX_LIGHT);
    printf("PASS 0x14 06 三连播：温度 → 湿度 → 光照，逐句错开 %d ms\n", SU03T_CHAIN_STEP_MS);
}

/* 光照取不到数时不能把后面的节奏打乱 */
static void test_chain_degraded(void)
{
    setup();
    g_state.light = -1.0;                    /* 光照无效 */
    send_frame(SU03T_PFX_QUERY, 6);
    assert(g_frames_n == 1 && g_frames[0][2] == SU03T_VAL_IDX_TEMP);
    tick_advance(SU03T_CHAIN_STEP_MS);
    assert(g_frames_n == 2 && g_frames[1][2] == SU03T_VAL_IDX_HUMI);
    tick_advance(SU03T_CHAIN_STEP_MS);
#if SU03T_ACTION_VOICE_ENABLED
    /* 词条烧好、开关打开：第三句换成固定句「现在没数据」（6 字节） */
    assert(g_frames_n == 3 && g_frames[2][2] == SU03T_SAY_NO_DATA && g_frame_len[2] == 6);
    printf("PASS 0x14 06 光照无效：仍按顺序播完，第三句换成「现在没数据」\n");
#else
    /* 开关没开：固定句按设计整体不发，只剩两句数值 */
    assert(g_frames_n == 2);
    printf("PASS 0x14 06 光照无效：固定句开关未开，只发两句数值（符合设计）\n");
#endif
}

/* 0x15 01~07：七个页号映射 */
static void test_page_abs(void)
{
    /* 期望：01→记录一页(1) 02→记录二页(2) 03→主页(0) 04→诊断(3) 05→生活提醒(4) 06→联系人(5) 07→配对(6) */
    static const int expect[7] = {1, 2, 0, 3, 4, 5, 6};
    for (int i = 0; i < 7; ++i) {
        setup();
        send_frame(SU03T_PFX_PAGE, (uint8_t)(i + 1));
        assert(su03t_take_page() == expect[i]);
        assert(su03t_take_page() == -1);
    }
    printf("PASS 0x15 01~07 七个页面各自直达\n");
}

/* 0x15 08/09：相对翻页给哨兵，由主循环按当前页加减 */
static void test_page_relative(void)
{
    setup();
    send_frame(SU03T_PFX_PAGE, 8);
    assert(su03t_take_page() == SU03T_PAGE_NEXT);
    send_frame(SU03T_PFX_PAGE, 9);
    assert(su03t_take_page() == SU03T_PAGE_PREV);
    printf("PASS 0x15 08/09 下一页=%d 上一页=%d\n", SU03T_PAGE_NEXT, SU03T_PAGE_PREV);
}

/* 越界参数必须一个动作都不触发 */
static void test_invalid(void)
{
    setup();
    send_frame(SU03T_PFX_PAGE, 10);
    assert(su03t_take_page() == -1);
    send_frame(SU03T_PFX_PAGE, 0);
    assert(su03t_take_page() == -1);
    send_frame(SU03T_PFX_QUERY, 7);
    assert(g_frames_n == 0);
    printf("PASS 越界参数 15 0A / 15 00 / 14 07 全部拒绝\n");
}

/* 老用例回归：15 01 仍然给 1 */
static void test_legacy(void)
{
    setup();
    send_frame(SU03T_PFX_PAGE, 1);
    assert(su03t_take_page() == 1);
    send_frame(SU03T_PFX_QUERY, 4);
    assert(g_frames_n == 1 && g_frame_len[0] == 13 && g_frames[0][2] == SU03T_VAL_IDX_LIGHT);
    printf("PASS 回归：15 01 → 1，14 04 → 13 字节光照数值帧\n");
}

int main(void)
{
    test_chain_three();
    test_chain_degraded();
    test_page_abs();
    test_page_relative();
    test_invalid();
    test_legacy();
    puts("全部通过");
    return 0;
}
