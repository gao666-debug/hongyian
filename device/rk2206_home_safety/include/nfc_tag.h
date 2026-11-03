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

#ifndef __NFC_TAG_H__
#define __NFC_TAG_H__

/*
 * 把板载 NFC 标签(NT3H1201)写成一个“求助/信息”标签：
 * 手机开启 NFC 贴一下，可读到文本信息 + 打开状态页链接
 */
void nfc_tag_init(void);

#endif /* __NFC_TAG_H__ */
