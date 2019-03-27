/* mbed Microcontroller Library
 * Copyright (c) 2018 ARM Limited
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
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS //Required for PRIu64
#endif

#include "mbed.h"
#include "greentea-client/test_env.h"
#include "unity.h"
#include "utest.h"
#include "mbed_trace.h"
#include <inttypes.h>
#include <stdlib.h>
#include "BufferedBlockDevice.h"
#include "BlockDevice.h"

/* OFR_DBG */
#include "mbed.h"


#if COMPONENT_SPIF
#include "SPIFBlockDevice.h"
#endif

#if COMPONENT_QSPIF
#include "QSPIFBlockDevice.h"
#endif

#if COMPONENT_DATAFLASH
#include "DataFlashBlockDevice.h"
#endif

#if COMPONENT_SD
#include "SDBlockDevice.h"
#endif

#if COMPONENT_FLASHIAP
#include "FlashIAPBlockDevice.h"
#endif

using namespace utest::v1;

#define TEST_BLOCK_COUNT 1000
#define TEST_ERROR_MASK 16
#define TEST_NUM_OF_THREADS 5
#define TEST_THREAD_STACK_SIZE 1024

const struct {
    const char *name;
    bd_size_t (BlockDevice::*method)() const;
} ATTRS[] = {
    {"read size",    &BlockDevice::get_read_size},
    {"program size", &BlockDevice::get_program_size},
    {"erase size",   &BlockDevice::get_erase_size},
    {"total size",   &BlockDevice::size},
};

enum bd_type {
    spif = 0,
    qspif,
    dataflash,
    sd,
    flashiap,
    default_bd
};

uint8_t bd_arr[5] = {0};

static uint8_t test_iteration = 0;

static SingletonPtr<PlatformMutex> _mutex;

BlockDevice *block_device = NULL;

#if COMPONENT_FLASHIAP
static inline uint32_t align_up(uint32_t val, uint32_t size)
{
    return (((val - 1) / size) + 1) * size;
}
#endif

static BlockDevice *get_bd_instance(uint8_t bd_type)
{
    switch (bd_arr[bd_type]) {
        case spif: {
#if COMPONENT_SPIF
            static SPIFBlockDevice default_bd(
                MBED_CONF_SPIF_DRIVER_SPI_MOSI,
                MBED_CONF_SPIF_DRIVER_SPI_MISO,
                MBED_CONF_SPIF_DRIVER_SPI_CLK,
                MBED_CONF_SPIF_DRIVER_SPI_CS,
                MBED_CONF_SPIF_DRIVER_SPI_FREQ
            );
            return &default_bd;
#endif
            break;
        }
        case qspif: {
#if COMPONENT_QSPIF
            static QSPIFBlockDevice default_bd(
                MBED_CONF_QSPIF_QSPI_IO0,
                MBED_CONF_QSPIF_QSPI_IO1,
                MBED_CONF_QSPIF_QSPI_IO2,
                MBED_CONF_QSPIF_QSPI_IO3,
                MBED_CONF_QSPIF_QSPI_SCK,
                MBED_CONF_QSPIF_QSPI_CSN,
                MBED_CONF_QSPIF_QSPI_POLARITY_MODE,
                MBED_CONF_QSPIF_QSPI_FREQ
            );
            return &default_bd;
#endif
            break;
        }
        case dataflash: {
#if COMPONENT_DATAFLASH
            static DataFlashBlockDevice default_bd(
                MBED_CONF_DATAFLASH_SPI_MOSI,
                MBED_CONF_DATAFLASH_SPI_MISO,
                MBED_CONF_DATAFLASH_SPI_CLK,
                MBED_CONF_DATAFLASH_SPI_CS
            );
            return &default_bd;
#endif
            break;
        }
        case sd: {
#if COMPONENT_SD
            static SDBlockDevice default_bd(
                MBED_CONF_SD_SPI_MOSI,
                MBED_CONF_SD_SPI_MISO,
                MBED_CONF_SD_SPI_CLK,
                MBED_CONF_SD_SPI_CS
            );
            return &default_bd;
#endif
            break;
        }
        case flashiap: {
#if COMPONENT_FLASHIAP
#if (MBED_CONF_FLASHIAP_BLOCK_DEVICE_SIZE == 0) && (MBED_CONF_FLASHIAP_BLOCK_DEVICE_BASE_ADDRESS == 0xFFFFFFFF)

            size_t flash_size;
            uint32_t start_address;
            uint32_t bottom_address;
            mbed::FlashIAP flash;

            int ret = flash.init();
            if (ret != 0) {
                return NULL;
            }

            //Find the start of first sector after text area
            bottom_address = align_up(FLASHIAP_APP_ROM_END_ADDR, flash.get_sector_size(FLASHIAP_APP_ROM_END_ADDR));
            start_address = flash.get_flash_start();
            flash_size = flash.get_flash_size();

            ret = flash.deinit();

            static FlashIAPBlockDevice default_bd(bottom_address, start_address + flash_size - bottom_address);

#else

            static FlashIAPBlockDevice default_bd;

#endif
            return &default_bd;
#endif
            break;
        }
    }
    return NULL;
}

