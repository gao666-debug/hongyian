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
 
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "los_queue.h"
#include "los_task.h"
#include "los_tick.h"
#include "ohos_init.h"

#include "iot_watchdog.h"

#include "bh1750.h"
#include "button.h"
#include "config_network.h"
#include "door.h"
#include "event_log.h"
#include "home_safety.h"
#include "key.h"
#include "key_actions.h"
#include "monitor_logic.h"
#include "kv_store.h"
#include "mpu6050.h"
#include "mq2.h"
#include "mqtt_client.h"
#include "net_server.h"
#include "nfc_tag.h"
#include "reset.h"
#include "rgb.h"
#include "safety_runtime.h"
#include "living.h"
#include "sntp_client.h"
#include "safety_stats.h"
#include "sht30.h"
#include "sntp_client.h"
#include "su_03t.h"
#include "ui.h"
#include <string.h>

/* 网络模式选择（定义在文件后部，此处前向声明供长按处理调用） */
static int net_mode_load(void);
static int net_mode_save(int mode);
static int net_mode_auto_pick(void);

#define MSG_QUEUE_LENGTH 2
#define LOOP_INTERVAL_MS 200 /* 采集/判断周期 */
#define ENV_READ_INTERVAL_MS 500 /* Deadline-based polling; no tick-count drift. */
#define GAS_EVERY 2          /* 每 2 拍读一次燃气(约 400ms) */
#define PRINT_EVERY_LOOPS 10 /* 每 10 拍打印一次状态(约 2s) */
#define IMU_BURST_COUNT 1    /* Fixed gateway: health sample only; no human-fall burst sampling. */
#define IMU_BURST_GAP_MS 12  /* 突发采样间隔(ms)，8×12≈96ms 采样窗 */
#define IMU_DEBUG_G 1.2f     /* 加速度超过此值即打印，便于调试 */

#define WDT_GATE_MS 10000 /* 主线程心跳间隔超过此值则不再喂狗（复位） */

/* 队列传递的采样数据 */
typedef struct
{
    double temp;    /* 温度(℃) */
    double humi;    /* 湿度(%RH) */
    double gas_ppm; /* 燃气浓度(ppm) */
    double light;   /* 光照强度(lx) */
    bool body;      /* Debounced PIR activity, not person identity or distance. */
    short accel[3]; /* 加速度原始值 x,y,z（本拍最后一次） */
    double gmag;    /* 本拍突发采样的峰值加速度（g） */
    bool env_valid, imu_valid, light_valid, body_valid;
    int env_cached, env_age_s;
    unsigned int sample_up;
    float burst_g[IMU_BURST_COUNT];
    uint64_t burst_ms[IMU_BURST_COUNT];
    double gmin; /* 本拍突发采样的最小加速度（g），用于失重检测 */
} safety_sample_t;

static unsigned int m_msg_queue;
static safety_sample_t g_sample = {0};
static volatile bool g_imu_ready = false;
static volatile bool g_body_initialized = false;

/* 上一次报警状态，用于识别报警上升沿 */
static bool g_alarm = false;
/* 主循环心跳（看门狗用） */
static volatile uint64_t g_heartbeat_ms = 0;
static volatile bool g_reboot_requested = false;

/***************************************************************
 * 函数名称: wdt_thread
 * 说    明: 看门狗线程：主循环心跳正常才喂狗，否则触发复位
 ***************************************************************/
static void wdt_thread(void *arg)
{
    IoTWatchDogEnable(20); /* 约 22 秒超时 */

    while (1)
    {
        uint64_t now = LOS_TickCountGet();
        if (!g_reboot_requested && (now - g_heartbeat_ms) < WDT_GATE_MS)
        {
            IoTWatchDogKick();
        }
        else
        {
            printf("[wdt] heartbeat stale %u ms, no kick\r\n",
                   (unsigned int)(now - g_heartbeat_ms));
        }
        LOS_Msleep(1000);
    }
}

/***************************************************************
 * 函数名称: nfc_init_thread
 * 说    明: 后台写 NFC 标签（避免占用/阻塞启动流程）
 ***************************************************************/
