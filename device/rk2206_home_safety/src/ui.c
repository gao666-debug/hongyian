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

#include "ui.h"
#include "key.h"
#include "home_safety.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lcd.h"
#include "event_log.h"
#include "safety_runtime.h"
#include "net_server.h"
#include "config_network.h"
#include "lwip/ip_addr.h"
#include "los_tick.h"
#include "los_task.h"

static const char *volatile s_sensor_stage = "START";
static volatile unsigned int s_sensor_stage_ms;
void ui_sensor_progress(const char *stage)
{
    s_sensor_stage_ms=(unsigned int)LOS_TickCountGet();
    s_sensor_stage=stage;
}

/* 主界面：标题 + 两栏(5 行 x 2 列 = 10 项) */
#define NCELL   12
#define COL_X0  4
#define COL_X1  164
#define ROW_Y0  48
#define ROW_STEP 30
#define VAL_DX  52
#define COL_W   156

static const char *s_labels[NCELL] = {
    "温度", "湿度", "人体", "燃气", "光照",
    "烟雾", "跌倒", "求助", "房门", "状态", "夜灯", "服药"
};

/* 上一次各单元文本，用于只重画变化项 */
static char s_prev[NCELL][40];
static bool s_prev_valid = false;
static char s_network_prev[48];
/* Main/event pages invalidate these caches. Re-entering a page clears once;
 * subsequent calls touch only rows whose displayed text or color changed. */
static int s_draw_page = -1;
static char s_page_rows[9][64];
static uint16_t s_page_colors[9];
static int page_begin(int page)
{
    if (s_draw_page == page) return 0;
    s_draw_page = page;
    memset(s_page_rows, 0, sizeof(s_page_rows));
    memset(s_page_colors, 0, sizeof(s_page_colors));
    lcd_fill(0, 0, LCD_W, LCD_H, LCD_WHITE);
    return 1;
}
static void page_row(int row, int y, const char *text, uint16_t color, int size, int chinese)
{
    if (!strcmp(s_page_rows[row], text) && s_page_colors[row] == color) return;
    lcd_fill(0, y, LCD_W, y+size, LCD_WHITE);
    if (chinese) lcd_show_chinese(4,y,(uint8_t*)text,color,LCD_WHITE,size,0);
    else lcd_show_string(4,y,(uint8_t*)text,color,LCD_WHITE,size,0);
    snprintf(s_page_rows[row],sizeof(s_page_rows[row]),"%s",text);
    s_page_colors[row] = color;
}


static void draw_wifi_icon(int linked, int rssi)
{
    int level = rssi >= -55 ? 3 : rssi >= -70 ? 2 : 1;
    if (!linked || rssi >= 0 || rssi < -120) level = 0;
    uint16_t outer=level>=3?LCD_DARKBLUE:LCD_GRAY;
    uint16_t middle=level>=2?LCD_DARKBLUE:LCD_GRAY;
    uint16_t inner=level>=1?LCD_DARKBLUE:LCD_GRAY;
    const int x=5,y=28;
    lcd_draw_line(x,y+5,x+4,y+2,outer);lcd_draw_line(x+4,y+2,x+10,y,outer);
    lcd_draw_line(x+10,y,x+16,y+2,outer);lcd_draw_line(x+16,y+2,x+20,y+5,outer);
    lcd_draw_line(x+4,y+9,x+7,y+7,middle);lcd_draw_line(x+7,y+7,x+10,y+6,middle);
    lcd_draw_line(x+10,y+6,x+13,y+7,middle);lcd_draw_line(x+13,y+7,x+16,y+9,middle);
    lcd_draw_line(x+7,y+12,x+10,y+10,inner);lcd_draw_line(x+10,y+10,x+13,y+12,inner);
    lcd_fill(x+9,y+14,x+11,y+16,linked?LCD_DARKBLUE:LCD_GRAY);
    if(!linked)lcd_draw_line(x,y,x+20,y+16,LCD_RED);
}

