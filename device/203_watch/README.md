# 203_watch —— WS63 手表扩展模块

在 WS63（HiSilicon，RISC-V + LiteOS）上，把 `200_lcd_display`、`201_accelerometers`、
`202_heart_rate_oximetry_sensor` 三个官方 demo 合成一个可穿戴应用：一块 1.28″ 圆屏 +
MPU6050 + MAX3010x，**一个固件、一个入口、两条任务**。

## 当前状态

| 部分 | 状态 | 说明 |
| --- | --- | --- |
| 编译 / 打包 / 烧录 | 通 | 脚本在 `tools/`，串口 `COM10` |
| 应用框架 | 通 | 双任务、三页面、传感器独立容错 |
| MAX3010x 心率血氧 | 通 | `PART_ID = 0x11`，实测 hr 93~153、SpO2 96~100% |
| MPU6050 加速度 | 不稳定 | v12 读到过 `WHO_AM_I = 0x68`，之后多为 `i2c read fail` |
| GC9A01 圆屏 | **未点亮** | 背光亮、画面全灰，控制器不应答 |
| 整板自检 `board_check.c` | 通 | 上电打印引脚状态、I2C、短路图、屏应答 |

屏是唯一未解决的问题。全部实测数据、每一轮排除了什么，见
[`docs/点亮排查记录.md`](docs/点亮排查记录.md)；引脚级依据（含两块板的网表）见
[`docs/硬件与引脚.md`](docs/硬件与引脚.md)。

## 硬件映射

| 外设 | 接口 | 引脚 |
| --- | --- | --- |
| 1.28″ 圆屏 GC9A01 240×240 | 软 SPI | SCL=IO7 MOSI=IO9 DC=IO10 CS=IO11 RST=IO12 |
| MPU6050 加速度计 | I2C1 @0x68 | SCL=IO16 SDA=IO15 |
| MAX3010x 心率血氧 | I2C1 @0x57 | 同上 |

> `bsp_max30102.c` 虽然叫 30102，实际写入的寄存器（0x06 模式 / 0x07 SpO2 / 0x09 LED /
> 0x02~0x04 FIFO 指针 / 0x05 FIFO 数据）全是 **MAX30100** 的地址，与原理图丝印一致。
> 不要按 30102 的地址表改。
>
> 屏的时钟脚存在争议：按芯片引脚定义是 IO7，但底板网表里 J1-8/J2-8 都接 IO6。
> 详见 `docs/硬件与引脚.md` 的「时钟到底在哪个脚」。

## 任务与节奏

```
watch_sensor_task  (prio 20)          watch_ui_task (prio 24)
  ├─ MAX3010x  每 10ms 读 1 组  100Hz    ├─ LCD 局部刷新 100ms
  ├─ MPU6050   每 20ms 读 1 组   50Hz    ├─ 翻手腕切页（Z < -0.6g）
  ├─ 计步状态机                          └─ 无加速度计时 6s 自动轮播
  └─ 攒满 500 点算一次 HR/SpO2
```

采样用 `uapi_tcxo_get_ms()` 做绝对时标对齐，不用 `osal_mdelay` 累加，
避免漂移到 `algorithm.h` 的 `FS=100` 之外导致心率算错。

## 三个页面

1. **WATCH**：开机时长 HH:MM:SS + 步数 + 心率（无 RTC，时间为上电计时）
2. **HEART**：大字心率 + SpO2 + 脉搏柱状图
3. **MOTION**：三轴加速度（mg→g）+ 合加速度 + 步数

翻手腕（屏幕朝下）切下一页；加速度计离线时每 6 秒自动轮播。

## 容错设计

每个传感器都有独立在线标志，任一失效不影响其他部分：

- MPU6050 探测 `WHO_AM_I == 0x68`，失败则运动页显示 `MPU6050 OFFLINE`
- MAX3010x 探测 `PART_ID`（0x11=30100 / 0x15=30102），失败则心率显示 `---`
- 手指没放上去时用 IR 幅度兜底（`ir_max < 5000` 或摆动 < 100 视为无手指），
  避免算法在平信号上输出离谱数值

## 整板自检

`board_check.c` 在上电、起任务之前跑一遍，单独占 CPU、单独占 I2C 引脚，
避免和传感器任务抢总线。分五段：

