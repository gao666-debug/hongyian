> 仓库配置：先执行 `cp src/cfg.example.h src/cfg.h`，在本机填写配置。`cfg.h` 已忽略，请勿提交真实凭据。
> `CFG_IOTDA_PASSWORD` 由代码直接作为 MQTT password 使用，不能直接填原始设备密钥；须填写与 clientId 匹配的签名结果。跨小时连接成功不代表永久有效。

# 环境节点 · 说明（WS63 星闪）

> 板子：**WS63 星闪开发板**（fbb_ws63 SDK）+ E53 扩展板
> 用途：光照 / 烟雾 / 人体 / 门磁 四路环境采集，本地开门提示灯
> 固件名：`920_home_cloud`，版本 `ws63-home-1.0`

---

## 一、源码（`src/`，9 个文件，完整）

| 文件 | 职责 |
|---|---|
| `cfg.h` | **所有配置集中在这里**：WiFi、IoTDA 三元组、引脚、上报节奏 |
| `main.c` | 任务编排，只管流程 |
| `net.c/h` | WiFi STA 连接 + SNTP 校时 |
| `sensors.c/h` | 四路采集 + 开门提示灯逻辑 |
| `iotda.c/h` | MQTT 连接与上报（属性 + 消息双发） |
| `CMakeLists.txt` | 编译入口 |

> ⚠️ `cfg.h` **已脱敏**：WiFi SSID/密码、IoTDA 设备密钥都是占位符，
> 烧录前必须换成真实值。`device_id` / `client_id` / `service_id` 保留了原值，用于核对配置。

---

## 二、编译

前置：SDK 在 `D:/ws63_dev/fbb_ws63-master`，且有 `/d/ws63_dev/env.sh` 配置文件。

```bash
# 增量编译（约 1-3 分钟）
/d/ws63_dev/build.sh

# 清理后全量编译（约 15-25 分钟）
/d/ws63_dev/build.sh clean
```

实际执行的是：

```bash
cd /d/ws63_dev/fbb_ws63-master/src
python build.py -nhso ws63-liteos-app > /d/ws63_dev/_logs/build.log 2>&1
```

### ⚠️ 两个必须遵守的坑

1. **必须带 `-nhso`**
   HSO 数据库生成脚本 `mk_hso_prim_xml.py` 在这类机器上会永久卡死。
   它只给 IDE 做符号索引，跟固件无关，跳过不影响功能。

2. **输出绝对不能接 `| tee`**
   `build.py` 会拉起 ninja，再经 cmd.exe 派生 ar/gcc，整条链路输出一旦走管道，
   下游缓冲区写满就**死锁**（表现为只剩一个 cmd.exe 空转、`.obj/.a` 不再增长）。
   要看进度直接 `tail -f /d/ws63_dev/_logs/build.log`。

另外：**PATH 里不能有 ccache**，否则编译会挂死。

产物：`fbb_ws63-master/src/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg`

---

## 三、烧录

```bash
/d/ws63_dev/burn.sh COM6        # 串口按实际改（本机开发板是 COM6，CH340）
```

### ⚠️ 关键操作

看到串口打印 **"Please reset the device"** 后，在板子上按 **一次** `RST` 键。

**之后绝对不能再按** —— 中途复位会打断 ymodem 传输，报
`ACK receiving timed out / Send ymodem start frame failed`，只能重来。

---

## 四、引脚与接线

全部定义在 `src/cfg.h`：

| 外设 | 接口类型 | 引脚 / 通道 | 宏 |
|---|---|---|---|
| 光敏 | ADC | **通道 5** | `CFG_LIGHT_ADC_CH 5` |
| 烟雾 | ADC | **通道 0** | `CFG_SMOKE_ADC_CH 0` |
| 人体感应（PIR） | GPIO | **GPIO14** | `CFG_PIR_PIN 14` |
| 门磁 | GPIO | **GPIO6** | `CFG_DOOR_PIN 6` |
| 开门提示灯 | GPIO | **GPIO10**（扩展板灯组） | `CFG_LAMP_PIN 10` |
| 主板灯带接口 | GPIO | **GPIO9**（本板实测点不亮，代码里拉低） | `CFG_TAIL_LED_PIN 9` |

**门磁极性**：磁铁靠近时读**高电平** → 判定为「门关闭」。
宏 `CFG_DOOR_MAGNET_NEAR_IS_HIGH = 1`，去抖 `CFG_DOOR_DEBOUNCE = 3`（连续 3 次一致才认）。

**提示灯**：开门点亮，关门后继续亮 **15 秒**（`CFG_LAMP_HOLD_MS 15000`）。

---

## 五、上报

| 项 | 值 |
|---|---|
| device_id | `demo-environment` |
| 产品 ID | `6ae6af2518855b39c55ba694`（产品名 `33`） |
| **service_id** | **`11`** |
| 上报频率 | **10 秒**定时全量；门磁/人体变化时立即补发（最小间隔 1 秒防抖） |
| 上报通路 | **双发**：属性上报 `properties/report` + 自定义消息 `messages/up` |

**两条通路缺一不可**：

- 属性上报 → 刷新 IoTDA 设备影子（控制台能看到），转发给后端时 `resource=device.property`
- 自定义消息 → 转发给后端时 `resource=device.message`

后端原本只接受 `device.message`，所以只发属性上报的话，后端收不到任何数据。

完整报文样例、字段单位（`light`/`smoke` 是 **ADC 电压 mV**，不是百分比）见
`../01_总览/系统总览与设备清单.md` 第 3.1 / 3.2 节。

---

## 六、命令下发：不支持

这块板**只上报，不接收命令**（没订阅命令 Topic）。后端如果强发，会等 15 秒超时返回 `state=unknown`。

---

## 七、排障（按顺序）

| 现象 | 查什么 |
|---|---|
| 串口没有 `[IOTDA] report ...` | WiFi 没连上，或 `CFG_WIFI_SSID/PASS` 不对（**只支持 2.4G，5G 热点连不上**） |
| 有 report 但平台没数据 | IoTDA「消息跟踪」看 `service ID is not defined` → 物模型服务 ID 与 `CFG_IOTDA_SERVICE_ID` 不一致 |
| 平台有数据但后端没入库 | 转发规则的 `where` 没覆盖这台设备（见 `WS63上云接入与运维说明.md` 第 2.3 节） |
| 后端 `inbox` 涨但数据不更新 | `id` 重复被去重，或 `boot_id` 没有递增（被当成旧消息丢弃） |
| 编译卡住 | 检查是否带 `-nhso`、PATH 是否有 ccache、是否误用了 `\| tee` |

更完整的华为云侧配置（产品/物模型/转发规则/clientId 时间戳）见同目录
`WS63上云接入与运维说明.md`。
