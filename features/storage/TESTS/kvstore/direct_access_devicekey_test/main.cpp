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
#include "DeviceKey.h"
#include "KVStore.h"
#include "KVMap.h"
#include "kv_config.h"
#include "TDBStore.h"
#include "HeapBlockDevice.h"
#include "FlashSimBlockDevice.h"
#include "FlashIAPBlockDevice.h"
#include "SlicingBlockDevice.h"
#include "DirectAccessDevicekey.h"
#include "greentea-client/test_env.h"
#include "unity.h"
#include "utest.h"
#include <stdlib.h>

using namespace utest::v1;
using namespace mbed;

#define TEST_DEVICEKEY_LENGTH 32

static inline uint32_t align_up(uint64_t val, uint64_t size)
{
    return (((val - 1) / size) + 1) * size;
}

static inline uint32_t align_down(uint64_t val, uint64_t size)
{
    return (((val) / size)) * size;
}

int  get_virtual_TDBStore_position(uint32_t conf_start_address, uint32_t conf_size,
                                   uint32_t *tdb_start_address, uint32_t *tdb_end_address)
{
    uint32_t bd_final_size;
    uint32_t flash_end_address;
    uint32_t flash_start_address;
    uint32_t aligned_start_address;
    uint32_t aligned_end_address;
    uint32_t end_address;
    FlashIAP flash;

    int ret = flash.init();
    if (ret != 0) {
        return -1;
    }

    //Get flash parameters before starting
    flash_start_address = flash.get_flash_start();
    flash_end_address = flash_start_address + flash.get_flash_size();;

    bool request_default = false;
    if (conf_start_address == 0 && conf_size == 0) {
        request_default = true;
        conf_size = 1;
    }

    conf_start_address = flash_end_address - conf_size;
    aligned_start_address = conf_start_address;
    aligned_start_address = align_down(conf_start_address, flash.get_sector_size(conf_start_address));
    //Skip this check if default parameters are set (0 for base address and 0 conf_size).
    //We will calculate the address and size by ourselves
    if (conf_start_address != aligned_start_address && !request_default) {
        printf("KV Config: Internal block device start address is not aligned. Consider changing the size parameter");
        flash.deinit();
        return -1;
    }

    if (request_default) {
        //update start_address to double the size for TDBStore needs
        bd_final_size = (flash_end_address - aligned_start_address) * 2;
        conf_start_address = (flash_end_address - bd_final_size);
        aligned_start_address = align_down(conf_start_address, flash.get_sector_size(conf_start_address));
    } else {
        bd_final_size = (flash_end_address - aligned_start_address);
    }

    (*tdb_start_address) = aligned_start_address;
    (*tdb_end_address) = aligned_start_address + bd_final_size;

    flash.deinit();

    return 0;
}

//
//void test_direct_access_to_devicekey_zero_offset()
//{
//    utest_printf("Test Direct Access To DeviceKey Test with zero offset\n");
//
//    /* TDBStore can not work on SD Card BD */
//#if COMPONENT_SD
//    const size_t ul_bd_size  = 8 * 4096;
//    const size_t rbp_bd_size = 4 * 4096;
//    HeapBlockDevice heap_bd(ul_bd_size + rbp_bd_size, 1, 1, 4096);
//    FlashSimBlockDevice *flash_bd = new FlashSimBlockDevice(&heap_bd);
//#else
//    BlockDevice *bd = BlockDevice::get_default_instance();
//    BlockDevice *flash_bd = bd;
//#endif
//    int err = flash_bd->init();
//    TEST_ASSERT_EQUAL(0, err);
//
//    TDBStore *tdb = new TDBStore(flash_bd);
//    /* Start by Init and Reset to TDBStore */
//    err = tdb->init();
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//    err = tdb->reset();
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//
//    // Assign a dummy DeviceKey, and set via tdb
//    uint8_t device_key_in[TEST_DEVICEKEY_LENGTH] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
//    err = tdb->reserved_data_set(device_key_in, TEST_DEVICEKEY_LENGTH);
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//
//    // Now use Direct Access To DeviceKey to retrieve it */
//    uint8_t device_key_out[TEST_DEVICEKEY_LENGTH] = {0};
//    size_t actual_data_size = 0;
//    err = direct_access_to_devicekey(flash_bd, 0, (uint32_t)(flash_bd->size()), device_key_out, TEST_DEVICEKEY_LENGTH,
//                                     &actual_data_size);
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//
//    /* Assert DeviceKey value and length */
//    TEST_ASSERT_EQUAL(actual_data_size, TEST_DEVICEKEY_LENGTH);
//    for (int i_ind = 0; i_ind < TEST_DEVICEKEY_LENGTH; i_ind++) {
//        TEST_ASSERT_EQUAL(device_key_out[i_ind], device_key_in[i_ind]);
//    }
//
//    delete tdb;
//
//    err = flash_bd->deinit();
//    TEST_ASSERT_EQUAL(0, err);
//}
//
//void test_direct_access_to_devicekey_with_offset()
//{
//    utest_printf("Test Direct Access To DeviceKey Test with given offset\n");
//
//    /* TDBStore can not work on SD Card BD */
//#if COMPONENT_SD
//    const size_t ul_bd_size  = 8 * 4096;
//    const size_t rbp_bd_size = 4 * 4096;
//    HeapBlockDevice heap_bd(ul_bd_size + rbp_bd_size, 1, 1, 4096);
//    FlashSimBlockDevice *flash_bd = new FlashSimBlockDevice(&heap_bd);
//#else
//    BlockDevice *bd = BlockDevice::get_default_instance();
//    BlockDevice *flash_bd = bd;
//#endif
//
//    bd_addr_t start_offset = 4096;
//    bd_addr_t end_offset = 5 * 4096;
//
//    SlicingBlockDevice *slbd = new SlicingBlockDevice(flash_bd, start_offset, end_offset);
//
//    int err = flash_bd->init();
//    TEST_ASSERT_EQUAL(0, err);
//
//    TDBStore *tdb = new TDBStore(slbd);
//    /* Start by Init and Reset to TDBStore */
//    err = tdb->init();
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//    err = tdb->reset();
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//
//    // Assign a dummy DeviceKey, and set via tdb
//    uint8_t device_key_in[TEST_DEVICEKEY_LENGTH] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32};
//    err = tdb->reserved_data_set(device_key_in, TEST_DEVICEKEY_LENGTH);
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//
//    // Now use Direct Access To DeviceKey to retrieve it */
//    uint8_t device_key_out[TEST_DEVICEKEY_LENGTH] = {0};
//    size_t actual_data_size = 0;
//    err = direct_access_to_devicekey(flash_bd, start_offset, end_offset, device_key_out, TEST_DEVICEKEY_LENGTH,
//                                     &actual_data_size);
//    TEST_ASSERT_EQUAL_ERROR_CODE(0, err);
//
//    /* Assert DeviceKey value and length */
//    TEST_ASSERT_EQUAL(actual_data_size, TEST_DEVICEKEY_LENGTH);
//    for (int i_ind = 0; i_ind < TEST_DEVICEKEY_LENGTH; i_ind++) {
//        TEST_ASSERT_EQUAL(device_key_out[i_ind], device_key_in[i_ind]);
//    }
//
//    delete tdb;
//
//    err = flash_bd->deinit();
//    TEST_ASSERT_EQUAL(0, err);
//}

