/*
 * hy_sensor.c —— 加速度（MPU6050）+ 心率血氧（MAX3010x）一次点亮
 *
 * 设计原则（都是上一轮排查换来的）：
 *   1. 任何结论都要有日志证据。所以先量线电平，再 init 总线，再探器件，
 *      每一步都把 errcode_t 的完整 32 位打出来——不要用 char 接返回值。
 *   2. 引脚组合不写死。WEAR V0 原理图 / 官方 201、202 例程给的是
 *      SCL=IO16、SDA=IO15（I2C1），但旧 watch_sensor.c 里写的是对调的，
 *      所以三组都试一遍，谁通就用谁。
 *   3. SDA 被拉死时先做 9 个时钟的总线恢复（从机把 SDA 咬住时有效），
 *      恢复不了再判定为外部硬短，不再反复重试刷屏。
 *   4. 器件地址兼容：MPU6050 的 ADO 在 J2-7，拉高即 0x69，两个都探。
 *      MAX30100(0x11) 与 MAX30102(0x15) 寄存器映射不同，按 PART_ID 分支。
 *
 * 采样节奏（tcxo 绝对时标对齐，不用 mdelay 累加）：
 *   10ms 一次  -> MAX3010x 读 FIFO（实测约 200Hz，算法按 FS=200 配置）
 *   20ms 一次  -> MPU6050 读 6 字节加速度（50Hz）
 */
#include "hy_sensor.h"

#include "pinctrl.h"
#include "gpio.h"
#include "i2c.h"
#include "errcode.h"
#include "common_def.h"
#include "soc_osal.h"
#include "tcxo.h"
#include "watchdog.h"
#include "algorithm.h"

#include <stdio.h>
#include <string.h>

hy_sensor_state_t g_hy;

/* ============================================================
 * 一、错误码解码
 * ============================================================ */

static const char *err_name(errcode_t e)
{
    switch (e) {
        case ERRCODE_SUCC:                      return "OK";
        case ERRCODE_FAIL:                      return "FAIL";
        case ERRCODE_I2C_NOT_INIT:              return "I2C_NOT_INIT";
        case ERRCODE_I2C_ALREADY_INIT:          return "I2C_ALREADY_INIT";
        case ERRCODE_I2C_INVALID_PARAMETER:     return "INVALID_PARAM";
        case ERRCODE_I2C_RATE_INVALID:          return "RATE_INVALID";
        case ERRCODE_I2C_ADDRESS_INVLID:        return "ADDRESS_INVALID";
        case ERRCODE_I2C_TIMEOUT:               return "TIMEOUT 总线超时(线被拉死/没上拉)";
        case ERRCODE_I2C_ACK_ERR:               return "ACK_ERR 从机无应答(线通了但器件不在)";
        case ERRCODE_I2C_WAIT_EXCEPTION:        return "WAIT_EXCEPTION";
        case ERRCODE_I2C_DMA_CONFIG_ERROR:      return "DMA_CONFIG_ERROR";
        case ERRCODE_I2C_DMA_TRANSFER_ERROR:    return "DMA_TRANSFER_ERROR";
        default:                                return "OTHER";
    }
}

/* ============================================================
 * 二、最底层事务（不复用 bsp_iic.c，避免和 201/202 的同名符号冲突）
 * ============================================================ */

static int raw_wr(unsigned char bus, unsigned char dev, unsigned char *buf, unsigned len, errcode_t *out)
{
    i2c_data_t d = {0};

    d.send_buf = buf;
    d.send_len = len;
    *out = uapi_i2c_master_write((i2c_bus_t)bus, dev, &d);
    return (*out == ERRCODE_SUCC) ? 0 : -1;
}

static int raw_rd(unsigned char bus, unsigned char dev, unsigned char *buf, unsigned len, errcode_t *out)
{
    i2c_data_t d = {0};

    d.receive_buf = buf;
    d.receive_len = len;
    *out = uapi_i2c_master_read((i2c_bus_t)bus, dev, &d);
    return (*out == ERRCODE_SUCC) ? 0 : -1;
}

/* 写一个寄存器：先发寄存器地址，再发数据（两段式，这些芯片都这么用） */
static int reg_wr(unsigned char dev, unsigned char reg, unsigned char val)
{
    unsigned char buf[2];
    errcode_t e = 0;

    buf[0] = reg;
    buf[1] = val;
    return raw_wr((unsigned char)g_hy.bus, dev, buf, 2, &e);
}

/* 最近一次寄存器事务的错误码，供限流打印时带上真实原因 */
static errcode_t s_last_err;

/* 读寄存器：先写地址指针，再连续读 len 字节 */
static int reg_rd(unsigned char dev, unsigned char reg, unsigned char *buf, unsigned len)
{
    unsigned char ptr = reg;

    if (raw_wr((unsigned char)g_hy.bus, dev, &ptr, 1, &s_last_err) != 0) {
        return -1;
    }
    return raw_rd((unsigned char)g_hy.bus, dev, buf, len, &s_last_err);
}

/* ============================================================
 * 三、引脚电平诊断 + 总线恢复
 * ============================================================ */

