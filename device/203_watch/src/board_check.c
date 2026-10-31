/*
 * 整板自检（BOARD CHECK）
 *
 * 目的：不再靠「看一眼屏亮不亮」来猜，而是让板子自己把电气状态报上来。
 *
 * 能查出来的东西：
 *   - 某个 LCD 信号脚是不是被短路到 GND / 3V3（上拉下拉回读不一致）
 *   - I2C 上到底挂了哪些器件（确认 3010x / MPU6050 在不在）
 *   - 屏到底有没有在跟 MCU 说话（RDDID 读寄存器：屏会驱动 SDA 回数据）
 *   - 屏的 SCL 究竟落在哪个 GPIO 上（逐个脚试，唯一能读回 ID 的那个就是）
 *
 * 不能查出来的：断线。手指按不到、万用表量不到的地方，只能靠第 4 段间接判断。
 */
#include "board_check.h"

#include "lcd_init.h"
#include "lcd.h"

#include "gpio.h"
#include "pinctrl.h"
#include "tcxo.h"
#include "watchdog.h"
#include "soc_osal.h"

#include <stdio.h>

int g_bc_lcd_clk = -1;

/* ---------------- 公共小工具 ---------------- */

static void gpio_out(int pin, int val)
{
    uapi_pin_set_mode(pin, 0);
    uapi_gpio_set_val(pin, (gpio_level_t)val);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
}

/* ---------------- 1. 引脚占用清单 ---------------- */

static void bc_pinmap(void)
{
    printf("[bc] ---- 1. 引脚占用 ----\r\n");
    printf("[bc] IO0 ~ IO5 : QSPI0 主 Flash 取指总线，写进去必硬 fault，禁止当 GPIO\r\n");
    printf("[bc] IO7=CLK IO9=MOSI IO10=DC IO11=CS IO12=RST   LCD 软件 SPI\r\n");
    printf("[bc] IO15=SCL IO16=SDA                          I2C1 (mode2)\r\n");
    printf("[bc] IO6 IO8 IO13 IO14                          扩展排针空闲脚\r\n");
}

/* ---------------- 2. 引脚负载检测 ----------------
 *
 * 一个脚先上拉读一次、再下拉读一次：
 *   1/0  -> 两种状态都跟得上，说明这根线是高阻（悬空，或只接了一个 CMOS 输入）
 *   0/0  -> 外部有东西把它死死拉低（对地短路 / 被别的输出脚拉低）
 *   1/1  -> 外部有东西把它死死拉高（对电源短路 / 被别的输出脚拉高）
 * 屏的 SCL/SDA/DC/CS/RST 都是 CMOS 输入，正常应该读到 1/0。
 */
static void bc_pin_load(void)
{
    static const struct {
        const char *name;
        int pin;
        int restore;
    } tab[] = {
        { "CLK ", 7, 1 }, { "MOSI", 9, 1 }, { "DC  ", 10, 1 }, { "CS  ", 11, 1 }, { "RST ", 12, 1 },
        { "IO6 ", 6, 0 }, { "IO8 ", 8, 0 }, { "IO13", 13, 0 }, { "IO14", 14, 0 },
    };
    int i;

    printf("[bc] ---- 2. 引脚负载（上拉/下拉）----\r\n");

    for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++) {
        int up, dn;
        const char *verdict;

        uapi_pin_set_mode(tab[i].pin, 0);
        uapi_pin_set_pull(tab[i].pin, PIN_PULL_TYPE_UP);
        uapi_gpio_set_dir(tab[i].pin, GPIO_DIRECTION_INPUT);
        uapi_tcxo_delay_ms(2);
        up = (int)uapi_gpio_get_val(tab[i].pin);

        uapi_pin_set_pull(tab[i].pin, PIN_PULL_TYPE_DOWN);
        uapi_tcxo_delay_ms(2);
        dn = (int)uapi_gpio_get_val(tab[i].pin);

        if (up == 1 && dn == 0) {
            verdict = "高阻(没被短路)";
        } else if (up == 0 && dn == 0) {
            verdict = "被外部拉低";
        } else if (up == 1 && dn == 1) {
            verdict = "被外部拉高";
        } else {
            verdict = "抖动";
        }
        printf("[bc] IO%-2d %s up=%d dn=%d  %s\r\n", tab[i].pin, tab[i].name, up, dn, verdict);

        uapi_pin_set_pull(tab[i].pin, PIN_PULL_TYPE_DISABLE);

        /* 只把 LCD 那 5 根恢复成输出高。被外部拉低的脚（比如 IO6 接的是
         * MAX30100 的 ADO，硬件上直接接地）绝对不能输出高，否则就是把
         * 3V3 往地上怼。 */
        if (tab[i].restore) {
            gpio_out(tab[i].pin, 1);
        } else {
            uapi_gpio_set_dir(tab[i].pin, GPIO_DIRECTION_INPUT);
        }
    }
}