| 段 | 测什么 | 怎么看 |
| --- | --- | --- |
| 1 | 引脚清单 | 哪些脚是 Flash 总线（禁止当 GPIO）、哪些是外设 |
| 2 | 三态 + 驱动回读 | `up=1 dn=0` 高阻正常；`1/1` 外部上拉；`0/0` 外部拉低；`drv=10` 才能推电平 |
| 3 | I2C 位翻转扫描 | 已知有假阴性，仅作参考（正式驱动读 `PART_ID` 才是准的） |
| 3b | 短路图 | 9 个候选脚两两配对，驱动一根读另一根，报出直连的脚对 |
| 4 | RDDID(0x04) 全组合扫描 | 屏会主动驱动 SDA 回 4 字节；只有「能浮高」的脚才允许当 SDA |
| 5 | 肉眼终审 | 9 种引脚组合各做一次完整初始化 + 整屏填红，每组停 2 秒 |

第 4 段依赖屏把 SDA 反向驱动；有些模组在信号线上串了 1 kΩ、或 SDA 侧只做了单向
电平转换，读回永远是 FF，此时第 4 段会给出**假阴性**。所以必须有第 5 段兜底：
不读、只写，用眼睛看。

## 编译

```bash
# 1. 只留 203 这个 demo 开关（避免多个 app_run）
#    src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config
#    CONFIG_SAMPLE_SUPPORT_203_WATCH=y

# 2. 增量编译（不要用 -c 全量模式，见下方「已知坑」）
cd /d/ws/src && source /d/ws63/env.sh
python build.py -nhso -j8 ws63-liteos-app
```

产物：`src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`

`tools/build.sh` 是本次用的一键脚本（编译 + 抓错误 + 打日志）。

## 烧录与抓日志

```bash
python tools/pack_fw.py                                   # 打成本仓库的 .fwpkg
python tools/flash.py COM10 6 <fwpkg> 3600                # 烧录，需按 RST 进入 ROM 下载模式
python tools/capture.py COM10 60 boot.log                 # 抓 60 秒串口
python tools/watch_boot.py COM10 1800 boot.log            # 常驻等开机自检，抓到后再录 75 秒
```

WS63 的下载模式在芯片内部 ROM：**只有复位后最初几十毫秒**接受 `EF BE AD DE` 并进入
传输。所以必须让「脚本开始发魔术字」和「按下复位」在时间上重叠。`flash.py` 的
第 4 个参数就是握手窗口，给大一点（如 3600）就不用赶时间。

## 已知坑

1. **`build.sh` 的 `-c` 全量模式会卡死在 `ar.exe`**
   app 构建会并行拉起 flashboot 子构建，两者抢 `output\ws63` 同一棵树，
   `ar.exe` 零 CPU 挂住十几分钟。绕法：先 `build.py -nhso -c ws63-flashboot`，
   再 `build.py -nhso ws63-liteos-app` 增量编。
2. **并发度太高也会挂**：`ninja -j20` 时最后一批 `gcc` 全部零 CPU 卡住，降到 `-j8` 正常。
3. **加新 demo 目录必须同时改三处**：`demo/CMakeLists.txt` 的
   `add_subdirectory_if_exist`、`demo/Kconfig` 的 config 项、target 的 `.config` 开关。
4. **IO0~IO5 是 QSPI0 主 Flash 总线**，写成 GPIO 会直接硬 fault（不是死机，是取指失败）。
   自检里所有引脚扫描都必须避开这一段。

## 文件

| 文件 | 来源 |
| --- | --- |
| `main.c` / `watch_ui.c` / `watch_sensor.c` / `board_check.c` | 本次新写 |
| `lcd.c` `lcd_init.c` `lcd.h` `lcd_init.h` `lcdfont.h` | 拷自 `200_lcd_display`，`lcd_init` 改为运行时可换引脚 |
| `bsp_iic.c` `bsp_iic.h` | 拷自 `202`（`201` 那份有运算符优先级 bug） |
| `algorithm.c` `algorithm.h` | 拷自 `202`（Maxim 心率血氧算法） |
| `bsp_mpu6050.c/h` `bsp_max30102.c/h` `bsp_max30102_blood.c/h` | 拷自 `201`/`202`，**未参与编译**，仅作参考 |

`lcd_init` 的引脚用全局变量 `g_lcd_clk_pin` / `g_lcd_mosi_pin` / `g_lcd_dc_pin` /
`g_lcd_cs_pin` / `g_lcd_rst_pin`，由 `LCD_SetPins()` / `LCD_SetRstPin()` 在运行时改，
这样自检可以在不重编译的情况下把 9 个候选脚全试一遍。

本目录不含 SDK、不含固件二进制，按上面的步骤自行构建。