static void nfc_init_thread(void *arg)
{
    /* NT3H uses EI2C2_M0 (PD5/PD6), not the PA0/PA1 sensor bus. */
    nfc_tag_init();
}

/***************************************************************
 * 函数名称: imu_init_thread
 * 说    明: 后台初始化 MPU6050（其内部有 ~1.2s 延时），避免阻塞启动显示
 ***************************************************************/
/***************************************************************
 * 函数名称: sensor_read_thread
 * 说    明: 传感器采集线程（PIR / SHT30 / 燃气 / 加速度 / 按键）
 ***************************************************************/
/* PIR is a digital input: sample independently so an I2C retry cannot delay it. */
static void body_read_thread(void *arg)
{
    (void)arg;
    /* Keep the original hardware initialization order: I2C first, PA3 last.
     * The input task may start early, but must never race bus pin setup. */
    while(!g_body_initialized)LOS_Msleep(10);
    while(1) {
        bool active;
        body_induction_get_state(&active);
        LOS_Msleep(10);
    }
}

static void sensor_read_thread(void *arg)
{
    unsigned int tick = 0;
    uint64_t last_retry = 0;
    uint64_t next_env_read = 0;
    monitor_env_t env_cache = {0};
    ui_sensor_progress("SHT-INIT");
    sht30_init();
    ui_sensor_progress("LIGHT-INIT");
    bh1750_init();
    body_induction_dev_init();
    g_body_initialized=true;
    mq2_dev_init();
    ui_sensor_progress("GAS-CAL");
    mq2_ppm_calibration();
    ui_sensor_progress("IMU-INIT");
    g_imu_ready = (mpu6050_init() == 0);
    while (1)
    {
        ui_sensor_progress("PIR");
        body_diagnostics_t body_diag;
        body_induction_diagnostics(&body_diag);
        g_sample.body_valid = body_diag.filtered_valid;
        g_sample.body = body_diag.filtered_active;
        if (LOS_TickCountGet() >= next_env_read)
        {
            next_env_read = LOS_TickCountGet() + ENV_READ_INTERVAL_MS;
            double t[2] = {0};
            ui_sensor_progress("SHT-READ");
            bool read_ok = sht30_read_data(t) == 0;
            monitor_env_observe(&env_cache, read_ok, t[0], t[1], LOS_TickCountGet());
            ui_sensor_progress("LIGHT-READ");
            g_sample.light_valid = (bh1750_read_checked(&g_sample.light) == 0);
        }
        uint64_t env_now = LOS_TickCountGet();
        g_sample.env_valid = monitor_env_available(&env_cache, env_now);
        g_sample.env_cached = g_sample.env_valid && !env_cache.last_read_ok;
        g_sample.env_age_s = env_cache.seen ? (int)((env_now - env_cache.good_ms) / 1000) : -1;
        if (env_cache.seen) { g_sample.temp = env_cache.temp; g_sample.humi = env_cache.humi; }
        if (tick % GAS_EVERY == 0) {
            ui_sensor_progress("GAS-READ");
            g_sample.gas_ppm = get_mq2_ppm();
        }
        uint64_t now = LOS_TickCountGet();
        ui_sensor_progress("RETRY");
        if ((!g_imu_ready || !g_sample.env_valid) && now - last_retry >= 10000)
        {
            if (!g_imu_ready)
                g_imu_ready = (mpu6050_init() == 0);
            if (!g_sample.env_valid)
                sht30_init();
            last_retry = now;
        }
        g_sample.imu_valid = g_imu_ready;
        g_sample.gmag = 0;
        g_sample.gmin = 100;
        ui_sensor_progress("IMU-READ");
        for (int i = 0; i < IMU_BURST_COUNT; i++)
        {
            short acc[3] = {0};
            float m = 1.0f;
            if (!g_imu_ready || mpu6050_read_data(acc) != 0)
            {
                g_sample.imu_valid = false;
                g_imu_ready = false;
            }
            else
            {
                float x = acc[0], y = acc[1], z = acc[2];
                m = sqrtf(x * x + y * y + z * z) / ACCEL_LSB_PER_G;
            }
            g_sample.burst_g[i] = m;
            g_sample.burst_ms[i] = LOS_TickCountGet();
            if (m > g_sample.gmag)
                g_sample.gmag = m;
            if (m < g_sample.gmin)
                g_sample.gmin = m;
            memcpy(g_sample.accel, acc, sizeof(acc));
            LOS_Msleep(IMU_BURST_GAP_MS);
        }
        g_sample.sample_up = (unsigned int)(LOS_TickCountGet() / 1000);
        ui_sensor_progress("PUBLISH");
        LOS_QueueWriteCopy(m_msg_queue, &g_sample, sizeof(g_sample), LOS_WAIT_FOREVER);
        ui_sensor_progress("SLEEP");
        tick++;
        LOS_Msleep(LOOP_INTERVAL_MS);
    }
}

