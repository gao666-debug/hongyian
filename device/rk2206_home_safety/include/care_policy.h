/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CARE_POLICY_H
#define CARE_POLICY_H
#include <stdint.h>
void care_policy_init(void);
/* 0 paused, 1 normal, 2 record only; epoch is UTC, schedule is UTC+8. */
int care_policy_mode(unsigned epoch);
int care_policy_mode_at(unsigned epoch,uint64_t now);
int care_policy_pause(unsigned minutes,uint64_t now);
int care_policy_apply(const char *json);
int care_policy_json(char *out, int size, unsigned epoch, uint64_t now);
#endif