void test_init_bd()
{
    utest_printf("\nTest Init block device.\n");

    block_device = get_bd_instance(test_iteration);

    TEST_SKIP_UNLESS_MESSAGE(block_device != NULL, "no block device found.");

    int err = block_device->init();
    TEST_ASSERT_EQUAL(0, err);
}


/*******************************************************************************************/

#define SAMPLE_TIME             2000    // msec
#define LOOP_TIME               3000    // msec
#define L_NUM_WORKER_THREADS 1
 
uint64_t prev_idle_time = 0;
int32_t wait_time = 5000;      // usec

void calc_cpu_usage()
{
    mbed_stats_cpu_t stats;
    mbed_stats_cpu_get(&stats);
 
    uint64_t diff = (stats.idle_time - prev_idle_time);
    uint8_t idle = (diff * 100) / (SAMPLE_TIME*1000);    // usec;
    uint8_t usage = 100 - ((diff * 100) / (SAMPLE_TIME*1000));    // usec;;
    prev_idle_time = stats.idle_time;
 
    printf("Idle: %d Usage: %d \n", idle, usage);
}

int g_threads_alive = 1;

static void cpu_thread_job(void *a_arg)
{
    // Request the shared queue
    EventQueue *stats_queue = mbed_event_queue();
    int id;
 
    id = stats_queue->call_every(SAMPLE_TIME, calc_cpu_usage);

    while (g_threads_alive) {
	wait_ms(500);
    }
}


static void worker_thread_job(void *a_arg)
{
   int i_ind = 0;
   float j_ind = 2.3;
   int l_counter = 0;

   printf("Starting Worker Thread\n");
   while (g_threads_alive) {
	i_ind+=23;

	l_counter++;
	if (l_counter % 7 == 0) {
	wait_ms(1);
	i_ind+=1117;
	}

	i_ind/=j_ind;
	j_ind+=0.27;
	if (i_ind > 0xFFFFFFF) {
	printf("\nOFR_DBG i_ind: %d\n",i_ind);
	i_ind = 0;
	}
   }    
}