/* ---- 三态 + 驱动能力 ----
 *
 * 之前那版「线上电容」测法是错的：它先把脚推高再用内部下拉放电，而内部下拉
 * 只有几十 kΩ，一根十几 pF 的空脚和一根接了几百 pF 的线，都在第一个读周期就
 * 归零，读数当然全是 0——测不出任何东西，还误导了结论。
 *
 * 改成三态判定：给足 20ms 让上下拉把线拉到稳态，再看读到什么。
 *   1/0 -> 高阻（正常的 CMOS 输入脚，或空脚）
 *   1/1 -> 外部有上拉电阻
 *   0/0 -> 外部被拉低
 * 再单独测一次「驱动回读」：输出高读回、输出低读回，验证这个脚真的能推电平。
 * 之前只验证过 IO11，另外四根没验证过——万一某根脚的输出通路没打开，
 * 时钟根本发不出去，屏就是全灰、读 ID 全 FF，和现在的现象完全一致。
 */
static void bc_pin_state(void)
{
    static const struct {
        const char *name;
        int pin;
        int restore;
    } tab[] = {
        { "CLK ", 7, 1 }, { "MOSI", 9, 1 }, { "DC  ", 10, 1 }, { "CS  ", 11, 1 }, { "RST ", 12, 1 },
        { "IO8 ", 8, 0 }, { "IO13", 13, 0 }, { "IO14", 14, 0 },
    };
    int i;

    printf("[bc] ---- 2b. 三态 + 驱动回读（每态稳定 20ms）----\r\n");
    printf("[bc] 1/0=高阻(正常)  1/1=外部上拉  0/0=外部拉低  drv=10 才算能推电平\r\n");

    for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++) {
        int pin = tab[i].pin;
        int up, dn, drv_h, drv_l;
        const char *verdict;

        /* 顺序很重要：先定方向再定上下拉，反过来读到的往往是上一次的状态 */
        uapi_pin_set_mode(pin, 0);
        uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
        uapi_pin_set_pull(pin, PIN_PULL_TYPE_UP);
        uapi_tcxo_delay_ms(20);
        up = (int)uapi_gpio_get_val(pin);

        uapi_pin_set_pull(pin, PIN_PULL_TYPE_DOWN);
        uapi_tcxo_delay_ms(20);
        dn = (int)uapi_gpio_get_val(pin);

        /* 驱动回读：输出态下读输入采样值，能看到自己推出去的电平才算通 */
        uapi_pin_set_pull(pin, PIN_PULL_TYPE_DISABLE);
        uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
        uapi_gpio_set_val(pin, 1);
        uapi_tcxo_delay_ms(1);
        drv_h = (int)uapi_gpio_get_val(pin);
        uapi_gpio_set_val(pin, 0);
        uapi_tcxo_delay_ms(1);
        drv_l = (int)uapi_gpio_get_val(pin);

        if (up == 1 && dn == 0) {
            verdict = "高阻";
        } else if (up == 1 && dn == 1) {
            verdict = "外部上拉";
        } else if (up == 0 && dn == 0) {
            verdict = "外部拉低";
        } else {
            verdict = "抖动";
        }
        printf("[bc] IO%-2d %s up=%d dn=%d drv=%d%d %s%s\r\n", pin, tab[i].name, up, dn,
               drv_h, drv_l, verdict,
               (drv_h == 1 && drv_l == 0) ? "" : "  <== 这脚推不动电平");

        uapi_pin_set_pull(pin, PIN_PULL_TYPE_DISABLE);
        if (tab[i].restore) {
            gpio_out(pin, 1);
        } else {
            uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
        }
    }
}

