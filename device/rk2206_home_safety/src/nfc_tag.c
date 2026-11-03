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

#include "nfc.h"

/* The URI helper adds the NDEF http:// prefix (0x03); keep only the suffix here. */
#define NFC_URI "192.168.2.1:8080/"

void nfc_tag_init(void)
{
    printf("[nfc] init & write tag ...\r\n");
    if (nfc_init() != 0) {
        printf("[nfc] init failed; tag not updated\r\n");
        return;
    }
    /* One URI record is both first and last, so phone dispatch sees the URL
     * rather than a preceding plain-text record. It replaces the old message. */
    if (!nfc_store_uri_http(NDEFFirstPos, (uint8_t *)NFC_URI)) {
        printf("[nfc] write uri failed; tag not updated\r\n");
        return;
    }
    printf("[nfc] tag written: uri=\"http://%s\"\r\n", NFC_URI);
}
