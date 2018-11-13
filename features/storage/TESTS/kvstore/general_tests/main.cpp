/* Copyright (c) 2017 ARM Limited
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

#include "KVStore.h"
#include "SlicingBlockDevice.h"
#include "greentea-client/test_env.h"
#include "unity/unity.h"
#include "utest/utest.h"
#include "FlashSimBlockDevice.h"
#include "TDBStore.h"
#include "Thread.h"
#include "mbed_error.h"
#include "FileSystem.h"
#include "FileSystemStore.h"
#include "SecureStore.h"

using namespace utest::v1;
using namespace mbed;

static const char   data[] = "data";
static const char   key[] = "key";
static char         buffer[20] = {};
static const size_t data_size = 5;
static size_t       actual_size = 0;
static const size_t buffer_size = 20;
static const int    num_of_threads = 3;
static const char   num_of_keys = 3;

static char *keys[] = {"key1", "key2", "key3"};

KVStore::info_t info;
KVStore::iterator_t kvstore_it;

#define TEST_ASSERT_EQUAL_ERROR_CODE(expected, actual) \
TEST_ASSERT_EQUAL(expected & MBED_ERROR_STATUS_CODE_MASK, actual & MBED_ERROR_STATUS_CODE_MASK)

KVStore *kvstore = NULL;
FileSystem *fs = NULL;
BlockDevice *bd = NULL;
FlashSimBlockDevice *flash_bd = NULL;
SlicingBlockDevice *ul_bd = NULL, *rbp_bd = NULL;

enum kv_setup {TDBStoreSet = 1, FSStoreSet = 2, SecStoreSet = 3};
static int kv_setup = 1;

const size_t ul_bd_size = 16 * 4096;
const size_t rbp_bd_size = 8 * 4096;

/*----------------initialization------------------*/

//init the blockdevice
static void kvstore_init()
{
    int res = 0;
    if (kv_setup == TDBStoreSet) {
        bd = BlockDevice::get_default_instance();
        TEST_SKIP_UNLESS(bd != NULL);
        res = bd->init();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);
        if (bd->get_erase_value() == -1) {
            flash_bd = new FlashSimBlockDevice(bd);
            kvstore = new TDBStore(flash_bd);
        } else {
            kvstore = new TDBStore(bd);
        }
    }
    if (kv_setup == FSStoreSet) {
        bd = BlockDevice::get_default_instance();
        TEST_SKIP_UNLESS(bd != NULL);
        res = bd->init();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);

        fs = FileSystem::get_default_instance();
        TEST_SKIP_UNLESS(fs != NULL);
        res = fs->mount(bd);
        if (res) {
            res = fs->reformat(bd);
            TEST_ASSERT_EQUAL_ERROR_CODE(0, res);
        }
        kvstore = new FileSystemStore(fs);
    }
    if (kv_setup == SecStoreSet) {
        bd = BlockDevice::get_default_instance();
        TEST_SKIP_UNLESS(bd != NULL);
        res = bd->init();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);
        if (bd->get_erase_value() == -1) {
            flash_bd = new FlashSimBlockDevice(bd);
            ul_bd = new SlicingBlockDevice(flash_bd, 0, ul_bd_size);
            rbp_bd = new SlicingBlockDevice(flash_bd, ul_bd_size, ul_bd_size + rbp_bd_size);
        } else {
            ul_bd = new SlicingBlockDevice(bd, 0, ul_bd_size);
            rbp_bd = new SlicingBlockDevice(bd, ul_bd_size, ul_bd_size + rbp_bd_size);
        }
        TDBStore *ul_kv = new TDBStore(ul_bd);
        TDBStore *rbp_kv = new TDBStore(rbp_bd);
        kvstore = new SecureStore(ul_kv, rbp_kv);
    }

    TEST_SKIP_UNLESS(kvstore != NULL);

    res = kvstore->init();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    kv_setup++;
}

//deinit the blockdevice
static void kvstore_deinit()
{
    int res = 0;

    TEST_SKIP_UNLESS(kvstore != NULL);

    if (kv_setup == TDBStoreSet) {
        res = bd->deinit();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);

        if (bd->get_erase_value() == -1) {
            delete flash_bd;
        }
    }
    if (kv_setup == FSStoreSet) {
        res = bd->deinit();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);

        fs = FileSystem::get_default_instance();
        TEST_SKIP_UNLESS(fs != NULL);
        res = fs->unmount();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);
    }
    if (kv_setup == SecStoreSet) {
        res = bd->deinit();
        TEST_ASSERT_EQUAL_ERROR_CODE(0, res);
        if (bd->get_erase_value() == -1) {
            delete flash_bd;
        }
        delete ul_bd;
        delete rbp_bd;
    }

    res = kvstore->deinit();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    delete kvstore;
}

/*----------------set()------------------*/