void test_direct_access_to_device_inject_root()
{
    kv_init_storage_config();
    DeviceKey& devkey = DeviceKey::get_instance();
    uint32_t rkey[DEVICE_KEY_16BYTE / sizeof(uint32_t)];
    uint32_t key[DEVICE_KEY_16BYTE / sizeof(uint32_t)];
    KVMap& kv_map = KVMap::get_instance();
    KVStore *inner_store = kv_map.get_internal_kv_instance(NULL);
    TEST_ASSERT_NOT_EQUAL(NULL, inner_store);

    BlockDevice *flash_bd = kv_map.get_internal_blockdevice_instance("");
    TEST_ASSERT_NOT_EQUAL(NULL, flash_bd);

    int ret = inner_store->reset();
    TEST_ASSERT_EQUAL_INT(DEVICEKEY_SUCCESS, ret);

    memcpy(key, "1234567812345678", sizeof(key));
    ret = devkey.device_inject_root_of_trust(key, DEVICE_KEY_16BYTE);
    TEST_ASSERT_EQUAL_INT(DEVICEKEY_SUCCESS, ret);

    // Now use Direct Access To DeviceKey to retrieve it */
    uint32_t internal_start_address =  MBED_CONF_STORAGE_FILESYSTEM_INTERNAL_BASE_ADDRESS;
    uint32_t internal_rbp_size =  MBED_CONF_STORAGE_FILESYSTEM_RBP_INTERNAL_SIZE;
    /*   size_t rbp_num_of_enteries =  MBED_CONF_STORAGE_FILESYSTEM_RBP_NUMBER_OF_ENTRIES;

       if (internal_rbp_size == 0) {
           internal_rbp_size = 4 * 1024 * rbp_num_of_enteries / 32;
       } */

    uint32_t tdb_st_add = 0;
    uint32_t tdb_end_add = 0;

    ret = get_virtual_TDBStore_position(internal_start_address, internal_rbp_size, &tdb_st_add, &tdb_end_add);
    TEST_ASSERT_EQUAL(0, ret);

    memset(rkey, 0, sizeof(rkey));
    size_t actual_data_size = 0;

    ret = direct_access_to_devicekey(tdb_st_add, tdb_end_add, rkey, DEVICE_KEY_16BYTE, &actual_data_size);
    TEST_ASSERT_EQUAL_ERROR_CODE(0, ret);
    /* Assert DeviceKey value and length */
    TEST_ASSERT_EQUAL(actual_data_size, DEVICE_KEY_16BYTE);
    TEST_ASSERT_EQUAL_INT32_ARRAY(key, rkey, DEVICE_KEY_16BYTE / sizeof(uint32_t));
}

// Test setup
utest::v1::status_t greentea_failure_handler(const Case *const source, const failure_t reason)
{
    greentea_case_failure_abort_handler(source, reason);
    return STATUS_CONTINUE;
}

Case cases[] = {
    //Case("Testing direct access to devicekey with zero offset", test_direct_access_to_devicekey_zero_offset, greentea_failure_handler),
    //Case("Testing direct access to devicekey with given offset ", test_direct_access_to_devicekey_with_offset, greentea_failure_handler),
    Case("Testing direct access to injected devicekey ", test_direct_access_to_device_inject_root, greentea_failure_handler),
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