static void safety_monitor_thread(void *arg)
{
    safety_sample_t sample = {0};
    bool have_sample=false;
    uint64_t received_ms=0;
    int received_env_age=0;
    safety_fall_t fall = {0};
    bool help_on = false, mute_forced = false, prev_body = false, door_prev = false,
         prev_alarm[5] = {0};
    int ids[5] = {0};
    uint64_t starts[5] = {0}, beep_start = 0;
    bool door_seeded = false, noact_warned = false, env_alarm = false;
    monitor_care_t care = {0};
    unsigned int door_count = 0, loops = 0;
    int page = KEY_ADC_DIAGNOSTIC ? 3 : 0, edit_field = -1, edit_result = -1;
    living_config_t draft = {0};
    int last_med_state[3]={-1,-1,-1},last_dose_day=-1;
    g_heartbeat_ms = LOS_TickCountGet();
    care.last_activity_ms = g_heartbeat_ms;
    while (1)
    {
        unsigned int size = sizeof(sample);
        bool fresh=LOS_QueueReadCopy(m_msg_queue, &sample, &size, 50) == LOS_OK;
        uint64_t now = LOS_TickCountGet();
        if(fresh) {
            have_sample=true;g_heartbeat_ms=now;received_ms=now;
            received_env_age=sample.env_age_s;
        } else {
            if(!have_sample)continue; /* Never publish a fabricated zero sample. */
            /* A queue scheduling timeout is not a sensor failure. Bound the age
             * of the retained frame instead of invalidating normal 212ms cycles. */
            uint64_t age=now-received_ms;
            sample.env_age_s=received_env_age+(int)(age/1000);
            sample.env_cached=sample.env_valid;
            if(age+(uint64_t)(received_env_age<0?0:received_env_age)*1000>6000) {
                sample.env_valid=false;sample.env_cached=0;
            }
            if(age>1500)sample.body_valid=sample.imu_valid=sample.light_valid=false;
        }
        safety_sample_t *v = &sample;
        body_diagnostics_t live_body;body_induction_diagnostics(&live_body);
        v->body_valid=live_body.filtered_valid;
        v->body=live_body.filtered_active;
        safety_config_t cfg;
        safety_config_get(&cfg);
        if (v->body_valid) {
            if (v->body && !prev_body) safety_stats_activity();
            prev_body = v->body;
        }
        /* Fixed home gateway: board motion is not human motion.
         * A validated external wearable needs its own event source. */
        memset(&fall, 0, sizeof(fall));
        bool living_key=false,events_dirty=false;
        key_id_t key = key_take_event();
#if KEY_ADC_DIAGNOSTIC
        key=KEY_NONE;
#endif
        if (key == KEY_UP)
            help_on = !help_on;
        else if (key == KEY_DOWN) {
            living_silence();
            if (page == 4 && edit_field >= 0 && !g_alarm) {
                living_key=true;
                if(edit_field==0)draft.night_auto=!draft.night_auto;
                if(edit_field>=1 && edit_field<=9) {
                    int slot=(edit_field-1)/3,part=(edit_field-1)%3;
                    int enabled=living_slot_enabled(&draft,slot),minute=living_slot_minute(&draft,slot);
                    if(part==0)enabled=!enabled;
                    if(part==1)minute=((minute/60+1)%24)*60+minute%60;
                    if(part==2)minute=minute/60*60+(minute+1)%60;
                    living_slot_set(&draft,slot,enabled,minute);
                }
                if(edit_field>=10) {
                    living_status_t ls;living_get(&ls);int slot=edit_field-10;
                    char payload[144];snprintf(payload,sizeof(payload),"{\"dose_day\":%d,\"slot\":%d,\"taken\":%s,\"expected_medicine_revision\":%u}",ls.dose_day,slot,ls.slot_state[slot]==4?"false":"true",ls.medicine_revision);
                    edit_result=living_execute(SAFETY_CMD_MEDICINE_STATE,payload,sntp_get_epoch());
                }
            } else mute_forced = true;
        }
        else if (key == KEY_LEFT)
        {
            page = (page + 1) % 5;
            edit_field=-1;edit_result=-1;
            /* Display worker will render the latest requested page. */
        }
        else if (key == KEY_RIGHT)
        {
            living_status_t pending;living_get(&pending);
            k6_action_t action=k6_action(page,edit_field,g_alarm||help_on||fall.active,
                                         living_pending_slot(&pending)>=0,false);
            if(action==K6_ACK) {
                help_on=false;memset(&fall,0,sizeof(fall));
                mute_forced=true;living_silence();
                printf("[key] K6 ACK: help/fall cleared; sensor alarms remain\r\n");
            }
            else if(action==K6_MEDICINE) {
                living_key=true;
                int slot=-1;edit_result=living_confirm_next(sntp_get_epoch(),&slot);
                if(edit_result==0) {
                    edit_result=100+slot;
                    printf("[living] K6 confirmed slot=%d day=%d\r\n",slot+1,pending.dose_day);
                }
                page=4;edit_field=-1;
            }
            else if (action==K6_EDIT) {
                living_key=true;
                living_status_t ls;living_get(&ls);
                if(edit_field<0) {draft=ls.config;edit_field=0;edit_result=-1;}
                else {edit_field=(edit_field+1)%13;edit_result=-1;}
            }
        }

        safety_command_t cmd;
        bool have_cmd = safety_command_take(&cmd) != 0;
        int command_result=0;
        if (have_cmd)
        {
            if (cmd.code == SAFETY_CMD_ACK)
            {
                help_on = false;
                memset(&fall, 0, sizeof(fall));
            }
            if(cmd.code==SAFETY_CMD_ACK || cmd.code==SAFETY_CMD_MUTE) {mute_forced = true;living_silence();}
            else if(cmd.code==SAFETY_CMD_SOS) {
                help_on=true;mute_forced=false;beep_start=now;
                printf("[cloud] SOS applied: local help alarm active\r\n");
            }
            else command_result=living_execute(cmd.code,cmd.payload,sntp_get_epoch());
        }
        key_id_t long_key=key_take_long_event();
#if KEY_ADC_DIAGNOSTIC
        long_key=KEY_NONE; /* Do not reboot or clear history on an ADC misclassification. */
#endif
        if(long_key==KEY_RIGHT) {
            k6_action_t action=k6_action(page,edit_field,g_alarm||help_on||fall.active,false,true);
            if(action==K6_CLEAR_EVENTS) {
                event_log_clear();events_dirty=true;
                printf("[key] K6 long: local event history cleared\r\n");
            } else if(action==K6_EDIT) {
                living_status_t ls;living_get(&ls);draft=ls.config;edit_field=0;edit_result=-1;
            } else if(action==K6_SAVE) {edit_result=living_save(&draft);edit_field=-1;}
            else if(g_alarm||help_on||fall.active)
                printf("[key] K6 long ignored: active alarm; short press to ACK\r\n");
        }
        if (long_key == KEY_LEFT)
        {
            int mode = net_get_mode();
            int next = mode == NET_MODE_STA ? NET_MODE_AP : NET_MODE_STA;
            if (net_mode_save(next) == 0) {
                printf("[mode] saved %s, rebooting\r\n", next == NET_MODE_STA ? "STA" : "AP");
                g_reboot_requested = true;
                LOS_Msleep(1200);
                RebootDevice(3);
                /* Wait for reset without toggling the persisted preference again. */
                while (1) LOS_Msleep(1000);
            } else printf("[mode] save failed; keeping current mode\r\n");
        }
        unsigned int wireless = 0;
#if WIRELESS_BTN_ENABLED
        wireless = button_take_events();
#if KEY_ADC_DIAGNOSTIC
        wireless=0; /* Keep diagnostic page visible, even with noisy receiver lines. */
#endif
        if (wireless & WIRELESS_SOS) {
            living_status_t pending;living_get(&pending);
            if (help_on) {
                help_on=false;
                living_silence();
                wireless=WIRELESS_ACK; /* Suppress the acknowledged voice only. */
                printf("[wireless] HELP confirmed; sensor alarms preserved\n");
            } else if (living_pending_slot(&pending)>=0) {
                int slot=-1;
                edit_result=living_confirm_next(sntp_get_epoch(),&slot);
                if (!edit_result) edit_result=100+slot;
                page=4;edit_field=-1;
                wireless=0; /* Dose confirmation must not mute safety alarms. */
                printf("[wireless] medicine confirm slot=%d result=%d\n",slot,edit_result);
            } else {
                help_on=true;
                mute_forced=false;
                printf("[wireless] SOS applied\n");
            }
        }
#endif
        bool door = door_prev, door_valid = false, door_changed = false;
#if DOOR_SENSOR_ENABLED
        door_valid = door_read(now, &door);
#endif
        /* Only confirmed transitions indicate activity, never a held-open door.
         * Seed after boot/read failure without inventing an opening event. */
        if (door_valid) {
            if (door_seeded && door != door_prev) {
                door_changed = true;
                if (door) door_count++;
                event_log_add(door ? "DOOR" : "DOOR_CLOSE", door_count);
                events_dirty=true;
            }
            door_prev = door;
        }
        door_seeded = door_valid;
        monitor_care_result_t care_result = monitor_care_step(&care, now,
            v->body_valid, v->body, key != KEY_NONE || long_key != KEY_NONE,
            door_changed, cfg.noact_ms);
        unsigned int inactive = care_result.inactive_ms;
        /* A sensor outage cannot falsely end a previously detected environment alarm. */
        if (v->env_valid)
            env_alarm = v->temp < cfg.temp_low || v->temp > cfg.temp_high ||
                        v->humi < cfg.humi_low || v->humi > cfg.humi_high;
        bool env = env_alarm;
        bool gas = v->gas_ppm > cfg.gas_limit, smoke = v->gas_ppm > SMOKE_PPM_LIMIT;
        bool noact_warning = care_result.warning;
        bool noact_alert = care_result.alarm;
        if (noact_warning && !noact_warned)
            printf("[care] no activity: local response window 30s; not a fall diagnosis\r\n");
        noact_warned = noact_warning;
        bool alarms[5] = {help_on, fall.active, gas, noact_alert, env};
        const char *tags[5] = {"HELP", "FALL", "GAS", "NOACT", "ENV"};
        int values[5] = {0, (int)(fall.peak * 100), (int)v->gas_ppm, (int)(inactive / 1000),
                         (int)v->temp};
        bool any = false, changed = false;
        int status = 0, new_voice = -1;
        unsigned int rising, falling;
        unsigned int ending_ids[5];
        memcpy(ending_ids, ids, sizeof(ending_ids));
        safety_alarm_edges(alarms, prev_alarm, &rising, &falling);
        for (int i = 0; i < 5; i++)
        {
            if (alarms[i])
            {
                any = true;
                if (!status)
                    status = i + 1;
            }
            if (rising & (1U << i))
            {
                starts[i] = now;
                ids[i] = event_log_add(tags[i], values[i]);
                safety_stats_alarm(i + 1);
                changed = true;
            }
            else if (falling & (1U << i))
            {
                event_log_set_duration(ids[i], (int)((now - starts[i]) / 1000));
                ids[i] = 0;
                changed = true;
            }
        }
        new_voice = safety_voice_select(alarms, rising,
                         (have_cmd && cmd.code<=SAFETY_CMD_MUTE) || ((wireless & (WIRELESS_ACK|WIRELESS_MUTE)) && !(wireless & WIRELESS_SOS)) || (!living_key && (key == KEY_DOWN || key == KEY_RIGHT))) - 1;
        if (new_voice >= 0)
        {
            /* Higher-priority new alarms must receive a fresh audible window. */
            if (!g_alarm || status == new_voice + 1)
            {
                beep_start = now;
                if (!(have_cmd && cmd.code<=SAFETY_CMD_MUTE) && (living_key || (key != KEY_DOWN && key != KEY_RIGHT)))
                    mute_forced = false;
            }
            /* Speech is scheduled below, after mute and medicine state are known. */
        }
        bool beep = any && !mute_forced && (now - beep_start < cfg.mute_ms);
        living_tick(sntp_get_epoch(),now,v->light,v->light_valid,any || noact_warning);
        living_status_t ls;living_get(&ls);
        for(int slot=0;slot<3;slot++) {
            if(ls.slot_state[slot]!=last_med_state[slot] || ls.dose_day!=last_dose_day) {
                printf("[living] slot=%d medicine=%d day=%d revision=%u\r\n",slot+1,ls.slot_state[slot],ls.dose_day,ls.medicine_revision);
                if(ls.slot_state[slot]==3){event_log_add("MED_DUE",slot+1);changed=true;}
                if(ls.slot_state[slot]==4 && last_med_state[slot]==3){event_log_add("MED_TAKEN",slot+1);changed=true;}
                if(ls.slot_state[slot]==3 && last_med_state[slot]==4){event_log_add("MED_UNTAKEN",slot+1);changed=true;}
                last_med_state[slot]=ls.slot_state[slot];
            }
        }
        last_dose_day=ls.dose_day;
        unsigned medicine_voice_mask=0;
        for(int slot=0;slot<3;slot++)if(ls.slot_state[slot]==3)medicine_voice_mask|=1U<<slot;
        bool voice_alarms[5]={help_on,fall.active,gas,noact_alert && v->body_valid,
                              env && v->env_valid};
        su03t_process_voices(voice_alarms,medicine_voice_mask,living_beep(now),
                             any && !beep,now);
        bool care_beep = noact_warning && !noact_alert && !any &&
                         ((inactive - cfg.noact_ms) / 1000U) % 4U == 0;
        monitor_outputs_t outputs = monitor_outputs(any, help_on, fall.active || gas,
            noact_warning, beep, care_beep, ls.light_owner == 2,
            living_beep(now), ls.night_on);
        beep_set_state(outputs.beep);
        alarm_light_set_gpio(outputs.alarm_light);
        rgb_set_color(outputs.r, outputs.g, outputs.b);
        g_alarm = any;
        if (!any) mute_forced = false;
        safety_state_t state = {0};
        state.temp = v->temp;
        state.humi = v->humi;
        state.gas = v->gas_ppm;
        state.light = v->light;
        state.body = v->body;
        state.inactive = inactive / 1000;
        state.alarm = any;
        state.status = status;
        state.help = help_on;
        state.fall = fall.active;
        state.gas_alarm = gas;
        state.door = door;
        state.door_valid = door_valid;
        state.smoke = smoke;
        state.env_valid = v->env_valid;
        state.env_cached = v->env_cached;
        state.env_age_s = v->env_age_s;
        state.body_valid = v->body_valid;
        state.imu_valid = v->imu_valid;
        state.mute = any && !beep;
        state.sample_up = v->sample_up;
        /* The LCD receives this exact local frame, not the global copy that
         * safety_state_set marks internally. Keep both publication paths valid. */
        state.sample_received = 1;
        safety_state_set(&state);
        for (int i = 0; i < 5; ++i) {
            if (rising & (1U << i)) mqtt_alarm_event(i + 1, values[i], ids[i], 0, 0);
            if (falling & (1U << i)) mqtt_alarm_event(i + 1, values[i], ending_ids[i], 1, (now - starts[i])/1000);
        }
        if (have_cmd)
            safety_command_done(&cmd, command_result);
        const char *text = status ? event_log_tag_cn(tags[status - 1]) : noact_warning ? "无活动" : "正常";
        ui_present(page,&state,text,edit_field>=0?&draft:NULL,edit_field,edit_result,
                   events_dirty||changed);
        if (loops % PRINT_EVERY_LOOPS == 0)
            printf("[safety] boot=%u up=%u env=%d imu=%d status=%d inactive=%us mute=%d\r\n",
                   safety_boot_id(), state.sample_up, state.env_valid, state.imu_valid, status,
                   state.inactive, state.mute);
        safety_stats_tick();
        loops++;
    }
}