static void draw_network_status(void)
{
    WifiLinkedInfo info = {0};
    int linked = GetLinkedInfo(&info) == WIFI_SUCCESS &&
                 info.connState == WIFI_CONNECTED && info.ipAddress;
    int ap = net_get_mode() == NET_MODE_AP;
    safety_mqtt_diag_t cloud;
    safety_mqtt_diag_get(&cloud);
    char line[48];
    snprintf(line,sizeof(line),"%d:%d:%d:%d",ap,linked,cloud.connected,info.rssi);
    if (strcmp(line,s_network_prev)) {
        lcd_fill(0,27,LCD_W-1,44,LCD_WHITE);
        draw_wifi_icon(linked,info.rssi);
        lcd_show_chinese(32,28,(uint8_t*)(ap?"本地热点":linked?"网络已连接":"网络未连接"),
                         linked?LCD_DARKBLUE:LCD_BLACK,LCD_WHITE,16,0);
        lcd_show_chinese(160,28,(uint8_t*)(ap?"云端未启用":cloud.connected?"云端已连接":"云端未连接"),
                         ap?LCD_GRAY:cloud.connected?LCD_DARKBLUE:LCD_RED,LCD_WHITE,16,0);
        snprintf(s_network_prev,sizeof(s_network_prev),"%s",line);
    }
}

static int cell_x(int i) { return (i % 2 == 0) ? COL_X0 : COL_X1; }
static int cell_y(int i) { return ROW_Y0 + ROW_STEP * (i / 2); }
static bool cell_is_ascii(int i) { return (i == 0 || i == 1 || i == 3 || i == 4); }

static void draw_static(void)
{
    s_draw_page = 0;
    int i;
    lcd_fill(0, 0, LCD_W, LCD_H, LCD_WHITE);
    lcd_show_chinese(88, 2, (uint8_t *)"居家安全监测", LCD_WHITE, LCD_RED, 24, 0);
    for (i = 0; i < NCELL; i++)
    {
        lcd_show_chinese(cell_x(i), cell_y(i), (uint8_t *)s_labels[i], LCD_BLACK, LCD_WHITE, 24, 0);
    }
    s_prev_valid = false;
    s_network_prev[0] = 0;
    draw_network_status();
}

void ui_init(void)
{
    lcd_init();
    draw_static();
}

void ui_show_main(void)
{
    draw_static();
}

/* 把本地 Unix 秒换算成 月/日/时/分 */
static void epoch_to_mdhm(unsigned int local, int *mo, int *dd, int *hh, int *mi)
{
    long days = (long)(local / 86400);
    long rem = (long)(local % 86400);
    long z = days + 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned long doe = (unsigned long)(z - era * 146097);
    unsigned long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned long mp = (5 * doy + 2) / 153;

    *dd = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *hh = (int)(rem / 3600);
    *mi = (int)((rem / 60) % 60);
}

