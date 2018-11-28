/*
 * Copyright (c) 2018 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MBED_DIRECT_ACCESS_DEVICEKEY_H
#define MBED_DIRECT_ACCESS_DEVICEKEY_H

#include "BlockDevice.h"
#include <stdio.h>

int direct_access_to_devicekey(BlockDevice *bd, uint32_t tdb_start_offset, uint32_t tdb_end_offset, void *data_buf, size_t data_buf_size, size_t *actual_data_size_ptr);

#endif /* MBED_DIRECT_ACCESS_DEVICEKEY_H */
