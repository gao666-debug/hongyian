/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "su_03t.h"
#include "voice_policy.h"
#include "living.h"
#include "sntp_client.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include "safety_runtime.h"
#include "los_tick.h"

#include "los_task.h"

#include "iot_errno.h"
#include "iot_uart.h"

/* 语音模块使用串口2 / UART2 (GPIO0_B2 / GPIO0_B3) */
#define UART2_HANDLE    EUART2_M1

/* 两字节指令的前缀/参数间隔上限：超过则认为前一个字节是噪声 */
#define VOICE_PFX_WINDOW_MS 500

/* 应答播报排队数量；连播（如温湿度）靠延时分帧，避免占住串口 */
#define VOICE_SAY_MAX 6

/* 上电语音自检：烧录新词条后临时置 1（编译参数 -DSU03T_SELF_TEST=1），
 * 上电后从 4 号到 23 号逐个播报一遍，每句间隔 2 秒，约 40 秒播完，
 * 用来确认哪条没烧进去、播错或没声音。验证完请改回 0，避免每次开机都念。
 * 自检走线程独立计时，不占用 VOICE_SAY_MAX 应答队列。 */
#ifndef SU03T_SELF_TEST
#define SU03T_SELF_TEST 0
#endif

typedef struct {
    uint8_t index;
    uint8_t is_value;   /* 0=固定句帧 1=数值帧 */
    double value;
    uint64_t due;
} voice_say_t;

static voice_policy_t s_voice;
static unsigned s_medicine_mask;

/* 两字节指令的状态机：s_pfx<0 表示空闲 */
static int s_pfx = -1;
static bool s_pfx_ext;
static uint64_t s_pfx_up;

static voice_say_t s_say[VOICE_SAY_MAX];
static int s_say_n;
static int s_page = -1;
static char s_note[32];
static uint64_t s_note_up;

#if SU03T_SELF_TEST
/* 自检游标：-1 表示未开始或已播完 */
static int s_selftest_idx = -1;
static uint64_t s_selftest_due;
#endif

static void write_frame(const uint8_t *data, unsigned size)
{
    /* Prevent query and alarm tasks from interleaving complete UART frames.
     * UART is configured nonblocking; never wait for playback under this lock. */
    safety_lock();
    int n = IoTUartWrite(UART2_HANDLE, data, size);
    safety_unlock();
    if (n != (int)size) printf("[voice] UART frame incomplete %d/%u\n", n, size);
}

/* 固定句应答帧：AA 55 <index> 00 55 AA。
 * 只在模块已配置 10~23 号应答词条时发送，工厂模块下整体不发送。 */
static void send_phrase(uint8_t index)
{
#if SU03T_ACTION_VOICE_ENABLED
    uint8_t buf[6] = {0xAA, 0x55, index, 0x00, 0x55, 0xAA};
    write_frame(buf, sizeof(buf));
#else
    (void)index;
#endif
}

static void say_push(uint8_t index, uint8_t is_value, double value, uint64_t now, unsigned delay)
{
    /* 数值帧（温湿度/光照）沿用出厂模板，始终可用；
     * 固定句应答需要模块配好 10~23 号词条，未配置时整句不发。 */
    if (!is_value && !SU03T_ACTION_VOICE_ENABLED) return;
    /* 同一句应答重复触发时只保留一条：连按两下开灯不必念两遍。 */
    if (!is_value) {
        for (int i = 0; i < s_say_n; ++i) {
            if (!s_say[i].is_value && s_say[i].index == index) {
                s_say[i].due = now + delay;
                return;
            }
        }
    }
    if (s_say_n >= VOICE_SAY_MAX) {
        memmove(s_say, s_say + 1, sizeof(s_say[0]) * (VOICE_SAY_MAX - 1));
        s_say_n = VOICE_SAY_MAX - 1; /* 丢最旧的一条，保证最新动作有应答 */
    }
    s_say[s_say_n].index = index;
    s_say[s_say_n].is_value = is_value;
    s_say[s_say_n].value = value;
    s_say[s_say_n].due = now + delay;
    s_say_n++;
}

/* 报警或静音期间，模块正在念报警句。此时动作应答如果立刻发，
 * 模块会当场切句，报警播到一半被「已开灯」顶掉，所以统一延后。
 * 报警句本身走 su03t_send_u8_msg，直接发，不受这里影响。 */
static unsigned reply_delay_ms(void)
{
    safety_state_t state;
    safety_state_get(&state);
    return (state.alarm || state.mute) ? SU03T_REPLY_DELAY_ALARM_MS : 0;
}

