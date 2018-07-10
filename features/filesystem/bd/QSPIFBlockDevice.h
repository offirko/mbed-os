/* mbed Microcontroller Library
 * Copyright (c) 2016 ARM Limited
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
#ifndef MBED_QSPIF_BLOCK_DEVICE_H
#define MBED_QSPIF_BLOCK_DEVICE_H


#define DEVICE_QSPI 1

#include <mbed.h>
#include "BlockDevice.h"
//#include "./mbed-os/drivers/QSPI.h"

#define QSPIF_MAX_REGIONS	4
 
class QSPIFBlockDevice : public BlockDevice {
public:
    /** Creates a QSPIFBlockDevice on a SPI bus specified by pins
     *
     *  @param mosi     SPI master out, slave in pin
     *  @param miso     SPI master in, slave out pin
     *  @param sclk     SPI clock pin
     *  @param csel     SPI chip select pin
     *  @param freq     Clock speed of the SPI bus (defaults to 40MHz)
     */
	 
    /** Creates a QSPIFBlockDevice on an SPI bus specified by pins
     *
     *  io0-io3 is used to specify the Pins used for Quad SPI mode
     *
     *  @param io0 1st IO pin used for sending/receiving data during data phase of a transaction
     *  @param io1 2nd IO pin used for sending/receiving data during data phase of a transaction
     *  @param io2 3rd IO pin used for sending/receiving data during data phase of a transaction
     *  @param io3 4th IO pin used for sending/receiving data during data phase of a transaction
     *  @param sclk QSPI Clock pin
     *  @param csel QSPI chip select pin
     *  @param clock_mode specifies the SPI Clock Polarity mode(Mode=0 uses CPOL=0, CPHA=0, Mode=1 uses CPOL=1, CPHA=1)
     *         default value = 0
     *  @param freq Clock frequency of the SPI bus (defaults to 40MHz)
     *
     */
    QSPIFBlockDevice(PinName io0, PinName io1, PinName io2, PinName io3, PinName sclk, PinName csel, int clock_mode, int freq=40000000);

    /** Initialize a block device
     *
     *  @return         0 on success or a negative error code on failure
     */
    virtual int init();

    /** Deinitialize a block device
     *
     *  @return         0 on success or a negative error code on failure
     */
    virtual int deinit();

    /** Read blocks from a block device
     *
     *  @param buffer   Buffer to write blocks to
     *  @param addr     Address of block to begin reading from
     *  @param size     Size to read in bytes, must be a multiple of read block size
     *  @return         0 on success, negative error code on failure
     */
    virtual int read(void *buffer, bd_addr_t addr, bd_size_t size);

    /** Program blocks to a block device
     *
     *  The blocks must have been erased prior to being programmed
     *
     *  @param buffer   Buffer of data to write to blocks
     *  @param addr     Address of block to begin writing to
     *  @param size     Size to write in bytes, must be a multiple of program block size
     *  @return         0 on success, negative error code on failure
     */
    virtual int program(const void *buffer, bd_addr_t addr, bd_size_t size);

    /** Erase blocks on a block device
     *
     *  The state of an erased block is undefined until it has been programmed
     *
     *  @param addr     Address of block to begin erasing
     *  @param size     Size to erase in bytes, must be a multiple of erase block size
     *  @return         0 on success, negative error code on failure
     */
    virtual int erase(bd_addr_t addr, bd_size_t size);

    /** Get the size of a readable block
     *
     *  @return         Size of a readable block in bytes
     */
    virtual bd_size_t get_read_size() const;

    /** Get the size of a programable block
     *
     *  @return         Size of a programable block in bytes
     *  @note Must be a multiple of the read size
     */
    virtual bd_size_t get_program_size() const;

    /** Get the size of a eraseable block
     *
     *  @return         Size of a eraseable block in bytes
     *  @note Must be a multiple of the program size
     */
    virtual bd_size_t get_erase_size() const;
	
    /** Get the size of a eraseable block
     *
	 *  @param addr     Address of block queried for erase sector size
     *  @return         Size of a eraseable sector in bytes
     *  @note Must be a multiple of the program size
     */
	bd_size_t get_erase_size(bd_addr_t addr);

    /** Get the total size of the underlying device
     *
     *  @return         Size of the underlying device in bytes
     */
    virtual bd_size_t size() const;
    
