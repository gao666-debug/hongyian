/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SAFETY_CLOUD_COMMANDS_H
#define SAFETY_CLOUD_COMMANDS_H
#include <stddef.h>
void cloud_command_accept(const char *topic, const char *body, size_t length);
/* A response becomes available only after monitor execution (or validation failure). */
int cloud_response_next(char *topic, size_t topic_size, char *body, size_t body_size, unsigned int *slot);
void cloud_response_sent(unsigned int slot);
#endif