void test_prog_read_multi_threaded()
{
    osStatus threadStatus;
    int j_ind;
    int t_ind = 0;
    int l_arg = 0;

    utest_printf("\nTest Program Read Multi Starts..\n");
    TEST_SKIP_UNLESS_MESSAGE(block_device != NULL, "no block device found.");

    /* Print Block Device Properties */
    for (unsigned atr = 0; atr < sizeof(ATTRS) / sizeof(ATTRS[0]); atr++) {
        static const char *prefixes[] = {"", "k", "M", "G"};
        for (int i_ind = 3; i_ind >= 0; i_ind--) {
            bd_size_t size = (block_device->*ATTRS[atr].method)();
            if (size >= (1ULL << 10 * i_ind)) {
                utest_printf("%s: %llu%sbytes (%llubytes)\n",
                             ATTRS[atr].name, size >> 10 * i_ind, prefixes[i_ind], size);
                break;
            }
        }
    }


    bd_size_t block_size = 4*1024; // Programing in 4KB Chunks
    unsigned addrwidth = ceil(log(float(block_device->size() - 1)) / log(float(16))) + 1;

    uint8_t *write_block = new (std::nothrow) uint8_t[block_size];
    uint8_t *read_block = new (std::nothrow) uint8_t[block_size];

    int b, val_rand;
    bd_size_t i_ind = 0;
    int err = 0;
    // Make sure block address per each test is unique
    static unsigned block_seed = 1;
    unsigned seed;
    bd_addr_t block = 64*1024*1024 % block_device->size();


    if (!write_block || !read_block) {
        utest_printf("Not enough memory for test\n");
        return;
    }

    srand(block_seed++);


    /* Erase 64MB at address 64M */
    printf("\n Now Erasing...\n");
    err = block_device->erase(block, 64*1024*1024);
    TEST_ASSERT_EQUAL(0, err);

    /* Select initial 'random' Seed for Write/Read data */
    seed = rand();
    // Fill with random sequence
    srand(seed);

    /* ***A. Programing Data at 4KB Chunks */

    block_size = 4096;
    printf("\n Started Programing...\n");
    for (b = 0; b < TEST_BLOCK_COUNT; b++) {
       memset(write_block,0,4096);
       for (i_ind = 0; i_ind < block_size; i_ind++) {
	   write_block[i_ind] = 0xff & rand();
       }
       err = block_device->program(write_block, block, block_size);
       TEST_ASSERT_EQUAL(0, err);
       block += block_size;
    }
    printf("\n Programing Done...\n");


    /* ***B. Creat CPU Measuring thread and Worker Thread */
    rtos::Thread **bd_thread = new (std::nothrow) rtos::Thread*[L_NUM_WORKER_THREADS+1];

    memset(bd_thread, 0, (L_NUM_WORKER_THREADS+1) * sizeof(rtos::Thread *));

    bd_thread[0] = new (std::nothrow) rtos::Thread((osPriority_t)((int)osPriorityNormal), TEST_THREAD_STACK_SIZE);
    threadStatus = bd_thread[0]->start(callback(cpu_thread_job, (void *)&l_arg));
    if (threadStatus != 0) {
       utest_printf("Thread %d Start Failed!\n", t_ind + 1);
    }

    for (t_ind = 1; t_ind < (L_NUM_WORKER_THREADS+1); t_ind++) {

        bd_thread[t_ind] = new (std::nothrow) rtos::Thread((osPriority_t)((int)osPriorityNormal), TEST_THREAD_STACK_SIZE);
 
        threadStatus = bd_thread[t_ind]->start(callback(worker_thread_job, (void *)&l_arg));
        if (threadStatus != 0) {
            utest_printf("Thread %d Start Failed!\n", t_ind + 1);
            break;
        }
    }

    /* Measure Only Worker Thread CPU for 10 seconds */
    wait(10);



   /* ***C. Start Reading 64MB from address 64M at 1KB Chunks */
   block = 64*1024*1024 % block_device->size();
    
   /* use same seed as for program, to repeat the same expected data sequence */
   srand(seed);

   block_size = 1024;

   printf("\n Now Reading...\n");

   for (b = 0; b < TEST_BLOCK_COUNT*4; b++) {
       memset(read_block,0,4096);

	/* read 1024 bytes chunk */
       err = block_device->read(read_block, block, block_size);
       TEST_ASSERT_EQUAL(0, err);

        /* verify content with expected content according to 'rand' sequence */
       for (i_ind = 0; i_ind < block_size; i_ind++) {
		val_rand = rand();
		if ((0xff & val_rand) != read_block[i_ind]) {
		    utest_printf("\n Assert Failed Buf Read - block:size: %llx:%llu \n", block, block_size);
		    utest_printf("\n pos: %llu, exp: %02x, act: %02x \n", i_ind, (0xff & val_rand), read_block[i_ind]);
		}
		TEST_ASSERT_EQUAL(0xff & val_rand, read_block[i_ind]);
       }

       block += block_size;
   }

    delete[] write_block;
    delete[] read_block;


    printf("\nOFR_DBG WAiting For Threads to Join\n");
    g_threads_alive = 0;

    for (j_ind = 0; j_ind < t_ind; j_ind++) {
        bd_thread[j_ind]->join();
    }

    if (bd_thread) {
        for (j_ind = 0; j_ind < t_ind; j_ind++) {
            delete bd_thread[j_ind];
        }

        delete[] bd_thread;
    }

}




void test_deinit_bd()
{
    utest_printf("\nTest deinit block device.\n");

    test_iteration++;

    TEST_SKIP_UNLESS_MESSAGE(block_device != NULL, "no block device found.");

    int err = block_device->deinit();
    TEST_ASSERT_EQUAL(0, err);

    block_device = NULL;
}