void ui_show_events(int page)
{
    s_draw_page = 1;
    int i;
    int base = page * 8;
    char pg[8];

    lcd_fill(0, 0, LCD_W, LCD_H, LCD_WHITE);
    lcd_show_chinese(112, 2, (uint8_t *)"事件记录", LCD_WHITE, LCD_RED, 24, 0);
    snprintf(pg, sizeof(pg), "%d/2", page + 1);
    lcd_show_string(262, 6, (uint8_t *)pg, LCD_RED, LCD_WHITE, 16, 0);
    lcd_draw_line(0, 30, LCD_W - 1, 30, LCD_BLACK);

    for (i = 0; i < 8; i++)
    {
        char line[48];
        char tag[16];
        char tbuf[16];
        unsigned int up = 0;
        int dur = 0;
        const char *p;
        int t = 0;
        int y = 34 + i * 25;

        if (!event_log_get(base + i, line, sizeof(line)))
        {
            break;
        }

        /* 解析 "up=<秒>s <TAG> <值> d=<持续秒>" */
        p = line;
        if (strncmp(p, "up=", 3) == 0)
        {
            p += 3;
            up = (unsigned int)atoi(p);
            while (*p != 0 && *p != 's') { p++; }
            if (*p == 's') { p++; }
            while (*p == ' ') { p++; }
        }
        while (*p != 0 && *p != ' ' && t < (int)sizeof(tag) - 1)
        {
            tag[t++] = *p++;
        }
        tag[t] = 0;
        {
            const char *d = strstr(line, "d=");
            if (d != NULL)
            {
                dur = atoi(d + 2);
            }
        }

        /* 名称(24px) + 时间(16px) + 时长(16px) */
        lcd_show_chinese(4, y, (uint8_t *)event_log_tag_cn(tag), LCD_BLACK, LCD_WHITE, 24, 0);
        {
            const char *de = strstr(line, "e=");
            unsigned int epoch = (de != NULL) ? (unsigned int)atoi(de + 2) : 0;
            if (epoch > 0)
            {
                int mo, dd, hh, mi;
                epoch_to_mdhm(epoch + 8 * 3600, &mo, &dd, &hh, &mi);   /* 东八区 UTC+8 */
                snprintf(tbuf, sizeof(tbuf), "%02d-%02d %02d:%02d", mo, dd, hh, mi);
            }
            else
            {
                snprintf(tbuf, sizeof(tbuf), "%u:%02u", up / 60, up % 60);  /* 未对时: 开机时长 */
            }
        }
        lcd_show_string(108, y + 4, (uint8_t *)tbuf, LCD_GRAY, LCD_WHITE, 16, 0);
        snprintf(tbuf, sizeof(tbuf), "%ds", dur);
        lcd_show_string(208, y + 4, (uint8_t *)tbuf, LCD_GRAY, LCD_WHITE, 16, 0);
    }
}

static void cell_text(int i, char *out, int n,
                      double temp, double humi, bool body, double gas_ppm, double light,
                      bool smoke, bool fall_suspect, bool help_on, bool door_open,
                      const char *status, const safety_state_t *view,
                      const living_status_t *life)
{
    if ((i == 0 || i == 1) && !view->env_valid) { snprintf(out,n,"N/A"); return; }
    if (i == 8 && !view->door_valid) { snprintf(out,n,"--"); return; }
    switch (i)
    {
    case 0: snprintf(out, n, "%s%.1f C", view->env_cached ? "~" : "", temp); break;
    case 1: snprintf(out, n, "%s%.1f %%", view->env_cached ? "~" : "", humi); break;
    case 2: snprintf(out, n, "%s", !view->body_valid ? "待确认" : body ? "有活动" : "无活动"); break;
    case 3: snprintf(out, n, "%.0f ppm", gas_ppm); break;
    case 4: snprintf(out, n, "%.0f lx", light); break;
    case 5: snprintf(out, n, "%s", smoke ? "报警" : "正常"); break;
    case 6: snprintf(out, n, "%s", "未开启"); break;
    case 7: snprintf(out, n, "%s", help_on ? "开" : "关"); break;
    case 8: snprintf(out, n, "%s", door_open ? "开" : "关"); break;
    case 9: snprintf(out, n, "%s", status); break;
    case 10: snprintf(out,n,"%s",life->light_owner==3?"报警":life->light_owner==2?"提醒":life->night_on?"亮":life->config.night_auto?"自动":"关");break;
    case 11: snprintf(out,n,"%s",living_medicine_text(living_pending_slot(life)>=0?3:life->medicine_state));break;
    default: out[0] = 0; break;
    }
}

