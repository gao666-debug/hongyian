/*
 * 手表扩展模块 - 传感器层
 *
 * 只用一个任务跑 I2C，LCD 的软 SPI 在 UI 任务里，两者不抢总线。
 *
 * 采样节奏（用 tcxo 校准，不依赖 mdelay 的精度）：
 *   每 10ms : MAX3010x 读 1 组 (red, ir)  -> 100Hz，对齐 algorithm.h 的 FS=100
 *   每 20ms : MPU6050  读 1 组 (x, y, z)   -> 50Hz
 *
 * 心率血氧每攒满 500 点（5 秒）算一次，之后滑动 200 点（2 秒）复算一次。
 */
#include "watch_sensor.h"

#include "bsp_iic.h"
#include "algorithm.h"

#include "common_def.h"
#include "soc_osal.h"
#include "watchdog.h"
#include "tcxo.h"

#include <stdio.h>
#include <string.h>

/* ---------------- I2C 连接 ---------------- */
#define WATCH_I2C_BUS       1
#define WATCH_I2C_SCL       15      /* 与板载 OLED 示例一致：SCL=15 */
#define WATCH_I2C_SDA       16      /* SDA=16 */
#define WATCH_I2C_BAUD      100000

/* ---------------- MPU6050 ---------------- */
#define MPU_ADDR            0x68
#define MPU_REG_SMPLRT_DIV  0x19
#define MPU_REG_CONFIG      0x1A
#define MPU_REG_GYRO_CFG    0x1B
#define MPU_REG_ACCEL_CFG   0x1C
#define MPU_REG_ACCEL_OUT   0x3B
#define MPU_REG_PWR_MGMT_1  0x6B
#define MPU_REG_WHO_AM_I    0x75
#define MPU_LSB_PER_G       16384   /* 必须配合 ACCEL_CFG=0x00(±2g) */

/* ---------------- MAX3010x（原理图丝印 MAX30100） ---------------- */
#define MAX_ADDR            0x57
#define MAX_REG_INTR_STATUS 0x00
#define MAX_REG_INTR_ENABLE 0x01
#define MAX_REG_FIFO_WR     0x02
#define MAX_REG_OVF         0x03
#define MAX_REG_FIFO_RD     0x04
#define MAX_REG_FIFO_DATA   0x05
#define MAX_REG_MODE        0x06
#define MAX_REG_SPO2_CFG    0x07
#define MAX_REG_LED_CFG     0x09
#define MAX_REG_PART_ID     0xFF

#define MAX_PART_30100      0x11
#define MAX_PART_30102      0x15

/* ---------------- 采样参数 ---------------- */
#define SAMPLE_TICK_MS      10
#define WATCH_BUF_LEN       500
#define WATCH_BUF_DROP      200

/* 计步门限：1g = 1000mg，所以 mg^2 单位下 1.69e6 = 1.3g^2，0.64e6 = 0.8g^2 */
#define STEP_HI_M2          1690000
#define STEP_LO_M2          640000

watch_state_t g_watch;

static uint32_t s_ir[WATCH_BUF_LEN];
static uint32_t s_red[WATCH_BUF_LEN];
static int s_fill;

/* ================= 基础 I2C 读写 ================= */

static int i2c_wr(uint8_t dev, uint8_t reg, uint8_t val)
{
    uint8_t cmd[2] = {reg, val};
    return (int)bsp_iic_master_write_data(WATCH_I2C_BUS, dev, cmd, 2);
}

static int i2c_rd(uint8_t dev, uint8_t reg, uint8_t *buf, uint32_t len)
{
    uint8_t cmd[1] = {reg};
    int ret = (int)bsp_iic_master_write_data(WATCH_I2C_BUS, dev, cmd, 1);
    if (ret != 0) {
        return ret;
    }
    return (int)bsp_iic_master_read_data(WATCH_I2C_BUS, dev, buf, len);
}

/* ================= MPU6050 ================= */

static int mpu_probe(void)
{
    uint8_t id = 0;
    if (i2c_rd(MPU_ADDR, MPU_REG_WHO_AM_I, &id, 1) != 0) {
        printf("[watch] MPU6050 i2c read fail\r\n");
        return 0;
    }
    printf("[watch] MPU6050 WHO_AM_I = 0x%02x\r\n", id);
    return (id == 0x68) ? 1 : 0;
}

static void mpu_init(void)
{
    i2c_wr(MPU_ADDR, MPU_REG_PWR_MGMT_1, 0x80);   /* 软复位 */
    osal_mdelay(100);
    i2c_wr(MPU_ADDR, MPU_REG_PWR_MGMT_1, 0x00);   /* 解除休眠，用内部 8M 时钟 */
    i2c_wr(MPU_ADDR, MPU_REG_SMPLRT_DIV, 0x04);   /* 采样率 1k/(1+4) = 200Hz */
    i2c_wr(MPU_ADDR, MPU_REG_CONFIG, 0x03);       /* DLPF 44Hz，抑制手抖高频 */
    i2c_wr(MPU_ADDR, MPU_REG_GYRO_CFG, 0x18);     /* ±2000dps，本模块不用陀螺仪 */
    i2c_wr(MPU_ADDR, MPU_REG_ACCEL_CFG, 0x00);    /* ±2g -> 16384 LSB/g，必须和 MPU_LSB_PER_G 一致 */
    osal_mdelay(50);
}