/* ---------- 网络模式（AP / STA）选择 ---------- */
#define NET_MODE_KV_KEY "netmode"

/* 读取用户偏好；无/非法返回 -1 表示"自动" */
static int net_mode_load(void)
{
    char buf[8] = {0};

    if (UtilsGetValue(NET_MODE_KV_KEY, buf, sizeof(buf)) >= 0 && buf[0] != 0)
    {
        if (strcmp(buf, "STA") == 0)
        {
            return NET_MODE_STA;
        }
        if (strcmp(buf, "AP") == 0)
        {
            return NET_MODE_AP;
        }
    }
    return -1;
}

static int net_mode_save(int mode)
{
    char check[8] = {0};
    const char *value = mode == NET_MODE_STA ? "STA" : "AP";
    safety_storage_lock();
    int rc = UtilsSetValue(NET_MODE_KV_KEY, value);
    if (rc == 0 && (UtilsGetValue(NET_MODE_KV_KEY, check, sizeof(check)) < 0 || strcmp(check, value))) rc = -1;
    safety_storage_unlock();
    return rc;
}

/* 自动选：能连上路由器(GB) → STA(云端)；否则 → AP(本地) */
static int net_mode_auto_pick(void)
{
    if (mqtt_connect_wifi() == 0) {
        printf("[mode] auto -> STA (cloud)\r\n");
        return NET_MODE_STA;
    }
    SetWifiModeOff();
    printf("[mode] auto -> AP (local)\r\n");
    return NET_MODE_AP;
}