private:	
    // Internal functions
	
    /********************************/
    /*   Calls to QSPI Driver APIs  */
    /********************************/
	// Send Program => Write command to Driver
    qspi_status_t _qspiSendProgramCommand(unsigned int progInstruction, const void *buffer, bd_addr_t addr, bd_size_t *size);

	// Send Read command to Driver
    qspi_status_t _qspiSendReadCommand(unsigned int readInstruction, void *buffer, bd_addr_t addr, bd_size_t size);

	// Send Erase => command_transfer command to Driver
    qspi_status_t _qspiSendEraseCommand(unsigned int eraseInstruction, bd_addr_t addr, bd_size_t size);

	// Send Generic command_transfer command to Driver
    qspi_status_t _qspiSendGeneralCommand(unsigned int instructionint, bd_addr_t addr, const char *tx_buffer, size_t tx_length, const char *rx_buffer, size_t rx_length);

    // Send Bus configure_format command to Driver
    qspi_status_t _qspiConfiureFormat(qspi_bus_width_t inst_width, qspi_bus_width_t address_width, qspi_address_size_t address_size, qspi_bus_width_t alt_width, qspi_alt_size_t alt_size, qspi_bus_width_t data_width, int dummy_cycles);

    // Send set_frequency command to Driver
    qspi_status_t _qspiSetFrequency(int freq);
    /********************************/


    // Verify registers and Reset Flash Memory
	int _resetFlashMem();
	
	// Configure Write Enable in Status Register
    int _setWriteEnable();
	
	// Wait on status register until write not-in-progress
    bool _isMemReady();


    /* SFDP Detection and Parsing Functions */
    /****************************************/
    int _sfdpParseSFDPHeaders(uint32_t &basic_table_addr, size_t &basic_table_size,
    						 uint32_t &sector_map_table_addr, size_t &sector_map_table_size);
    int _sfdpParseBasicParamTable(uint32_t basic_table_addr, size_t basic_table_size);
    int _sfdpParseSectorMapTable(uint32_t sector_map_table_addr, size_t sector_map_table_size);
    int _sfdpDetectBestBusReadMode(uint8_t *basicParamTablePtr, bool &setQuadEnable, bool &isQPIMode, unsigned int &readInst);
    int _sfdpSetQuadEnabled(uint8_t *basicParamTablePtr);
    int _sfdpSetQPIEnabled(uint8_t *basicParamTablePtr);
    int _sfdpDetectPageSize(uint8_t *basicParamTablePtr);
    int _sfdpDetectEraseTypesInstAndSize(uint8_t *basicParamTablePtr, unsigned int &erase4KInst, unsigned int *eraseTypeInstArr, unsigned int *eraseTypeSizeArr);

    /* Utilities Functions */
    /***********************/
    int _utilsFindAddrRegion(int offset);
    int _utilsIterateNextLargestEraseType(uint8_t &bitfield, int size, int offset, int boundry);
    int _utilsMathPower(int base, int exp);

private:

    // QSPI Driver Object
    QSPI _qspi;
    //DigitalOut _cs;
    bool is_initialized;
    static SingletonPtr<PlatformMutex> _mutex;

    // Command Instructions
    unsigned int _readInstruction;
    unsigned int _progInstruction;
    unsigned int _eraseInstruction;
    unsigned int _erase4KInst;
    unsigned int _eraseTypeInstArr[4];
    unsigned int _eraseTypeSizeArr[4];

    // Sector Regions Map
    int _regions_count; //number of regions
    int _region_size_bytes[QSPIF_MAX_REGIONS]; //regions size
    int _region_high_boundary[QSPIF_MAX_REGIONS]; //region high address offset boundary
    uint8_t _region_erase_types[QSPIF_MAX_REGIONS]; //region erase type
    int _minCommonEraseType; // minimal common erase size for all regions (-1 if none)
    int _minCommonEraseSize; // minimal common erase size for all regions

    int _pageSizeBytes; // Page size - 256 Bytes default
    bd_size_t _deviceSizeBytes;

    // Bus speed configuration
    qspi_bus_width_t _inst_width; //Bus width for Instruction phase
    qspi_bus_width_t _address_width; //Bus width for Address phase
    qspi_address_size_t _address_size;
    qspi_bus_width_t _alt_width; //Bus width for Alt phase
    qspi_alt_size_t _alt_size;
    qspi_bus_width_t _data_width; //Bus width for Data phase
    int _dummy_and_mode_cycles;


};

#endif