//bad params : key is null
static void set_key_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(NULL, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : key length over key max size
static void set_key_length_exceeds_max()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char key_max[KVStore::MAX_KEY_SIZE + 1] = {0};
    memset(key_max, '*', KVStore::MAX_KEY_SIZE);
    int res = kvstore->set(key_max, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : buffer is null, non zero size
static void set_buffer_null_size_not_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, NULL, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : undefined flag
static void set_key_undefined_flags()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 16);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : buffer full, size is 0
static void set_buffer_size_is_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, 0, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set same key several times
static void set_same_key_several_time()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

static void test_thread_set(char *th_key)
{
    int res = kvstore->set((char *)th_key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//get several keys multithreaded
static void set_several_keys_multithreaded()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    rtos::Thread kvstore_thread[num_of_threads];
    osStatus threadStatus;

    kvstore_thread[0].start(callback(test_thread_set, keys[0]));
    kvstore_thread[1].start(callback(test_thread_set, keys[1]));
    kvstore_thread[2].start(callback(test_thread_set, keys[2]));


    for (int i = 0; i < num_of_threads; i++) {
        threadStatus = kvstore_thread[i].join();
        if (threadStatus != 0) {
            utest_printf("\nthread %d join failed!", i + 1);
        }
    }

    for (int i = 0; i < num_of_threads; i++) {
        int res = kvstore->get(keys[i], buffer, buffer_size, &actual_size, 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

        TEST_ASSERT_EQUAL_STRING_LEN(buffer, data, data_size);

    }

    int res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key "write once" and try to set it again
static void set_write_once_flag_try_set_twice()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, KVStore::WRITE_ONCE_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set(key, data, data_size, KVStore::WRITE_ONCE_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_WRITE_PROTECTED, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key "write once" and try to remove it
static void set_write_once_flag_try_remove()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, KVStore::WRITE_ONCE_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_WRITE_PROTECTED, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key value one byte size
static void set_key_value_one_byte_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char data_one = 'a';
    int res = kvstore->set(key, &data_one, 1, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = strncmp(buffer, &data_one, 1);
    TEST_ASSERT_EQUAL_ERROR_CODE(0, res);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key value two byte size
static void set_key_value_two_byte_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char data_two[2] = "d";
    int res = kvstore->set(key, data_two, 2, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_STRING_LEN(buffer, data_two, 1);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key value five byte size
static void set_key_value_five_byte_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char data_five[5] = "data";
    int res = kvstore->set(key, data_five, 5, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_STRING_LEN(buffer, data_five, 4);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key value fifteen byte size
static void set_key_value_fifteen_byte_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char data_fif[15] = "data_is_everyt";
    int res = kvstore->set(key, data_fif, 15, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_STRING_LEN(buffer, data_fif, 14);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key value seventeen byte size
static void set_key_value_seventeen_byte_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char data_fif[17] = "data_is_everythi";
    int res = kvstore->set(key, data_fif, 17, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_STRING_LEN(buffer, data_fif, 16);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set several different key value byte size
static void set_several_key_value_sizes()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char name[7] = "name_";
    char c[2] = {0};
    int i = 0, res = 0;

    for (i = 0; i < 30; i++) {
        c[0] = i + '0';
        name[6] = c[0];
        res = kvstore->set(name, name, sizeof(name), 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    }

    for (i = 0; i < 30; i++) {
        c[0] = i + '0';
        name[6] = c[0];
        res = kvstore->get(name, buffer, sizeof(buffer), &actual_size, 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
        TEST_ASSERT_EQUAL_STRING_LEN(name, buffer, sizeof(name));
        memset(buffer, 0, sizeof(buffer));
    }

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key with ROLLBACK flag without AUTHENTICATION flag
static void Sec_set_key_rollback_without_auth_flag()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, KVStore::REQUIRE_REPLAY_PROTECTION_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//set key with ROLLBACK flag and retrieve it, set it again with no ROLBACK
static void Sec_set_key_rollback_set_again_no_rollback()
{
    char key_name[7] = "name";

    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key_name, data, data_size, KVStore::REQUIRE_REPLAY_PROTECTION_FLAG || KVStore::REQUIRE_INTEGRITY_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key_name, buffer, sizeof(buffer), &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_STRING_LEN(data, buffer, sizeof(data));
    memset(buffer, 0, sizeof(buffer));

    res = kvstore->set(key_name, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_WRITE_PROTECTED, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key with ENCRYPT flag and retrieve it
static void Sec_set_key_encrypt()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, KVStore::REQUIRE_CONFIDENTIALITY_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, sizeof(buffer), &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_STRING_LEN(data, buffer, sizeof(data));
    memset(buffer, 0, sizeof(buffer));

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key with AUTH flag and retrieve it
static void Sec_set_key_auth()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, KVStore::REQUIRE_INTEGRITY_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, sizeof(buffer), &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_STRING_LEN(data, buffer, sizeof(data));
    memset(buffer, 0, sizeof(buffer));

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

/*----------------get()------------------*/

//bad params : key is null
static void get_key_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->get(NULL, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : key length over key max size
static void get_key_length_exceeds_max()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char key_max[KVStore::MAX_KEY_SIZE + 1] = {0};
    memset(key_max, '*', KVStore::MAX_KEY_SIZE);
    int res = kvstore->get(key_max, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : buffer is null, non zero size
static void get_buffer_null_size_not_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->get(key, NULL, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//bad params : buffer full, size is 0
static void get_buffer_size_is_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, NULL, 0, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, 0, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//buffer_size smaller than data real size
static void get_buffer_size_smaller_than_data_real_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char big_data[25] = "data";

    int res = kvstore->set(key, big_data, sizeof(big_data), 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_STRING_LEN(buffer, big_data, &actual_size);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//buffer_size bigger than data real size
static void get_buffer_size_bigger_than_data_real_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char big_buffer[25] = {};
    res = kvstore->get(key, big_buffer, sizeof(big_buffer), &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_STRING_LEN(big_buffer, data, &actual_size);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//offset bigger than data size
static void get_offset_bigger_than_data_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, data_size + 1);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_SIZE, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//get a non existing key
static void get_non_existing_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//get a removed key
static void get_removed_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set the same key twice and get latest data
static void get_key_that_was_set_twice()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char new_data[] = "new_data";
    res = kvstore->set(key, new_data, sizeof(new_data), 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_STRING_LEN(buffer, new_data, &actual_size);
    memset(buffer, 0, buffer_size);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

static void test_thread_get(const void *th_key)
{
    int res = kvstore->get((char *)th_key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//get several keys multithreaded
static void get_several_keys_multithreaded()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    rtos::Thread kvstore_thread[num_of_threads];
    osStatus threadStatus;

    for (int i = 0; i < num_of_threads; i++) {
        int res = kvstore->set(keys[i], data, data_size, 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    }

    kvstore_thread[0].start(callback(test_thread_get, "key1"));
    kvstore_thread[1].start(callback(test_thread_get, "key2"));
    kvstore_thread[2].start(callback(test_thread_get, "key3"));

    for (int i = 0; i < num_of_threads; i++) {
        threadStatus = kvstore_thread[i].join();
        if (threadStatus != 0) {
            utest_printf("\nthread %d join failed!", i + 1);
        }
    }

    int res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

/*----------------remove()------------------*/

//bad params : key is null
static void remove_key_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->remove(NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : key length over key max size
static void remove_key_length_exceeds_max()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char key_max[KVStore::MAX_KEY_SIZE + 1] = {0};
    memset(key_max, '*', KVStore::MAX_KEY_SIZE);
    int res = kvstore->remove(key_max);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//key doesn’t exist
static void remove_non_existing_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char new_key[] = "remove_key";
    int res = kvstore->remove(new_key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//key already removed
static void remove_removed_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//key exist - valid flow
static void remove_existed_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

/*----------------get_info()------------------*/

//bad params : key is null
static void get_info_key_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->get_info(NULL, &info);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : key length over key max size
static void get_info_key_length_exceeds_max()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char key_max[KVStore::MAX_KEY_SIZE + 1] = {0};
    memset(key_max, '*', KVStore::MAX_KEY_SIZE);
    int res = kvstore->get_info(key_max, &info);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : &info is null
static void get_info_info_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->get_info(key, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//get_info of non existing key
static void get_info_non_existing_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char new_key[] = "get_info_key";
    int res = kvstore->get_info(new_key, &info);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//get_info of removed key
static void get_info_removed_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(key);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get_info(key, &info);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//get_info of existing key - valid flow
static void get_info_existed_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, KVStore::WRITE_ONCE_FLAG);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get_info(key, &info);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_ERROR_CODE(info.flags, KVStore::WRITE_ONCE_FLAG);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//get_info of overwritten key
static void get_info_overwritten_key()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char new_key[] = "get_info_key";
    int res = kvstore->set(new_key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char new_data[] = "new_data";
    res = kvstore->set(key, new_data, sizeof(new_data), 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get_info(key, &info);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    TEST_ASSERT_EQUAL_ERROR_CODE(info.size, sizeof(new_data));

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

/*----------------iterator_open()------------------*/

//bad params : it is null
static void iterator_open_it_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->iterator_open(NULL, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

/*----------------iterator_next()------------------*/

//key valid, key_size 0
static void iterator_next_key_size_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char key[KVStore::MAX_KEY_SIZE];

    res = kvstore->iterator_next(kvstore_it, key, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//iteartor_next with empty list
static void iterator_next_empty_list()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char key[KVStore::MAX_KEY_SIZE];

    res = kvstore->iterator_next(kvstore_it, key, sizeof(key));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);
}

//iterator_next for one key list
static void iterator_next_one_key_list()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->set(key, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char key[KVStore::MAX_KEY_SIZE];

    res = kvstore->iterator_next(kvstore_it, key, sizeof(key));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//iteartor_next with empty list (all keys removed)
static void iterator_next_empty_list_keys_removed()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char new_key_1[] = "it_1";
    int res = kvstore->set(new_key_1, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char new_key_2[] = "it_2";
    res = kvstore->set(new_key_2, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(new_key_1);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->remove(new_key_2);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char key[KVStore::MAX_KEY_SIZE];

    res = kvstore->iterator_next(kvstore_it, key, sizeof(key));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//iteartor_next with non matching prefix (empty list)
static void iterator_next_empty_list_non_matching_prefix()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    char new_key_1[] = "it_1";
    int res = kvstore->set(new_key_1, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char new_key_2[] = "it_2";
    res = kvstore->set(new_key_2, data, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->iterator_open(&kvstore_it, "Key*");
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char key[KVStore::MAX_KEY_SIZE];

    res = kvstore->iterator_next(kvstore_it, key, sizeof(key));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//iteartor_next with several overwritten keys
static void iterator_next_several_overwritten_keys()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    for (int i = 0; i < num_of_keys; i++) {
        int res = kvstore->set(key, data, data_size, 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    }

    int res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char key[KVStore::MAX_KEY_SIZE];

    res = kvstore->iterator_next(kvstore_it, key, sizeof(key));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->iterator_next(kvstore_it, key, sizeof(key));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_ITEM_NOT_FOUND, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//iterator_next for full list - check key names for validation
static void iterator_next_full_list()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int i = 0;
    for (i = 0; i < num_of_keys; i++) {
        int res = kvstore->set(keys[i], data, data_size, 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
    }

    int res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    char temp_key[KVStore::MAX_KEY_SIZE];

    for (i = 0; i < num_of_keys; i++) {
        res = kvstore->iterator_next(kvstore_it, temp_key, sizeof(temp_key));
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

        res = kvstore->get(temp_key, buffer, buffer_size, &actual_size, 0);
        TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
        TEST_ASSERT_EQUAL_STRING(keys[i], temp_key);
    }

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

/*----------------iterator_close()------------------*/

//iterator_close right after iterator_open
static void iterator_close_right_after_iterator_open()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    int res = kvstore->iterator_open(&kvstore_it, NULL);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->iterator_close(kvstore_it);
}

/*----------------set_start()------------------*/

//bad params : key is null
static void set_start_key_is_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_start(&handle, NULL, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//bad params : key_size over max_size
static void set_start_key_size_exceeds_max_size()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    char key_max[KVStore::MAX_KEY_SIZE + 1] = {0};
    memset(key_max, '*', KVStore::MAX_KEY_SIZE);
    int res = kvstore->set_start(&handle, key_max, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

//final_data_size is 0
static void set_start_final_data_size_is_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_start(&handle, key, 0, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//final_data_size is smaller than actual data
static void set_start_final_data_size_is_smaller_than_real_data()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    size_t new_data_size = 20;
    int res = kvstore->set_start(&handle, key, new_data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, data, data_size);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_SIZE, res);
}

//final_data_size is smaller than actual data
static void set_start_final_data_size_is_bigger_than_real_data()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    char new_data[] = "new_data_buffer";
    int res = kvstore->set_start(&handle, key, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, new_data, sizeof(new_data));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_SIZE, res);
}

/*----------------set_add_data()------------------*/

//bad params : value_data is null
static void set_add_data_value_data_is_null()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_start(&handle, key, data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, NULL, sizeof(data_size));
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//bad params : value_data is valid, data_size is 0
static void set_add_data_data_size_is_zero()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_start(&handle, key, 0, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, NULL, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//data_size is bigger than actual data
static void set_add_data_data_size_bigger_than_real_data()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    size_t new_data_size = 20;
    int res = kvstore->set_start(&handle, key, new_data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, data, new_data_size - 1);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, data, data_size);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_SIZE, res);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set different data_sizes chunks of data in the same transaction
static void set_add_data_set_different_data_size_in_same_transaction()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    char new_data[] = "new_data_tests";
    size_t new_data_size = 15;

    int res = kvstore->set_start(&handle, key, new_data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, new_data, new_data_size - 5);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, new_data, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_add_data(handle, new_data + (new_data_size - 5), 5);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, buffer, buffer_size, &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    TEST_ASSERT_EQUAL_STRING(new_data, buffer);

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set key value five Kbyte size
static void set_add_data_set_key_value_five_Kbytes()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    size_t new_data_size = 5000;
    char temp_buf[50] = {};
    char read_temp_buf[50] = {};
    unsigned int i = 0;

    int res = kvstore->set_start(&handle, key, new_data_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    for (i = 0; i < (new_data_size / sizeof(temp_buf)); i++) {
        memset(temp_buf, '*', sizeof(temp_buf));
        res = kvstore->set_add_data(handle, temp_buf, sizeof(temp_buf));
    }

    res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->get(key, read_temp_buf, sizeof(read_temp_buf), &actual_size, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    TEST_ASSERT_EQUAL_STRING_LEN(temp_buf, read_temp_buf, sizeof(temp_buf));

    res = kvstore->reset();
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

//set_add_data without set_start
static void set_add_data_without_set_start()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_add_data(handle, data, data_size);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

/*----------------set_finalize()------------------*/

//set_finalize without set_start
static void set_finalize_without_set_start()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_ERROR_INVALID_ARGUMENT, res);
}

static void set_finalize_right_after_set_start()
{
    TEST_SKIP_UNLESS(kvstore != NULL);

    KVStore::set_handle_t handle;

    int res = kvstore->set_start(&handle, key, 0, 0);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);

    res = kvstore->set_finalize(handle);
    TEST_ASSERT_EQUAL_ERROR_CODE(MBED_SUCCESS, res);
}

/*----------------setup------------------*/

utest::v1::status_t greentea_failure_handler(const Case *const source, const failure_t reason)
{
    greentea_case_failure_abort_handler(source, reason);
    return STATUS_CONTINUE;
}

Case cases[] = {

    /*----------------TDBStore------------------*/

    Case("TDB_kvstore_init", kvstore_init), //must be first

    Case("TDB_set_key_null", set_key_null, greentea_failure_handler),
    Case("TDB_set_key_length_exceeds_max", set_key_length_exceeds_max, greentea_failure_handler),
    Case("TDB_set_buffer_null_size_not_zero", set_buffer_null_size_not_zero, greentea_failure_handler),
    Case("TDB_set_key_undefined_flags", set_key_undefined_flags, greentea_failure_handler),
    Case("TDB_set_buffer_size_is_zero", set_buffer_size_is_zero, greentea_failure_handler),
    Case("TDB_set_same_key_several_time", set_same_key_several_time, greentea_failure_handler),
    Case("TDB_set_several_keys_multithreaded", set_several_keys_multithreaded, greentea_failure_handler),
    Case("TDB_set_write_once_flag_try_set_twice", set_write_once_flag_try_set_twice, greentea_failure_handler),
    Case("TDB_set_write_once_flag_try_remove", set_write_once_flag_try_remove, greentea_failure_handler),
    Case("TDB_set_key_value_one_byte_size", set_key_value_one_byte_size, greentea_failure_handler),
    Case("TDB_set_key_value_two_byte_size", set_key_value_two_byte_size, greentea_failure_handler),
    Case("TDB_set_key_value_five_byte_size", set_key_value_five_byte_size, greentea_failure_handler),
    Case("TDB_set_key_value_fifteen_byte_size", set_key_value_fifteen_byte_size, greentea_failure_handler),
    Case("TDB_set_key_value_seventeen_byte_size", set_key_value_seventeen_byte_size, greentea_failure_handler),
    Case("TDB_set_several_key_value_sizes", set_several_key_value_sizes, greentea_failure_handler),

    Case("TDB_get_key_null", get_key_null, greentea_failure_handler),
    Case("TDB_get_key_length_exceeds_max", get_key_length_exceeds_max, greentea_failure_handler),
    Case("TDB_get_buffer_null_size_not_zero", get_buffer_null_size_not_zero, greentea_failure_handler),
    Case("TDB_get_buffer_size_is_zero", get_buffer_size_is_zero, greentea_failure_handler),
    Case("TDB_get_buffer_size_smaller_than_data_real_size", get_buffer_size_smaller_than_data_real_size, greentea_failure_handler),
    Case("TDB_get_buffer_size_bigger_than_data_real_size", get_buffer_size_bigger_than_data_real_size, greentea_failure_handler),
    Case("TDB_get_offset_bigger_than_data_size", get_offset_bigger_than_data_size, greentea_failure_handler),
    Case("TDB_get_non_existing_key", get_non_existing_key, greentea_failure_handler),
    Case("TDB_get_removed_key", get_removed_key, greentea_failure_handler),
    Case("TDB_get_key_that_was_set_twice", get_key_that_was_set_twice, greentea_failure_handler),
    Case("TDB_get_several_keys_multithreaded", get_several_keys_multithreaded, greentea_failure_handler),

    Case("TDB_remove_key_null", remove_key_null, greentea_failure_handler),
    Case("TDB_remove_key_length_exceeds_max", remove_key_length_exceeds_max, greentea_failure_handler),
    Case("TDB_remove_non_existing_key", remove_non_existing_key, greentea_failure_handler),
    Case("TDB_remove_removed_key", remove_removed_key, greentea_failure_handler),
    Case("TDB_remove_existed_key", remove_existed_key, greentea_failure_handler),

    Case("TDB_get_info_key_null", get_info_key_null, greentea_failure_handler),
    Case("TDB_get_info_key_length_exceeds_max", get_info_key_length_exceeds_max, greentea_failure_handler),
    Case("TDB_get_info_info_null", get_info_info_null, greentea_failure_handler),
    Case("TDB_get_info_non_existing_key", get_info_non_existing_key, greentea_failure_handler),
    Case("TDB_get_info_removed_key", get_info_removed_key, greentea_failure_handler),
    Case("TDB_get_info_existed_key", get_info_existed_key, greentea_failure_handler),
    Case("TDB_get_info_overwritten_key", get_info_overwritten_key, greentea_failure_handler),

    Case("TDB_iterator_open_it_null", iterator_open_it_null, greentea_failure_handler),

    Case("TDB_iterator_next_key_size_zero", iterator_next_key_size_zero, greentea_failure_handler),
    Case("TDB_iterator_next_empty_list", iterator_next_empty_list, greentea_failure_handler),
    Case("TDB_iterator_next_one_key_list", iterator_next_one_key_list, greentea_failure_handler),
    Case("TDB_iterator_next_empty_list_keys_removed", iterator_next_empty_list_keys_removed, greentea_failure_handler),
    Case("TDB_iterator_next_empty_list_non_matching_prefix", iterator_next_empty_list_non_matching_prefix, greentea_failure_handler),
    Case("TDB_iterator_next_several_overwritten_keys", iterator_next_several_overwritten_keys, greentea_failure_handler),
    Case("TDB_iterator_next_full_list", iterator_next_full_list, greentea_failure_handler),

    Case("TDB_iterator_close_right_after_iterator_open", iterator_close_right_after_iterator_open, greentea_failure_handler),

    Case("TDB_set_start_key_is_null", set_start_key_is_null, greentea_failure_handler),
    Case("TDB_set_start_key_size_exceeds_max_size", set_start_key_size_exceeds_max_size, greentea_failure_handler),
    Case("TDB_set_start_final_data_size_is_zero", set_start_final_data_size_is_zero, greentea_failure_handler),
    Case("TDB_set_start_final_data_size_is_smaller_than_real_data", set_start_final_data_size_is_smaller_than_real_data, greentea_failure_handler),
    Case("TDB_set_start_final_data_size_is_bigger_than_real_data", set_start_final_data_size_is_bigger_than_real_data, greentea_failure_handler),

    Case("TDB_set_add_data_value_data_is_null", set_add_data_value_data_is_null, greentea_failure_handler),
    Case("TDB_set_add_data_data_size_is_zero", set_add_data_data_size_is_zero, greentea_failure_handler),
    Case("TDB_set_add_data_data_size_bigger_than_real_data", set_add_data_data_size_bigger_than_real_data, greentea_failure_handler),
    Case("TDB_set_add_data_set_different_data_size_in_same_transaction", set_add_data_set_different_data_size_in_same_transaction, greentea_failure_handler),
    Case("TDB_set_add_data_set_key_value_five_Kbytes", set_add_data_set_key_value_five_Kbytes, greentea_failure_handler),
    Case("TDB_set_add_data_without_set_start", set_add_data_without_set_start, greentea_failure_handler),

    Case("TDB_set_finalize_without_set_start", set_finalize_without_set_start, greentea_failure_handler),
    Case("TDB_set_finalize_right_after_set_start", set_finalize_right_after_set_start, greentea_failure_handler),

    Case("TDB_kvstore_deinit", kvstore_deinit),

    /*----------------FSStore------------------*/

    Case("FS_kvstore_init", kvstore_init),

    Case("FS_set_key_null", set_key_null, greentea_failure_handler),
    Case("FS_set_key_length_exceeds_max", set_key_length_exceeds_max, greentea_failure_handler),
    Case("FS_set_buffer_null_size_not_zero", set_buffer_null_size_not_zero, greentea_failure_handler),
    Case("FS_set_key_undefined_flags", set_key_undefined_flags, greentea_failure_handler),
    Case("FS_set_buffer_size_is_zero", set_buffer_size_is_zero, greentea_failure_handler),
    Case("FS_set_same_key_several_time", set_same_key_several_time, greentea_failure_handler),
    Case("FS_set_several_keys_multithreaded", set_several_keys_multithreaded, greentea_failure_handler),
    Case("FS_set_write_once_flag_try_set_twice", set_write_once_flag_try_set_twice, greentea_failure_handler),
    Case("FS_set_write_once_flag_try_remove", set_write_once_flag_try_remove, greentea_failure_handler),
    Case("FS_set_key_value_one_byte_size", set_key_value_one_byte_size, greentea_failure_handler),
    Case("FS_set_key_value_two_byte_size", set_key_value_two_byte_size, greentea_failure_handler),
    Case("FS_set_key_value_five_byte_size", set_key_value_five_byte_size, greentea_failure_handler),
    Case("FS_set_key_value_fifteen_byte_size", set_key_value_fifteen_byte_size, greentea_failure_handler),
    Case("FS_set_key_value_seventeen_byte_size", set_key_value_seventeen_byte_size, greentea_failure_handler),
    Case("FS_set_several_key_value_sizes", set_several_key_value_sizes, greentea_failure_handler),

    Case("FS_get_key_null", get_key_null, greentea_failure_handler),
    Case("FS_get_key_length_exceeds_max", get_key_length_exceeds_max, greentea_failure_handler),
    Case("FS_get_buffer_null_size_not_zero", get_buffer_null_size_not_zero, greentea_failure_handler),
    Case("FS_get_buffer_size_is_zero", get_buffer_size_is_zero, greentea_failure_handler),
    Case("FS_get_buffer_size_smaller_than_data_real_size", get_buffer_size_smaller_than_data_real_size, greentea_failure_handler),
    Case("FS_get_buffer_size_bigger_than_data_real_size", get_buffer_size_bigger_than_data_real_size, greentea_failure_handler),
    Case("FS_get_offset_bigger_than_data_size", get_offset_bigger_than_data_size, greentea_failure_handler),
    Case("FS_get_non_existing_key", get_non_existing_key, greentea_failure_handler),
    Case("FS_get_removed_key", get_removed_key, greentea_failure_handler),
    Case("FS_get_key_that_was_set_twice", get_key_that_was_set_twice, greentea_failure_handler),
    Case("FS_get_several_keys_multithreaded", get_several_keys_multithreaded, greentea_failure_handler),

    Case("FS_remove_key_null", remove_key_null, greentea_failure_handler),
    Case("FS_remove_key_length_exceeds_max", remove_key_length_exceeds_max, greentea_failure_handler),
    Case("FS_remove_non_existing_key", remove_non_existing_key, greentea_failure_handler),
    Case("FS_remove_removed_key", remove_removed_key, greentea_failure_handler),
    Case("FS_remove_existed_key", remove_existed_key, greentea_failure_handler),

    Case("FS_get_info_key_null", get_info_key_null, greentea_failure_handler),
    Case("FS_get_info_key_length_exceeds_max", get_info_key_length_exceeds_max, greentea_failure_handler),
    Case("FS_get_info_info_null", get_info_info_null, greentea_failure_handler),
    Case("FS_get_info_non_existing_key", get_info_non_existing_key, greentea_failure_handler),
    Case("FS_get_info_removed_key", get_info_removed_key, greentea_failure_handler),
    Case("FS_get_info_existed_key", get_info_existed_key, greentea_failure_handler),
    Case("FS_get_info_overwritten_key", get_info_overwritten_key, greentea_failure_handler),

    Case("FS_iterator_open_it_null", iterator_open_it_null, greentea_failure_handler),

    Case("FS_iterator_next_key_size_zero", iterator_next_key_size_zero, greentea_failure_handler),
    Case("FS_iterator_next_empty_list", iterator_next_empty_list, greentea_failure_handler),
    Case("FS_iterator_next_one_key_list", iterator_next_one_key_list, greentea_failure_handler),
    Case("FS_iterator_next_empty_list_keys_removed", iterator_next_empty_list_keys_removed, greentea_failure_handler),
    Case("FS_iterator_next_empty_list_non_matching_prefix", iterator_next_empty_list_non_matching_prefix, greentea_failure_handler),
    Case("FS_iterator_next_several_overwritten_keys", iterator_next_several_overwritten_keys, greentea_failure_handler),
    Case("FS_iterator_next_full_list", iterator_next_full_list, greentea_failure_handler),

    Case("FS_iterator_close_right_after_iterator_open", iterator_close_right_after_iterator_open, greentea_failure_handler),

    Case("FS_set_start_key_is_null", set_start_key_is_null, greentea_failure_handler),
    Case("FS_set_start_key_size_exceeds_max_size", set_start_key_size_exceeds_max_size, greentea_failure_handler),
    Case("FS_set_start_final_data_size_is_zero", set_start_final_data_size_is_zero, greentea_failure_handler),
    Case("FS_set_start_final_data_size_is_smaller_than_real_data", set_start_final_data_size_is_smaller_than_real_data, greentea_failure_handler),
    Case("FS_set_start_final_data_size_is_bigger_than_real_data", set_start_final_data_size_is_bigger_than_real_data, greentea_failure_handler),

    Case("FS_set_add_data_value_data_is_null", set_add_data_value_data_is_null, greentea_failure_handler),
    Case("FS_set_add_data_data_size_is_zero", set_add_data_data_size_is_zero, greentea_failure_handler),
    Case("FS_set_add_data_data_size_bigger_than_real_data", set_add_data_data_size_bigger_than_real_data, greentea_failure_handler),
    Case("FS_set_add_data_set_different_data_size_in_same_transaction", set_add_data_set_different_data_size_in_same_transaction, greentea_failure_handler),
    Case("FS_set_add_data_set_key_value_five_Kbytes", set_add_data_set_key_value_five_Kbytes, greentea_failure_handler),
    Case("FS_set_add_data_without_set_start", set_add_data_without_set_start, greentea_failure_handler),

    Case("FS_set_finalize_without_set_start", set_finalize_without_set_start, greentea_failure_handler),
    Case("FS_set_finalize_right_after_set_start", set_finalize_right_after_set_start, greentea_failure_handler),

    Case("FS_kvstore_deinit", kvstore_deinit),

    /*----------------SecureStore------------------*/

    Case("Sec_kvstore_init", kvstore_init),

    Case("Sec_set_key_null", set_key_null, greentea_failure_handler),
    Case("Sec_set_key_length_exceeds_max", set_key_length_exceeds_max, greentea_failure_handler),
    Case("Sec_set_buffer_null_size_not_zero", set_buffer_null_size_not_zero, greentea_failure_handler),
    Case("Sec_set_buffer_size_is_zero", set_buffer_size_is_zero, greentea_failure_handler),
    Case("Sec_set_same_key_several_time", set_same_key_several_time, greentea_failure_handler),
    Case("Sec_set_several_keys_multithreaded", set_several_keys_multithreaded, greentea_failure_handler),
    Case("Sec_set_write_once_flag_try_set_twice", set_write_once_flag_try_set_twice, greentea_failure_handler),
    Case("Sec_set_write_once_flag_try_remove", set_write_once_flag_try_remove, greentea_failure_handler),
    Case("Sec_set_key_value_one_byte_size", set_key_value_one_byte_size, greentea_failure_handler),
    Case("Sec_set_key_value_two_byte_size", set_key_value_two_byte_size, greentea_failure_handler),
    Case("Sec_set_key_value_five_byte_size", set_key_value_five_byte_size, greentea_failure_handler),
    Case("Sec_set_key_value_fifteen_byte_size", set_key_value_fifteen_byte_size, greentea_failure_handler),
    Case("Sec_set_key_value_seventeen_byte_size", set_key_value_seventeen_byte_size, greentea_failure_handler),
    Case("Sec_set_several_key_value_sizes", set_several_key_value_sizes, greentea_failure_handler),
    Case("Sec_set_key_rollback_without_auth_flag", Sec_set_key_rollback_without_auth_flag, greentea_failure_handler),
    Case("Sec_set_key_rollback_set_again_no_rollback", Sec_set_key_rollback_set_again_no_rollback, greentea_failure_handler),
    Case("Sec_set_key_encrypt", Sec_set_key_encrypt, greentea_failure_handler),
    Case("Sec_set_key_auth", Sec_set_key_auth, greentea_failure_handler),

    Case("Sec_get_key_null", get_key_null, greentea_failure_handler),
    Case("Sec_get_key_length_exceeds_max", get_key_length_exceeds_max, greentea_failure_handler),
    Case("Sec_get_buffer_null_size_not_zero", get_buffer_null_size_not_zero, greentea_failure_handler),
    Case("Sec_get_buffer_size_is_zero", get_buffer_size_is_zero, greentea_failure_handler),
    Case("Sec_get_buffer_size_smaller_than_data_real_size", get_buffer_size_smaller_than_data_real_size, greentea_failure_handler),
    Case("Sec_get_buffer_size_bigger_than_data_real_size", get_buffer_size_bigger_than_data_real_size, greentea_failure_handler),
    Case("Sec_get_offset_bigger_than_data_size", get_offset_bigger_than_data_size, greentea_failure_handler),
    Case("Sec_get_non_existing_key", get_non_existing_key, greentea_failure_handler),
    Case("Sec_get_removed_key", get_removed_key, greentea_failure_handler),
    Case("Sec_get_key_that_was_set_twice", get_key_that_was_set_twice, greentea_failure_handler),
    Case("Sec_get_several_keys_multithreaded", get_several_keys_multithreaded, greentea_failure_handler),

    Case("Sec_remove_key_null", remove_key_null, greentea_failure_handler),
    Case("Sec_remove_key_length_exceeds_max", remove_key_length_exceeds_max, greentea_failure_handler),
    Case("Sec_remove_non_existing_key", remove_non_existing_key, greentea_failure_handler),
    Case("Sec_remove_removed_key", remove_removed_key, greentea_failure_handler),
    Case("Sec_remove_existed_key", remove_existed_key, greentea_failure_handler),

    Case("Sec_get_info_key_null", get_info_key_null, greentea_failure_handler),
    Case("Sec_get_info_key_length_exceeds_max", get_info_key_length_exceeds_max, greentea_failure_handler),
    Case("Sec_get_info_info_null", get_info_info_null, greentea_failure_handler),
    Case("Sec_get_info_non_existing_key", get_info_non_existing_key, greentea_failure_handler),
    Case("Sec_get_info_removed_key", get_info_removed_key, greentea_failure_handler),
    Case("Sec_get_info_existed_key", get_info_existed_key, greentea_failure_handler),
    Case("Sec_get_info_overwritten_key", get_info_overwritten_key, greentea_failure_handler),

    Case("Sec_iterator_open_it_null", iterator_open_it_null, greentea_failure_handler),

    Case("Sec_iterator_next_key_size_zero", iterator_next_key_size_zero, greentea_failure_handler),
    Case("Sec_iterator_next_empty_list", iterator_next_empty_list, greentea_failure_handler),
    Case("Sec_iterator_next_one_key_list", iterator_next_one_key_list, greentea_failure_handler),
    Case("Sec_iterator_next_empty_list_keys_removed", iterator_next_empty_list_keys_removed, greentea_failure_handler),
    Case("Sec_iterator_next_empty_list_non_matching_prefix", iterator_next_empty_list_non_matching_prefix, greentea_failure_handler),
    Case("Sec_iterator_next_several_overwritten_keys", iterator_next_several_overwritten_keys, greentea_failure_handler),
    Case("Sec_iterator_next_full_list", iterator_next_full_list, greentea_failure_handler),

    Case("Sec_iterator_close_right_after_iterator_open", iterator_close_right_after_iterator_open, greentea_failure_handler),

    Case("Sec_set_start_key_is_null", set_start_key_is_null, greentea_failure_handler),
    Case("Sec_set_start_key_size_exceeds_max_size", set_start_key_size_exceeds_max_size, greentea_failure_handler),
    Case("Sec_set_start_final_data_size_is_zero", set_start_final_data_size_is_zero, greentea_failure_handler),
    Case("Sec_set_start_final_data_size_is_smaller_than_real_data", set_start_final_data_size_is_smaller_than_real_data, greentea_failure_handler),
    Case("Sec_set_start_final_data_size_is_bigger_than_real_data", set_start_final_data_size_is_bigger_than_real_data, greentea_failure_handler),

    Case("Sec_set_add_data_value_data_is_null", set_add_data_value_data_is_null, greentea_failure_handler),
    Case("Sec_set_add_data_data_size_is_zero", set_add_data_data_size_is_zero, greentea_failure_handler),
    Case("Sec_set_add_data_data_size_bigger_than_real_data", set_add_data_data_size_bigger_than_real_data, greentea_failure_handler),
    Case("Sec_set_add_data_set_different_data_size_in_same_transaction", set_add_data_set_different_data_size_in_same_transaction, greentea_failure_handler),
    Case("Sec_set_add_data_set_key_value_five_Kbytes", set_add_data_set_key_value_five_Kbytes, greentea_failure_handler),
    Case("Sec_set_add_data_without_set_start", set_add_data_without_set_start, greentea_failure_handler),

    Case("Sec_set_finalize_without_set_start", set_finalize_without_set_start, greentea_failure_handler),
    Case("Sec_set_finalize_right_after_set_start", set_finalize_right_after_set_start, greentea_failure_handler),

    Case("Sec_kvstore_deinit", kvstore_deinit),

};


utest::v1::status_t greentea_test_setup(const size_t number_of_cases)
{
    GREENTEA_SETUP(3000, "default_auto");
    return greentea_test_setup_handler(number_of_cases);
}

Specification specification(greentea_test_setup, cases, greentea_test_teardown_handler);

int main()
{
    return !Harness::run(specification);
}
