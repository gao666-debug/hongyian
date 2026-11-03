/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SAFETY_STATS_H
#define SAFETY_STATS_H
void safety_stats_init(void);
void safety_stats_activity(void);
void safety_stats_alarm(int code);
void safety_stats_tick(void);
int safety_stats_json(char *out, int n);
#endif
