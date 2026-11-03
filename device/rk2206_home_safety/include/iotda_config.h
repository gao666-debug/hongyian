#ifndef SAFETY_IOTDA_CONFIG_H
#define SAFETY_IOTDA_CONFIG_H
/* Tests use public dummy credentials and never read the device credential file. */
#ifdef SAFETY_HOST_TEST
#define IOTDA_HOST "example.invalid"
#define IOTDA_DEVICE_ID "test_device"
#define IOTDA_CLIENT_ID "test_device_0_0_2026101310"
#define IOTDA_PASSWORD "test-only"
#else
#include "iotda_private.h"
#endif
/* Explicit demo transport selection; never downgrade automatically. */
#ifndef IOTDA_TLS_ENABLED
#define IOTDA_TLS_ENABLED 0
#endif
#if IOTDA_TLS_ENABLED
#define IOTDA_PORT 8883
#else
#define IOTDA_PORT 1883
#endif
#define IOTDA_SERVICE_ID "HomeSafety"
#define IOTDA_REPORT_TOPIC "$oc/devices/" IOTDA_DEVICE_ID "/sys/properties/report"
#endif
