#include "local_access.h"
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

/* 320x240 overview: safety first, four large readings, fixed power information. */
#define UI_BLUE 0x2AD1
#define UI_INK 0x21A9
#define UI_MUTED 0x63B0
#define UI_LINE 0xD73D
#define UI_BG 0xF7BF
#define UI_RED 0xC228
#define UI_AMBER 0x82A4
#define UI_GOOD 0x2C70
static char s_overview[12][80];
static uint16_t s_overview_color[12];
typedef struct { char from[80]; int from_size,to_size,active,step; uint64_t start; } metric_anim_t;
static metric_anim_t s_metric[4];
static void metric_animate(void)
{
    uint64_t now=LOS_TickCountGet();
    for(int i=0;i<4;i++) {
        metric_anim_t *a=&s_metric[i];
        if(!a->active)continue;
        unsigned elapsed=(unsigned)(now-a->start);
        int step=elapsed>=240?32:(int)(elapsed*32/240);
        if(step==a->step)continue;
        a->step=step;
        lcd_metric_slide(121+(i%2)*102,75+(i/2)*82,a->from,s_overview[6+i],
                         a->from_size,a->to_size,step,UI_INK,LCD_WHITE);
        if(step==32)a->active=0;
    }
}
static int __attribute__((unused)) metric_active(void)
{
    for(int i=0;i<4;i++)if(s_metric[i].active)return 1;
    return 0;
}

static char s_network_prev[48];
/* Main/event pages invalidate these caches. Re-entering a page clears once;
 * subsequent calls touch only rows whose displayed text or color changed. */
static int s_draw_page = -1;
static char s_page_rows[16][96];
static uint16_t s_page_colors[16],s_page_backgrounds[16];
static int page_begin(int page)
{
    if (s_draw_page == page) return 0;
    s_draw_page = page;
    memset(s_page_rows, 0, sizeof(s_page_rows));
    memset(s_page_colors, 0, sizeof(s_page_colors));
    memset(s_page_backgrounds, 0, sizeof(s_page_backgrounds));
    lcd_fill(0, 0, LCD_W, LCD_H, LCD_WHITE);
    return 1;
}
#if KEY_ADC_DIAGNOSTIC
static void page_row(int row, int y, const char *text, uint16_t color, int size, int chinese)
{
    if (!strcmp(s_page_rows[row], text) && s_page_colors[row] == color) return;
    lcd_fill(0, y, LCD_W, y+size, LCD_WHITE);
    if (chinese) lcd_show_chinese(4,y,(uint8_t*)text,color,LCD_WHITE,size,0);
    else lcd_show_string(4,y,(uint8_t*)text,color,LCD_WHITE,size,0);
    snprintf(s_page_rows[row],sizeof(s_page_rows[row]),"%s",text);
    s_page_colors[row] = color;
}