static void cell_draw(int i, const char *txt,
                      bool smoke, bool fall_suspect, bool help_on, bool alarmed)
{
    int x = cell_x(i) + VAL_DX;
    int y = cell_y(i);
    int x_end = cell_x(i) + COL_W;
    uint16_t fc = LCD_BLACK;

    if (x_end >= LCD_W) { x_end = LCD_W - 1; }
    lcd_fill(cell_x(i) + VAL_DX, y, x_end, y + 23, LCD_WHITE);

    if (i == 5) { fc = smoke ? LCD_RED : LCD_BLACK; }
    else if (i == 6) { fc = fall_suspect ? LCD_RED : LCD_BLACK; }
    else if (i == 7) { fc = help_on ? LCD_RED : LCD_BLACK; }
    else if (i == 9) { fc = alarmed ? LCD_RED : LCD_BLACK; }

    if (cell_is_ascii(i))
    {
        lcd_show_string(x, y, (uint8_t *)txt, fc, LCD_WHITE, 24, 0);
    }
    else
    {
        lcd_show_chinese(x, y, (uint8_t *)txt, fc, LCD_WHITE, 24, 0);
    }
}

static void ui_update_snapshot(double temp, double humi, bool body, double gas_ppm, double light,
               bool smoke, bool fall_suspect, bool help_on, bool door_open,
               bool alarmed, const char *status, const safety_state_t *view,
               const living_status_t *life)
{
    int i;
    char cur[NCELL][40];
    draw_network_status();

    for (i = 0; i < NCELL; i++)
    {
        cell_text(i, cur[i], sizeof(cur[i]), temp, humi, body, gas_ppm, light,
                  smoke, fall_suspect, help_on, door_open, status, view, life);
    }

    if (!s_prev_valid)
    {
        for (i = 0; i < NCELL; i++)
        {
            cell_draw(i, cur[i], smoke, fall_suspect, help_on, alarmed);
            strncpy(s_prev[i], cur[i], sizeof(s_prev[i]) - 1);
            s_prev[i][sizeof(s_prev[i]) - 1] = 0;
        }
        s_prev_valid = true;
        return;
    }

    for (i = 0; i < NCELL; i++)
    {
        if (strcmp(cur[i], s_prev[i]) != 0)
        {
            cell_draw(i, cur[i], smoke, fall_suspect, help_on, alarmed);
            strncpy(s_prev[i], cur[i], sizeof(s_prev[i]) - 1);
            s_prev[i][sizeof(s_prev[i]) - 1] = 0;
        }
    }
}


void ui_update(double temp, double humi, bool body, double gas_ppm, double light,
               bool smoke, bool fall_suspect, bool help_on, bool door_open,
               bool alarmed, const char *status)
{
    safety_state_t view; living_status_t life;
    safety_state_get(&view); living_get(&life);
    ui_update_snapshot(temp,humi,body,gas_ppm,light,smoke,fall_suspect,help_on,
                       door_open,alarmed,status,&view,&life);
}

