/*
 * 手表扩展模块 - 界面层
 *
 * 屏：1.28" 圆屏 GC9A01，240x240，软 SPI（IO7/9/10/11/12）
 * 因为圆屏四角被切掉，所有内容都往中间收，y 从 30 到 215 之间排。
 *
 * 翻页：把手表翻过来（Z 轴 < -0.6g）切下一页；加速度计没起来时改成 6 秒自动轮播。
 */
#include "watch_ui.h"

#include "lcd.h"
#include "lcd_init.h"
#include "watch_sensor.h"

#include "soc_osal.h"
#include "watchdog.h"
#include "tcxo.h"

#include <stdio.h>
#include <string.h>

#define UI_TICK_MS      100
#define SCR_W           240

/* 1 = 上电先做「时钟脚扫描」，把 SPI 时钟逐个挪到候选脚上，用于定位飞线落点。
 * 0 = 直接按 IO7 点亮（正常使用）。 */
#define LCD_CLK_SWEEP   0

/* 1 = 点亮诊断模式：开机后无限循环「重新初始化 + 刷一条彩色横带」，
 *     不再进入表盘。屏只要有任何一下接通，那一瞬间就会立刻出颜色。
 * 0 = 正常模式（五色自检一次，然后进表盘）。
 *     现在整板自检（board_check.c）已经能把屏的时钟脚直接找出来，
 *     不再需要人肉数横条，所以回到正常模式。 */
#define LCD_RETRY_TEST  0

#define C_BG            BLACK
#define C_TITLE         GRAY
#define C_TIME          CYAN
#define C_HR            RED
#define C_SPO2          GBLUE
#define C_STEP          GRED
#define C_TEXT          LGRAY
#define C_DOT_ON        CYAN
#define C_DOT_OFF       GRAYBLUE

static int s_page;
static int s_drawn = -1;
static int s_flip_armed = 1;
static uint64_t s_last_auto;

/* 往指定 y 居中打一行字 */
static void put_center(u16 y, const char *s, u16 fc, u16 bc, u8 size)
{
    int w = (int)strlen(s) * (size / 2);
    int x = (SCR_W - w) / 2;
    if (x < 0) {
        x = 0;
    }
    LCD_ShowString((u16)x, y, (const u8 *)s, fc, bc, size, 0);
}

static void put_at(u16 x, u16 y, const char *s, u16 fc, u16 bc, u8 size)
{
    LCD_ShowString(x, y, (const u8 *)s, fc, bc, size, 0);
}

/* ---------------- 只画变化的部分 ----------------
 *
 * 主循环每 100ms 走一圈，但传感器值一秒才变几次。以前每圈把整页文字全部重画，
 * 在软 SPI 上等于把带宽全浪费在没变的字上，还会闪。
 * 这里按「行槽位」记住上一次画的字符串，内容一样就直接跳过。
 * 满屏重绘（换页）时调 slot_reset() 让缓存失效。
 */
#define SLOT_NUM    8
static char s_slot_text[SLOT_NUM][40];

static void slot_reset(void)
{
    int i;

    for (i = 0; i < SLOT_NUM; i++) {
        s_slot_text[i][0] = '\0';
    }
}

static int slot_put(int slot, u16 y, const char *s, u16 fc, u16 bc, u8 size)
{
    if (slot < 0 || slot >= SLOT_NUM) {
        return 0;
    }
    if (strncmp(s_slot_text[slot], s, sizeof(s_slot_text[0]) - 1) == 0) {
        return 0;                       /* 内容没变，不占 SPI 带宽 */
    }
    strncpy(s_slot_text[slot], s, sizeof(s_slot_text[0]) - 1);
    s_slot_text[slot][sizeof(s_slot_text[0]) - 1] = '\0';
    put_center(y, s, fc, bc, size);
    return 1;
}

