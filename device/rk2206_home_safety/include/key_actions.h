/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SAFETY_KEY_ACTIONS_H
#define SAFETY_KEY_ACTIONS_H
#include <stdbool.h>
typedef enum { K6_NONE, K6_ACK, K6_MEDICINE, K6_EDIT, K6_CLEAR_EVENTS, K6_SAVE } k6_action_t;
static inline k6_action_t k6_action(int page,int edit_field,bool alarm,bool pending,bool held)
{
    if (held) {
        if (alarm) return K6_NONE; /* Do not erase an active alarm's local record. */
        if (page==1 || page==2) return K6_CLEAR_EVENTS;
        if (page==4) return edit_field<0 ? K6_EDIT : edit_field<10 ? K6_SAVE : K6_NONE;
        return K6_NONE;
    }
    if (alarm) return K6_ACK;
    if (edit_field<0 && pending) return K6_MEDICINE;
    if (page==4) return K6_EDIT;
    return K6_NONE; /* Short press never deletes records. */
}
#endif
