# 203_watch —— WS63 手表扩展模块

在 WS63（HiSilicon，RISC-V + LiteOS）上，把 `200_lcd_display`、`201_accelerometers`、
`202_heart_rate_oximetry_sensor` 三个官方 demo 合成一个可穿戴节点：1.28″ 圆屏 +
MPU6050 + MAX3010x，**一个固件、一个入口、两条任务**，并把手环数据上报到华为云 IoTDA。

## 当前状态

| 部分 | 状态 | 说明 |
| --- | --- | --- |
| 编译 / 打包 / 烧录 | 通 | `tools/` 一键脚本，串口 `COM10` |
| GC9A01 圆屏 240×240 | 通 | `lcd_bringup.c` 固定引脚初始化，SPI 提速 |
| MPU6050 加速度 | 通 | I2C1 @0x68/0x69，±8g（4096 LSB/g），100 Hz |
| MAX3010x 心率血氧 | 通 | @0x57，200 Hz；静息心率 53~60 bpm，42 窗 39 有效 |
| 计步 | 通 | 模长过零判定，1.3g 抬脚 / 0.8g 落地，250 ms 去抖 |
| 摔倒检测 | 通 | 冲击 ≥2 g → 5 s 静置 → 疑似摔倒，6 个场景实测符合预期 |
| 华为云上云 | 通 | WiFi → DHCP → SNTP → MQTT 1883 → 每 10 s 属性上报 |
| 血氧 SpO2 | 参考 | 93~100%，未单独标定，只能看趋势 |

上云的参数、坑和验收清单见 [`docs/上云接入说明.md`](docs/上云接入说明.md)；
传感器与屏的全部排查结论见 [`docs/传感器与上云开发纪要.md`](docs/传感器与上云开发纪要.md)。

## 硬件映射

| 外设 | 接口 | 引脚 |
| --- | --- | --- |
| 1.28″ 圆屏 GC9A01 240×240 | 软 SPI | SCL=IO7 MOSI=IO9 DC=IO10 CS=IO11 RST=IO12 |
| MPU6050 加速度计 | I2C1 @0x68 | SCL=IO16 SDA=IO15 |
| MAX3010x 心率血氧 | I2C1 @0x57 | 同上 |

> `bsp_max30102.c` 虽然叫 30102，实际写入的寄存器（0x06 模式 / 0x07 SpO2 / 0x09 LED /
> 0x02~0x04 FIFO 指针 / 0x05 FIFO 数据）全是 **MAX30100** 的地址，与原理图丝印一致，不要按 30102 的地址表改。
>
> **传感器在 WEAR 板上，不在底板**：裸板扫描两个地址全 `ACK_ERR`（线序正常、器件不在），
> 插上 WEAR 立刻应答。烧录时必须拔掉 WEAR，GPIO1 被拉高会进不了 ROM 下载模式。

## 任务与节奏

```
hy_sensor_task (prio 20)                        hy_cloud_task (prio 16)
  ├─ MAX3010x 200 Hz 采样，5 s 一窗算心率血氧      ├─ WiFi 关联 + DHCP + SNTP
  ├─ MPU6050  100 Hz 采样，计步 + 摔倒状态机       ├─ MQTT 3.1.1 连接（HMAC-SHA256）
  └─ 每 1 s 打印一行状态                          └─ 每 10 s 上报 8 个属性，摔倒立刻补报
                     hy_lcd_task (prio 24)
                       └─ LCD 局部刷新，5 s 一帧
```

采样用 `uapi_tcxo_get_ms()` 做绝对时标对齐，不用 `osal_mdelay` 累加，避免漂移到
`algorithm.h` 的 `FS=200` 之外导致心率算错。

## 摔倒判定

```
冲击 ≥ 2000 mg           → FALL_WATCH（观察 500 ms 回弹期）
回弹期结束后 5 s 内
  模长峰峰 ≤ 300 mg       → FALL_ALARM（疑似摔倒，fall_state=2，fall_count+1）
  出现 ≥ 600 mg 的活动    → 作废（坐下、放桌上、拍桌子都会被排除）
```

判定为摔倒后每 5 s 复报一次，直到有人把板子拿起来（活动量回升）才回到 0。
手机/云端看到的是 `fall_state`（此刻状态）和 `fall_count`（上电以来累计次数）两个属性。

## 上云报文

```json
{"services":[{"service_id":"11","properties":{
  "heart_rate":60,"spo2":100,
  "accel_x":-188,"accel_y":-168,"accel_z":951,
  "steps":2,"fall_state":0,"fall_count":0}}]}
```