/* 网络模式初始化任务：必须在独立大栈任务里跑——SetWifiModeOn 等 Wi-Fi 调用很深，
 * 放在启动线程(Bootstrap, 小栈)会栈溢出。此任务决定模式并拉起对应服务。 */
static void net_mode_init_task(void *arg)
{
    int mode = net_mode_load();

    if (mode < 0)
    {
        mode = net_mode_auto_pick();
    }
    net_set_mode(mode);
    printf("[mode] current = %s\r\n", (mode == NET_MODE_STA) ? "STA(cloud)" : "AP(local)");

#if NET_SERVER_ENABLED
    net_server_start(); /* HTTP 服务（AP 模式开热点；STA 模式走 STA IP） */
#endif
#if MQTT_ENABLED
    if (mode == NET_MODE_STA)
    {
        mqtt_start(); /* 仅 STA 模式上云 */
    }
#endif
}

/***************************************************************
 * 函数名称: home_safety_example
 * 说    明: 开机自启动调用函数
 ***************************************************************/
static void home_safety_init_task(void *arg)
{
    (void)arg;
    unsigned int thread_id;
    TSK_INIT_PARAM_S task = {0};
    unsigned int ret = LOS_OK;

    printf("\r\n[home_safety] FW=%s init task started\r\n", SAFETY_FW);
    if (safety_runtime_init() != 0)
        return;
    living_init();
    printf("[home_safety] runtime ready\r\n");
    mqtt_config_init();
    event_log_init();
    safety_stats_init();
    printf("[home_safety] history ready\r\n");

    /* ---- 版本自报横幅：一眼确认板子里跑的是哪一版固件 ---- */
    printf("\r\n[home_safety] ===== FW=%s coex=%d net=%d mqtt=%d build=%s %s =====\r\n",
           SAFETY_FW, WIFI_COEXIST, NET_SERVER_ENABLED, MQTT_ENABLED, __DATE__, __TIME__);

    /* ---- 先点亮 LCD 和绿灯，让用户尽快看到反馈 ---- */
    ui_init(); /* LCD 最先初始化（占用 SPI / PC0-PC3） */
    ui_start(); /* 独立显示任务；监测任务仅发布最新画面快照。 */
    rgb_dev_init();
    rgb_set_color(0, 255, 0); /* 正常=绿，开机即亮 */
    beep_dev_init();
    alarm_light_init();
    su03t_init(); /* 内部创建语音接收线程 */
#if DOOR_SENSOR_ENABLED
    door_dev_init();
#endif
#if WIRELESS_BTN_ENABLED
    button_dev_init();
#else
    printf("[diag] wireless disabled: B7/B0/A2/B1 not initialized by receiver code\r\n");
#endif
    key_dev_init(); /* 内部创建按键扫描线程 */
    /* 传感器（包括 MPU）初始化在采集线程里做。 */

    /* 网络模式选择 + 网络服务在独立大栈任务里启动（见 net_mode_init_task 注释） */
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)net_mode_init_task;
    task.uwStackSize = 8192;
    task.pcName = "net mode init";
    task.usTaskPrio = 24;
    if (LOS_TaskCreate(&thread_id, &task) != LOS_OK)
    {
        printf("Failed to create net mode task\r\n");
    }

