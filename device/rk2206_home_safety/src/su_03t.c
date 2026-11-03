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

#include <stdio.h>
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

/* 供语音查询回传的实时数值 */
static int s_query_prefix=-1;
static uint64_t s_prefix_up;
static voice_policy_t s_voice;
static unsigned s_medicine_mask;
static void write_frame(const uint8_t *data,unsigned size)
{
    /* Prevent query and alarm tasks from interleaving complete UART frames.
     * UART is configured nonblocking; never wait for playback under this lock. */
    safety_lock();
    int n=IoTUartWrite(UART2_HANDLE,data,size);
    safety_unlock();
    if(n!=(int)size)printf("[voice] UART frame incomplete %d/%u\n",n,size);
}
void su03t_process_voices(const bool alarms[5],unsigned medicine_mask,
                         int medicine_audible,int muted,uint64_t now)
{
    unsigned active=0;
    for(unsigned i=0;i<5;i++)if(alarms[i])active|=1U<<i;
    medicine_mask &= 7U;
    if(medicine_mask & ~s_medicine_mask) {
        s_voice.previous &= ~(1U<<5); /* A newly due dose, not an undo of another slot. */
    }
    s_medicine_mask=medicine_mask;
    if(medicine_mask)active|=1U<<5;
    unsigned rank=voice_policy_next(&s_voice,active,muted || (active==32U && !medicine_audible),now);
    if(!rank)return;
    const uint8_t indices[6]={8,7,6,4,5,9};
    if(rank==6 && !SU03T_MEDICINE_PROFILE_ENABLED) {
        printf("[voice] medicine skipped: index 9 vocabulary not verified\n");return;
    }
    su03t_send_u8_msg(indices[rank-1],0);
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

    write_frame(buf,sizeof(buf));
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
    if(index<4 || index>9 || (index==9 && !SU03T_MEDICINE_PROFILE_ENABLED)) {
        printf("[voice] refused unconfigured phrase index=%u\n",index);return;
    }
    uint8_t buf[6] = {0};
    uint8_t *buf_ptr = buf;

    *buf_ptr++ = 0xAA;
    *buf_ptr++ = 0x55;
    *buf_ptr++ = index;
    *buf_ptr++ = dat;
    *buf_ptr++ = 0x55;
    *buf_ptr++ = 0xAA;

    write_frame(buf,sizeof(buf));
}

/***************************************************************
 * 函数名称: su03t_update_values
 * 说    明: 更新供语音查询回传的实时数值
 ***************************************************************/
static void answer_query(uint8_t command)
{
    /* Factory command 3 means light, not gas. No gas response until configured. */
    if (command != 1 && command != 2) return;
    safety_state_t state;
    safety_quality_t quality;
    safety_state_get(&state);
    safety_state_quality(&state, (unsigned int)(LOS_TickCountGet()/1000), &quality);
    if (state.alarm || state.mute) return; /* Safety alarm/mute wins over numeric queries. */
    if (!quality.env_available) {
        printf("[voice] query skipped: temperature/humidity unavailable\r\n");
        return;
    }
    su03t_send_double_msg(command == 1 ? SU03T_VAL_IDX_TEMP : SU03T_VAL_IDX_HUMI,
                         command == 1 ? state.temp : state.humi);
}
void su03t_receive_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        uint64_t now=LOS_TickCountGet();
        if(s_query_prefix>=0 && now-s_prefix_up>500)s_query_prefix=-1;
        if(s_query_prefix>=0){
            int prefix=s_query_prefix;s_query_prefix=-1;
            if(prefix==3 && byte>=1 && byte<=3)answer_query(byte);
            else if(prefix==1 && (byte==1 || byte==2)){
                living_light_override(byte==1?1:0);
                printf("[voice] light %s (alarm retains priority)\n",byte==1?"ON":"OFF");
            }else if(prefix==0 && (byte==1 || byte==2))living_light_override(byte==1?-1:0);
            else if(byte==0 || byte==1 || byte==3){s_query_prefix=byte;s_prefix_up=now;}
        }else if(byte==0 || byte==1 || byte==3){s_query_prefix=byte;s_prefix_up=now;}

    }
}

/***************************************************************
 * 函数名称: su_03t_thread
 * 说    明: 语音模块接收线程：解析查询指令并回传实时数值
 ***************************************************************/
static void su_03t_thread(void *arg)
{
    while (1)
    {
        uint8_t data[64] = {0};
        int rec_len = IoTUartRead(UART2_HANDLE, data, sizeof(data));
        if (rec_len > 0 && rec_len <= (int)sizeof(data)) su03t_receive_bytes(data, rec_len);

        LOS_Msleep(200);
    }
}

/***************************************************************
 * 函数名称: su03t_init
 * 说    明: 初始化语音模块串口（115200 8N1）并启动接收线程
 ***************************************************************/
void su03t_init(void)
{
    s_voice=(voice_policy_t){0};s_medicine_mask=0;s_query_prefix=-1;
    printf("[voice] alarm_profile=%d (0=factory-safe, 1=custom indices 4..8)\r\n", SU03T_ALARM_PROFILE_ENABLED);
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
}