/* ---------------- 3. I2C 扫描（软件位翻转）----------------
 *
 * 不用驱动里的 I2C：自检跑在传感器任务之前，先把总线初始化占了，
 * 后面传感器任务再 init 就会失败。这里直接两根脚当开漏用（板上有 4.7K 上拉），
 * 扫完把脚放成高阻，不留下任何副作用。
 */
#define BB_SCL  15
#define BB_SDA  16

static void bb_dly(void)
{
    volatile int i;

    /* 之前只写 60 次，合不到 1us，等效 I2C 时钟跑到 1MHz 以上，
     * 从机根本来不及应答，扫出来是「0 个器件」的假阴性。
     * 240 次 ≈ 5us 半周期 ≈ 100kHz，跟正式驱动一个量级。 */
    for (i = 0; i < 240; i++) {
        __asm__ volatile ("nop");
    }
}

static void bb_low(int pin)
{
    uapi_gpio_set_val(pin, 0);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
    bb_dly();
}

static void bb_rel(int pin)
{
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
    bb_dly();
}

static void bb_start(void)
{
    bb_rel(BB_SDA);
    bb_rel(BB_SCL);
    bb_low(BB_SDA);
    bb_low(BB_SCL);
}

static void bb_stop(void)
{
    bb_low(BB_SDA);
    bb_rel(BB_SCL);
    bb_rel(BB_SDA);
}

/* 返回 1 = 从机给 ACK */
static int bb_tx(u8 dat)
{
    int i;
    int ack;

    for (i = 0; i < 8; i++) {
        if (dat & 0x80) {
            bb_rel(BB_SDA);
        } else {
            bb_low(BB_SDA);
        }
        bb_rel(BB_SCL);
        bb_low(BB_SCL);
        dat <<= 1;
    }
    bb_rel(BB_SDA);
    bb_rel(BB_SCL);
    ack = (uapi_gpio_get_val(BB_SDA) == 0) ? 1 : 0;
    bb_low(BB_SCL);

    return ack;
}

static void bc_i2c_scan(void)
{
    int addr;
    int found = 0;

    printf("[bc] ---- 3. I2C 扫描 SCL=IO15 SDA=IO16 ----\r\n");

    uapi_pin_set_mode(BB_SCL, 0);
    uapi_pin_set_mode(BB_SDA, 0);
    uapi_pin_set_pull(BB_SCL, PIN_PULL_TYPE_UP);
    uapi_pin_set_pull(BB_SDA, PIN_PULL_TYPE_UP);
    bb_rel(BB_SCL);
    bb_rel(BB_SDA);
    uapi_tcxo_delay_ms(2);
    printf("[bc] 空闲电平 SCL=%d SDA=%d (都要是 1)\r\n",
           (int)uapi_gpio_get_val(BB_SCL), (int)uapi_gpio_get_val(BB_SDA));

    for (addr = 0x08; addr <= 0x77; addr++) {
        int ack;

        bb_start();
        ack = bb_tx((u8)(addr << 1));
        bb_stop();

        if (ack) {
            found++;
            if (addr == 0x57) {
                printf("[bc] ACK 0x%02X  MAX3010x 血氧/心率\r\n", addr);
            } else if (addr == 0x68) {
                printf("[bc] ACK 0x%02X  MPU6050 六轴\r\n", addr);
            } else {
                printf("[bc] ACK 0x%02X  (未知器件)\r\n", addr);
            }
        }
    }
    printf("[bc] I2C 共发现 %d 个器件\r\n", found);

    /* 收尾：放成高阻，交给后面的 I2C 控制器 */
    uapi_gpio_set_dir(BB_SCL, GPIO_DIRECTION_INPUT);
    uapi_gpio_set_dir(BB_SDA, GPIO_DIRECTION_INPUT);
}