#if NFC_TAG_ENABLED
    /* NFC 写标签放后台线程 */
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)nfc_init_thread;
    task.uwStackSize = 4096;
    task.pcName = "nfc init thread";
    task.usTaskPrio = 25;
    if (LOS_TaskCreate(&thread_id, &task) != LOS_OK)
    {
        printf("Failed to create nfc task\r\n");
    }
#endif

    sntp_start(); /* 网络对时（后台线程，联网后生效） */

    ret =
        LOS_QueueCreate("safety_queue", MSG_QUEUE_LENGTH, &m_msg_queue, 0, sizeof(safety_sample_t));
    if (ret != LOS_OK)
    {
        printf("Failed to create message queue ret:0x%x\n", ret);
        return;
    }

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)body_read_thread;
    task.uwStackSize = 4096;
    task.pcName = "PIR input";
    task.usTaskPrio = 24;
    ret = LOS_TaskCreate(&thread_id,&task);
    if(ret != LOS_OK)printf("[pir] task create failed rc=%u\r\n",ret);

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)sensor_read_thread;
    task.uwStackSize = 4096;
    task.pcName = "sensor read thread";
    task.usTaskPrio = 24;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Failed to create sensor read task ret:0x%x\n", ret);
        return;
    }

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)safety_monitor_thread;
    task.uwStackSize = 8192; /* UI, JSON command validation and flash persistence. */
    task.pcName = "safety monitor thread";
    task.usTaskPrio = 24;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Failed to create safety monitor task ret:0x%x\n", ret);
        return;
    }

    /* 看门狗线程（栈稍大） */
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)wdt_thread;
    task.uwStackSize = 8192;
    task.pcName = "watchdog thread";
    task.usTaskPrio = 26;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Failed to create watchdog task ret:0x%x\n", ret);
        return;
    }
}

/* Bootstrap has a small stack: even KV/Flash initialization must run in
 * the application task, not in the APP_FEATURE_INIT callback. */
void home_safety_example(void)
{
    unsigned int thread_id;
    TSK_INIT_PARAM_S task = {0};
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)home_safety_init_task;
    task.uwStackSize = 16384;
    task.pcName = "safety init";
    task.usTaskPrio = 24;
    unsigned int ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
        printf("Failed to create safety init task ret:0x%x\r\n", ret);
}

APP_FEATURE_INIT(home_safety_example);