- 加速度单位 **mg**（1000 = 1 g），服务 ID 必须与产品物模型逐字一致；
- `heart_rate` / `spo2` 为 0 表示这次没测到（手指没贴上），不是真实读数。

## 编译

```bash
# 0. 配置（必做）：凭据不进仓库
cp src/hy_cloud_cfg.example.h src/hy_cloud_cfg.h   # 填 WiFi 与设备三元组

# 1. SDK 里只留 203 这个 demo（避免多个 app_run）
#    src/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config
#    CONFIG_SAMPLE_SUPPORT_203_WATCH=y
#    demo/CMakeLists.txt 白名单：hy_sensor_main|hy_sensor|hy_net|hy_mqtt|hy_sha256|hy_cloud|...

# 2. 增量编译（脚本已含"先删 libsamples.a"，别用 -c 全量）
sh tools/build.sh
```

产物：`src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`

`HONGYIAN_LCD_ONLY=ON` 可以只编圆屏点亮测试（`lcd_only_main.c`），不碰 I2C。

## 烧录与抓日志

```bash
python tools/pack_fw.py                                   # 打成本仓库的 .fwpkg
python tools/flash.py COM10 6 <fwpkg> 3600                # 拔掉 WEAR，按 RST 进 ROM 下载模式
python tools/capture.py COM10 60 boot.log                 # 抓 60 秒，端口掉线会自动重开
python tools/watch_boot.py COM10 1800 boot.log            # 常驻等开机，抓到横幅后再录 75 秒
```

## 上云自检（PC 端，先于烧板子）

```bash
python tools/hw_iotda_probe.py <域名> <设备ID> <密钥> [服务ID]   # 三元组 + 上报是否被接受
python tools/hw_iotda_shadow.py <域名> <设备ID> <密钥>           # 查设备影子，看平台到底存了什么
```

## 已知坑

1. **`build.sh` 的 `-c` 全量模式会卡死在 `ar.exe`**：app 构建与 flashboot 子构建抢同一棵
   `output\ws63`。绕法：先 `build.py -nhso -c ws63-flashboot`，再增量编 app。
2. **并发度太高也会挂**：`ninja -j20` 时最后一批 gcc 零 CPU 卡住，`-j8` 正常。
3. **换入口文件必须删 `libsamples.a`**：白名单只决定"编译哪些 .c"，旧 `.obj` 还在库里，
   会出现两个 `app_run`、镜像跑错应用。
4. **IO0~IO5 是 QSPI0 主 Flash 总线**，写成 GPIO 会直接硬 fault（取指失败）。
5. **明文 1883 强制校验时间戳**：ClientId 里带 `YYYYMMDDHH`(UTC)，所以必须先 SNTP 再连 MQTT。
6. **服务 ID 写错 = 平台静默丢弃**：连接不断、控制台仍"在线"，但物模型没有数据。
   用设备影子反查是最快的定位方式。

## 文件

| 文件 | 来源 |
| --- | --- |
| `hy_sensor_main.c` | 入口：创建传感器任务、LCD 任务、上云任务 |
| `hy_sensor.c` / `hy_sensor.h` | MPU6050 + MAX3010x 采样、心率算法、计步、摔倒状态机 |
| `hy_net.c` / `hy_net.h` | WiFi 初始化/关联、DHCP、SNTP，含 lwip 内存池自检 |
| `hy_mqtt.c` / `hy_mqtt.h` | 手写 MQTT 3.1.1 客户端（CONNECT/PUBLISH/PINGREQ） |
| `hy_sha256.c` / `hy_sha256.h` | SHA-256 / HMAC-SHA256，华为云设备签名用 |
| `hy_cloud.c` / `hy_cloud.h` | 三元组、属性上报、断线重连 |
| `hy_cloud_cfg.example.h` | 上云参数模板（复制成 `hy_cloud_cfg.h` 后填写，不进仓库） |
| `lcd_bringup.c` / `lcd_bringup.h` | 圆屏固定引脚初始化 |
| `bsp_iic.c` / `bsp_iic.h` | 拷自 `202`（`201` 那份有运算符优先级 bug） |
| `algorithm.c` / `algorithm.h` | 拷自 `202`，`FS` 改为 200 |
| `main.c` `watch_ui.c` `watch_sensor.c` `board_check.c` | 早期整板调试版，保留参考，不在默认构建里 |
| `bsp_mpu6050.c/h` `bsp_max30102*.c/h` | 拷自 `201`/`202`，未参与编译，仅作参考 |

本目录不含 SDK、不含固件二进制，按上面的步骤自行构建。