static void say_phrase(uint8_t index, uint64_t now) { say_push(index, 0, 0, now, reply_delay_ms()); }

static void say_flush(uint64_t now)
{
    int keep = 0;
    for (int i = 0; i < s_say_n; ++i) {
        if (s_say[i].due <= now) {
            if (s_say[i].is_value) su03t_send_double_msg(s_say[i].index, s_say[i].value);
            else send_phrase(s_say[i].index);
        } else {
            s_say[keep++] = s_say[i];
        }
    }
    s_say_n = keep;
}

static void set_note(const char *text, uint64_t now)
{
    safety_lock();
    snprintf(s_note, sizeof(s_note), "%s", text);
    s_note_up = now;
    safety_unlock();
}

int su03t_note(char *out, int n, uint64_t now)
{
    int valid = 0;
    if (!out || n <= 0) return 0;
    safety_lock();
    if (s_note[0] && now - s_note_up <= 3000) {
        snprintf(out, n, "%s", s_note);
        valid = 1;
    }
    safety_unlock();
    return valid;
}

int su03t_take_page(void)
{
    int page;
    safety_lock();
    page = s_page;
    s_page = -1;
    safety_unlock();
    return page;
}

static void request_page(int page)
{
    safety_lock();
    s_page = page;
    safety_unlock();
}

void su03t_process_voices(const bool alarms[5], unsigned medicine_mask,
                         int medicine_audible, int muted, uint64_t now)
{
    unsigned active = 0;
    for (unsigned i = 0; i < 5; i++) if (alarms[i]) active |= 1U << i;
    medicine_mask &= 7U;
    if (medicine_mask & ~s_medicine_mask) {
        s_voice.previous &= ~(1U << 5); /* A newly due dose, not an undo of another slot. */
    }
    s_medicine_mask = medicine_mask;
    if (medicine_mask) active |= 1U << 5;
    unsigned rank = voice_policy_next(&s_voice, active, muted || (active == 32U && !medicine_audible), now);
    if (!rank) return;
    const uint8_t indices[6] = {8, 7, 6, 4, 5, 9};
    if (rank == 6 && !SU03T_MEDICINE_PROFILE_ENABLED) {
        printf("[voice] medicine skipped: index 9 vocabulary not verified\n"); return;
    }
    su03t_send_u8_msg(indices[rank - 1], 0);
}

/***************************************************************
 * 函数名称: su03t_send_double_msg
 * 说    明: 发送 double 类型数值到语音模块
 *          帧：AA 55 <index> <8字节double小端> 55 AA
 ***************************************************************/
void su03t_send_double_msg(uint8_t index, double dat)
{
    if (!isfinite(dat)) return;
    uint8_t buf[13] = {0};
    uint8_t *buf_ptr = buf;
    uint8_t *u8_ptr = (uint8_t *)&dat;

    *buf_ptr++ = 0xAA;
    *buf_ptr++ = 0x55;
    *buf_ptr++ = index;

    for (uint8_t i = 0; i < sizeof(double); i++)
    {
        *buf_ptr++ = u8_ptr[i];
    }

    *buf_ptr++ = 0x55;
    *buf_ptr++ = 0xAA;

    write_frame(buf, sizeof(buf));
}

/***************************************************************
 * 函数名称: su03t_send_u8_msg
 * 说    明: 发送 uint8_t 类型数据的报警触发帧
 *          帧：AA 55 <index> <dat> 55 AA
 ***************************************************************/
void su03t_send_u8_msg(uint8_t index, uint8_t dat)
{
#if !SU03T_ALARM_PROFILE_ENABLED
    /* Do not send alarm IDs into factory temperature/humidity templates. */
    printf("[voice] alarm index=%u skipped: custom profile not enabled\r\n", index);
    (void)dat;
    return;
#endif
    if (index < 4 || index > 9 || (index == 9 && !SU03T_MEDICINE_PROFILE_ENABLED)) {
        printf("[voice] refused unconfigured phrase index=%u\n", index); return;
    }
    uint8_t buf[6] = {0};
    uint8_t *buf_ptr = buf;

    *buf_ptr++ = 0xAA;
    *buf_ptr++ = 0x55;
    *buf_ptr++ = index;
    *buf_ptr++ = dat;
    *buf_ptr++ = 0x55;
    *buf_ptr++ = 0xAA;

    write_frame(buf, sizeof(buf));
}

/***************************************************************
 * 说    明: 数值/状态查询的应答
 ***************************************************************/
