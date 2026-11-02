/**
 * 联网与授时：Wi-Fi STA 连接 + SNTP 校时
 */

#ifndef HOME_CLOUD_NET_H
#define HOME_CLOUD_NET_H

#include "td_type.h"

/* 阻塞直到联网成功并完成校时；成功返回 0。
 * 只能在任务里调用（内部会 sleep 等 DHCP）。 */
td_s32 net_connect(td_void);

/* 当前 UTC 秒数（校时基准 + 已过时间），未校时返回 0 */
td_u32 net_epoch_now(td_void);

/* 开机以来秒数 */
td_u32 net_uptime_sec(td_void);

/* 把 Unix 时间换算成 UTC 年月日时（华为云 clientId 用 UTC） */
td_void net_epoch_to_utc(td_u32 epoch, td_s32 *year, td_s32 *mon, td_s32 *day, td_s32 *hour);

#endif /* HOME_CLOUD_NET_H */