void ui_show_diagnostics(void)
{
    char lines[9][48],ip[16]="";WifiLinkedInfo li;safety_state_t state;safety_config_t cfg;
    safety_state_get(&state);safety_config_get(&cfg);memset(&li,0,sizeof(li));
    if(net_get_mode()==NET_MODE_AP)strcpy(ip,"192.168.2.1");
    else if(GetLinkedInfo(&li)==WIFI_SUCCESS&&li.ipAddress){ip4_addr_t a;a.addr=li.ipAddress;ip4addr_ntoa_r(&a,ip,sizeof(ip));}
    snprintf(lines[0],sizeof(lines[0]),"设备状态 %s",SAFETY_FW);
    snprintf(lines[1],sizeof(lines[1]),"模式: %s",net_get_mode()==NET_MODE_AP?"本地热点":"联网模式");
    snprintf(lines[2],sizeof(lines[2]),"地址: %s:8080",ip[0]?ip:"未连接");
    snprintf(lines[3],sizeof(lines[3]),"云端: %s",net_get_mode()==NET_MODE_AP?"本地模式":safety_mqtt_is_connected()?"已连接":"未连接");
    snprintf(lines[4],sizeof(lines[4]),"温湿度:%s",state.env_valid?"正常":"异常");
    snprintf(lines[5],sizeof(lines[5]),"人体:%s",!state.body_valid?"待确认":state.body?"有活动":"无活动");
    snprintf(lines[6],sizeof(lines[6]),"无活动:%u秒",state.inactive);
    living_status_t life;living_get(&life);
    snprintf(lines[7],sizeof(lines[7]),"夜灯:%s 服药:%s",life.night_on?"开启":"关闭",living_medicine_text(life.medicine_state));
    snprintf(lines[8],sizeof(lines[8]),"K5翻页  长按K5切换网络");
#if KEY_ADC_DIAGNOSTIC
    snprintf(lines[5],sizeof(lines[5]),"KEY ADC %.3fV",key_last_voltage());
    snprintf(lines[6],sizeof(lines[6]),"K3 K4 K5 K6: read voltage");
    snprintf(lines[7],sizeof(lines[7]),"KEY ACTIONS OFF / WDT ON");
    snprintf(lines[8],sizeof(lines[8]),"Release keys at boot; then press");
#endif
    page_begin(2);
    for(int i=0;i<9;i++)page_row(i,4+i*25,lines[i],LCD_BLACK,16,1);
}

void ui_show_living(const living_config_t *draft,int field,int result) {
 living_status_t s;living_get(&s);living_config_t c=draft?*draft:s.config;
 char line[80];
 if(page_begin(3))lcd_show_chinese(112,2,(uint8_t*)"生活提醒",LCD_WHITE,LCD_DARKBLUE,24,0);
 snprintf(line,sizeof(line),"%s夜灯 %s",field==0?">":" ",c.night_auto?"自动":"关闭");
 page_row(0,35,line,LCD_BLACK,24,1);
 for(int i=0;i<3;i++) {
  int minute=living_slot_minute(&c,i),selected=(field>=1&&field<=9&&(field-1)/3==i)||field==10+i;
  snprintf(line,sizeof(line),"%s%d %02d:%02d %s",selected?">":" ",i+1,minute/60,minute%60,living_slot_enabled(&c,i)?living_medicine_text(s.slot_state[i]):"未开启");
  page_row(i+1,69+i*28,line,s.slot_state[i]==3?LCD_RED:LCD_DARKBLUE,16,1);
 }
 int pending=living_pending_slot(&s);
 const char *part=field<0?"K6设置":field==0?"夜灯":field>=10?"服药状态":(field-1)%3==0?"开启关闭":(field-1)%3==1?"时":"分";
 if(field<0 && pending>=0)snprintf(line,sizeof(line),"K6确认第%d次 K4消音",pending+1);
 else snprintf(line,sizeof(line),"%s K6选择 K4修改",part);
 page_row(4,158,line,LCD_BLACK,16,1);
 if(result>=100 && result<=102)snprintf(line,sizeof(line),"已确认第%d时段服药",result-99);
 else snprintf(line,sizeof(line),"%s",result==0?"已保存或已确认":result==4?"配置已变更 请重新编辑":result>0?"操作失败 请重试":field<0?"长按K6设置 K5翻页":"长按K6保存 K5取消翻页");
 page_row(5,183,line,LCD_BLACK,16,1);
 page_row(6,208,s.time_synced?"北京时间 已同步":"时间待同步",LCD_BLACK,16,1);
}

/* Only this worker draws after ui_init. The safety monitor never waits for
 * SPI or a display queue: a slow display simply skips obsolete snapshots. */