/* 报警或静音期间的查询：报警优先，但不装哑巴。
 * 老人问一句总要有个回应，所以仍播「暂不可用」，只是这句会被
 * reply_delay_ms() 自动延后，等报警句念完再说，不会把报警顶掉。 */
static bool query_blocked(uint64_t now)
{
    safety_state_t state;
    safety_state_get(&state);
    if (!state.alarm && !state.mute) return false;
    printf("[voice] query skipped: alarm/mute active\r\n");
    say_phrase(SU03T_SAY_NO_DATA, now);
    return true;
}

static void answer_query(uint8_t command, uint64_t now)
{
    /* Factory command 3 means light, not gas. */
    if (command != 1 && command != 2) return;
    if (query_blocked(now)) return;
    safety_state_t state;
    safety_quality_t quality;
    safety_state_get(&state);
    safety_state_quality(&state, (unsigned int)(now / 1000), &quality);
    if (!quality.env_available) {
        printf("[voice] query skipped: temperature/humidity unavailable\r\n");
        say_phrase(SU03T_SAY_NO_DATA, now);
        return;
    }
    double value = command == 1 ? state.temp : state.humi;
    say_push(command == 1 ? SU03T_VAL_IDX_TEMP : SU03T_VAL_IDX_HUMI, 1, value, now, 0);
}

static void answer_gas(uint64_t now)
{
    safety_state_t state;
    safety_config_t cfg;
    if (query_blocked(now)) return;
    safety_state_get(&state);
    safety_config_get(&cfg);
    bool bad = isfinite(state.gas) && state.gas > cfg.gas_limit;
    printf("[voice] gas query ppm=%.0f limit=%.0f -> %s\r\n",
           state.gas, cfg.gas_limit, bad ? "high" : "normal");
    say_phrase(bad ? SU03T_SAY_GAS_BAD : SU03T_SAY_GAS_OK, now);
}

static void answer_light(uint64_t now)
{
    safety_state_t state;
    if (query_blocked(now)) return;
    safety_state_get(&state);
    if (!isfinite(state.light) || state.light < 0) {
        say_phrase(SU03T_SAY_NO_DATA, now);
        return;
    }
    printf("[voice] light query lux=%.0f\r\n", state.light);
    say_push(SU03T_VAL_IDX_LIGHT, 1, state.light, now, 0);
}

/* ---------------- 动作类指令 ---------------- */
static void voice_help(uint8_t param, uint64_t now)
{
    if (param == 1) {
        unsigned int id = 0;
        int rc = safety_command_submit(SAFETY_CMD_SOS, "voice", &id);
        printf("[voice] SOS submit rc=%d id=%u\r\n", rc, id);
        if (rc == 0) {
            set_note("语音呼救", now);
            say_phrase(SU03T_SAY_SOS, now);
        } else {
            set_note("呼救提交失败", now);
            say_phrase(SU03T_SAY_BUSY, now);
        }
    } else if (param == 2) {
        unsigned int id = 0;
        int rc = safety_command_submit(SAFETY_CMD_ACK, "voice", &id);
        printf("[voice] ACK submit rc=%d id=%u\r\n", rc, id);
        set_note(rc == 0 ? "语音解除求助" : "解除失败", now);
        if (rc == 0) say_phrase(SU03T_SAY_ACK, now);
    }
}

static void voice_mute(uint8_t param, uint64_t now)
{
    unsigned int id = 0;
    int code = param == 1 ? SAFETY_CMD_MUTE : param == 2 ? SAFETY_CMD_UNMUTE : 0;
    if (!code) return;
    int rc = safety_command_submit(code, "voice", &id);
    printf("[voice] %s submit rc=%d id=%u\r\n", param == 1 ? "MUTE" : "UNMUTE", rc, id);
    if (rc) return;
    if (param == 1) {
        set_note("语音静音", now);
        say_phrase(SU03T_SAY_MUTE, now);
    } else {
        set_note("语音取消静音", now);
        say_phrase(SU03T_SAY_UNMUTE, now);
    }
}

/* 指定时段确认；成功返回 0 */
static int confirm_slot(int slot, unsigned int epoch)
{
    living_status_t s;
    living_get(&s);
    if (slot < 0 || slot > 2) return 1;
    if (!living_slot_enabled(&s.config, slot) || s.slot_state[slot] != 3) return 1;
    char payload[160];
    snprintf(payload, sizeof(payload),
             "{\"dose_day\":%d,\"slot\":%d,\"taken\":true,\"expected_medicine_revision\":%u}",
             s.dose_day, slot, s.medicine_revision);
    return living_execute(SAFETY_CMD_MEDICINE_STATE, payload, epoch);
}

