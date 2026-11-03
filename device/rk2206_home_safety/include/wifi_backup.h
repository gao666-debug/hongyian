/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SAFETY_WIFI_BACKUP_H
#define SAFETY_WIFI_BACKUP_H
#ifdef SAFETY_HOST_TEST
#define WIFI_BACKUP_SSID "CONFIGURE_LOCALLY"
#define WIFI_BACKUP_PASSWORD "host-test-only"
#define WIFI_EXTRA_SSID "CONFIGURE_LOCALLY"
#define WIFI_EXTRA_PASSWORD "host-test-extra-only"
#else
#include "wifi_private.h"
#endif
#endif