static void mpu_read_acc(int *ax, int *ay, int *az)
{
    uint8_t buf[6] = {0};
    int16_t raw[3];

    if (i2c_rd(MPU_ADDR, MPU_REG_ACCEL_OUT, buf, 6) != 0) {
        return;
    }
    raw[0] = (int16_t)((buf[0] << 8) | buf[1]);
    raw[1] = (int16_t)((buf[2] << 8) | buf[3]);
    raw[2] = (int16_t)((buf[4] << 8) | buf[5]);

    /* 转成 mg：raw * 1000 / 16384 */
    *ax = ((int)raw[0] * 1000) / MPU_LSB_PER_G;
    *ay = ((int)raw[1] * 1000) / MPU_LSB_PER_G;
    *az = ((int)raw[2] * 1000) / MPU_LSB_PER_G;
}

/* 计步状态机：一上升沿（>1.3g）再一回落（<0.8g）记一步，2 次之间至少 250ms */
static void step_update(int m2)
{
    static int armed;
    static uint64_t last_ms;

    if (m2 > STEP_HI_M2 && !armed) {
        armed = 1;
    } else if (armed && m2 < STEP_LO_M2) {
        uint64_t now = uapi_tcxo_get_ms();
        if (now - last_ms > 250) {
            g_watch.steps++;
            last_ms = now;
        }
        armed = 0;
    }
}

/* ================= MAX3010x ================= */

static int max_probe(void)
{
    uint8_t id = 0;
    if (i2c_rd(MAX_ADDR, MAX_REG_PART_ID, &id, 1) != 0) {
        printf("[watch] MAX3010x i2c read fail\r\n");
        return 0;
    }
    printf("[watch] MAX3010x PART_ID = 0x%02x (0x11=30100 / 0x15=30102)\r\n", id);
    return (id == MAX_PART_30100 || id == MAX_PART_30102) ? 1 : 0;
}

static int max_init(void)
{
    uint8_t mode = 0;

    i2c_wr(MAX_ADDR, MAX_REG_MODE, 0x40);        /* RESET */
    osal_mdelay(50);

    i2c_wr(MAX_ADDR, MAX_REG_INTR_ENABLE, 0xF0);
    i2c_wr(MAX_ADDR, MAX_REG_INTR_STATUS, 0x00);
    i2c_wr(MAX_ADDR, MAX_REG_MODE, 0x03);        /* SpO2 模式（RED + IR） */
    i2c_wr(MAX_ADDR, MAX_REG_SPO2_CFG, 0x47);    /* 高分辨率使能，100Hz，1600us 脉宽 */
    i2c_wr(MAX_ADDR, MAX_REG_LED_CFG, 0x33);     /* RED_PA=3, IR_PA=3 */

    i2c_wr(MAX_ADDR, MAX_REG_FIFO_WR, 0x00);
    i2c_wr(MAX_ADDR, MAX_REG_OVF, 0x00);
    i2c_wr(MAX_ADDR, MAX_REG_FIFO_RD, 0x0F);     /* 让 FIFO 尽快攒够再读，避开开头空点 */
    osal_mdelay(50);

    if (i2c_rd(MAX_ADDR, MAX_REG_MODE, &mode, 1) != 0) {
        printf("[watch] MAX3010x config readback fail\r\n");
        return 0;
    }
    printf("[watch] MAX3010x MODE = 0x%02x\r\n", mode);
    return (mode == 0x03) ? 1 : 0;
}

static void max_read_one(uint32_t *red, uint32_t *ir)
{
    uint8_t buf[4] = {0};
    uint8_t wr = 0;
    uint8_t rd = 0;

    /* MAX30100 的 FIFO 空着去读会 NAK，10ms 轮一次必然大量失败并刷屏。
     * 先比读写指针，确认有样本再读。 */
    if (i2c_rd(MAX_ADDR, MAX_REG_FIFO_WR, &wr, 1) != 0) {
        return;
    }
    if (i2c_rd(MAX_ADDR, MAX_REG_FIFO_RD, &rd, 1) != 0) {
        return;
    }
    if (wr == rd) {
        return;                      /* FIFO 空，本轮跳过 */
    }

    if (i2c_rd(MAX_ADDR, MAX_REG_FIFO_DATA, buf, 4) != 0) {
        return;
    }
    *red = ((uint32_t)buf[0] << 8) | buf[1];
    *ir  = ((uint32_t)buf[2] << 8) | buf[3];
}

