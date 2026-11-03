/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SAFETY_MQTT_TLS_H
#define SAFETY_MQTT_TLS_H
#include "MQTTClient.h"
int safety_tls_connect(Network *network, const char *hostname);
void safety_tls_close(void);
#endif
