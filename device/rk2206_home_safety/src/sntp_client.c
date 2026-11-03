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

#include "sntp_client.h"

#include <stdio.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

#include "los_task.h"
#include "los_tick.h"

#define NTP_SERVER_IP   0xCB6B0658UL   /* 203.107.6.88 = ntp.aliyun.com */
#define NTP_PORT        123
#define NTP_DELTA       2208988800UL   /* 1900~1970 秒差 */
#define NTP_SYNC_PERIOD (3600 * 1000)

static volatile unsigned int  g_epoch = 0;
static volatile uint64_t      g_sync_ms = 0;

unsigned int sntp_get_epoch(void)
{
    uint64_t now;

    if (g_epoch == 0)
    {
        return 0;
    }
    now = LOS_TickCountGet();
    return g_epoch + (unsigned int)((now - g_sync_ms) / 1000);
}

static int do_sntp_once(void)
{
    int fd;
    struct sockaddr_in sa;
    struct timeval tv;
    unsigned char pkt[48];
    int n;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(NTP_PORT);
    sa.sin_addr.s_addr = htonl(NTP_SERVER_IP);

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;   /* LI=0 VN=3 Mode=3(client) */

    sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&sa, sizeof(sa));
    n = recv(fd, pkt, sizeof(pkt), 0);
    close(fd);

    if (n < 48)
    {
        return -1;
    }

    unsigned int tx = ((unsigned int)pkt[40] << 24) | ((unsigned int)pkt[41] << 16) |
                      ((unsigned int)pkt[42] << 8) | (unsigned int)pkt[43];
    if (tx < NTP_DELTA)
    {
        return -1;
    }
    g_epoch = tx - NTP_DELTA;
    g_sync_ms = LOS_TickCountGet();
    return 0;
}

static void sntp_task(void *arg)
{
    while (1)
    {
        if (do_sntp_once() == 0)
        {
            printf("[sntp] synced, utc=%u\r\n", g_epoch);
            LOS_Msleep(NTP_SYNC_PERIOD);
        }
        else
        {
            printf("[sntp] sync fail, retry 10s\r\n");
            LOS_Msleep(10000);
        }
    }
}

void sntp_start(void)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)sntp_task;
    task.uwStackSize = 4096;
    task.pcName = "sntp task";
    task.usTaskPrio = 25;
    if (LOS_TaskCreate(&tid, &task) != LOS_OK)
    {
        printf("[sntp] create task fail\r\n");
    }
}