void test_get_type_functionality()
{
    utest_printf("\nTest get blockdevice type..\n");

    block_device = BlockDevice::get_default_instance();
    TEST_SKIP_UNLESS_MESSAGE(block_device != NULL, "no block device found.");

    const char *bd_type = block_device->get_type();
    TEST_ASSERT_NOT_EQUAL(0, bd_type);

#if COMPONENT_QSPIF
    TEST_ASSERT_EQUAL(0, strcmp(bd_type, "QSPIF"));
#elif COMPONENT_SPIF
    TEST_ASSERT_EQUAL(0, strcmp(bd_type, "SPIF"));
#elif COMPONENT_DATAFLASH
    TEST_ASSERT_EQUAL(0, strcmp(bd_type, "DATAFLASH"));
#elif COMPONENT_SD
    TEST_ASSERT_EQUAL(0, strcmp(bd_type, "SD"));
#elif COMPONET_FLASHIAP
    TEST_ASSERT_EQUAL(0, strcmp(bd_type, "FLASHIAP"));
#endif
}

utest::v1::status_t greentea_failure_handler(const Case *const source, const failure_t reason)
{
    greentea_case_failure_abort_handler(source, reason);
    return STATUS_CONTINUE;
}

typedef struct {
    const char *description;
    const case_handler_t case_handler;
    const case_failure_handler_t failure_handler;
} template_case_t;

template_case_t template_cases[] = {
    {"Testing Init block device", test_init_bd, greentea_failure_handler},
    {"Testing prog read multi", test_prog_read_multi_threaded, greentea_failure_handler},
    {"Testing Deinit block device", test_deinit_bd, greentea_failure_handler},
};

template_case_t def_template_case = {"Testing get type functionality", test_get_type_functionality, greentea_failure_handler};

utest::v1::status_t greentea_test_setup(const size_t number_of_cases)
{
    return greentea_test_setup_handler(number_of_cases);
}

int get_bd_count()
{
    int count = 0;

#if COMPONENT_SPIF
    bd_arr[count++] = spif;           //0
#endif
#if COMPONENT_QSPIF
    bd_arr[count++] = qspif;          //1
#endif
#if COMPONENT_DATAFLASH
    bd_arr[count++] = dataflash;      //2
#endif
#if COMPONENT_SD
    bd_arr[count++] = sd;             //3
#endif
#if COMPONENT_FLASHIAP
    bd_arr[count++] = flashiap;       //4
#endif

    return count;
}

static const char *prefix[] = {"SPIF ", "QSPIF ", "DATAFLASH ", "SD ", "FLASHIAP ", "DEFAULT "};

int main()
{
    GREENTEA_SETUP(3000, "default_auto");

    // We want to replicate our test cases to different types
    size_t num_cases = sizeof(template_cases) / sizeof(template_case_t);
    size_t total_num_cases = 0;

    int bd_count = get_bd_count();

    void *raw_mem = new (std::nothrow) uint8_t[(bd_count * num_cases + 1) * sizeof(Case)];
    Case *cases = static_cast<Case *>(raw_mem);

    for (int j = 0; j < bd_count; j++) {
        for (size_t i = 0; i < num_cases; i++) {
            char desc[128], *desc_ptr;
            sprintf(desc, "%s%s", prefix[bd_arr[j]], template_cases[i].description);
            desc_ptr = new char[strlen(desc) + 1];
            strcpy(desc_ptr, desc);
            new (&cases[total_num_cases]) Case((const char *) desc_ptr, template_cases[i].case_handler,
                                               template_cases[i].failure_handler);
            total_num_cases++;
        }

        //Add test_get_type_functionality once, runs on default blockdevice
        if (j == bd_count - 1) {
            char desc[128], *desc_ptr;
            sprintf(desc, "%s%s", prefix[default_bd], def_template_case.description);
            desc_ptr = new char[strlen(desc) + 1];
            strcpy(desc_ptr, desc);
            new (&cases[total_num_cases]) Case((const char *) desc_ptr, def_template_case.case_handler,
                                               def_template_case.failure_handler);
            total_num_cases++;
        }
    }



    Specification specification(greentea_test_setup, cases, total_num_cases,
                                greentea_test_teardown_handler, (test_failure_handler_t)greentea_failure_handler);

    return !Harness::run(specification);
}