/* ---- LCD 总线诊断：短路图 + RDDID 全组合扫描 ----
 *
 * 只做两件「不猜」的事：
 *
 * A. 短路图：9 个候选脚两两配对，先把 j 预充到 0，再驱动 i 为 1，读 j。
 *    j 是纯高阻输入，浮空会保持原电平，所以只有真的连在一起 j 才会变 1。
 *    板子上如果有两根信号线被短接，SPI 永远不可能通——先把这个排除。
 *
 * B. RDDID 扫描：GC9A01 的 0x04 命令会让屏主动驱动 SDA 回 4 个字节。
 *    没人驱动时内部上拉把线拉满，读回全 FF；
 *    屏真的在回话，至少会有一个 0 位。
 *
 *    关键修正（v27）：只有「能浮高」的脚才允许当 SDA。
 *    被外部死死拉低的脚（实测 IO6/IO13/IO14）永远读 0，
 *    拿它当 SDA 会造出「全 00」这种假的应答——v26 就是被这个假警报骗停的。
 *
 * CS 和 RST 一律不驱动：模组自带 CS 下拉(10k)和 RST 上拉，
 * 悬空就等于「片选有效 + 复位已释放」，这样未知数从 5 个降到 3 个。
 */
static void id_dly(void)
{
    volatile int i;

    for (i = 0; i < 40; i++) {
        __asm__ volatile ("nop");
    }
}

#define IDP_N 9
static const int idp_pins[IDP_N] = { 6, 7, 8, 9, 10, 11, 12, 13, 14 };

/* 全部候选脚放成输入高阻，把控制权交回模组自己的上下拉 */
static void idp_release_all(void)
{
    int i;

    for (i = 0; i < IDP_N; i++) {
        uapi_pin_set_mode(idp_pins[i], 0);
        uapi_gpio_set_dir(idp_pins[i], GPIO_DIRECTION_INPUT);
        uapi_pin_set_pull(idp_pins[i], PIN_PULL_TYPE_DISABLE);
    }
}

static void idp_out(int pin, int v)
{
    uapi_pin_set_mode(pin, 0);
    uapi_gpio_set_val(pin, (gpio_level_t)v);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
}

static void idp_in(int pin)
{
    uapi_pin_set_mode(pin, 0);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull(pin, PIN_PULL_TYPE_DISABLE);
}