/* 底部三个小方块指示当前页 */
static void draw_dots(void)
{
    int i;
    int x0 = SCR_W / 2 - 22;

    for (i = 0; i < WATCH_PAGE_NUM; i++) {
        u16 x = (u16)(x0 + i * 18);
        LCD_Fill(x, 214, x + 8, 222, (i == s_page) ? C_DOT_ON : C_DOT_OFF);
    }
}

/* ---------------- 第 0 页：综合表盘 ----------------
 * 一页把 时间 / 心率 / 血氧 / 步数 / 三轴加速度 全摆上，
 * 不用再靠翻腕切页才能看到传感器数据。
 */
static int isqrt(int v);

static void fmt_mg(char *buf, int mg)
{
    int neg = (mg < 0);
    int v = neg ? -mg : mg;

    if (v > 9999) {
        v = 9999;
    }
    sprintf(buf, "%c%d.%02d", neg ? '-' : '+', v / 1000, (v % 1000) / 10);
}

static void page_clock_static(void)
{
    put_center(30, "WATCH", C_TITLE, C_BG, 16);
    LCD_DrawLine(56, 52, 184, 52, GRAYBLUE);
    draw_dots();
}

static void page_clock_live(void)
{
    char buf[40];
    char hx[12];
    char hy[12];
    char hz[12];
    int t = g_watch.been_on;

    /* 时间 */
    sprintf(buf, "%02d:%02d:%02d", (t / 3600) % 100, (t / 60) % 60, t % 60);
    slot_put(0, 64, buf, C_TIME, C_BG, 32);

    /* 心率 + 血氧 挤一行 */
    if (g_watch.hr > 0) {
        sprintf(buf, "HR %3d", g_watch.hr);
    } else {
        sprintf(buf, "HR  --");
    }
    if (g_watch.spo2 > 0) {
        char sp[16];

        sprintf(sp, "  SpO2 %3d%%", g_watch.spo2);
        strcat(buf, sp);
    } else {
        strcat(buf, "  SpO2 --%");
    }
    slot_put(1, 112, buf, C_HR, C_BG, 16);

    /* 步数 */
    sprintf(buf, "STEPS %4d", g_watch.steps);
    slot_put(2, 136, buf, C_STEP, C_BG, 16);

    /* 三轴加速度 + 合加速度 */
    if (g_watch.acc_ok) {
        int mag;

        fmt_mg(hx, g_watch.ax);
        fmt_mg(hy, g_watch.ay);
        fmt_mg(hz, g_watch.az);
        sprintf(buf, "X%s Y%s Z%s", hx, hy, hz);
        slot_put(3, 162, buf, C_TEXT, C_BG, 16);

        mag = isqrt(g_watch.ax * g_watch.ax + g_watch.ay * g_watch.ay + g_watch.az * g_watch.az);
        if (mag > 9999) {
            mag = 9999;
        }
        sprintf(buf, "|a| %d.%02dg", mag / 1000, (mag % 1000) / 10);
        slot_put(4, 186, buf, C_SPO2, C_BG, 16);
    } else {
        slot_put(3, 162, "MPU6050 OFFLINE", C_HR, C_BG, 16);
        slot_put(4, 186, "                ", C_BG, C_BG, 16);
    }
}

/* ---------------- 第 1 页：心率血氧 ---------------- */
#define BAR_NUM 24
static int s_bars[BAR_NUM];

static void page_heart_static(void)
{
    put_center(34, "HEART", C_TITLE, C_BG, 16);
    LCD_DrawLine(60, 56, 180, 56, GRAYBLUE);
    put_center(126, "BPM", C_TITLE, C_BG, 16);
    put_center(184, "SpO2   ---%", C_TITLE, C_BG, 16);
    draw_dots();
}