/* 把脚切回普通 GPIO，用指定上下拉读电平；-2 表示切不回 GPIO（脚被外设占着） */
static int pin_level(int pin, pin_pull_t pull)
{
    if (uapi_pin_set_mode((pin_t)pin, PIN_MODE_0) != ERRCODE_SUCC) {
        return -2;
    }
    uapi_gpio_set_dir((pin_t)pin, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull((pin_t)pin, pull);
    osal_mdelay(5);
    return (int)uapi_gpio_get_val((pin_t)pin);
}

/* 释放 SCL 为高电平：方向先切输入（严禁"先写电平再切方向"，WS63 会返回 0x80001006） */
static void scl_release(int pin)
{
    uapi_gpio_set_dir((pin_t)pin, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull((pin_t)pin, PIN_PULL_TYPE_UP);
}

static void scl_pull_low(int pin)
{
    uapi_pin_set_pull((pin_t)pin, PIN_PULL_TYPE_DISABLE);
    uapi_gpio_set_dir((pin_t)pin, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val((pin_t)pin, 0);
}

/*
 * I2C 总线恢复：从机在传输中被复位、正好把 SDA 咬在低电平时，
 * 主机打 9 个 SCL 时钟再发一个 STOP，可以把它推回空闲态。
 * 只驱动 SCL（主机本来就拥有它），SDA 全程只观察、绝不推挽输出，
 * 所以即使 SDA 是硬短路也不会两头对打。
 */
static int bus_recover(int scl, int sda)
{
    int i;

    uapi_pin_set_mode((pin_t)scl, PIN_MODE_0);
    uapi_pin_set_mode((pin_t)sda, PIN_MODE_0);
    uapi_gpio_set_dir((pin_t)sda, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull((pin_t)sda, PIN_PULL_TYPE_UP);
    scl_release(scl);
    osal_mdelay(2);

    if (uapi_gpio_get_val((pin_t)sda) != 0) {
        return 0;                       /* SDA 本来就是高，不用恢复 */
    }

    for (i = 0; i < 9; ++i) {
        scl_pull_low(scl);
        uapi_tcxo_delay_us(5);
        scl_release(scl);
        uapi_tcxo_delay_us(5);
    }

    /* 补一个 STOP：SCL 高时 SDA 由低变高 */
    scl_pull_low(scl);
    uapi_tcxo_delay_us(5);
    scl_release(scl);
    uapi_tcxo_delay_us(5);

    return (uapi_gpio_get_val((pin_t)sda) != 0) ? 0 : -1;
}

/* ============================================================
 * 四、探测：三组引脚组合 × 两个器件地址
 * ============================================================ */

struct bus_cand {
    unsigned char bus;
    unsigned char scl;
    unsigned char sda;
    const char *note;
};

static const struct bus_cand s_cands[] = {
    {1, 16, 15, "WEAR V0 原理图 / 官方 201、202 例程"},
    {1, 15, 16, "旧 watch_sensor.c 的写法（SCL/SDA 对调）"},
    {0, 18, 17, "SDK 允许的第三组 I2C 脚"},
};

static unsigned int s_rfail;

/* 采样阶段的失败限流打印：前 8 次全打，之后每 500 次一条 */
static void note_fail(const char *who, unsigned char addr, errcode_t e, unsigned int *cnt)
{
    unsigned int n = (*cnt)++;

    if (n < 8u || (n % 500u) == 0u) {
        printf("[hy-sensor] %s addr=0x%02X ret=0x%08X (%s) 累计%u次\r\n",
               who, (unsigned)addr, (unsigned)e, err_name(e), n + 1u);
    }
}

/*
 * 单次探测：读一个已知寄存器。
 * 返回 0 = 读到了（器件在），-1 = 失败（*e 里有完整错误码）。
 */
static int probe_reg(unsigned char addr, unsigned char reg, unsigned char *val, errcode_t *e_w, errcode_t *e_r)
{
    unsigned char ptr = reg;
    unsigned char buf = 0;

    if (raw_wr((unsigned char)g_hy.bus, addr, &ptr, 1, e_w) != 0) {
        *e_r = 0xFFFFFFFFu;             /* 表示"未执行"，和真实错误码区分开 */
        return -1;
    }
    if (raw_rd((unsigned char)g_hy.bus, addr, &buf, 1, e_r) != 0) {
        return -1;
    }
    *val = buf;
    return 0;
}

/* 打印一组候选脚的探测过程，返回 1=有器件应答 0=总线通但没人应答 -1=总线不通 */
static int bus_try(const struct bus_cand *c, int verbose)
{
    errcode_t e_w68 = 0, e_r68 = 0, e_w69 = 0, e_r69 = 0;
    errcode_t e_w57 = 0, e_r57 = 0;
    errcode_t init_ret;
    unsigned char id68 = 0, id69 = 0, id57 = 0;
    int ok68, ok69, ok57;
    int alive = 0;

    g_hy.bus = c->bus;
    g_hy.scl_pin = c->scl;
    g_hy.sda_pin = c->sda;

    uapi_i2c_deinit((i2c_bus_t)c->bus);             /* 上一次尝试可能已经 init 过 */
    uapi_pin_set_pull((pin_t)c->scl, PIN_PULL_TYPE_DISABLE);
    uapi_pin_set_pull((pin_t)c->sda, PIN_PULL_TYPE_DISABLE);
    uapi_pin_set_mode((pin_t)c->scl, 2);            /* mode2 = I2C 功能 */
    uapi_pin_set_mode((pin_t)c->sda, 2);

    init_ret = uapi_i2c_master_init((i2c_bus_t)c->bus, 100000, 0);
    if (verbose) {
        printf("[hy-sensor] 试 bus=%u SCL=IO%u SDA=IO%u (%s) init ret=0x%08X (%s)\r\n",
               (unsigned)c->bus, (unsigned)c->scl, (unsigned)c->sda, c->note,
               (unsigned)init_ret, err_name(init_ret));
    }
    if (init_ret != ERRCODE_SUCC && init_ret != ERRCODE_I2C_ALREADY_INIT) {
        return -1;
    }

    ok68 = (probe_reg(0x68, 0x75, &id68, &e_w68, &e_r68) == 0);
    ok69 = (probe_reg(0x69, 0x75, &id69, &e_w69, &e_r69) == 0);
    ok57 = (probe_reg(0x57, 0xFF, &id57, &e_w57, &e_r57) == 0);

    if (ok68 || ok69 || ok57) {
        alive = 1;
    } else {
        /* 两个地址都是 ACK_ERR 说明时钟出去了、只是没人应答 -> 总线本身是好的；
         * 只要出现 TIMEOUT，就是线的问题，可以换一组脚再试。 */
        int ack_only = (e_r68 == ERRCODE_I2C_ACK_ERR || e_w68 == ERRCODE_I2C_ACK_ERR) &&
                       (e_r57 == ERRCODE_I2C_ACK_ERR || e_w57 == ERRCODE_I2C_ACK_ERR);
        alive = ack_only ? 0 : -1;
    }

    if (verbose) {
        if (ok68) {
            printf("[hy-sensor]   MPU6050 0x68 WHO_AM_I=0x%02X\r\n", id68);
        } else {
            printf("[hy-sensor]   MPU6050 0x68 写=0x%08X (%s) 读=0x%08X (%s)\r\n",
                   (unsigned)e_w68, err_name(e_w68), (unsigned)e_r68, err_name(e_r68));
        }
        if (ok69) {
            printf("[hy-sensor]   MPU6050 0x69 WHO_AM_I=0x%02X（ADO 被拉高）\r\n", id69);
        } else {
            printf("[hy-sensor]   MPU6050 0x69 写=0x%08X (%s) 读=0x%08X (%s)\r\n",
                   (unsigned)e_w69, err_name(e_w69), (unsigned)e_r69, err_name(e_r69));
        }
        if (ok57) {
            printf("[hy-sensor]   MAX3010x 0x57 PART_ID=0x%02X (0x11=30100 / 0x15=30102)\r\n", id57);
        } else {
            printf("[hy-sensor]   MAX3010x 0x57 写=0x%08X (%s) 读=0x%08X (%s)\r\n",
                   (unsigned)e_w57, err_name(e_w57), (unsigned)e_r57, err_name(e_r57));
        }
    }

    /* 记下结果，供后续 init 使用 */
    if (ok68) {
        g_hy.mpu_addr = 0x68;
    } else if (ok69) {
        g_hy.mpu_addr = 0x69;
    }
    if (ok57) {
        g_hy.max_part = id57;
    }
    return alive;
}

/* 把探测结论写成一段人能直接读的话 */
static void report_bus_verdict(void)
{
    printf("[hy-sensor] ---- 总线结论 ----\r\n");
    printf("[hy-sensor] 上电空闲电平: SCL=%d SDA=%d (1=高 0=低 -2=脚被占用)\r\n",
           g_hy.scl_idle, g_hy.sda_idle);
    if (g_hy.bus < 0) {
        printf("[hy-sensor] 三组引脚全部 TIMEOUT：没有一组能完成 I2C 事务。\r\n");
        if (g_hy.sda_idle == 0) {
            printf("[hy-sensor] SDA 空闲即为低，且 9 时钟恢复%s。\r\n",
                   g_hy.sda_dead ? "无效——判定为外部硬短，先拔掉 WEAR 再复测" : "后已释放");
        }
        return;
    }
    printf("[hy-sensor] 可用组合: bus=%d SCL=IO%d SDA=IO%d\r\n",
           g_hy.bus, g_hy.scl_pin, g_hy.sda_pin);
}

/* ============================================================
 * 五、MPU6050
 * ============================================================ */

#define MPU_REG_SMPLRT_DIV   0x19
#define MPU_REG_CONFIG       0x1A
#define MPU_REG_GYRO_CFG     0x1B
#define MPU_REG_ACCEL_CFG    0x1C
#define MPU_REG_ACCEL_OUT    0x3B
#define MPU_REG_PWR_MGMT_1   0x6B
#define MPU_REG_WHO_AM_I     0x75

/* 量程改成 ±8g（AFS_SEL=2, 0x10）：摔倒冲击常在 3~6g，
 * ±2g 会直接削顶，峰值看不出来，摔倒与磕碰就分不开了。
 * 代价只是分辨率 0.24mg，对计步和姿态毫无影响。 */
#define MPU_LSB_PER_G        4096        /* 必须配合 ACCEL_CFG=0x10(±8g) */

static int mpu_init(void)
{
    unsigned char rb = 0;

    reg_wr((unsigned char)g_hy.mpu_addr, MPU_REG_PWR_MGMT_1, 0x80);   /* 软复位 */
    osal_mdelay(100);
    reg_wr((unsigned char)g_hy.mpu_addr, MPU_REG_PWR_MGMT_1, 0x00);   /* 退出休眠，内部 8M 时钟 */
    osal_mdelay(10);
    reg_wr((unsigned char)g_hy.mpu_addr, MPU_REG_SMPLRT_DIV, 0x04);   /* 1k/(1+4) = 200Hz */
    reg_wr((unsigned char)g_hy.mpu_addr, MPU_REG_CONFIG, 0x03);       /* DLPF 44Hz */
    reg_wr((unsigned char)g_hy.mpu_addr, MPU_REG_GYRO_CFG, 0x18);     /* ±2000dps，本模块不用 */
    reg_wr((unsigned char)g_hy.mpu_addr, MPU_REG_ACCEL_CFG, 0x10);    /* ±8g -> 4096 LSB/g（摔检测需要大动态范围） */
    osal_mdelay(50);

    /* 回读确认真的写进去了：0x6B 的 bit6(SLEEP) 必须是 0 */
    if (reg_rd((unsigned char)g_hy.mpu_addr, MPU_REG_PWR_MGMT_1, &rb, 1) != 0) {
        printf("[hy-sensor] MPU6050 PWR_MGMT_1 回读失败\r\n");
        return -1;
    }
    printf("[hy-sensor] MPU6050 PWR_MGMT_1=0x%02X (%s)\r\n", rb, (rb & 0x40) ? "仍在休眠!" : "已唤醒");
    if (reg_rd((unsigned char)g_hy.mpu_addr, MPU_REG_ACCEL_CFG, &rb, 1) != 0) {
        printf("[hy-sensor] MPU6050 ACCEL_CONFIG 回读失败\r\n");
        return -1;
    }
    printf("[hy-sensor] MPU6050 ACCEL_CONFIG=0x%02X (量程 ±%dg)\r\n", rb,
           (rb & 0x18) == 0x00 ? 2 : ((rb & 0x18) == 0x08 ? 4 : ((rb & 0x18) == 0x10 ? 8 : 16)));
    return 0;
}

static void mpu_read_acc(int *ax, int *ay, int *az)
{
    unsigned char buf[6] = {0};
    int16_t raw[3];

    if (reg_rd((unsigned char)g_hy.mpu_addr, MPU_REG_ACCEL_OUT, buf, 6) != 0) {
        note_fail("MPU6050 读取加速度", (unsigned char)g_hy.mpu_addr, s_last_err, &s_rfail);
        return;
    }
    raw[0] = (int16_t)((buf[0] << 8) | buf[1]);
    raw[1] = (int16_t)((buf[2] << 8) | buf[3]);
    raw[2] = (int16_t)((buf[4] << 8) | buf[5]);

    *ax = ((int)raw[0] * 1000) / MPU_LSB_PER_G;
    *ay = ((int)raw[1] * 1000) / MPU_LSB_PER_G;
    *az = ((int)raw[2] * 1000) / MPU_LSB_PER_G;
}

/* 计步：加速度模长 >1.3g 记上升沿，回落 <0.8g 记一步，间隔至少 250ms */
static void step_update(int m2)
{
    static int armed;
    static uint64_t last_ms;

    if (m2 > 1690000 && !armed) {
        armed = 1;
    } else if (armed && m2 < 640000) {
        uint64_t now = uapi_tcxo_get_ms();
        if (now - last_ms > 250) {
            g_hy.steps++;
            last_ms = now;
        }
        armed = 0;
    }
}

/* ============================================================
 * 五点五、摔倒检测
 *
 * 判定链（先冲击、后静止，两个条件都满足才算）：
 *   1. 冲击：合加速度模长 >= 2.0g。正常人走路、坐下、拍手都在 1.5g 内，
 *      而摔倒落地瞬间通常是 3~6g，所以 2.0g 这个门限不会误报日常动作。
 *   2. 静止：冲击后 5 秒内，模长峰峰值 <= 300mg。人一旦摔倒起不来，
 *      手部几乎不动，模长稳定在 1g 附近；而正常磕碰之后必然会继续
 *      活动（抬手、走路），1 秒内峰峰值就超过 600mg，直接被排除。
 *
 * 冲击后 500ms 内不计入静止统计：落地有回弹和抖动，那一段不是「静止」。
 *
 * 报警后不清状态，继续盯：检测到活动才解除，否则每 5 秒复报一次。
 * 这样即使人躺着不动，告警也不会自己消失。
 * ============================================================ */

#define FALL_IDLE      0
#define FALL_WATCH     1        /* 已检出冲击，正在观察是否静止 */
#define FALL_ALARM     2        /* 疑似摔倒，等人工确认或活动解除 */

#define FALL_IMPACT_MG    2000  /* 冲击门限 2.0g */
#define FALL_RING_MS      500   /* 冲击后回弹期，不计入静止统计 */
#define FALL_STILL_MS     5000  /* 静止观察窗 5 秒 */
#define FALL_STILL_PTP    300   /* 静止判据：模长峰峰 <=300mg */
#define FALL_MOVE_PTP     600   /* 活动判据：1 秒峰峰 >=600mg */
#define FALL_REPEAT_MS    5000  /* 报警复报间隔 */

static int s_fall_state;
static uint64_t s_fall_t0;       /* 冲击时刻 */
static int s_fall_peak;          /* 本次冲击峰值 mg */
static int s_still_min;
static int s_still_max;
static int s_act_min;
static int s_act_max;
static uint64_t s_act_t0;
static int s_act_ptp;            /* 上一个完整 1 秒窗的模长峰峰值 */
static int s_act_ready;          /* 1 = s_act_ptp 是「重置后」的完整窗，才可用 */
static int s_ring_done;          /* 1 = 已过冲击回弹期 */
static uint64_t s_ff_ms;         /* 最近一次失重（|a|<0.6g）时刻 */
static uint64_t s_alarm_t;       /* 上次报警时刻 */

static int isqrt_u32(uint32_t v)
{
    uint32_t r = 0;
    uint32_t bit = 1u << 30;

    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0u) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (int)r;
}

static const char *fall_state_name(void)
{
    if (s_fall_state == FALL_WATCH) {
        return "观察";
    }
    if (s_fall_state == FALL_ALARM) {
        return "疑似摔倒";
    }
    return "正常";
}

static void fall_update(int ax, int ay, int az, uint64_t now)
{
    int m = isqrt_u32((uint32_t)(ax * ax + ay * ay + az * az));   /* 合加速度，mg */

    if (m > g_hy.acc_peak) {
        g_hy.acc_peak = m;           /* 给状态行看，方便标定力度 */
    }
    if (m < 600) {
        s_ff_ms = now;               /* 记住失重时刻：真正的摔倒大多先失重 */
    }

    /* --- 活动度：1 秒滑动窗的模长峰峰值，每秒结算一次 --- */
    if (s_act_t0 == 0u) {
        s_act_t0 = now;
        s_act_min = m;
        s_act_max = m;
    }
    /* 只把「非冲击」样本计入活动度。撞击尖峰本身不算运动——
     * 否则第二次撞击（实测 8451mg）会把整次摔倒判成「冲击后有活动」而漏报。 */
    if (m < FALL_IMPACT_MG) {
        if (m < s_act_min) {
            s_act_min = m;
        }
        if (m > s_act_max) {
            s_act_max = m;
        }
    }
    if (now - s_act_t0 >= 1000u) {
        s_act_ptp = s_act_max - s_act_min;
        s_act_ready = 1;
        s_act_t0 = now;
        s_act_min = m;
        s_act_max = m;
    }

    switch (s_fall_state) {
    case FALL_IDLE:
        if (m >= FALL_IMPACT_MG) {
            s_fall_state = FALL_WATCH;
            s_fall_t0 = now;
            s_fall_peak = m;
            s_still_min = m;
            s_still_max = m;
            s_act_t0 = now;          /* 关键：冲击尖峰不能算作「之后的运动」 */
            s_act_min = m;
            s_act_max = m;
            s_act_ready = 0;
            s_ring_done = 0;
            g_hy.fall_peak_mg = m;
            printf("[hy-fall] 冲击 %d.%02dg%s，开始观察是否静止（%d 秒）\r\n",
                   m / 1000, (m % 1000) / 10,
                   (s_ff_ms != 0u && now - s_ff_ms <= 600u) ? "（前有失重）" : "",
                   FALL_STILL_MS / 1000);
        }
        break;

    case FALL_WATCH:
        if (m >= FALL_IMPACT_MG && now - s_fall_t0 > 300u) {
            /* 连续撞击（比如滚下楼梯），重新计时，峰值取大的 */
            s_fall_t0 = now;
            if (m > s_fall_peak) {
                s_fall_peak = m;
            }
            s_still_min = m;
            s_still_max = m;
            break;
        }
        if (!s_ring_done && now - s_fall_t0 >= FALL_RING_MS) {
            s_ring_done = 1;
            s_act_t0 = now;          /* 落地回弹段也不算活动 */
            s_act_min = m;
            s_act_max = m;
            s_act_ready = 0;
            /* 静止统计也必须从这里重新起算。
             * 否则初值还是冲击峰（4000+mg），5 秒窗的峰峰值永远超门限，
             * 真摔倒会被判成「未静止」——线上第一版就是这样漏报的。 */
            s_still_min = m;
            s_still_max = m;
        }
        if (s_ring_done) {
            if (m < s_still_min) s_still_min = m;
            if (m > s_still_max) s_still_max = m;
            if (m > s_fall_peak) s_fall_peak = m;
            g_hy.fall_peak_mg = s_fall_peak;
        }

        /* 冲击后还在动，就不是摔倒。必须等重置后的完整 1 秒窗，否则会读到旧的峰峰值 */
        if (s_act_ready && s_act_ptp >= FALL_MOVE_PTP) {
            printf("[hy-fall] 已排除：冲击后有活动（1 秒峰峰 %dmg）\r\n", s_act_ptp);
            s_fall_state = FALL_IDLE;
            break;
        }

        g_hy.fall_still_ms = (int)(now - s_fall_t0);
        if (now - s_fall_t0 >= FALL_STILL_MS) {
            int ptp = s_still_max - s_still_min;

            if (ptp <= FALL_STILL_PTP) {
                s_fall_state = FALL_ALARM;
                s_alarm_t = now;
                g_hy.fall_events++;
                g_hy.fall_state = FALL_ALARM;
                /* 必须拆成多行短打印：SDK 的 printf 有固定缓冲，
                 * 一行塞 40 多个汉字（每字 3 字节）会报
                 * "Output illegal string! vsnprintf_s failed!" 而整条丢失。 */
                printf("\r\n[hy-fall] ===== 疑似摔倒 =====\r\n");
                /* 注意 %01ds 不是 %01fs：写成 %f 会被当成浮点转换，
                 * 实参却是整数，SDK 的 printf 读到垃圾后报
                 * "Output illegal string" 并丢整行。 */
                printf("[hy-fall] 冲击 %d.%02dg 静止 %d.%01ds\r\n",
                       s_fall_peak / 1000, (s_fall_peak % 1000) / 10,
                       (int)((now - s_fall_t0) / 1000u),
                       (int)(((now - s_fall_t0) % 1000u) / 100u));
                printf("[hy-fall] 抖动 %dmg 模长 %dmg\r\n\r\n",
                       ptp, (s_still_min + s_still_max) / 2);
            } else {
                printf("[hy-fall] 已排除：未静止（5 秒内抖动 %dmg > %dmg）\r\n",
                       ptp, FALL_STILL_PTP);
                s_fall_state = FALL_IDLE;
            }
        }
        break;

    case FALL_ALARM:
        g_hy.fall_still_ms = (int)(now - s_fall_t0);
        if (s_act_ready && s_act_ptp >= FALL_MOVE_PTP) {
            printf("[hy-fall] 已解除（检测到活动，峰峰 %dmg）\r\n", s_act_ptp);
            s_fall_state = FALL_IDLE;
            g_hy.fall_state = FALL_IDLE;
            g_hy.fall_still_ms = 0;
        } else if (now - s_alarm_t >= FALL_REPEAT_MS) {
            s_alarm_t = now;
            printf("[hy-fall] 仍在静止，疑似摔倒持续 %d 秒，请确认本人状态\r\n",
                   (int)((now - s_fall_t0) / 1000));
        }
        break;

    default:
        s_fall_state = FALL_IDLE;
        break;
    }

    g_hy.fall_state = s_fall_state;
}

/* ============================================================
 * 六、MAX3010x（30100 / 30102 两套映射）
 * ============================================================ */

/* MAX30100 寄存器 */
#define M100_INTR_STATUS  0x00
#define M100_INTR_ENABLE  0x01
#define M100_FIFO_WR      0x02
#define M100_OVF          0x03
#define M100_FIFO_RD      0x04
#define M100_FIFO_DATA    0x05
#define M100_MODE         0x06
#define M100_SPO2_CFG     0x07
#define M100_LED_CFG      0x09

/* MAX30102 寄存器 */
#define M102_INT_STATUS1  0x00
#define M102_INT_ENABLE1  0x01
#define M102_FIFO_WR      0x04
#define M102_OVF          0x05
#define M102_FIFO_RD      0x06
#define M102_FIFO_DATA    0x07
#define M102_FIFO_CFG     0x08
#define M102_MODE_CFG     0x09
#define M102_SPO2_CFG     0x0A
#define M102_LED1_PA      0x0C
#define M102_LED2_PA      0x0D

#define MAX_ADDR          0x57

/* 运行期选定的寄存器地址与每对样本字节数（4=30100 / 6=30102） */
static unsigned char s_max_wr = M100_FIFO_WR;
static unsigned char s_max_rd = M100_FIFO_RD;
static unsigned char s_max_data = M100_FIFO_DATA;
static unsigned char s_max_ovf = M100_OVF;
static unsigned char s_max_bytes = 4;

/* 诊断用：保留最近 64 组 FIFO 原始字节，用来验证「拆字方式」对不对。
 * 只读不干扰采样——数据本来就是从 FIFO 读出来的，顺手存一份。 */
#define HY_RB_LEN 64
static unsigned char s_rb[HY_RB_LEN][6];
static int s_rb_i;
static int s_rb_n;

static int max_init_30100(void)
{
    unsigned char rb7 = 0;
    unsigned char rb9 = 0;

    /* MODE=0x03 SpO2(RED+IR)；
     * SPO2_CFG=0x0C -> RGE=00(2048nA 满量程，增益最高) + SR=011(200Hz) + PW=00(200us)；
     *   DC 实测约占满量程 27%，换到 2048nA 后约 54%，脉搏交流也同比放大一倍。
     * LED_CFG=0x77 -> RED/IR 各 0x7 档电流。 */
    reg_wr(MAX_ADDR, 0x06, 0x40);          /* MODE bit6 = RESET */
    osal_mdelay(50);
    reg_wr(MAX_ADDR, M100_INTR_ENABLE, 0xF0);
    reg_wr(MAX_ADDR, M100_INTR_STATUS, 0x00);
    reg_wr(MAX_ADDR, M100_MODE, 0x03);
    reg_wr(MAX_ADDR, M100_SPO2_CFG, 0x0C);
    reg_wr(MAX_ADDR, M100_LED_CFG, 0x77);
    reg_wr(MAX_ADDR, M100_FIFO_WR, 0x00);
    reg_wr(MAX_ADDR, M100_OVF, 0x00);
    reg_wr(MAX_ADDR, M100_FIFO_RD, 0x00);
    osal_mdelay(20);

    /* 回读确认：上一版只回读了 MODE，SPO2_CFG 写没写进去其实一直是盲区 */
    if (reg_rd(MAX_ADDR, M100_SPO2_CFG, &rb7, 1) == 0 &&
        reg_rd(MAX_ADDR, M100_LED_CFG, &rb9, 1) == 0) {
        printf("[hy-sensor] 回读 SPO2_CFG=0x%02X(%s) LED_CFG=0x%02X\r\n",
               rb7, (rb7 == 0x0C) ? "写入成功" : "与期望 0x0C 不符", rb9);
    }

    s_max_wr = M100_FIFO_WR;
    s_max_rd = M100_FIFO_RD;
    s_max_data = M100_FIFO_DATA;
    s_max_ovf = M100_OVF;
    s_max_bytes = 4;
    return 0;
}

static int max_init_30102(void)
{
    /* MODE=0x03 SpO2；SPO2_CFG=0x47 -> 100Hz + 411us/18bit；
     * LED1/LED2 电流分开写 0x33。 */
    reg_wr(MAX_ADDR, M102_MODE_CFG, 0x40);
    osal_mdelay(50);
    reg_wr(MAX_ADDR, M102_INT_ENABLE1, 0xC0);
    reg_wr(MAX_ADDR, M102_INT_STATUS1, 0x00);
    reg_wr(MAX_ADDR, M102_FIFO_CFG, 0x00);
    reg_wr(MAX_ADDR, M102_MODE_CFG, 0x03);
    reg_wr(MAX_ADDR, M102_SPO2_CFG, 0x47);
    reg_wr(MAX_ADDR, M102_LED1_PA, 0x33);
    reg_wr(MAX_ADDR, M102_LED2_PA, 0x33);
    reg_wr(MAX_ADDR, M102_FIFO_WR, 0x00);
    reg_wr(MAX_ADDR, M102_OVF, 0x00);
    reg_wr(MAX_ADDR, M102_FIFO_RD, 0x00);
    osal_mdelay(20);

    s_max_wr = M102_FIFO_WR;
    s_max_rd = M102_FIFO_RD;
    s_max_data = M102_FIFO_DATA;
    s_max_ovf = M102_OVF;
    s_max_bytes = 6;
    return 0;
}

static int max_init(void)
{
    unsigned char mode = 0;

    if (g_hy.max_part == 0x11) {
        max_init_30100();
        if (reg_rd(MAX_ADDR, M100_MODE, &mode, 1) == 0) {
            printf("[hy-sensor] MAX30100 MODE=0x%02X (%s)\r\n", mode, (mode == 0x03) ? "SpO2 模式已启动" : "写入未生效");
        }
    } else if (g_hy.max_part == 0x15) {
        max_init_30102();
        if (reg_rd(MAX_ADDR, M102_MODE_CFG, &mode, 1) == 0) {
            printf("[hy-sensor] MAX30102 MODE=0x%02X (%s)\r\n", mode, (mode == 0x03) ? "SpO2 模式已启动" : "写入未生效");
        }
    } else {
        printf("[hy-sensor] MAX3010x PART_ID=0x%02X 不在已知范围，按 30100 寄存器映射试\r\n",
               g_hy.max_part);
        max_init_30100();
    }
    return 0;
}

/* 从 FIFO 取一对样本；返回 1 = 取到，0 = FIFO 空 */
static int max_read_pair(uint32_t *red, uint32_t *ir)
{
    unsigned char wr = 0;
    unsigned char rd = 0;
    unsigned char buf[6] = {0};

    if (reg_rd(MAX_ADDR, s_max_wr, &wr, 1) != 0) {
        return 0;
    }
    if (reg_rd(MAX_ADDR, s_max_rd, &rd, 1) != 0) {
        return 0;
    }
    if (wr == rd) {
        return 0;                      /* FIFO 空，本轮跳过（空读会 NAK 刷屏） */
    }
    if (reg_rd(MAX_ADDR, s_max_data, buf, s_max_bytes) != 0) {
        note_fail("MAX3010x FIFO 读", MAX_ADDR, s_last_err, &s_rfail);
        return 0;
    }

    if (s_max_bytes == 4) {            /* MAX30100：每路 16 位 */
        *red = ((uint32_t)buf[0] << 8) | buf[1];
        *ir  = ((uint32_t)buf[2] << 8) | buf[3];
    } else {                           /* MAX30102：每路 18 位，存低 16 位够算法用 */
        *red = ((uint32_t)buf[0] << 8) | buf[1];
        *ir  = ((uint32_t)buf[3] << 8) | buf[4];
    }

    /* 顺手存一份原始字节（环形覆盖，保留最近 64 组） */
    memcpy(s_rb[s_rb_i], buf, s_max_bytes);
    s_rb_i = (s_rb_i + 1) % HY_RB_LEN;
    if (s_rb_n < HY_RB_LEN) {
        s_rb_n++;
    }
    return 1;
}

/* ============================================================
 * 七、心率血氧计算
 * ============================================================ */

#define HY_BUF_LEN     1000             /* 200Hz × 5s，与 algorithm.h 的 FS 保持一致 */
#define HY_BUF_DROP    400

static uint32_t s_ir[HY_BUF_LEN];
static uint32_t s_red[HY_BUF_LEN];
static int s_fill;

/* 采样率自校准记录：只用于核对，不再改变喂给算法的数据。
 * MAX30100 的 SPO2_CONFIG 位定义与 30102 不同，厂商例程把 0x47 混着用，
 * 实际出点常在 200Hz 上下，所以算法侧直接按 FS=200 配置。 */
static unsigned int s_raw_total;
static unsigned int s_rate_cnt;
static uint64_t s_rate_ms;
static unsigned int s_sps;

/* ============================================================
 * 六点五、时域兜底心率（Maxim 算法不灵时用）
 *
 * Maxim 那套算法对采样率、直流水平、波形形态都很敏感，实测常出现
 * 「spo2 有效、hr 有效但数值 400 多」这种离谱结果。这里再挂一条简单路径：
 * 拿整窗 1000 点（200Hz × 5s）算直流 -> 交流带迟滞找上升沿过零 -> 间隔中位数。
 * 一次性算完，不维护流状态，也不会被零星噪声带偏。
 * ============================================================ */

#define HR2_MAX_IV 16
#define HR2_MIN_GAP 80              /* 两次心跳至少隔 80 点 = 400ms（200Hz） */
#define HR2_MS_PER_PT 5             /* 200Hz -> 5ms/点 */

/* 心率估计：去漂移 -> 强平滑 -> 自相关
 *
 * 依据（都是日志算出来的，不是猜的）：
 *   1. 芯片只出 11 位有效数据，量化台阶 32 计数；
 *   2. 增益加倍后 DC≈33000、摆幅≈1500，但相邻样本差的中位数是 224 计数——
 *      这是光子散粒噪声的水平（sqrt(33000)≈182），噪声把真实脉搏盖住了。
 *      200Hz 下噪声不相关，脉搏相关，所以「先重平滑、再自相关」能提出来：
 *      20 点滑动平均把噪声压到 1/4.5，1Hz 的脉搏几乎不衰减（sinc(0.1)≈0.98）。
 *   3. 峰值检测要门限，门限在弱信号下不是太高（什么都不认）就是太低（认噪声）；
 *      自相关不需要门限，只看「哪个周期的重复性最强」。
 *
 * 峰值检测保留，仅作为交叉验证一起打印。
 */
#define HY_AC_WIN   600                 /* 3s @200Hz，压掉 0.3Hz 以下的漂移 */
#define HY_LP_N     20                  /* 20 点滑动平均 ≈ 10Hz 低通 */
#define HY_LAG_MIN  60                  /* 300ms -> 200bpm 上限 */
#define HY_LAG_MAX  400                 /* 2s    -> 30bpm 下限 */
#define HY_CORR_MIN 250                 /* 归一化相关度低于 0.25 就不认 */

static int hr_from_buffer(uint32_t ir_min, uint32_t ir_max, int *amp_out, int *drift_out,
                          int *ivn_out, int *corr_out, int *hr_ac_out, int *hr_pk_out)
{
    static int ac[HY_BUF_LEN];
    static int sm[HY_BUF_LEN];
    unsigned int iv[HR2_MAX_IV];
    long run = 0;
    long sum = 0;
    int bmin = 0x7FFFFFFF;
    int bmax = -0x7FFFFFFF;
    int hi = -0x7FFFFFFF;
    int lo = 0x7FFFFFFF;
    int th;
    int last_peak = -10000;
    int ivn = 0;
    int best_lag = 0;
    int best_corr = 0;
    int i;
    int j;

    *amp_out = 0;
    *drift_out = 0;
    *ivn_out = 0;
    *corr_out = 0;
    *hr_ac_out = 0;
    *hr_pk_out = 0;
    (void)ir_min;
    (void)ir_max;

    /* 1. 去漂移：3 秒滑动均值当基线 */
    for (i = 0; i < HY_BUF_LEN; ++i) {
        int n;
        int base;

        run += (int)s_ir[i];
        if (i >= HY_AC_WIN) {
            run -= (int)s_ir[i - HY_AC_WIN];
        }
        n = (i < HY_AC_WIN) ? (i + 1) : HY_AC_WIN;
        base = (int)(run / n);
        ac[i] = (int)s_ir[i] - base;
        if (base < bmin) bmin = base;
        if (base > bmax) bmax = base;
    }
    *drift_out = bmax - bmin;

    /* 2. 20 点滑动平均：压噪声的主力 */
    for (i = 0; i < HY_BUF_LEN; ++i) {
        int n;

        sum += ac[i];
        if (i >= HY_LP_N) {
            sum -= ac[i - HY_LP_N];
        }
        n = (i < HY_LP_N) ? (i + 1) : HY_LP_N;
        sm[i] = (int)(sum / n);
    }

    for (i = 0; i < HY_BUF_LEN; ++i) {
        if (sm[i] > hi) hi = sm[i];
        if (sm[i] < lo) lo = sm[i];
    }
    *amp_out = hi - lo;

    /* 3. 自相关：lag 60~400 点，归一化到千分比 */
    for (i = HY_LAG_MIN; i <= HY_LAG_MAX; ++i) {
        int64_t num = 0;
        int64_t d1 = 0;
        int64_t d2 = 0;
        int corr;

        for (j = 0; j + i < HY_BUF_LEN; ++j) {
            num += (int64_t)sm[j] * (int64_t)sm[j + i];
            d1 += (int64_t)sm[j] * (int64_t)sm[j];
            d2 += (int64_t)sm[j + i] * (int64_t)sm[j + i];
        }
        if (d1 <= 0 || d2 <= 0) {
            continue;
        }
        /* 必须用 64 位：sm 幅度上千、点数上千，num 能到 1e8 量级，
         * 再乘 2000 就溢出 int32 了——上一版相关度恒为 0.000 就是这个原因。 */
        corr = (int)((2000 * num) / (d1 + d2));    /* AM-GM 归一化，范围 0~1000 */
        if (corr > best_corr) {
            best_corr = corr;
            best_lag = i;
        }
    }

    /* 取第一个达到最大值 85% 的 lag，避免把 2 倍周期当成基频 */
    if (best_lag > 0 && best_corr >= HY_CORR_MIN) {
        int weak = (best_corr * 85) / 100;

        for (i = HY_LAG_MIN; i <= best_lag; ++i) {
            int64_t num = 0;
            int64_t d1 = 0;
            int64_t d2 = 0;
            int corr;

            for (j = 0; j + i < HY_BUF_LEN; ++j) {
                num += (int64_t)sm[j] * (int64_t)sm[j + i];
                d1 += (int64_t)sm[j] * (int64_t)sm[j];
                d2 += (int64_t)sm[j + i] * (int64_t)sm[j + i];
            }
            if (d1 <= 0 || d2 <= 0) {
                continue;
            }
            corr = (int)((2000 * num) / (d1 + d2));
            if (corr >= weak) {
                best_lag = i;
                break;
            }
        }
        *corr_out = best_corr;
        if (best_lag > HY_LAG_MIN + 1) {  /* 命中下边界说明相关峰被截断，不是真周期 */
            *hr_ac_out = (int)(60000u / ((unsigned int)best_lag * HR2_MS_PER_PT));
        } else {
            *hr_ac_out = 0;
        }
    }

    /* 4. 峰值检测（交叉验证用，门限放到 ±1/6 幅度，弱信号才认得出） */
    th = (hi - lo) / 6;
    if (th < 16) {
        th = 16;
    }
    for (i = 3; i < HY_BUF_LEN - 3; ++i) {
        if (sm[i] <= th) {
            continue;
        }
        if (sm[i] < sm[i - 1] || sm[i] < sm[i + 1]) {
            continue;
        }
        if ((i - last_peak) < HR2_MIN_GAP) {
            continue;
        }
        if (last_peak > 0) {
            unsigned int ivms = (unsigned int)(i - last_peak) * HR2_MS_PER_PT;

            if (ivms >= 330u && ivms <= 2000u && ivn < HR2_MAX_IV) {
                iv[ivn++] = ivms;
            }
        }
        last_peak = i;
    }
    *ivn_out = ivn;

    if (ivn >= 3) {                      /* 峰值法也独立产出一个值，供交叉校验 */
        for (i = 1; i < ivn; ++i) {
            unsigned int k = iv[i];

            j = i - 1;
            while (j >= 0 && iv[j] > k) {
                iv[j + 1] = iv[j];
                --j;
            }
            iv[j + 1] = k;
        }
        *hr_pk_out = (int)(60000u / iv[ivn / 2]);
    }
    return *hr_ac_out;
}

/* 多窗投票：连续 5 窗取中位数，单窗误判不会直接跳出去。
 *
 * 另加「保持」：某窗算不出有效值时不清零，沿用上一次的中位数，
 * 连续 3 窗（约 15 秒）都算不出才归零。理由很实际——
 * 5 秒一窗，只要有一窗相关度掉下去就跳 0，屏幕上就成了 76/0/76/0 乱跳，
 * 比慢一点更没法看。 */
static int hr_vote(int x)
{
    static int hist[5];
    static int hn;
    static int wr;
    static int miss;
    int tmp[6];
    int n = 0;
    int i;
    int j;

    if (x > 0) {
        hist[wr] = x;                /* 写指针独立自增取模。
                                      * 上一版写的是 hist[hn % 5]，而 hn 涨到 5 就不再涨，
                                      * 5 % 5 恒为 0 —— 只有第 0 格在更新，其余 4 格永远停在
                                      * 最初那批 200，中位数自然恒等于 200。 */
        wr = (wr + 1) % 5;
        if (hn < 5) {
            hn++;
        }
        miss = 0;
    } else if (hn == 0) {
        return 0;
    } else if (++miss >= 3) {
        hn = 0;
        wr = 0;
        miss = 0;
        return 0;
    }

    for (i = 0; i < hn; ++i) {
        tmp[n++] = hist[i];
    }
    for (i = 1; i < n; ++i) {
        int k = tmp[i];

        j = i - 1;
        while (j >= 0 && tmp[j] > k) {
            tmp[j + 1] = tmp[j];
            --j;
        }
        tmp[j + 1] = k;
    }
    return tmp[n / 2];
}

/* 相邻差值的中位数：越小越连续，用来自动挑出正确的拆字方式 */
static unsigned int med_diff(const unsigned int *v, int n)
{
    unsigned int d[HY_RB_LEN];
    int i;
    int j;

    if (n < 2) {
        return 0;
    }
    for (i = 1; i < n; ++i) {
        d[i - 1] = (v[i] > v[i - 1]) ? (v[i] - v[i - 1]) : (v[i - 1] - v[i]);
    }
    n -= 1;
    for (i = 1; i < n; ++i) {
        unsigned int k = d[i];

        j = i - 1;
        while (j >= 0 && d[j] > k) {
            d[j + 1] = d[j];
            --j;
        }
        d[j + 1] = k;
    }
    return d[n / 2];
}

static void variant_stat(const char *name, const unsigned int *v, int n)
{
    unsigned int mn = 0xFFFFFFFFu;
    unsigned int mx = 0;
    int low_nz = 0;
    int i;

    for (i = 0; i < n; ++i) {
        if (v[i] < mn) mn = v[i];
        if (v[i] > mx) mx = v[i];
        if ((v[i] & 0x1Fu) != 0u) {
            low_nz++;
        }
    }
    printf("[diag]   %-22s min=%5u max=%5u swing=%5u 低5位非零=%2d/%d 相邻差中位数=%u\r\n",
           name, (unsigned)mn, (unsigned)mx, (unsigned)(mx - mn), low_nz, n, med_diff(v, n));
}

/* 一次性把「原始字节 / 四种拆字对比 / 波形快照」打出来。
 * 打印量必须小：上一版打 200 点（约 1.5KB），串口阻塞期间 MAX30100 的
 * 16 深度 FIFO 溢出后彻底停摆，之后再无样本。 */
static void diag_dump(uint32_t ir_min, uint32_t ir_max)
{
    unsigned int va[HY_RB_LEN];
    unsigned int vb[HY_RB_LEN];
    unsigned int vc[HY_RB_LEN];
    unsigned int vd[HY_RB_LEN];
    int n = s_rb_n;
    int show;
    int i;

    if (n <= 0) {
        return;
    }
    show = (n < 8) ? n : 8;

    printf("[diag] ===== FIFO 原始字节(%d 组，只列最近 %d 组) =====\r\n", n, show);
    for (i = 0; i < show; ++i) {
        int idx = (s_rb_i - 1 - i + HY_RB_LEN * 2) % HY_RB_LEN;
        const unsigned char *b = s_rb[idx];

        printf("[diag] #%02d %02X %02X %02X %02X %s red=%5u ir=%5u\r\n",
               i, b[0], b[1], b[2], b[3], (s_max_bytes == 4) ? "A" : "",
               (unsigned)(((uint32_t)b[0] << 8) | b[1]),
               (unsigned)((s_max_bytes == 4) ? (((uint32_t)b[2] << 8) | b[3])
                                             : (((uint32_t)b[3] << 8) | b[4])));
    }

    if (s_max_bytes == 4) {
        for (i = 0; i < n; ++i) {
            int idx = (s_rb_i - n + i + HY_RB_LEN * 2) % HY_RB_LEN;
            const unsigned char *b = s_rb[idx];

            va[i] = ((unsigned int)b[2] << 8) | b[3];
            vb[i] = ((unsigned int)b[3] << 8) | b[2];
            vc[i] = ((unsigned int)b[0] << 8) | b[1];
            vd[i] = ((unsigned int)b[1] << 8) | b[0];
        }
        printf("[diag] ===== 四种拆字方式对比（相邻差中位数越小越连续） =====\r\n");
        variant_stat("A (b2<<8)|b3 当前", va, n);
        variant_stat("B (b3<<8)|b2", vb, n);
        variant_stat("C (b0<<8)|b1 RED", vc, n);
        variant_stat("D (b1<<8)|b0", vd, n);
    }

    printf("[diag] ===== 波形快照 40 点（每 %d 点取 1）ir_min=%u ir_max=%u =====\r\n",
           HY_BUF_LEN / 40, (unsigned)ir_min, (unsigned)ir_max);
    for (i = 0; i < 40; ++i) {
        printf("%5u ", (unsigned)s_ir[i * (HY_BUF_LEN / 40)]);
        if ((i % 10) == 9) {
            printf("\r\n");
        }
    }
}

static void hr_recalc(void)
{
    int spo2 = 0;
    int hr = 0;
    char spo2_valid = 0;
    char hr_valid = 0;
    uint32_t ir_min = 0xFFFFFFFFu;
    uint32_t ir_max = 0;
    uint32_t red_max = 0;
    int i;

    maxim_heart_rate_and_oxygen_saturation(s_ir, HY_BUF_LEN, s_red,
                                           &spo2, &spo2_valid, &hr, &hr_valid);

    for (i = 0; i < HY_BUF_LEN; ++i) {
        if (s_ir[i] < ir_min) ir_min = s_ir[i];
        if (s_ir[i] > ir_max) ir_max = s_ir[i];
        if (s_red[i] > red_max) red_max = s_red[i];
    }

    /* 原始值一定要打出来：手指没放上去 / 光太弱 / 饱和，都要靠这几个数判断，
     * 否则只能盲猜算法为什么给 0。 */
    printf("[hy-sensor] max3010x raw: ir_max=%u ir_min=%u swing=%u red_max=%u\r\n",
           (unsigned)ir_max, (unsigned)ir_min, (unsigned)(ir_max - ir_min), (unsigned)red_max);

    /* 诊断：第一窗无条件 dump（先拿到原始字节），之后有手指时最多再补两次。
     * 手指没放上去时 ir_max 只有 2000 出头，dump 出来看不出波形。 */
    {
        static int diag_cnt;
        static uint64_t diag_ms;
        uint64_t now_ms = uapi_tcxo_get_ms();

        if (diag_cnt == 0 || (ir_max >= 6000u && diag_cnt < 3 && (now_ms - diag_ms) > 3000u)) {
            diag_dump(ir_min, ir_max);
            diag_ms = now_ms;
            diag_cnt++;
        }
    }

    /* 手指放上去时 ir_max 是 1.1 万~2.6 万，没放是 2000~3600，用 6000 一刀切开 */
    if (ir_max < 6000u || (ir_max - ir_min) < 200u) {
        g_hy.hr = 0;
        g_hy.spo2 = 0;
        g_hy.ir_pct = 0;
        printf("[hy-sensor] 判定：无手指 / 信号太弱（ir_max=%u 需 >6000，swing=%u 需 >200）\r\n",
               (unsigned)ir_max, (unsigned)(ir_max - ir_min));
        return;
    }

    g_hy.ir_pct = (int)(((ir_max - ir_min) * 100u) / ir_max);
    if (g_hy.ir_pct > 100) {
        g_hy.ir_pct = 100;
    }

    /* 饱和保护：换到高增益（2048nA）后如果顶到满量程，退回 4096nA */
    if (ir_max >= 62000u) {
        static int backed_off;

        if (!backed_off) {
            reg_wr(MAX_ADDR, M100_SPO2_CFG, 0x2C);
            backed_off = 1;
            printf("[hy-sensor] IR 接近满量程(%u)，增益退回 4096nA\r\n", (unsigned)ir_max);
        }
    }

    /* 主用自研算法，Maxim 只作为参考打印 */
    {
        int amp = 0;
        int drift = 0;
        int ivn = 0;
        int corr = 0;
        int hr_ac = 0;
        int hr_pk = 0;
        int est = 0;
        int voted;
        int moving = 0;

        (void)hr_from_buffer(ir_min, ir_max, &amp, &drift, &ivn, &corr, &hr_ac, &hr_pk);

        /* 运动伪影门限：手指一挪，直流基线的漂移能到上千计数，
         * 比脉搏大一个量级，任何算法在这段上都只会给垃圾。
         * 实测手指压紧/挪动时 漂移=983~1319、幅度=1747~1905；
         * 静息时 漂移=160~450、幅度=420~970。取 700 作分界。 */
        if (drift > 700 || amp > 1500) {
            moving = 1;
        }

        if (!moving) {
            int ac_ok = (corr >= 400) && (hr_ac >= 40 && hr_ac <= 200);
            int pk_ok = (ivn >= 3) && (hr_pk >= 40 && hr_pk <= 200);

            if (ac_ok && pk_ok) {
                int d = (hr_ac > hr_pk) ? (hr_ac - hr_pk) : (hr_pk - hr_ac);
                int tol = hr_ac / 5;            /* 允许 20% 偏差 */

                if (tol < 10) {
                    tol = 10;
                }
                if (d <= tol) {
                    est = (hr_ac + hr_pk) / 2;  /* 两条独立路径互相印证才算数 */
                } else {
                    est = hr_ac;                /* 打架时信自相关（对弱信号更稳） */
                    printf("[hr] 交叉校验不合：自相关=%d 峰值=%d，按自相关取\r\n", hr_ac, hr_pk);
                }
            } else if (ac_ok) {
                est = hr_ac;
            } else if (pk_ok) {
                est = hr_pk;
            }
        }

        voted = hr_vote(est);
        if (voted >= 40 && voted <= 200) {
            g_hy.hr = voted;
        } else {
            g_hy.hr = 0;
        }
        g_hy.spo2 = (spo2_valid && spo2 >= 70 && spo2 <= 100) ? spo2 : 0;
        printf("[hy-sensor] 心率=%d bpm | 自相关=%d(相关度0.%03d) 峰值=%d(间隔%d个) 本窗=%d%s | "
               "幅度=%d 漂移=%d | Maxim=%d/%d 血氧=%d%%(%d)\r\n",
               g_hy.hr, hr_ac, corr, hr_pk, ivn, est, moving ? " 运动中" : "",
               amp, drift, hr, (int)hr_valid, g_hy.spo2, (int)spo2_valid);
    }
}

static void max_push(uint32_t red, uint32_t ir)
{
    if (s_fill >= HY_BUF_LEN) {
        hr_recalc();
        memmove(s_ir, s_ir + HY_BUF_DROP, sizeof(uint32_t) * (HY_BUF_LEN - HY_BUF_DROP));
        memmove(s_red, s_red + HY_BUF_DROP, sizeof(uint32_t) * (HY_BUF_LEN - HY_BUF_DROP));
        s_fill = HY_BUF_LEN - HY_BUF_DROP;
    }
    s_ir[s_fill] = ir;
    s_red[s_fill] = red;
    s_fill++;
    g_hy.samples = s_fill;
}

/* ============================================================
 * 八、任务主体
 * ============================================================ */

#define HY_TICK_MS      10
#define HY_ACC_DIV      1               /* 10ms -> 100Hz（摔倒冲击靠这个抓尖峰） */

static void sensor_bring_up(void)
{
    int i;
    int alive;
    int cand_ok = -1;

    memset(&g_hy, 0, sizeof(g_hy));
    g_hy.bus = -1;
    g_hy.scl_pin = 16;
    g_hy.sda_pin = 15;

    printf("\r\n[hy-sensor] ================ 传感器点亮 BEGIN ================\r\n");

    /* --- A 段：先看线，再动总线 --- */
    g_hy.scl_idle = pin_level(16, PIN_PULL_TYPE_UP);
    g_hy.sda_idle = pin_level(15, PIN_PULL_TYPE_UP);
    printf("[hy-sensor] A 段 空闲电平 SCL=IO16:%d SDA=IO15:%d\r\n", g_hy.scl_idle, g_hy.sda_idle);

    if (g_hy.sda_idle == 0) {
        printf("[hy-sensor] SDA 空闲为低，做 9 时钟总线恢复...\r\n");
        g_hy.sda_dead = (bus_recover(16, 15) != 0) ? 1 : 0;
        printf("[hy-sensor] 恢复结果: %s\r\n", g_hy.sda_dead ? "仍为低（外部硬短）" : "已释放为高");
        if (!g_hy.sda_dead) {
            g_hy.sda_idle = 1;
        }
    }
    uapi_pin_set_pull((pin_t)16, PIN_PULL_TYPE_DISABLE);
    uapi_pin_set_pull((pin_t)15, PIN_PULL_TYPE_DISABLE);

    /* --- B 段：三组引脚依次试 --- */
    printf("[hy-sensor] B 段 依次尝试 I2C 引脚组合\r\n");
    for (i = 0; i < (int)(sizeof(s_cands) / sizeof(s_cands[0])); ++i) {
        alive = bus_try(&s_cands[i], 1);
        if (alive == 1) {
            cand_ok = i;
            break;
        }
        if (alive == 0 && cand_ok < 0) {
            cand_ok = i;                /* 总线通但没人应答：留着，后面还能确认 */
        }
    }

    if (cand_ok < 0) {
        g_hy.bus = -1;
        report_bus_verdict();
        printf("[hy-sensor] ================ 点亮失败：I2C 完全不通 ================\r\n");
        return;
    }

    g_hy.bus = s_cands[cand_ok].bus;
    g_hy.scl_pin = s_cands[cand_ok].scl;
    g_hy.sda_pin = s_cands[cand_ok].sda;
    g_hy.bus_ok = 1;
    report_bus_verdict();

    /* --- C 段：初始化器件 --- */
    if (g_hy.mpu_addr == 0x68 || g_hy.mpu_addr == 0x69) {
        if (mpu_init() == 0) {
            g_hy.mpu_ok = 1;
            printf("[hy-sensor] MPU6050 就绪 (addr 0x%02X)\r\n", g_hy.mpu_addr);
        }
    } else {
        printf("[hy-sensor] MPU6050 未应答：加速度通道关闭\r\n");
    }

    if (g_hy.max_part != 0) {
        max_init();
        g_hy.max_ok = 1;
        printf("[hy-sensor] MAX3010x 就绪 (PART_ID=0x%02X, 每组 %u 字节)\r\n",
               g_hy.max_part, (unsigned)s_max_bytes);
    } else {
        printf("[hy-sensor] MAX3010x 未应答：心率血氧通道关闭\r\n");
    }

    printf("[hy-sensor] ===== VERDICT bus=%d SCL=IO%d SDA=IO%d MPU=%s MAX=%s =====\r\n",
           g_hy.bus, g_hy.scl_pin, g_hy.sda_pin,
           g_hy.mpu_ok ? "OK" : "FAIL", g_hy.max_ok ? "OK" : "FAIL");
}

void hy_sensor_task(void)
{
    uint64_t next_tick;
    int acc_div = 0;
    int print_div = 0;

    sensor_bring_up();

    if (!g_hy.mpu_ok && !g_hy.max_ok) {
        printf("[hy-sensor] 两个传感器都没起来，任务转入静默喂狗（不再重试，避免刷屏）\r\n");
        for (;;) {
            uapi_watchdog_kick();
            osal_msleep(500);
        }
    }

    printf("[hy-sensor] 开始采样：加速度 50Hz / 心率血氧约 200Hz（算法 FS=200），每 500ms 打印一行\r\n");
    next_tick = uapi_tcxo_get_ms();
    s_rate_ms = next_tick;

    for (;;) {
        uapi_watchdog_kick();

        /* --- MAX3010x：每次把 FIFO 里的样本都取干净 --- */
        if (g_hy.max_ok) {
            static int empty_ticks;
            int guard = 0;
            int got = 0;
            uint32_t red = 0;
            uint32_t ir = 0;

            while (guard++ < 8 && max_read_pair(&red, &ir)) {
                got = 1;
                s_rate_cnt++;
                s_raw_total++;
                max_push(red, ir);          /* 原始 200Hz 全量入算法，不再抽点 */
            }

            /* FIFO 卡死恢复：连续 500ms 一个样本都没有，就复位读写指针并清溢出。
             * 实测一次长打印（200 点波形）会让 16 深度 FIFO 溢出后彻底停摆，
             * buf 卡在 601、速率掉到 0Hz——重启一次才恢复。这条兜住它。 */
            if (got) {
                empty_ticks = 0;
            } else if (++empty_ticks >= 50) {
                empty_ticks = 0;
                reg_wr(MAX_ADDR, s_max_wr, 0x00);
                reg_wr(MAX_ADDR, s_max_rd, 0x00);
                reg_wr(MAX_ADDR, s_max_ovf, 0x00);
                printf("[hy-sensor] FIFO 500ms 无数据，已复位读写指针并清溢出\r\n");
            }

            /* 每 2 秒统计一次真实采样率，只用于核对（算法按 FS=200 配置） */
            {
                uint64_t now = uapi_tcxo_get_ms();

                if (now - s_rate_ms >= 2000u) {
                    s_sps = (unsigned int)((uint64_t)s_rate_cnt * 1000u / (now - s_rate_ms));
                    s_rate_cnt = 0;
                    s_rate_ms = now;
                }
            }
        }

        /* --- MPU6050 --- */
        if (++acc_div >= HY_ACC_DIV) {
            acc_div = 0;
            if (g_hy.mpu_ok) {
                int ax = 0;
                int ay = 0;
                int az = 0;

                mpu_read_acc(&ax, &ay, &az);
                g_hy.ax = ax;
                g_hy.ay = ay;
                g_hy.az = az;
                step_update(ax * ax + ay * ay + az * az);
                fall_update(ax, ay, az, uapi_tcxo_get_ms());
            }
        }

        /* --- 每 500ms 一行状态 --- */
        if (++print_div >= 50) {
            print_div = 0;
            printf("[hy-sensor] acc ax=%5d ay=%5d az=%5d mg pk=%4dmg | steps=%d | fall=%s(%d次) | "
                   "hr=%d spo2=%d | buf=%d/%d | %uHz\r\n",
                   g_hy.ax, g_hy.ay, g_hy.az, g_hy.acc_peak, g_hy.steps,
                   fall_state_name(), g_hy.fall_events,
                   g_hy.hr, g_hy.spo2, g_hy.samples, HY_BUF_LEN, s_sps);
            g_hy.acc_peak = 0;
        }

        /* 绝对时标对齐，避免累积漂移；用 msleep 让出 CPU 给低优先级任务 */
        next_tick += HY_TICK_MS;
        {
            int64_t wait = (int64_t)next_tick - (int64_t)uapi_tcxo_get_ms();

            if (wait > 0) {
                osal_msleep((uint32_t)wait);
            } else if (wait < -200) {
                next_tick = uapi_tcxo_get_ms();
            }
        }
    }
}