/* 输入 + 上拉，能读到 1 就说明这根线外部没有被拉死，可以当 SDA 用 */
static int idp_float_ok(int pin)
{
    int v;

    uapi_pin_set_mode(pin, 0);
    uapi_gpio_set_dir(pin, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull(pin, PIN_PULL_TYPE_UP);
    uapi_tcxo_delay_ms(2);
    v = (int)uapi_gpio_get_val(pin);
    uapi_pin_set_pull(pin, PIN_PULL_TYPE_DISABLE);

    return v;
}

/* A. 短路图 */
static void bc_short_map(void)
{
    int i;
    int j;
    int n = 0;

    printf("[bc] ---- 3b. 信号线短路图（驱动一根，读另一根）----\r\n");

    for (i = 0; i < IDP_N; i++) {
        for (j = 0; j < IDP_N; j++) {
            int v;

            if (i == j) {
                continue;
            }
            if (idp_pins[j] == 12) {
                continue;      /* IO12 有外部上拉，不能当被测端 */
            }

            idp_release_all();
            idp_out(idp_pins[j], 0);      /* 先预充 0 */
            idp_in(idp_pins[j]);

            idp_out(idp_pins[i], 1);      /* 驱动 i 为高 */
            uapi_tcxo_delay_ms(2);
            v = (int)uapi_gpio_get_val(idp_pins[j]);

            if (v == 1) {
                printf("[bc]     ! IO%-2d 与 IO%-2d 在外部直连\r\n", idp_pins[i], idp_pins[j]);
                n++;
            }
        }
    }

    if (n == 0) {
        printf("[bc]     9 个脚之间没有互相直连，没有短路\r\n");
    }
    idp_release_all();
}

/* B. 用 clk/mosi/dc 三根读一次 RDDID(0x04)，结果放 id[0..3] */
static void idp_read(int clk, int mosi, int dc, u8 *id)
{
    int b;
    int i;

    idp_out(clk, 1);
    idp_out(mosi, 1);
    idp_out(dc, 0);                    /* 命令态 */

    for (b = 0; b < 8; b++) {          /* 0x04 = 0000 0100，b=5 对应 bit2 */
        idp_out(clk, 0);
        idp_out(mosi, (b == 5) ? 1 : 0);
        id_dly();
        idp_out(clk, 1);
        id_dly();
    }

    uapi_gpio_set_val(dc, 1);          /* 数据态（读） */

    /* SDA 交给屏驱动：改成输入 + 上拉，没人回答时读回全 1 */
    uapi_gpio_set_dir(mosi, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull(mosi, PIN_PULL_TYPE_UP);
    uapi_tcxo_delay_ms(1);

    for (i = 0; i < 4; i++) {
        u8 v = 0;

        for (b = 0; b < 8; b++) {
            uapi_gpio_set_val(clk, 0);
            id_dly();
            v <<= 1;
            v |= (u8)(uapi_gpio_get_val(mosi) ? 1 : 0);
            uapi_gpio_set_val(clk, 1);
            id_dly();
        }
        id[i] = v;
    }

    uapi_pin_set_pull(mosi, PIN_PULL_TYPE_DISABLE);
}

static void bc_lcd(void)
{
    u8 id[4];
    int a;
    int b;
    int c;
    int i;
    int fok[IDP_N];
    int hit = 0;
    int combos = 0;
    int h_clk = 0;
    int h_mosi = 0;
    int h_dc = 0;

    printf("[bc] ---- 4. LCD RDDID 全组合扫描 ----\r\n");

    idp_release_all();

    /* 先给一次硬复位：IO12 实测有外部上拉，按原理图它就是 RST */
    idp_out(12, 0);
    uapi_tcxo_delay_ms(40);
    idp_in(12);
    uapi_tcxo_delay_ms(150);

    for (i = 0; i < IDP_N; i++) {
        fok[i] = idp_float_ok(idp_pins[i]);
        printf("[bc]     IO%-2d %s\r\n", idp_pins[i],
               fok[i] ? "能浮高  -> 可作 SDA" : "被拉死  -> 不能作 SDA");
    }

    for (a = 0; a < IDP_N && !hit; a++) {
        for (b = 0; b < IDP_N && !hit; b++) {
            if (b == a || !fok[b]) {
                continue;
            }
            for (c = 0; c < IDP_N && !hit; c++) {
                if (c == a || c == b) {
                    continue;
                }
                id[0] = 0xEE;
                id[1] = 0xEE;
                id[2] = 0xEE;
                id[3] = 0xEE;

                idp_read(idp_pins[a], idp_pins[b], idp_pins[c], id);
                combos++;

                if (id[0] != 0xFF || id[1] != 0xFF || id[2] != 0xFF || id[3] != 0xFF) {
                    printf("[bc] !!! 有应答 clk=IO%-2d mosi=IO%-2d dc=IO%-2d  id=%02X %02X %02X %02X\r\n",
                           idp_pins[a], idp_pins[b], idp_pins[c], id[0], id[1], id[2], id[3]);
                    hit = 1;
                    h_clk = idp_pins[a];
                    h_mosi = idp_pins[b];
                    h_dc = idp_pins[c];
                }
            }
            uapi_watchdog_kick();
        }
    }

    if (!hit) {
        printf("[bc] >>> 扫了 %d 组，屏一次都没有回话 <<<\r\n", combos);
        printf("[bc]     含义：屏的 SDA 从来没有被屏驱动过。\r\n");
        printf("[bc]     结合短路图，如果也没有短路，那就是「线没到」或「屏不工作」。\r\n");
        idp_release_all();
        return;
    }

    printf("[bc] >>> LCD 找到：clk=%d mosi=%d dc=%d <<<\r\n", h_clk, h_mosi, h_dc);
    g_bc_lcd_clk = h_clk;
    LCD_SetPins(h_clk, h_mosi, h_dc, LCD_CS_PIN);
}


/* ---------------- 5. 肉眼终审：写进去，看屏有没有变 ---------------- */

/* 为什么还要这一段：RDDID 是「读」，依赖屏把 SDA 反过来驱动。
 * 有些模组在信号线上串了 1k 电阻、或者 SDA 那一侧只做了单向电平转换，
 * 读回永远是 FF，这时候 RDDID 会给出假阴性。
 *
 * 所以最后不读、只写：按 9 种引脚组合各做一次完整初始化 + 整屏填红，
 * 每种在屏上停留 2 秒。只要屏在某一组变红，那一组就是对的。
 */
static const int vis_tab[][5] = {
    /* clk mosi  dc  cs  rst */
    {  7,   9,  10, 11, 12 },   /* 现在代码里用的 */
    {  8,   9,  10, 11, 12 },   /* 按底板网表推：SPI_CK 落在 IO8 */
    {  6,   9,  10, 11, 12 },   /* 另一种对位：SPI_CK 落在 IO6 */
    {  8,   7,  10, 11, 12 },
    {  7,   8,  10, 11, 12 },
    {  6,   8,  10, 11, 12 },
    {  9,   8,  10, 11, 12 },
    {  8,   9,  11, 10, 12 },   /* DC / CS 互换 */
    {  8,   9,  10, 12, 11 }    /* CS / RST 互换 */
};

static void bc_lcd_visual(void)
{
    int n = (int)(sizeof(vis_tab) / sizeof(vis_tab[0]));
    int i;

    printf("[bc] ---- 5. 肉眼终审：9 种组合各刷一次红屏（每组停 2 秒）----\r\n");
    printf("[bc]     请盯住屏：哪一组出现红色，就记住那个编号\r\n");

    for (i = 0; i < n; i++) {
        printf("[bc] [see] #%d  clk=IO%d mosi=IO%d dc=IO%d cs=IO%d rst=IO%d\r\n",
               i + 1, vis_tab[i][0], vis_tab[i][1], vis_tab[i][2], vis_tab[i][3], vis_tab[i][4]);

        LCD_SetPins(vis_tab[i][0], vis_tab[i][1], vis_tab[i][2], vis_tab[i][3]);
        LCD_SetRstPin(vis_tab[i][4]);
        LCD_Init();
        LCD_Fill(0, 0, 239, 239, 0xF800);      /* 纯红 */

        uapi_tcxo_delay_ms(2000);
        uapi_watchdog_kick();
    }

    /* 收尾回到代码默认值 */
    LCD_SetPins(LCD_SCL_PIN, LCD_SDA_PIN, LCD_DC_PIN, LCD_CS_PIN);
    LCD_SetRstPin(LCD_RST_PIN);
    LCD_Init();
    LCD_Fill(0, 0, 239, 239, 0x0000);
}

/* ---------------- 入口 ---------------- */

void board_check_run(void)
{
    printf("\r\n[bc] ============ BOARD CHECK ============\r\n");

    bc_pinmap();
    bc_pin_load();
    bc_pin_state();
    bc_i2c_scan();
    bc_short_map();
    bc_lcd();
    bc_lcd_visual();

    printf("[bc] ============ CHECK DONE ============\r\n\r\n");
}