#endif
static int panel_begin(int page,const char *title,const char *marker)
{
    if(!page_begin(page))return 0;
    lcd_fill(0,25,LCD_W,LCD_H,UI_BG);
    lcd_fill(0,0,LCD_W,25,UI_BLUE);
    lcd_show_chinese(8,4,(uint8_t*)title,LCD_WHITE,UI_BLUE,16,0);
    lcd_show_chinese(256,4,(uint8_t*)marker,LCD_WHITE,UI_BLUE,16,0);
    return 1;
}
static int panel_text(int row,int x,int y,int width,int height,const char *text,
                       uint16_t fg,uint16_t bg,int size)
{
    if(!strcmp(s_page_rows[row],text)&&s_page_colors[row]==fg&&s_page_backgrounds[row]==bg)return 0;
    lcd_fill(x,y,x+width,y+height,bg);
    lcd_show_chinese(x+4,y+(height-size)/2,(uint8_t*)text,fg,bg,size,0);
    snprintf(s_page_rows[row],sizeof(s_page_rows[row]),"%s",text);
    s_page_colors[row]=fg;s_page_backgrounds[row]=bg;
    return 1;
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

static void metric_icon(int kind,int x,int y)
{
    uint16_t color=UI_BLUE;
    if(kind==0) {
        lcd_draw_line(x+4,y+1,x+4,y+10,color);lcd_draw_line(x+8,y+1,x+8,y+10,color);
        lcd_draw_line(x+4,y+1,x+8,y+1,color);lcd_draw_circle(x+6,y+12,4,color);
    } else if(kind==1) {
        lcd_draw_line(x+6,y,x+1,y+8,color);lcd_draw_line(x+6,y,x+11,y+8,color);
        lcd_draw_line(x+1,y+8,x+1,y+12,color);lcd_draw_line(x+11,y+8,x+11,y+12,color);
        lcd_draw_line(x+1,y+12,x+6,y+15,color);lcd_draw_line(x+11,y+12,x+6,y+15,color);
    } else if(kind==2) {
        lcd_draw_circle(x+6,y+8,3,color);
        lcd_draw_line(x+6,y,x+6,y+3,color);lcd_draw_line(x+6,y+13,x+6,y+16,color);
        lcd_draw_line(x,y+8,x+1,y+8,color);lcd_draw_line(x+11,y+8,x+13,y+8,color);
        lcd_draw_line(x+1,y+2,x+3,y+4,color);lcd_draw_line(x+10,y+12,x+12,y+14,color);
    } else {
        for(int i=0;i<3;i++) {
            lcd_draw_line(x+i*4,y+14,x+i*4,y+10,color);
            lcd_draw_line(x+i*4,y+10,x+i*4+2,y+6,color);
            lcd_draw_line(x+i*4+2,y+6,x+i*4+2,y+2,color);
        }
    }
}

static void draw_static(void)
{
    s_draw_page=0;
    memset(s_overview,0,sizeof(s_overview));
    memset(s_metric,0,sizeof(s_metric));
    memset(s_overview_color,0,sizeof(s_overview_color));
    lcd_fill(0,0,LCD_W,LCD_H,UI_BG);
    lcd_fill(0,0,LCD_W,25,UI_BLUE);
    lcd_show_chinese(8,4,(uint8_t*)"颐鸿安",LCD_WHITE,UI_BLUE,16,0);
    lcd_show_chinese(144,4,(uint8_t*)"居家安全守护",LCD_WHITE,UI_BLUE,16,0);
    lcd_fill(4,49,109,211,LCD_WHITE);
    lcd_draw_rectangle(4,49,108,210,UI_LINE);
    lcd_show_chinese(12,55,(uint8_t*)"安全状态",UI_BLUE,LCD_WHITE,16,0);
    const char *labels[]={"温度","湿度","光照","烟气估算"};
    const char *units[]={"C","%","lx","参考ppm"};
    for(int i=0;i<4;i++) {
        int x=114+(i%2)*102,y=49+(i/2)*82;
        lcd_fill(x,y,x+98,y+79,LCD_WHITE);
        lcd_draw_rectangle(x,y,x+97,y+78,UI_LINE);
        lcd_show_chinese(x+7,y+5,(uint8_t*)labels[i],UI_MUTED,LCD_WHITE,16,0);
        lcd_show_chinese(x+7,y+59,(uint8_t*)units[i],UI_MUTED,LCD_WHITE,16,0);
        metric_icon(i,x+80,y+5);
    }
    s_network_prev[0]=0;
    draw_network_status();
}

/* Cache the displayed string AND its color: an alarm priority change must not
 * cause a full-screen clear, nor leave an old red indicator behind. */
static void overview_text(int id,int x,int y,int width,int size,const char *text,uint16_t color,uint16_t bg)
{
    if(!strcmp(s_overview[id],text)&&s_overview_color[id]==color)return;
    if(id>=6 && id<=9) {
        metric_anim_t *a=&s_metric[id-6];
        /* First frame, invalid/cache markers and interrupted animations snap:
         * never keep a stale or imaginary intermediate sensor value. */
        if(s_overview[id][0] && text[0]!='-' && 
           s_overview[id][0]!='-' && !a->active) {
            snprintf(a->from,sizeof(a->from),"%s",s_overview[id]);
            a->from_size=strlen(a->from)<=5?32:24;a->to_size=size;
            a->start=LOS_TickCountGet();a->step=-1;a->active=1;
            snprintf(s_overview[id],sizeof(s_overview[id]),"%s",text);
            s_overview_color[id]=color;
            return;
        }
        a->active=0;
    }
    if(id>=1 && id<=5) {
        lcd_fill(10,y+6,14,y+10,color);x=18;width=88;
    }
    lcd_fill(x,y,x+width,y+((id>=6 && id<=9)?32:size),bg);
    lcd_show_chinese(x,y,(uint8_t*)text,color,bg,size,0);
    snprintf(s_overview[id],sizeof(s_overview[id]),"%s",text);
    s_overview_color[id]=color;
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

/* Eight compact records per page, stable event snapshots instead of parsing
 * formatted strings. Zero duration means unknown/instant, never proof of recovery. */
void ui_show_events(int page)
{
    safety_event_t entries[16];
    safety_state_t state;living_status_t life;
    int count=event_log_snapshot(entries),base=page*8;
    safety_state_get(&state);living_get(&life);
    char text[96],marker[12];
    snprintf(marker,sizeof(marker),"%d/2",page+1);
    panel_begin(10+page,"颐鸿安 / 事件记录",marker);
    snprintf(text,sizeof(text),"最近%d条 / 最多16条  北京时间",count);
    panel_text(8,4,28,312,18,text,UI_MUTED,UI_BG,16);
    for(int row=0;row<8;row++) {
        int index=base+row,y=49+row*20;
        char signature[96]="",time_text[24]="",state_text[20]="";
        uint16_t fg=UI_INK,bg=row%2?UI_BG:LCD_WHITE;
        const char *name="";
        if(index<count) {
            safety_event_t *e=&entries[index];name=event_log_tag_cn(e->tag);
            int active=0;
            if(e->boot==safety_boot_id()) {
                if(!strcmp(e->tag,"HELP"))active=state.help;
                else if(!strcmp(e->tag,"FALL"))active=state.fall;
                else if(!strcmp(e->tag,"GAS"))active=state.gas_alarm;
                else if(!strcmp(e->tag,"NOACT"))active=state.status==4;
                else if(!strcmp(e->tag,"ENV"))active=state.status==5;
                else if(!strcmp(e->tag,"MED_DUE") && e->val>=1 && e->val<=3)active=life.slot_state[e->val-1]==3;
                for(int i=0;i<index;i++)if(!strcmp(entries[i].tag,e->tag) && (strcmp(e->tag,"MED_DUE") || entries[i].val==e->val)){active=0;break;}
            }
            int alarm_tag=!strcmp(e->tag,"HELP")||!strcmp(e->tag,"FALL")||!strcmp(e->tag,"GAS")||!strcmp(e->tag,"NOACT")||!strcmp(e->tag,"ENV");
            snprintf(state_text,sizeof(state_text),"%s",active?"报警中":e->dur>0?"已结束":alarm_tag?"待核实":"已记录");
            if(active)fg=UI_RED;
            if(e->epoch) {
                int mo,dd,hh,mi;epoch_to_mdhm(e->epoch+8*3600,&mo,&dd,&hh,&mi);
                snprintf(time_text,sizeof(time_text),"%02d-%02d %02d:%02d",mo,dd,hh,mi);
            } else snprintf(time_text,sizeof(time_text),"开机%um%us",e->up/60,e->up%60);
            snprintf(signature,sizeof(signature),"%u:%s:%s:%s",e->id,name,time_text,state_text);
        }
        if(row==0 && index>=count){snprintf(signature,sizeof(signature),"empty");fg=UI_MUTED;}
        if(strcmp(s_page_rows[row],signature)||s_page_colors[row]!=fg) {
            lcd_fill(4,y,316,y+20,bg);
            if(index<count) {
                lcd_show_chinese(9,y+2,(uint8_t*)name,fg,bg,16,0);
                lcd_show_chinese(91,y+2,(uint8_t*)time_text,UI_MUTED,bg,16,0);
                lcd_show_chinese(257,y+2,(uint8_t*)state_text,fg,bg,16,0);
            }
            else if(row==0)lcd_show_chinese(9,y+2,(uint8_t*)"本页暂无记录",UI_MUTED,bg,16,0);
            snprintf(s_page_rows[row],sizeof(s_page_rows[row]),"%s",signature);s_page_colors[row]=fg;
        }
    }
    const char *hint=state.alarm?"K6确认当前报警 K4消音 K5翻页":living_pending_slot(&life)>=0?"K6确认服药 K5翻页":"长按K6清记录 K5翻页";
    panel_text(10,4,215,312,22,hint,UI_BLUE,UI_BG,16);
}

static void ui_update_snapshot(double temp, double humi, bool body, double gas_ppm, double light,
               bool smoke, bool fall_suspect, bool help_on, bool door_open,
               bool alarmed, const char *status, const safety_state_t *view,
               const living_status_t *life)
{
    (void)fall_suspect;(void)door_open;(void)status;
    draw_network_status();
    char text[80];
    safety_quality_t quality;
    safety_state_quality(view,(unsigned int)(LOS_TickCountGet()/1000),&quality);
    int stale=quality.stale || !view->sample_received;
    int unknown=stale || !quality.env_available || !view->body_valid;
    const char *headline=help_on?"求助中":alarmed?(view->status==3?"烟气异常":view->status==4?"活动异常":view->status==5?"环境异常":"有异常"):unknown?"待核实":"正常";
    uint16_t risk=alarmed||help_on?UI_RED:unknown?UI_AMBER:UI_GOOD;
    if(s_overview_color[0]!=risk)lcd_draw_rectangle(4,49,108,210,risk);
    overview_text(0,9,78,96,24,headline,risk,LCD_WHITE);
    if(stale || !view->body_valid)snprintf(text,sizeof(text),"人体 待确认");
    else snprintf(text,sizeof(text),"人体 %s",body?"有活动":"无活动");
    overview_text(1,10,108,96,16,text,stale||!view->body_valid?UI_AMBER:body?UI_BLUE:UI_MUTED,LCD_WHITE);
    snprintf(text,sizeof(text),"求助 %s",stale?"待确认":help_on?"报警":"正常");
    overview_text(2,10,128,96,16,text,stale?UI_AMBER:help_on?UI_RED:UI_GOOD,LCD_WHITE);
    snprintf(text,sizeof(text),"烟气 %s",stale?"待确认":smoke||view->gas_alarm?"报警":"正常");
    overview_text(3,10,148,96,16,text,stale?UI_AMBER:smoke||view->gas_alarm?UI_RED:UI_GOOD,LCD_WHITE);
    snprintf(text,sizeof(text),"夜灯 %s",life->light_owner==3?"报警":life->light_owner==2?"提醒":life->night_on?"开启":"关闭");
    overview_text(4,10,168,96,16,text,UI_INK,LCD_WHITE);
    snprintf(text,sizeof(text),"服药 %s",living_medicine_text(living_pending_slot(life)>=0?3:life->medicine_state));
    overview_text(5,10,188,96,16,text,living_pending_slot(life)>=0?UI_AMBER:UI_INK,LCD_WHITE);
    for(int i=0;i<4;i++) {
        if(stale || (i<2 && !quality.env_available))snprintf(text,sizeof(text),"--");
        else if(i<2)snprintf(text,sizeof(text),"%.1f",i==0?temp:humi);
        else snprintf(text,sizeof(text),"%.0f",i==2?light:gas_ppm);
        overview_text(6+i,121+(i%2)*102,75+(i/2)*82,84,strlen(text)<=5?32:24,text,UI_INK,LCD_WHITE);
    }
    safety_event_t entry;int count=0;
    while(count<16 && event_log_entry(count,&entry))count++;
    if(stale || !view->body_valid)snprintf(text,sizeof(text),"活动待确认  记录%d",count);
    else if(view->inactive>=3600)snprintf(text,sizeof(text),"无活动%dh  记录%d",view->inactive/3600,count);
    else snprintf(text,sizeof(text),"无活动%ds  记录%d",view->inactive,count);
    overview_text(10,5,215,207,16,text,UI_MUTED,UI_BG);
    overview_text(11,218,215,100,16,"外接电源",UI_BLUE,UI_BG);
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
    safety_state_t state;WifiLinkedInfo info={0};char text[96],ip[20]="";
    safety_state_get(&state);
    safety_quality_t quality;safety_state_quality(&state,(unsigned int)(LOS_TickCountGet()/1000),&quality);
    int ap=net_get_mode()==NET_MODE_AP;
    int linked=GetLinkedInfo(&info)==WIFI_SUCCESS && info.connState==WIFI_CONNECTED && info.ipAddress;
    if(ap)strcpy(ip,"192.168.2.1");
    else if(linked){ip4_addr_t a;a.addr=info.ipAddress;ip4addr_ntoa_r(&a,ip,sizeof(ip));}
    panel_begin(2,"颐鸿安 / 设备状态",SAFETY_FW);
    snprintf(text,sizeof(text),"模式: %s",ap?"本地热点 / 离线可用":"联网模式 / 云端同步");
    panel_text(0,4,28,312,20,text,UI_BLUE,UI_BG,16);
    if(ap)snprintf(text,sizeof(text),"网络: HomeSafety 热点");
    else if(linked && info.ssid[0])snprintf(text,sizeof(text),"热点: %.32s",info.ssid);
    else snprintf(text,sizeof(text),"网络: %s",linked?"Wi-Fi已连接":"连接中 / 请检查热点");
    panel_text(1,8,52,304,22,text,!ap&&!linked?UI_AMBER:UI_INK,LCD_WHITE,16);
    snprintf(text,sizeof(text),"地址: %s%s",ip[0]?ip:"未获取IP",ip[0]?":8080":"");
    panel_text(2,8,75,304,22,text,UI_MUTED,LCD_WHITE,16);
    snprintf(text,sizeof(text),"云端: %s",ap?"本地模式未启用":safety_mqtt_is_connected()?"已连接":"未连接 / 自动重试");
    panel_text(3,8,98,304,22,text,ap?UI_MUTED:safety_mqtt_is_connected()?UI_BLUE:UI_AMBER,LCD_WHITE,16);
    snprintf(text,sizeof(text),"温湿度: %s",quality.stale?"采样待更新":!quality.env_available?"异常 / 待重试":state.env_cached?"缓存 / 正在重试":"正常");
    panel_text(4,8,121,304,22,text,!quality.env_available||state.env_cached?UI_AMBER:UI_INK,LCD_WHITE,16);
    snprintf(text,sizeof(text),"人体: %s",quality.stale||!state.body_valid?"待确认":state.body?"检测到活动":"暂未检测到活动");
    panel_text(5,8,144,304,22,text,quality.stale||!state.body_valid?UI_AMBER:UI_INK,LCD_WHITE,16);
    panel_text(6,8,167,304,22,"供电: 外接电源 / 电量未采集",UI_INK,LCD_WHITE,16);
    snprintf(text,sizeof(text),"%s",ap?"NFC或浏览器可查看本地页面":"同网访问上方地址 / 自动重连");
    panel_text(7,4,191,312,20,text,UI_MUTED,UI_BG,16);
    panel_text(8,4,215,312,22,"K5翻页  长按K5切换网络并重启",UI_BLUE,UI_BG,16);
}

void ui_show_living(const living_config_t *draft,int field,int result)
{
    static int previous_field=-999;
    living_status_t life;living_get(&life);living_config_t config=draft?*draft:life.config;
    safety_state_t state;safety_state_get(&state);
    int pending=living_pending_slot(&life),editing=field>=0;char text[96];
    panel_begin(3,"颐鸿安 / 生活提醒",editing?"编辑中":"3次/天");
    // The header marker changes without page re-entry, so cache it separately.
    panel_text(9,252,0,68,25,editing?"编辑中":"3次/天",LCD_WHITE,UI_BLUE,16);
    const char *part=field==0?"夜灯模式":field>=10?"服药状态":field>0?(field-1)%3==0?"提醒开关":(field-1)%3==1?"提醒小时":"提醒分钟":"";
    snprintf(text,sizeof(text),"%s%s",editing?"正在修改: ":"",editing?part:life.time_synced?"北京时间已同步":"时间待同步 / 请先联网");
    panel_text(0,4,28,312,20,text,editing?UI_BLUE:life.time_synced?UI_MUTED:UI_AMBER,UI_BG,16);
    snprintf(text,sizeof(text),"%s夜灯 %s / %s",field==0?"> ":"",config.night_auto?"自动":"手动",life.light_owner==3?"报警优先":life.light_owner==2?"提醒优先":life.night_on?"已亮":"已关");
    panel_text(1,8,52,304,27,text,UI_BLUE,field==0?UI_LINE:LCD_WHITE,16);
    for(int i=0;i<3;i++) {
        int minute=living_slot_minute(&config,i),selected=(field>=1&&field<=9&&(field-1)/3==i)||field==10+i;
        int enabled=living_slot_enabled(&config,i);
        int old_slot=previous_field>=10?previous_field-10:previous_field>=1?(previous_field-1)/3:-1;
        if(previous_field!=field && (selected || old_slot==i))s_page_rows[2+i][0]=0;
        int part_index=field>=1 && field<=9?(field-1)%3:-1;
        const char *slot_text=!enabled?"未开启":selected&&part_index==0?"已开启":living_medicine_text(life.slot_state[i]);
        snprintf(text,sizeof(text),"%s第%d次   %02d:%02d   %s",selected?"> ":"  ",i+1,minute/60,minute%60,slot_text);
        uint16_t fg=enabled&&life.slot_state[i]==3?UI_RED:enabled?UI_INK:UI_MUTED;
        if(panel_text(2+i,8,84+i*35,304,30,text,fg,selected?UI_LINE:LCD_WHITE,16) && selected) {
            int x0=part_index==1?92:part_index==2?116:156;
            int x1=part_index==1?108:part_index==2?132:204;
            lcd_draw_line(x0,108+i*35,x1,108+i*35,UI_BLUE);
        }
    }
    const char *feedback=result==0?"已保存或已确认":result==4?"配置已变更 / 请重新编辑":result>0?"操作失败 / 请重试":editing?(field>=10?"K4立即更改当天服药状态":"修改未保存 / K5取消"):
        state.alarm?"请先核实当前报警":pending>=0?"到时未服药 / 请核实后确认":"按计划提醒 / 可自主开启";
    if(result>=100 && result<=102)snprintf(text,sizeof(text),"已确认第%d次服药",result-99);
    else snprintf(text,sizeof(text),"%s",feedback);
    panel_text(5,4,191,312,20,text,result>0&&result<100?UI_AMBER:pending>=0&&!editing?UI_RED:UI_MUTED,UI_BG,16);
    const char *hint=state.alarm?"K6确认报警 K4消音 K5翻页":editing?(field>=10?"K4改状态 K6下一项 K5退出":"K4改 K6选 长按K6保存 K5取消"):
        pending>=0?"K6确认服药 K4消音 K5翻页":"K6设置 K5翻页";
    panel_text(6,4,215,312,22,hint,UI_BLUE,UI_BG,16);
    previous_field=field;
}

static void ui_show_contact(void) {
    int selected,board,reply,confirmed;char text[100];
    const char *sent[]={"暂无消息","我一切正常","请给我打电话","需要生活协助","网页文字消息"};
    const char *received[]={"等待家人回复","已收到消息","正在赶来","已联系人员协助","收到文字消息"};
    local_message_view(&selected,&board,&reply,&confirmed);
    panel_begin(20,"颐鸿安 / 联系家人","本地");
    panel_text(0,8,30,304,24,"选择要发送的消息",UI_MUTED,LCD_WHITE,16);
    panel_text(1,8,59,304,25,sent[selected],UI_BLUE,LCD_WHITE,16);
    snprintf(text,sizeof(text),"最近发送: %s",sent[board]);
    panel_text(2,8,90,304,25,text,UI_INK,LCD_WHITE,16);
    panel_text(3,8,121,304,25,received[reply],reply?UI_BLUE:UI_MUTED,LCD_WHITE,16);
    panel_text(4,8,151,304,23,reply?(confirmed?"已确认家人回复":"K6确认已看到回复"):"同一网络网页可回复",UI_MUTED,LCD_WHITE,16);
    panel_text(5,4,183,312,24,"紧急情况请按K3主动求助",UI_RED,UI_BG,16);
    panel_text(6,4,215,312,22,"K4选择  K6发送/确认  K5翻页",UI_BLUE,UI_BG,16);
}
static void ui_show_pairing(void) {
    char text[80];unsigned pending=local_pair_pending(LOS_TickCountGet());
    panel_begin(21,"颐鸿安 / 手机配对","本地");
    snprintf(text,sizeof(text),"已授权会话: %u / 4",local_pair_count(LOS_TickCountGet()));
    panel_text(0,8,30,304,24,text,UI_INK,LCD_WHITE,16);
    if(pending)snprintf(text,sizeof(text),"待确认请求: %u",pending);
    else snprintf(text,sizeof(text),"等待手机发起配对");
    panel_text(1,8,64,304,25,text,pending?UI_BLUE:UI_MUTED,LCD_WHITE,16);
    panel_text(2,8,101,304,24,"核对手机显示的请求编号",UI_INK,LCD_WHITE,16);
    panel_text(3,8,134,304,24,"请求60秒有效  授权30分钟",UI_MUTED,LCD_WHITE,16);
    panel_text(4,8,167,304,24,"重启后需要重新配对",UI_MUTED,LCD_WHITE,16);
    panel_text(5,4,215,312,22,"K6同意  K4撤销全部  K5翻页",UI_BLUE,UI_BG,16);
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
    if(page<0 || page>6 || !state || !status)return;
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
    static unsigned drawn_serial;
    static int drawn_page=-1;
    ui_frame_t f;
    safety_lock();f=s_requested;safety_unlock();
    if(!f.serial)return;
    if(f.serial==drawn_serial) {
        if(f.page==0)metric_animate();
        return;
    }
    int entering=drawn_page!=f.page;
    if(f.page==0) {
        if(entering)ui_show_main();
        ui_update_snapshot(f.state.temp,f.state.humi,f.state.body,f.state.gas,f.state.light,
                  f.state.smoke,f.state.fall,f.state.help,f.state.door,f.state.alarm,f.status,&f.state,&f.life);
    } else if(f.page<3) {
        ui_show_events(f.page-1);
    } else if(f.page==3)ui_show_diagnostics();
    else if(f.page==4)ui_show_living(f.has_draft?&f.draft:NULL,f.field,f.result);
    else if(f.page==5)ui_show_contact();
    else ui_show_pairing();
    if(f.page==0)metric_animate();
    else memset(s_metric,0,sizeof(s_metric));
    drawn_page=f.page;drawn_serial=f.serial;
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
    while(1){ui_render_pending();LOS_Msleep(metric_active()?40:100);}
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
