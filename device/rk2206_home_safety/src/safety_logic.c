/* SPDX-License-Identifier: Apache-2.0 */
#include "home_safety.h"
#include "safety_runtime.h"
int safety_voice_select(const bool alarms[5], unsigned int rising, bool suppressed)
{
    if (suppressed) return 0;
    for (int i = 0; i < 5; ++i)
        if (alarms[i]) return (rising & (1U << i)) ? i + 1 : 0;
    return 0;
}
#include <string.h>
void safety_fall_sample(safety_fall_t *f, float g, uint64_t now, bool valid)
{
    if (!valid)
    {
        f->free_seen = false;
        f->pending = false;
        return;
    }
    if (g < FALL_FREEFALL_G)
    {
        f->free_seen = true;
        f->free_ms = now;
    }
    else if (g > FALL_IMPACT_G && f->free_seen && now - f->free_ms <= FALL_WINDOW_MS)
    {
        f->pending = true;
        f->impact_ms = now;
        f->peak = g;
        f->free_seen = false;
    }
    if (f->free_seen && now - f->free_ms > FALL_WINDOW_MS)
        f->free_seen = false;
}
void safety_fall_confirm(safety_fall_t *f, uint64_t now, uint64_t last, bool body)
{
    if (f->active)
    {
        if (body)
        {
            f->active = false;
            f->pending = false;
        }
        return;
    }
    if (!f->pending)
        return;
    if (now - f->impact_ms > 120000UL)
    {
        f->pending = false;
        return;
    }
    if (now - f->impact_ms >= FALL_CONFIRM_MS && now - last >= FALL_CONFIRM_MS && !body)
        f->active = true;
}

void safety_alarm_edges(const bool alarms[5], bool previous[5], unsigned int *rising,
                        unsigned int *falling)
{
    *rising = 0;
    *falling = 0;
    for (int i = 0; i < 5; i++)
    {
        if (alarms[i] && !previous[i])
            *rising |= 1U << i;
        if (!alarms[i] && previous[i])
            *falling |= 1U << i;
        previous[i] = alarms[i];
    }
}