static void page_heart_live(void)
{
    char buf[24];
    int i;

    if (g_watch.hr > 0) {
        sprintf(buf, "%3d", g_watch.hr);
        put_center(72, buf, C_HR, C_BG, 32);
    } else {
        put_center(72, "---", C_HR, C_BG, 32);
    }

    if (g_watch.spo2 > 0) {
        sprintf(buf, "SpO2    %3d%%", g_watch.spo2);
    } else {
        sprintf(buf, "SpO2    ---%%");
    }
    put_center(184, buf, C_SPO2, C_BG, 16);

    /* 脉搏柱：把最近 N 次 IR 幅度画成小竖条 */
    for (i = 0; i < BAR_NUM - 1; i++) {
        s_bars[i] = s_bars[i + 1];
    }
    s_bars[BAR_NUM - 1] = g_watch.ir_pct;

    for (i = 0; i < BAR_NUM; i++) {
        int h = s_bars[i] * 22 / 100;
        u16 x = (u16)(SCR_W / 2 - BAR_NUM * 2 + i * 4);
        LCD_Fill(x, 154, x + 2, 154 + 22, C_BG);          /* 先清 */
        if (h > 0) {
            LCD_Fill(x, (u16)(176 - h), x + 2, 176, C_HR);
        }
    }
}

/* ---------------- 第 2 页：运动 ---------------- */
static void page_motion_static(void)
{
    put_center(34, "MOTION", C_TITLE, C_BG, 16);
    LCD_DrawLine(60, 56, 180, 56, GRAYBLUE);
    draw_dots();
}

static void fmt_g(char *buf, const char *axis, int mg)
{
    int neg = (mg < 0);
    int v = neg ? -mg : mg;

    if (v > 9999) {
        v = 9999;
    }
    sprintf(buf, "%s %c%d.%02dg", axis, neg ? '-' : '+', v / 1000, (v % 1000) / 10);
}

/* 整数开方（牛顿迭代），算合加速度用
 * 注意别用固定迭代次数：从 x=v 出发要收敛到 √v 需要 ~log2(v)/2 次，
 * 只跑 8 次在 v≈1e6 时还停在 4000 附近，会把 1.00g 算成 4.05g。 */
static int isqrt(int v)
{
    int x;
    int y;

    if (v <= 0) {
        return 0;
    }
    x = v;
    while (1) {
        y = (x + v / x) / 2;
        if (y >= x) {
            break;
        }
        x = y;
    }
    return x;
}

static void page_motion_live(void)
{
    char buf[24];
    int m2 = g_watch.ax * g_watch.ax + g_watch.ay * g_watch.ay + g_watch.az * g_watch.az;

    if (g_watch.acc_ok) {
        fmt_g(buf, "X", g_watch.ax);
        put_at(62, 74, buf, C_TIME, C_BG, 16);
        fmt_g(buf, "Y", g_watch.ay);
        put_at(62, 104, buf, C_TIME, C_BG, 16);
        fmt_g(buf, "Z", g_watch.az);
        put_at(62, 134, buf, C_TIME, C_BG, 16);
    } else {
        put_at(62, 74, "MPU6050 OFFLINE", C_HR, C_BG, 16);
        put_at(62, 104, "                ", C_BG, C_BG, 16);
        put_at(62, 134, "                ", C_BG, C_BG, 16);
    }

    /* 合加速度：静止平放时约 1.00g */
    {
        int mag = isqrt(m2);          /* 单位 mg */
        if (mag > 9999) {
            mag = 9999;
        }
        sprintf(buf, "|a| %d.%02dg", mag / 1000, (mag % 1000) / 10);
        put_at(62, 176, buf, C_TEXT, C_BG, 16);
    }

    sprintf(buf, "STEPS %4d", g_watch.steps);
    put_center(200, buf, C_STEP, C_BG, 16);
}

