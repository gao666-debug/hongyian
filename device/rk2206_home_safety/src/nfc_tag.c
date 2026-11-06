/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "nfc_tag.h"

#include <stdio.h>
#include <string.h>

#include "nfc.h"

static int tag_ready;
static char written_ip[16];

void nfc_tag_init(void)
{
    tag_ready = 0;
    written_ip[0] = 0;
    if (nfc_init() != 0) {
        printf("[nfc] init failed; tag not updated\r\n");
        return;
    }
    tag_ready = 1;
}

int nfc_tag_set_ipv4(const char *ip)
{
    unsigned int parts[4] = {0}, count = 0, digits = 0;
    const char *p;
    char uri[32];
    if (!ip || !*ip || strlen(ip) >= sizeof(written_ip)) return 0;
    for (p = ip; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            if (++digits > 3) return 0;
            parts[count] = parts[count] * 10 + (unsigned int)(*p - '0');
            if (parts[count] > 255) return 0;
        } else if (*p == '.' && digits && count < 3) {
            count++; digits = 0;
        } else return 0;
    }
    if (count != 3 || !digits || parts[0] == 0 || parts[0] >= 224 || parts[0] == 127) return 0;
    if (tag_ready && !strcmp(written_ip, ip)) return 1;
    if (!tag_ready) nfc_tag_init();
    if (!tag_ready) return 0;
    snprintf(uri, sizeof(uri), "%s:8080/", ip);
    /* One URI record is both first and last, so phone dispatch sees the URL
     * rather than a preceding plain-text record. It replaces the old message. */
    if (!nfc_store_uri_http(NDEFFirstPos, (uint8_t *)uri)) {
        printf("[nfc] write uri failed; tag not updated\r\n");
        return 0;
    }
    snprintf(written_ip, sizeof(written_ip), "%s", ip);
    printf("[nfc] tag written: uri=\"http://%s\"\r\n", uri);
    return 1;
}
