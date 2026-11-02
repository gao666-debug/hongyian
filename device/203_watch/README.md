# 手表 · 说明（WS63 穿戴节点）

> 板子：**WS63 星闪开发板** + **WEAR 扩展板**（1.28″ 圆屏 + MPU6050 六轴 + MAX3010x 心率血氧）
> 用途：心率 / 血氧 / 计步 / 跌倒检测，穿戴在被监护人身上
> 固件：`203_watch`（`src/hy_sensor_main.c` 为入口）

---

## 一、源码（`src/`，26 个文件）

| 文件组 | 职责 |
|---|---|
| `hy_sensor_main.c` | 应用入口，任务编排 |
| `hy_sensor.c/h`、`algorithm.c/h` | 采样、跌倒判定算法 |
| `bsp_mpu6050.c/h` | 六轴加速度 |
| `bsp_max30102*.c/h`、`bsp_max30102_blood.c/h` | 心率 / 血氧 |
| `bsp_iic.c/h` | I2C 总线 |
| `hy_net.c/h` | WiFi 连接 |
| `hy_mqtt.c/h` | MQTT 客户端 |
| `hy_cloud.c/h` | 华为云上报（属性上报组包） |
| `hy_cloud_cfg.example.h` | **配置模板**（真实配置 `hy_cloud_cfg.h` 不入库，含密钥） |
| `hy_sha256.c/h` | 华为云 clientId 的 HMAC-SHA256 校验 |
| `board_check.c/h` | 整板上电自检（引脚三态、I2C、短路图、LCD 扫描） |

配置项与真实值对照：

```c
// 照 hy_cloud_cfg.example.h 复制一份命名为 hy_cloud_cfg.h，然后填真实值
#define HY_HW_DEVICE_ID    "demo-watch"
#define HY_HW_SERVICE_ID   "11"
#define HY_WIFI_SSID       "你的WiFi"
#define HY_WIFI_PASS       "你的密码"
#define HY_HW_DEVICE_SECRET "控制台给的设备密钥"
```

> ⚠️ 本包**只有 `hy_cloud_cfg.example.h`（模板）**，没有真实配置。
> 注意 `HY_HW_DEVICE_ID` 末尾的 `_DEVICE_ID` 是模板占位符照抄留下的，
> **云端和固件必须完全一致**，不要单方面改成"好看的名字"（详见 `../01_总览/` 第六节说明）。

---

## 二、编译

```bash
export SDK_ROOT=/d/ws63_dev/fbb_ws63-master   # 默认是 /d/ws/src，按本机改
export PYTHON=/d/ws63_dev/tools/python311/python.exe
sh tools/build.sh
```

### ⚠️ 关键：每次编译前必须删 `libsamples.a`

```bash
rm -f $SDK_ROOT/src/application/samples/libsamples.a
```

原因：demo 目录的白名单只决定「编译哪些 `.c`」，**旧的 `.obj` 会留在静态库里**。
换过入口文件后如果不删，镜像里会出现**两个 `app_run`**，跑起来是旧应用，
表现为"烧录成功但屏幕/功能不对"。这个坑踩过多次。

另外两个通用约束（与环境节点相同）：
- 编译不能带 hso（`-nhso`）
- 输出不能接 `| tee`（会死锁）

---

## 三、烧录

```bash
python tools/flash.py COM6 6 240        # 端口 / 段号 / 超时秒数
```

流程是 ROM 握手 → YMODEM 传 loaderboot → 逐段写镜像。

### ⚠️ 烧录时必须拔掉 WEAR 扩展板

GPIO1 被 WEAR 板拉高就**进不了 ROM 下载模式**，必须先拔。
烧完再插回去。

`docs/圆屏固定引脚测试_给队友.md` 里有相应的引脚扫描方法。

---

## 四、引脚

### 圆屏（WEAR 板 `LCD1` 座，7 针直插，不可改线）

| 屏脚 | 网络 | WS63 引脚 |
|---|---|---|
| 1 | VCC | 挂板电源（模组背面自带 LDO） |
| 2 | GND | — |
| 3 | SPI0CK | **IO7** |
| 4 | SPI0MOSI | **IO9** |
| 5 | DC | **IO10** |
| 6 | SPI0CS | **IO11** |
| 7 | RST | **IO12** |

> 以 `docs/圆屏固定引脚测试_给队友.md` 为准（`docs/硬件与引脚.md` 前半部分是已被推翻的历史推断）。

### 传感器（I2C）

| 器件 | 总线 | 引脚 |
|---|---|---|
| MPU6050（六轴） | I2C | J3-1 = SCL，J3-2 = SDA |
| MAX3010x（心率血氧） | I2C | 同上（同一条总线） |

EXB 底板还有三个单针测试点：`J6-1 = IO3`、`J7-1 = IO7`、`J8-1 = IO9`。

---

## 五、上报

| 项 | 值 |
|---|---|
| device_id | `demo-watch` |
| 产品 ID | `6ae6a5c1e094d615927efdbd`（产品名 `22`） |
| **service_id** | **`11`** |
| 上报频率 | **10 秒** |
| 上报通路 | **只有属性上报** `properties/report` → 后端 `resource=device.property` |

完整报文样例、字段单位（`accel_x/y/z` 是 **mg**，`heart_rate=0` 表示没测到）
见 `../01_总览/系统总览与设备清单.md` 第 3.3 节。

> 注意：手表**不发自定义消息**，所以后端必须支持 `device.property` 才能收它的数据
> （后端 1.4.0 已支持）。

---

## 六、命令下发：不支持

手表只上报，不订阅命令 Topic。App 上不要给它做「立即测量」这类按钮。

---

## 七、已知问题

| 问题 | 状态 |
|---|---|
| **GC9A01 圆屏未点亮**（背光亮、画面全灰、控制器不应答） | ❌ 未解决。排查停在「时钟脚到底是哪一个」，详见 `docs/点亮排查记录.md` |
| 心率/血氧为 0 | 正常现象，表示手指没贴到传感器，不是故障 |
| `steps` / `fall_count` 重启归零 | 设计如此，历史统计要用后端的小时桶 |