static void voice_medicine(uint8_t param, uint64_t now)
{
    unsigned int epoch = sntp_get_epoch();
    if (!epoch) {
        printf("[voice] medicine: clock not synced\r\n");
        set_note("时间未同步", now);
        say_phrase(SU03T_SAY_NO_DATA, now);
        return;
    }
    int slot = -1, rc;
    if (param >= 1 && param <= 3) rc = confirm_slot((int)param - 1, epoch);
    else rc = living_confirm_next(epoch, &slot);
    printf("[voice] medicine confirm param=%u slot=%d rc=%d\r\n", param, slot, rc);
    if (rc == 0) {
        set_note("语音确认服药", now);
        say_phrase(SU03T_SAY_MED_OK, now);
    } else {
        set_note("无待服药", now);
        say_phrase(SU03T_SAY_MED_NONE, now);
    }
}

static void voice_light(uint8_t param, uint64_t now)
{
    if (param < 1 || param > 3) return;
    int mode = param == 1 ? 1 : param == 2 ? 0 : -1;
    living_light_override(mode);
    printf("[voice] light %s (alarm retains priority)\n",
           param == 1 ? "ON" : param == 2 ? "OFF" : "AUTO");
    set_note(param == 1 ? "语音开灯" : param == 2 ? "语音关灯" : "灯光自动", now);
    say_phrase(param == 1 ? SU03T_SAY_LIGHT_ON :
               param == 2 ? SU03T_SAY_LIGHT_OFF : SU03T_SAY_LIGHT_AUTO, now);
}

static void voice_query(uint8_t param, uint64_t now)
{
    switch (param) {
    case 1: answer_query(1, now); break;
    case 2: answer_query(2, now); break;
    case 3: answer_gas(now); break;
    case 4: answer_light(now); break;
    case 5:
        /* 温湿度连播：第二句延后 1.4 秒，避免模块连续收帧丢句 */
        answer_query(1, now);
        answer_query(2, now);
        if (s_say_n >= 2 && s_say[s_say_n - 1].index == SU03T_VAL_IDX_HUMI)
            s_say[s_say_n - 1].due = now + 1400;
        break;
    default: break;
    }
}

/* ---------------- 两字节指令解析 ----------------
 * 返回 1 表示已消费该参数；0 表示不匹配，参数可再当作新前缀。 */
static int handle_legacy(uint8_t prefix, uint8_t param, uint64_t now)
{
    if (prefix == 3) {
        if (param >= 1 && param <= 3) { answer_query(param, now); return 1; }
        return 0;
    }
    if (prefix == 1) {
        if (param == 1 || param == 2) { voice_light(param, now); return 1; }
        return 0;
    }
    if (prefix == 0) {
        if (param == 1 || param == 2) {
            living_light_override(param == 1 ? -1 : 0);
            printf("[voice] light %s\n", param == 1 ? "AUTO" : "OFF");
            set_note(param == 1 ? "灯光自动" : "语音关灯", now);
            say_phrase(param == 1 ? SU03T_SAY_LIGHT_AUTO : SU03T_SAY_LIGHT_OFF, now);
            return 1;
        }
        return 0;
    }
    return 0;
}

static int handle_extended(uint8_t prefix, uint8_t param, uint64_t now)
{
    switch (prefix) {
    case SU03T_PFX_HELP:
        if (param < 1 || param > 2) return 0;
        voice_help(param, now); return 1;
    case SU03T_PFX_MUTE:
        if (param < 1 || param > 2) return 0;
        voice_mute(param, now); return 1;
    case SU03T_PFX_MED:
        if (param < 1 || param > 4) return 0;
        voice_medicine(param, now); return 1;
    case SU03T_PFX_LIGHT:
        if (param < 1 || param > 3) return 0;
        voice_light(param, now); return 1;
    case SU03T_PFX_QUERY:
        if (param < 1 || param > 5) return 0;
        voice_query(param, now); return 1;
    case SU03T_PFX_PAGE:
        if (param == 1) { request_page(1); set_note("记录一页", now); }
        else if (param == 2) { request_page(2); set_note("记录二页", now); }
        else if (param == 3) { request_page(0); set_note("返回主页", now); }
        else return 0;
        printf("[voice] page request %u\n", param);
        return 1;
    default:
        return 0;
    }
}

