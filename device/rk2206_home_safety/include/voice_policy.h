/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HY_VOICE_POLICY_H
#define HY_VOICE_POLICY_H
#include <stdint.h>
typedef struct {
 unsigned previous, waiting;
 uint64_t last[6], last_any;
 unsigned sent, last_rank;
} voice_policy_t;
/* Bits/ranks: help, wearable fall, gas, inactivity, environment, medicine.
 * One announcement per episode, 30s anti-flap cooldown; higher urgency bypasses
 * the 5s inter-phrase gap. A lower alarm never interrupts an active higher one. */
static inline unsigned voice_policy_next(voice_policy_t *p,unsigned active,int muted,uint64_t now)
{
 p->waiting=(p->waiting | (active & ~p->previous)) & active;
 p->previous=active;
 if(muted || !active)return 0;
 unsigned i=0;while(i<6 && !(active&(1U<<i)))i++;
 if(i==6 || !(p->waiting&(1U<<i)))return 0;
 if((p->sent&(1U<<i)) && now-p->last[i]<30000)return 0;
 if(p->sent && now-p->last_any<5000 && i+1>=p->last_rank)return 0;
 p->waiting &= ~(1U<<i);p->last[i]=now;p->last_any=now;
 p->sent|=1U<<i;p->last_rank=i+1;return i+1;
}
#endif