typedef struct {
    safety_state_t state;
    living_status_t life;
    living_config_t draft;
    char status[32];
    int page,field,result,has_draft;
    unsigned serial,event_revision;
} ui_frame_t;
static ui_frame_t s_requested;
static int s_worker_started;
void ui_present(int page,const safety_state_t *state,const char *status,
                const living_config_t *draft,int field,int result,int events_changed)
{
    if(page<0 || page>4 || !state || !status)return;
    living_status_t life;living_get(&life);
    safety_lock();
    s_requested.life=life;
    s_requested.page=page;s_requested.state=*state;
    snprintf(s_requested.status,sizeof(s_requested.status),"%s",status);
    s_requested.has_draft=draft!=NULL;
    if(draft)s_requested.draft=*draft;
    s_requested.field=field;s_requested.result=result;
    s_requested.serial++;
    if(events_changed)s_requested.event_revision++;
    safety_unlock();
}
#ifndef SAFETY_HOST_TEST
static
#endif
void ui_render_pending(void)
{
    static unsigned drawn_serial,drawn_events;
    static int drawn_page=-1;
    ui_frame_t f;
    safety_lock();f=s_requested;safety_unlock();
    if(!f.serial || f.serial==drawn_serial)return;
    int entering=drawn_page!=f.page;
    if(f.page==0) {
        if(entering)ui_show_main();
        ui_update_snapshot(f.state.temp,f.state.humi,f.state.body,f.state.gas,f.state.light,
                  f.state.smoke,f.state.fall,f.state.help,f.state.door,f.state.alarm,f.status,&f.state,&f.life);
    } else if(f.page<3) {
        if(entering || drawn_events!=f.event_revision)ui_show_events(f.page-1);
    } else if(f.page==3)ui_show_diagnostics();
    else ui_show_living(f.has_draft?&f.draft:NULL,f.field,f.result);
    drawn_page=f.page;drawn_serial=f.serial;drawn_events=f.event_revision;
}
static void ui_task(void *arg)
{
    (void)arg;
#if KEY_ADC_DIAGNOSTIC
    float baseline=0.0f, previous=0.0f, held=0.0f;
    unsigned stable=0;
    bool seeded=false, have_held=false;
    while(1) {
        float v=key_last_voltage();
        /* ADC diagnostics must not wait for a sensor sample or a network query. */
        if(!seeded && v>0.05f) {baseline=v;seeded=true;}
        float change=v-previous;if(change<0)change=-change;
        if(change<=0.025f) {if(stable<3)stable++;} else stable=0;
        previous=v;
        float excursion=v-baseline;if(excursion<0)excursion=-excursion;
        if(seeded && stable>=3 && excursion>0.08f) {held=v;have_held=true;}
        char line[48];
        page_begin(5);
        page_row(0,4,"KEY ADC diagnostic",LCD_DARKBLUE,16,0);
        snprintf(line,sizeof(line),"LIVE: %.3f V",v);
        page_row(1,40,line,LCD_BLACK,24,0);
        if(have_held)snprintf(line,sizeof(line),"HOLD: %.3f V",held);
        else snprintf(line,sizeof(line),"HOLD: --");
        page_row(2,80,line,LCD_DARKBLUE,24,0);
        snprintf(line,sizeof(line),"BOOT: %.3f V",baseline);
        page_row(3,120,line,LCD_BLACK,16,0);
        page_row(4,150,"Press K3 / K4 / K5 / K6",LCD_BLACK,16,0);
        page_row(5,177,"HOLD keeps last stable change",LCD_BLACK,16,0);
        page_row(6,204,"Actions OFF; watchdog ON",LCD_BLACK,16,0);
        LOS_Msleep(50);
    }
#else
    while(1){ui_render_pending();LOS_Msleep(100);}
#endif
}
int ui_start(void)
{
    if(s_worker_started)return 0;
    unsigned id;TSK_INIT_PARAM_S task={0};
    task.pfnTaskEntry=(TSK_ENTRY_FUNC)ui_task;task.uwStackSize=8192;
    task.pcName="safety display";task.usTaskPrio=25;
    if(LOS_TaskCreate(&id,&task)!=LOS_OK){printf("[lcd] display task create failed\r\n");return -1;}
    s_worker_started=1;return 0;
}