/* ---------------- 时钟脚扫描 + 点亮自检 ----------------
 *
 * 背景：底板 J3 座上没有 SPI_CK（IO7），必须飞一根线到屏的 SCL。
 * 飞线接错一个脚很常见（丝印密、照片看不清），屏就永远全黑、背光常亮。
 *
 * 所以这里不赌某一个脚：把时钟依次挪到 6 个候选脚上，每个脚刷一块
 * 不同颜色的方块。只要线落在其中任意一个脚上，屏就会亮一次，
 * 串口同时打印当前是哪个脚 —— 亮的是什么颜色，就等于告诉我们是哪个脚。
 *
 *   顺序：IO7=RED  IO13=GREEN  IO14=BLUE  IO6=WHITE  IO8=YELLOW  IO3=CYAN
 *
 * 扫描完再把时钟放回 IO7，接一遍五色满屏，然后进表盘。
 */
#if LCD_CLK_SWEEP
static const int kClkCand[] = { 7, 13, 14, 6, 8, 3 };
static const u16 kClkColor[] = { RED, GREEN, BLUE, WHITE, YELLOW, CYAN };
static const char *kClkName[] = { "RED", "GREEN", "BLUE", "WHITE", "YELLOW", "CYAN" };

#define CLK_CAND_NUM  ((int)(sizeof(kClkCand) / sizeof(kClkCand[0])))

static void lcd_sweep_clk(void)
{
    int i;

    printf("\r\n[watch] ===== CLK pin sweep =====\r\n");
    printf("[watch] order:");
    for (i = 0; i < CLK_CAND_NUM; i++) {
        printf(" IO%d=%s", kClkCand[i], kClkName[i]);
    }
    printf("\r\n");

    for (i = 0; i < CLK_CAND_NUM; i++) {
        uint64_t t0 = uapi_tcxo_get_ms();

        printf("[watch] >>> CLK=IO%d  expect %s block\r\n", kClkCand[i], kClkName[i]);
        LCD_SetClkPin(kClkCand[i]);
        LCD_Init();
        LCD_Fill(60, 60, 179, 179, kClkColor[i]);
        printf("[watch] <<< CLK=IO%d done %ums\r\n", kClkCand[i],
               (unsigned)(uapi_tcxo_get_ms() - t0));
        uapi_tcxo_delay_ms(1800);
    }

    LCD_SetClkPin(LCD_SCL_PIN);
    LCD_Fill(0, 0, SCR_W - 1, SCR_W - 1, BLACK);
    printf("[watch] sweep done, CLK back to IO%d\r\n", LCD_SCL_PIN);
}
#endif

#if !LCD_RETRY_TEST
static void lcd_bringup_selftest(void)
{
    /* 白屏放第一位并且停最久：背光再暗，满屏白也最容易看出来，
     * 而且能和「只是背光在亮」一眼区分开——背光是均匀的暗蓝，
     * 白屏是整块亮白。
     *
     * 顺序：白 2.5s → 红 → 绿 → 蓝 → 黑，每色 0.8s。 */
    static const u16 colors[] = { WHITE, RED, GREEN, BLUE, BLACK };
    static const char *names[] = { "WHITE", "RED", "GREEN", "BLUE", "BLACK" };
    static const int dwell[] = { 2500, 800, 800, 800, 500 };
    uint64_t t0;
    int i;

#if LCD_CLK_SWEEP
    lcd_sweep_clk();
#endif

    printf("\r\n[watch] ===== LCD bring-up selftest =====\r\n");
    printf("[watch] 期望画面: 白2.5s -> 红 -> 绿 -> 蓝 -> 黑, 然后出现 LCD OK\r\n");
    printf("[watch] 只看到均匀暗蓝背光、没有整屏颜色 = SPI 没通, 查接线\r\n");

    /* 不再单独调 LCD_GPIO_Init + 引脚回读：LCD_Init 内部已经配好 5 个脚，
     * 回读那步实测会在 IO11 处把 UI 任务搞挂（见 lcd_init.h 的 LCD_PIN_PROBE 注释），
     * 把自检本身都挡住了，得不偿失。 */
    printf("[watch] step1: LCD_Init\r\n");
    t0 = uapi_tcxo_get_ms();
    LCD_Init();
    printf("[watch] step2: LCD_Init done in %ums\r\n", (unsigned)(uapi_tcxo_get_ms() - t0));

    for (i = 0; i < (int)(sizeof(colors) / sizeof(colors[0])); i++) {
        t0 = uapi_tcxo_get_ms();
        LCD_Fill(0, 0, SCR_W - 1, SCR_W - 1, colors[i]);
        printf("[watch] fill %-5s %ums\r\n", names[i], (unsigned)(uapi_tcxo_get_ms() - t0));
        uapi_tcxo_delay_ms(dwell[i]);
    }

    /* 收尾：一圈黑底 + 内容，便于确认圆屏四个角确实是切掉的 */
    LCD_Fill(0, 0, SCR_W - 1, SCR_W - 1, C_BG);
    put_center(96, "LCD OK", C_TIME, C_BG, 24);
    put_center(140, "240x240 GC9A01", C_TITLE, C_BG, 16);
    uapi_tcxo_delay_ms(800);

    printf("[watch] lcd selftest done\r\n");
}
#endif  /* !LCD_RETRY_TEST */

