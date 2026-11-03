/* SPDX-License-Identifier: Apache-2.0 */
#ifndef LIVING_H
#define LIVING_H
#include <stdint.h>
typedef struct { int night_auto, medicine_enabled, medicine_minute; unsigned revision; int extra_enabled[2], extra_minute[2]; } living_config_t;
typedef struct {
 living_config_t config;
 int light_valid, night_on, light_owner, medicine_state, dose_day, time_synced;
 unsigned confirmed_epoch, medicine_revision;
 int slot_state[3]; unsigned slot_epoch[3];
} living_status_t;
/* medicine: 0 disabled, 1 UTC unavailable, 2 scheduled, 3 pending, 4 confirmed.
 * owner: 0 off, 1 night, 2 medicine, 3 alarm. Timezone is UTC+8. */
int living_slot_enabled(const living_config_t *c, int slot);
int living_slot_minute(const living_config_t *c, int slot);
void living_slot_set(living_config_t *c, int slot, int enabled, int minute);
int living_pending_slot(const living_status_t *s);
/* Confirm one pending slot, earliest configured time first; ties use slot number. */
int living_confirm_next(unsigned epoch, int *slot);
void living_init(void);
void living_tick(unsigned epoch, uint64_t now, double lux, int valid, int alarm);
void living_get(living_status_t *out);
unsigned living_serial(void);
int living_beep(uint64_t now);
void living_silence(void);
/* -1 auto, 0 manually off, 1 manually on; reset on reboot/settings save. */
void living_light_override(int mode);
int living_validate(int code, const char *json);
int living_execute(int code, const char *json, unsigned epoch);
int living_save(const living_config_t *config);
int living_json(char *out, int size);
const char *living_medicine_text(int state);
#endif
