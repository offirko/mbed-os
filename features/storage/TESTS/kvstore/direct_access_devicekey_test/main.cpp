/* Copyright (c) 2018 Arm Limited
*
* SPDX-License-Identifier: Apache-2.0
*
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

#include "mbed.h"
#include <stdio.h>
#include <string.h>
#include "TDBStore.h"
#include "HeapBlockDevice.h"
#include "FlashSimBlockDevice.h"
#include "SlicingBlockDevice.h"
#include "DirectAccessDevicekey.h"
#include "greentea-client/test_env.h"
#include "unity.h"
#include "utest.h"
#include <stdlib.h>

using namespace utest::v1;
using namespace mbed;

#define TEST_DEVICEKEY_LENGTH 32

void test_direct_access_to_devicekey_zero_offset()
{
    utest_printf("Test Direct Access To DeviceKey Test with zero offset\n");

    /* TDBStore can not work on SD Card BD */
#if COMPONENT_SD
    const size_t ul_bd_size  = 8 * 4096;
    const size_t rbp_bd_size = 4 * 4096;
    HeapBlockDevice heap_bd(ul_bd_size + rbp_bd_size, 1, 1, 4096);
    FlashSimBlockDevice *flash_bd = new FlashSimBlockDevice(&heap_bd);
#else
    BlockDevice *bd = BlockDevice::get_default_instance();
    BlockDevice *flash_bd = bd;
#endif
    int err = flash_bd->init();
    TEST_ASSERT_EQUAL(0, err);

    TDBStore *tdb = new TDBStore(flash_bd);
    /* Start by Init and Reset to TDBStore */
    err = tdb->init();
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
    err = tdb->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);

    // Assign a dummy DeviceKey, and set via tdb
    uint8_t device_key_in[TEST_DEVICEKEY_LENGTH] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    err = tdb->reserved_data_set(device_key_in, TEST_DEVICEKEY_LENGTH);
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);

    // Now use Direct Access To DeviceKey to retrieve it */
    uint8_t device_key_out[TEST_DEVICEKEY_LENGTH] = {0};
    size_t actual_data_size = 0;
    err = direct_access_to_devicekey(flash_bd, 0, (uint32_t)(flash_bd->size()), device_key_out, TEST_DEVICEKEY_LENGTH,
                                     &actual_data_size);
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);

    /* Assert DeviceKey value and length */
    TEST_ASSERT_EQUAL(actual_data_size, TEST_DEVICEKEY_LENGTH);
    for (int i_ind = 0; i_ind < TEST_DEVICEKEY_LENGTH; i_ind++) {
        TEST_ASSERT_EQUAL(device_key_out[i_ind], device_key_in[i_ind]);
    }

    delete tdb;

    err = flash_bd->deinit();
    TEST_ASSERT_EQUAL(0, err);
}

void test_direct_access_to_devicekey_with_offset()
{
    utest_printf("Test Direct Access To DeviceKey Test with given offset\n");

    /* TDBStore can not work on SD Card BD */
#if COMPONENT_SD
    const size_t ul_bd_size  = 8 * 4096;
    const size_t rbp_bd_size = 4 * 4096;
    HeapBlockDevice heap_bd(ul_bd_size + rbp_bd_size, 1, 1, 4096);
    FlashSimBlockDevice *flash_bd = new FlashSimBlockDevice(&heap_bd);
#else
    BlockDevice *bd = BlockDevice::get_default_instance();
    BlockDevice *flash_bd = bd;
#endif

    bd_addr_t start_offset = 4096;
    bd_addr_t end_offset = 5 * 4096;

    SlicingBlockDevice *slbd = new SlicingBlockDevice(flash_bd, start_offset, end_offset);

    int err = flash_bd->init();
    TEST_ASSERT_EQUAL(0, err);

    TDBStore *tdb = new TDBStore(slbd);
    /* Start by Init and Reset to TDBStore */
    err = tdb->init();
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
    err = tdb->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);

    // Assign a dummy DeviceKey, and set via tdb
    uint8_t device_key_in[TEST_DEVICEKEY_LENGTH] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
    err = tdb->reserved_data_set(device_key_in, TEST_DEVICEKEY_LENGTH);
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);

    // Now use Direct Access To DeviceKey to retrieve it */
    uint8_t device_key_out[TEST_DEVICEKEY_LENGTH] = {0};
    size_t actual_data_size = 0;
    err = direct_access_to_devicekey(flash_bd, start_offset, end_offset, device_key_out, TEST_DEVICEKEY_LENGTH,
                                     &actual_data_size);
    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);

    /* Assert DeviceKey value and length */
    TEST_ASSERT_EQUAL(actual_data_size, TEST_DEVICEKEY_LENGTH);
    for (int i_ind = 0; i_ind < TEST_DEVICEKEY_LENGTH; i_ind++) {
        TEST_ASSERT_EQUAL(device_key_out[i_ind], device_key_in[i_ind]);
    }

    delete tdb;

    err = flash_bd->deinit();
    TEST_ASSERT_EQUAL(0, err);
}

// Test setup
utest::v1::status_t greentea_failure_handler(const Case *const source, const failure_t reason)
{
    greentea_case_failure_abort_handler(source, reason);
    return STATUS_CONTINUE;
}

Case cases[] = {
    Case("Testing direct access to devicekey with zero offset", test_direct_access_to_devicekey_zero_offset, greentea_failure_handler),
    Case("Testing direct access to devicekey with given offset ", test_direct_access_to_devicekey_with_offset, greentea_failure_handler),
};

utest::v1::status_t greentea_test_setup(const size_t number_of_cases)
{
    GREENTEA_SETUP(120, "default_auto");
    return greentea_test_setup_handler(number_of_cases);
}

Specification specification(greentea_test_setup, cases, greentea_test_teardown_handler);

int main()
{
    return !Harness::run(specification);
}