/* ---------------- 点亮诊断：分批刷带 ---------------- */

#if 0   /* 方向判别用过了：结论是屏没插反、5V0 没到 1 脚。换屏后不再需要，
         * 保留代码备查。需要复现时把 0 改 1 并在 lcd_retry_probe 里调用。 */

/* 插接方向 / 供电判别。
 *
 * 原理图：LCD1 的 1 脚 = VCC(5V0)，7 脚 = RST(J1-6 → IO12)，6 脚 = CS(J1-7 → IO11)。
 * 若屏插反 180°，则屏的 VCC 落到 IO12、屏的 GND 落到 IO11，
 * 背光 LED 就被接在 IO12 与 IO11 之间。
 *
 * 于是：
 *   IO12=1 / IO11=0  → LED 正向导通 → 亮
 *   IO12=0 / IO11=0  → LED 无电流   → 灭
 * 两个相位各停 2 秒、交替 4 轮：
 *   看到「亮2秒 / 灭2秒」规律闪烁  → 插反了（需要把屏转 180°）
 *   一直亮着不变                    → 插接正确、5V0 也在，问题在数据线
 *   一直全黑                        → 5V0 这条电源没送到 1 脚
 */
static void lcd_orient_probe(void)
{
    int i;

    printf("\r\n[or] ===== 插接方向 / 供电判别 =====\r\n");
    printf("[or] A: IO12=1 IO11=0 (2s) -> B: 两脚都=0 (2s), x4\r\n");
    printf("[or] 规律闪 = 插反; 常亮 = 供电与方向都对; 全黑 = 5V0 没到 1 脚\r\n");

    uapi_pin_set_mode(LCD_RST_PIN, 0);
    uapi_gpio_set_dir(LCD_RST_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_pin_set_mode(LCD_CS_PIN, 0);
    uapi_gpio_set_dir(LCD_CS_PIN, GPIO_DIRECTION_OUTPUT);

    for (i = 0; i < 4; i++) {
        uapi_gpio_set_val(LCD_RST_PIN, 1);
        uapi_gpio_set_val(LCD_CS_PIN, 0);
        printf("[or] #%d A\r\n", i);
        uapi_tcxo_delay_ms(2000);
        uapi_watchdog_kick();

        uapi_gpio_set_val(LCD_RST_PIN, 0);
        uapi_gpio_set_val(LCD_CS_PIN, 0);   /* B 相两脚都拉低：不给屏任何反压，只是关灯 */
        printf("[or] #%d B\r\n", i);
        uapi_tcxo_delay_ms(2000);
        uapi_watchdog_kick();
    }

    printf("[or] orient probe done\r\n");
}
#endif  /* 0 */

/* 最小初始化序列：只发 ST77xx/GC9A01 两边都认的标准命令。
 * 用来排除「GC9A01 私有命令把控制器搞进异常状态」这种可能。 */
static void __attribute__((unused)) lcd_init_minimal(void)
{
    LCD_GPIO_Init();

    LCD_RES_Clr();
    uapi_tcxo_delay_ms(120);
    LCD_RES_Set();
    uapi_tcxo_delay_ms(150);

    LCD_WR_REG(0x01);                 /* SWRESET */
    uapi_tcxo_delay_ms(120);
    LCD_WR_REG(0x11);                 /* SLPOUT  */
    uapi_tcxo_delay_ms(120);
    LCD_WR_REG(0x3A); LCD_WR_DATA8(0x05);   /* COLMOD 16bit */
    LCD_WR_REG(0x36); LCD_WR_DATA8(0x08);   /* MADCTL */
    LCD_WR_REG(0x21);                 /* INVON  */
    LCD_WR_REG(0x13);                 /* NORON  */
    LCD_WR_REG(0x29);                 /* DISPON */
    uapi_tcxo_delay_ms(60);
}

/* 满屏横条：把屏幕切成 n 条，红绿交替。
 *
 * 为什么用横条数量而不是颜色：这块屏开了 INVON，红可能显示成青、绿成品红，
 * 靠颜色判断容易认错。横条的条数不受反色影响，数几条就是几条，最可靠。 */
static void fill_stripes(int n)
{
    int i;

    for (i = 0; i < n; i++) {
        u16 y0 = (u16)((i * SCR_W) / n);
        u16 y1 = (u16)(((i + 1) * SCR_W) / n - 1);

        LCD_Fill(0, y0, SCR_W - 1, y1, (i & 1) ? GREEN : RED);
    }
}

/* 点亮排查：把「时钟落在哪个脚」和「信号怎么排」一起扫，用横条数量报结果。
 *
 * 依据：屏的 5 根信号里，RST/CS/DC/MOSI 都走扩展板 J1（已确认通电、I2C 也通），
 * 只有时钟 SPI_CK 单独走 J2 那一排，网络表里这条网络只挂了 LCD1-3 和 J2-8 两个脚。
 * 屏幕上数到几条横条，就对应下面哪一行：
 *
 *   1 条 → clk=IO7  mosi=IO9  dc=IO10 cs=IO11   （官方例程分配）
 *   2 条 → clk=IO6  mosi=IO9  dc=IO10 cs=IO11   （时钟错位到 IO6）
 *   3 条 → clk=IO8  mosi=IO9  dc=IO10 cs=IO11   （时钟错位到 IO8）
 *   4 条 → clk=IO7  mosi=IO9  dc=IO11 cs=IO10   （DC/CS 互换）
 *   5 条 → clk=IO9  mosi=IO7  dc=IO11 cs=IO10   （时钟与数据互换）
 *
 * 一条都数不到（还是均匀灰）＝时钟根本没通到任何可用 GPIO，只能从硬件上解决。 */
static void __attribute__((unused)) lcd_stripe_sweep(void)
{
    /* { clk, mosi, dc, cs } */
    static const int cfg[][4] = {
        { 7, 9, 10, 11 },
        { 6, 9, 10, 11 },
        { 8, 9, 10, 11 },
        { 7, 9, 11, 10 },
        { 9, 7, 11, 10 },
    };
    const int cases = (int)(sizeof(cfg) / sizeof(cfg[0]));
    int round = 0;

    printf("\r\n[sw] ===== pin drive check (CS first) =====\r\n");
    uapi_tcxo_delay_ms(300);
    LCD_PinProbe();
    printf("[sw] ===== pin drive check done =====\r\n");

    printf("[sw] ===== stripe sweep: 横条数 = 方案号 =====\r\n");

    for (;;) {
        int idx = round % cases;
        uint64_t t0;

        LCD_SetPins(cfg[idx][0], cfg[idx][1], cfg[idx][2], cfg[idx][3]);
        t0 = uapi_tcxo_get_ms();
        LCD_Init();
        fill_stripes(idx + 1);
        printf("[sw] #%02d clk=%d mosi=%d dc=%d cs=%d -> %d stripes %ums\r\n",
               round, cfg[idx][0], cfg[idx][1], cfg[idx][2], cfg[idx][3], idx + 1,
               (unsigned)(uapi_tcxo_get_ms() - t0));

        uapi_watchdog_kick();
        uapi_tcxo_delay_ms(2500);
        round++;
    }
}

/* ---------------- 任务 ---------------- */

static void page_render(int page)
{
    LCD_Fill(0, 0, SCR_W - 1, SCR_W - 1, C_BG);
    slot_reset();              /* 整屏已清空，缓存作废，接下来全部重画一次 */
    switch (page) {
        case 0:
            page_clock_static();
            page_clock_live();
            break;
        case 1:
            page_heart_static();
            page_heart_live();
            break;
        default:
            page_motion_static();
            page_motion_live();
            break;
    }
    s_drawn = page;
}

static void page_live(int page)
{
    switch (page) {
        case 0:
            page_clock_live();
            break;
        case 1:
            page_heart_live();
            break;
        default:
            page_motion_live();
            break;
    }
}

void watch_ui_task(void)
{
    uint64_t next_tick;
    uint64_t last_dash = 0;

    printf("\r\n[watch] ui task start\r\n");

    /* ---- 上电：横条扫描 ----
     * 屏插在板载 LCD1 座里，信号全部走板内，不需要接线。
     * 这里把「时钟脚 × 信号排列」几种组合轮流试，屏幕上数到几条横条
     * 就对应第几种组合。详见 lcd_stripe_sweep() 注释。 */
#if LCD_RETRY_TEST
    lcd_stripe_sweep();     /* 诊断模式：永不返回 */
#else
    lcd_bringup_selftest();
#endif

    s_page = 0;
    s_last_auto = uapi_tcxo_get_ms();
    page_render(s_page);

    next_tick = uapi_tcxo_get_ms();

    while (1) {
        uint64_t now;

        uapi_watchdog_kick();

        now = uapi_tcxo_get_ms();

        /* 每秒把表盘上的数据同步打一份到串口。
         * 屏没点亮时这就是唯一能看到数值的窗口；屏通了也能对照。 */
        if (now - last_dash >= 1000) {
            int mag = isqrt(g_watch.ax * g_watch.ax + g_watch.ay * g_watch.ay +
                            g_watch.az * g_watch.az);

            last_dash = now;
            printf("[dash] %02d:%02d:%02d | HR %3d | SpO2 %3d%% | STEP %4d |"
                   " ACC %+5d %+5d %+5d mg |a|=%d.%02dg | acc=%d hr=%d\r\n",
                   (g_watch.been_on / 3600) % 100, (g_watch.been_on / 60) % 60,
                   g_watch.been_on % 60, g_watch.hr, g_watch.spo2, g_watch.steps,
                   g_watch.ax, g_watch.ay, g_watch.az, mag / 1000, (mag % 1000) / 10,
                   g_watch.acc_ok, g_watch.hr_ok);
        }

        /* 翻手腕切页 */
        if (g_watch.acc_ok) {
            if (g_watch.az < -600 && s_flip_armed) {
                s_flip_armed = 0;
                s_page = (s_page + 1) % WATCH_PAGE_NUM;
                s_last_auto = now;
            } else if (g_watch.az > 0) {
                s_flip_armed = 1;
            }
        } else if (now - s_last_auto > 6000) {
            /* 没有加速度计就自动轮播 */
            s_last_auto = now;
            s_page = (s_page + 1) % WATCH_PAGE_NUM;
        }

        if (s_page != s_drawn) {
            page_render(s_page);
        } else {
            page_live(s_page);
        }

        next_tick += UI_TICK_MS;
        {
            int64_t wait = (int64_t)next_tick - (int64_t)uapi_tcxo_get_ms();
            if (wait > 0) {
                osal_msleep((uint32_t)wait);
            } else if (wait < -500) {
                next_tick = uapi_tcxo_get_ms();
            }
        }
    }
}