static void hr_recalc(void)
{
    int spo2 = 0;
    int hr = 0;
    char spo2_valid = 0;
    char hr_valid = 0;
    uint32_t ir_min = 0xFFFFFFFF;
    uint32_t ir_max = 0;
    int i;

    maxim_heart_rate_and_oxygen_saturation(s_ir, WATCH_BUF_LEN, s_red,
                                           &spo2, &spo2_valid, &hr, &hr_valid);

    /* 手指没放上去时 IR 几乎是平的，算法会给出离谱值，这里用信号幅度再兜一道 */
    for (i = 0; i < WATCH_BUF_LEN; i++) {
        if (s_ir[i] < ir_min) ir_min = s_ir[i];
        if (s_ir[i] > ir_max) ir_max = s_ir[i];
    }
    if (ir_max < 5000 || (ir_max - ir_min) < 100) {
        g_watch.hr = 0;
        g_watch.spo2 = 0;
        g_watch.ir_pct = 0;
        return;
    }

    g_watch.ir_pct = (int)(((ir_max - ir_min) * 100) / (ir_max ? ir_max : 1));
    if (g_watch.ir_pct > 100) {
        g_watch.ir_pct = 100;
    }

    if (hr_valid && hr > 30 && hr < 180) {
        g_watch.hr = hr;
    } else {
        g_watch.hr = 0;
    }
    if (spo2_valid && spo2 >= 70 && spo2 <= 100) {
        g_watch.spo2 = spo2;
    } else {
        g_watch.spo2 = 0;
    }
    printf("[watch] hr=%d spo2=%d ir_base=%u swing=%u\r\n",
           g_watch.hr, g_watch.spo2, (unsigned)ir_max, (unsigned)(ir_max - ir_min));
}

static void max_push(uint32_t red, uint32_t ir)
{
    if (s_fill >= WATCH_BUF_LEN) {
        hr_recalc();
        /* 丢掉最老的 200 点，保留最近 300 点，下次再补 200 点继续算 */
        memmove(s_ir, s_ir + WATCH_BUF_DROP, sizeof(uint32_t) * (WATCH_BUF_LEN - WATCH_BUF_DROP));
        memmove(s_red, s_red + WATCH_BUF_DROP, sizeof(uint32_t) * (WATCH_BUF_LEN - WATCH_BUF_DROP));
        s_fill = WATCH_BUF_LEN - WATCH_BUF_DROP;
    }
    s_ir[s_fill] = ir;
    s_red[s_fill] = red;
    s_fill++;
}

/* ================= 任务主体 ================= */

void watch_sensor_task(void)
{
    uint64_t next_tick;
    int acc_div = 0;

    printf("\r\n[watch] sensor task start\r\n");

    memset(&g_watch, 0, sizeof(g_watch));

    if (bsp_iic_master_init(WATCH_I2C_SCL, WATCH_I2C_SDA, WATCH_I2C_BAUD, 0) != 0) {
        printf("[watch] i2c master init fail\r\n");
    }
    osal_mdelay(100);

    if (mpu_probe()) {
        mpu_init();
        g_watch.acc_ok = 1;
        printf("[watch] MPU6050 ready\r\n");
    }
    if (max_probe()) {
        if (max_init()) {
            g_watch.hr_ok = 1;
            printf("[watch] MAX3010x ready\r\n");
        }
    }

    next_tick = uapi_tcxo_get_ms();

    while (1) {
        uapi_watchdog_kick();

        /* --- MAX3010x 100Hz --- */
        if (g_watch.hr_ok) {
            uint32_t red = 0;
            uint32_t ir = 0;
            max_read_one(&red, &ir);
            if (red != 0 || ir != 0) {
                max_push(red, ir);
            }
        }

        /* --- MPU6050 50Hz --- */
        acc_div++;
        if (acc_div >= 2) {
            acc_div = 0;
            if (g_watch.acc_ok) {
                int ax = 0;
                int ay = 0;
                int az = 0;
                mpu_read_acc(&ax, &ay, &az);
                g_watch.ax = ax;
                g_watch.ay = ay;
                g_watch.az = az;
                step_update(ax * ax + ay * ay + az * az);
            }
        }

        g_watch.been_on = (int)((uapi_tcxo_get_ms()) / 1000);

        /* 用绝对时标对齐 10ms，避免累积漂移。
         * 必须用 osal_msleep：osal_mdelay 是自旋忙等，会让低优先级的 UI 任务饿死 */
        next_tick += SAMPLE_TICK_MS;
        {
            int64_t wait = (int64_t)next_tick - (int64_t)uapi_tcxo_get_ms();
            if (wait > 0) {
                osal_msleep((uint32_t)wait);
            } else if (wait < -200) {
                next_tick = uapi_tcxo_get_ms();   /* 落后太多就重新对齐 */
            }
        }
    }
}