void su03t_receive_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        uint64_t now = LOS_TickCountGet();
        if (s_pfx >= 0 && now - s_pfx_up > VOICE_PFX_WINDOW_MS) s_pfx = -1;
        if (s_pfx >= 0) {
            uint8_t prefix = (uint8_t)s_pfx;
            bool ext = s_pfx_ext;
            s_pfx = -1;
            bool used = ext ? handle_extended(prefix, byte, now)
                            : handle_legacy(prefix, byte, now);
            if (used) continue;
            /* 不匹配：当前字节继续参与前缀识别，兼容“半包 + 新指令”连发 */
        }
        if (byte == 0 || byte == 1 || byte == 3) {
            s_pfx = byte;
            s_pfx_ext = false;
            s_pfx_up = now;
        } else if (byte >= SU03T_PFX_HELP && byte <= SU03T_PFX_PAGE) {
            s_pfx = byte;
            s_pfx_ext = true;
            s_pfx_up = now;
        }
    }
    say_flush(LOS_TickCountGet());
}

/***************************************************************
 * 函数名称: su_03t_thread
 * 说    明: 语音模块接收线程：解析指令、回传数值、异步播放应答
 ***************************************************************/
static void su_03t_thread(void *arg)
{
    (void)arg;
    while (1)
    {
        uint8_t data[64] = {0};
        int rec_len = IoTUartRead(UART2_HANDLE, data, sizeof(data));
        if (rec_len > 0 && rec_len <= (int)sizeof(data)) su03t_receive_bytes(data, rec_len);
        else say_flush(LOS_TickCountGet());

#if SU03T_SELF_TEST
        if (s_selftest_idx >= 4) {
            uint64_t now = LOS_TickCountGet();
            if (s_selftest_due == 0 || now >= s_selftest_due) {
                send_phrase((uint8_t)s_selftest_idx);
                printf("[voice] self test index=%d\r\n", s_selftest_idx);
                s_selftest_due = now + 2000;
                s_selftest_idx = s_selftest_idx >= SU03T_SAY_READY ? -1 : s_selftest_idx + 1;
            }
        }
#endif

        LOS_Msleep(100);
    }
}

/***************************************************************
 * 函数名称: su03t_init
 * 说    明: 初始化语音模块串口（115200 8N1）并启动接收线程
 ***************************************************************/
void su03t_init(void)
{
    s_voice = (voice_policy_t){0};
    s_medicine_mask = 0;
    s_pfx = -1; s_pfx_ext = false;
    s_say_n = 0; s_page = -1; s_note[0] = 0;
    printf("[voice] alarm_profile=%d action_voice=%d commands=call/mute/medicine/light/query/page\r\n",
           SU03T_ALARM_PROFILE_ENABLED, SU03T_ACTION_VOICE_ENABLED);
    IotUartAttribute attr;
    unsigned int ret = 0;
    unsigned int thread_id;
    TSK_INIT_PARAM_S task = {0};

    IoTUartDeinit(UART2_HANDLE);

    attr.baudRate = 115200;
    attr.dataBits = IOT_UART_DATA_BIT_8;
    attr.pad = IOT_FLOW_CTRL_NONE;
    attr.parity = IOT_UART_PARITY_NONE;
    attr.rxBlock = IOT_UART_BLOCK_STATE_NONE_BLOCK;
    attr.stopBits = IOT_UART_STOP_BIT_1;
    attr.txBlock = IOT_UART_BLOCK_STATE_NONE_BLOCK;

    ret = IoTUartInit(UART2_HANDLE, &attr);
    if (ret != IOT_SUCCESS)
    {
        printf("%s, %d: IoTUartInit(%d) failed!\n", __FILE__, __LINE__, ret);
        return;
    }

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)su_03t_thread;
    task.uwStackSize = 2048;
    task.pcName = "su-03t thread";
    task.usTaskPrio = 25;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Failed to create su-03t task ret:0x%x\n", ret);
    }

#if SU03T_BOOT_READY && SU03T_ACTION_VOICE_ENABLED
    /* 上电播一句「语音已就绪」，听到就说明 UART2、模块、词条三样都通；
     * 没声音则依次查接线、波特率、23 号词条。模块从断电到能收命令约 1 秒，
     * 所以延迟 1.5 秒再发，避免开机第一句丢帧。 */
    say_push(SU03T_SAY_READY, 0, 0, LOS_TickCountGet(), 1500);
    printf("[voice] boot greeting queued: index %d\r\n", SU03T_SAY_READY);
#endif

#if SU03T_SELF_TEST
    s_selftest_idx = SU03T_ACTION_VOICE_ENABLED ? 4 : -1;
    s_selftest_due = 0;
    printf("[voice] self test %s\r\n",
           s_selftest_idx >= 0 ? "queued 4..23" : "skipped (VOICE_DEDICATED_IDX=0)");
#endif
}
