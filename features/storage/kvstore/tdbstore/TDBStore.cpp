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

// ----------------------------------------------------------- Includes -----------------------------------------------------------

#include "TDBStore.h"

#include <algorithm>
#include <string.h>
#include <stdio.h>
#include "mbed_error.h"
#include "mbed_assert.h"
#include "mbed_wait_api.h"

using namespace mbed;

// --------------------------------------------------------- Definitions ----------------------------------------------------------

static const uint32_t delete_flag = (1 << 31);

typedef struct {
    uint32_t magic;
    uint16_t header_size;
    uint16_t revision;
    uint32_t flags;
    uint16_t key_size;
    uint16_t reserved;
    uint32_t data_size;
    uint32_t crc;
} record_header_t;

typedef struct {
    uint32_t  hash;
    bd_size_t bd_offset;
} ram_table_entry_t;

static const char *master_rec_key = "TDBS";
static const uint32_t tdbstore_magic = 0x54686683; // "TDBS" in ASCII
static const uint32_t tdbstore_revision = 1;

typedef struct {
    uint16_t version;
    uint16_t tdbstore_revision;
    uint32_t reserved;
} master_record_data_t;

typedef enum {
    TDBSTORE_AREA_STATE_NONE = 0,
    TDBSTORE_AREA_STATE_EMPTY,
    TDBSTORE_AREA_STATE_VALID,
} area_state_e;

static const uint32_t work_buf_size = 64;
static const uint32_t initial_crc = 0xFFFFFFFF;
static const uint32_t initial_max_keys = 16;

// incremental set handle
typedef struct {
    record_header_t header;
    bd_size_t bd_base_offset;
    bd_size_t bd_curr_offset;
    uint32_t offset_in_data;
    uint32_t ram_table_ind;
    uint32_t hash;
    bool new_key;
} inc_set_handle_t;

// iterator handle
typedef struct {
    size_t ram_table_ind;
    char *prefix;
} key_iterator_handle_t;


// -------------------------------------------------- Local Functions Declaration ----------------------------------------------------

// -------------------------------------------------- Functions Implementation ----------------------------------------------------

static inline uint32_t align_up(uint32_t val, uint32_t size)
{
    return (((val - 1) / size) + 1) * size;
}


// CRC32 calculation. Supports "rolling" calculation (using the initial value).
static uint32_t crc32(uint32_t init_crc, uint32_t data_size, const void *data_buf)
{
    uint32_t i, j;
    uint32_t crc, mask;
    const char *data = static_cast <const char *> (data_buf);

    crc = init_crc;
    for (i = 0; i < data_size; i++) {
        crc = crc ^ (uint32_t) (data[i]);
        for (j = 0; j < 8; j++) {
          mask = -(crc & 1);
          crc = (crc >> 1) ^ (0xEDB88320 & mask);
        }
    }
    return crc;
}


// Class member functions

TDBStore::TDBStore(BlockDevice *bd) : _ram_table(0), _max_keys(0),
    _num_keys(0), _bd(bd), _buff_bd(0),  _free_space_offset(0), _master_record_offset(0),
    _is_initialized(false), _active_area(0), _active_area_version(0), _size(0),
    _prog_size(0), _work_buf(0), _key_buf(0), _variant_bd_erase_unit_size(false), _inc_set_handle(0)
{
}

TDBStore::~TDBStore()
{
    deinit();
}

int TDBStore::read_area(uint8_t area, uint32_t offset, uint32_t size, void *buf)
{
    int ret = _buff_bd->read(buf, _area_params[area].address + offset, size);

    if (ret) {
        return MBED_ERROR_READ_FAILED;
    }

    return MBED_SUCCESS;
}

int TDBStore::write_area(uint8_t area, uint32_t offset, uint32_t size, const void *buf)
{
    int ret = _buff_bd->program(buf, _area_params[area].address + offset, size);
    if (ret) {
        return MBED_ERROR_WRITE_FAILED;
    }

    return MBED_SUCCESS;
}

int TDBStore::erase_erase_unit(uint8_t area, uint32_t offset)
{
    uint32_t bd_offset = _area_params[area].address + offset;
    uint32_t eu_size = _buff_bd->get_erase_size(bd_offset);

    int ret = _buff_bd->erase(bd_offset, eu_size);
    if (ret) {
        return MBED_ERROR_WRITE_FAILED;
    }
    return MBED_SUCCESS;
}

void TDBStore::calc_area_params()
{
    size_t bd_size = _bd->size();

    memset(_area_params, 0, sizeof(_area_params));
    size_t area_0_size = 0;
    bd_size_t prev_erase_unit_size = _bd->get_erase_size(area_0_size);
    _variant_bd_erase_unit_size = 0;

    while (area_0_size < bd_size / 2) {
        bd_size_t erase_unit_size = _bd->get_erase_size(area_0_size);
        _variant_bd_erase_unit_size |= (erase_unit_size != prev_erase_unit_size);
        area_0_size += erase_unit_size;
    }

    _area_params[0].address = 0;
    _area_params[0].size = area_0_size;
    _area_params[1].address = area_0_size;
    _area_params[1].size = bd_size - area_0_size;
}


// This function, reading a record from the BD, is used for multiple purposes:
// - Init (scan all records, no need to return file name and data)
// - Get (return file data)
// - Get first/next file (check whether name matches, return name if so)
int TDBStore::read_record(uint8_t area, uint32_t offset, char *key,
                            void *data_buf, uint32_t data_buf_size,
                            uint32_t& actual_data_size, size_t data_offset, bool copy_key,
                            bool copy_data, bool check_expected_key, bool calc_hash,
                            uint32_t& hash, uint32_t& flags, uint32_t& next_offset)
{
    int os_ret, ret;
    record_header_t header;
    uint32_t total_size, key_size, data_size;
    uint32_t curr_data_offset;
    char *user_key_ptr;
    uint32_t crc = initial_crc;
    // Upper layers typically use non zero offsets for reading the records chunk by chunk,
    // so only validate entire record at first chunk (otherwise we'll have a serious performance penalty).
    bool validate = (data_offset == 0);

    ret = MBED_SUCCESS;
    // next offset should only be updated to the end of record if successful
    next_offset = offset;

    os_ret = read_area(area, offset, sizeof(header), &header);
    if (os_ret) {
        return MBED_ERROR_READ_FAILED;
    }

    if (header.magic != tdbstore_magic) {
        return MBED_ERROR_INVALID_DATA_DETECTED;
    }

    offset += align_up(sizeof(header), _prog_size);

    key_size = header.key_size;
    data_size = header.data_size;
    flags = header.flags;

    if ((!key_size) || (key_size >= MAX_KEY_SIZE)) {
        return MBED_ERROR_INVALID_DATA_DETECTED;
    }

    total_size = key_size + data_size;

    if (offset + total_size >= _size) {
        return MBED_ERROR_INVALID_DATA_DETECTED;
    }

    if (data_offset > data_size) {
        return MBED_ERROR_INVALID_SIZE;
    }

    if (copy_data && data_buf_size && !data_buf) {
        return MBED_ERROR_INVALID_ARGUMENT;
    }

    actual_data_size = std::min(data_buf_size, data_size - data_offset);

    if (validate) {
        // Calculate CRC on header (excluding CRC itself)
        crc = crc32(crc, sizeof(record_header_t) - sizeof(crc), &header);
        curr_data_offset = 0;
    } else {
        // Non validation case: No need to read the key, nor the parts before data_offset
        // or after the actual part requested by the user.
        total_size = actual_data_size;
        // Mark code that key handling is finished
        key_size = 0;
        curr_data_offset = data_offset;
    }

    user_key_ptr = key;
    hash = initial_crc;

    while (total_size) {
        uint8_t *dest_buf;
        uint32_t chunk_size;
        if (key_size) {
            // This means that we're on the key part
            if (copy_key) {
                dest_buf = reinterpret_cast<uint8_t *> (user_key_ptr);
                chunk_size = key_size;
                user_key_ptr[key_size] = '\0';
            } else {
                dest_buf = _work_buf;
                chunk_size = std::min(key_size, work_buf_size);
            }
        } else {
            // This means that we're on the data part
            // We have four cases that need different handling:
            // 1. Before data_offset - read to work buffer
            // 2. After data_offset, but before actual part is finished - read to user buffer
            // 3. After actual part is finished - read to work buffer
            // 4. Copy data flag not set - read to work buffer
            if (curr_data_offset < data_offset) {
                chunk_size = std::min(work_buf_size, data_offset - curr_data_offset);
                dest_buf = _work_buf;
            } else if (copy_data && (curr_data_offset < actual_data_size)) {
                chunk_size = actual_data_size - curr_data_offset;
                dest_buf = static_cast<uint8_t *> (data_buf);
            } else {
                chunk_size = std::min(work_buf_size, total_size);
                dest_buf = _work_buf;
            }
        }
        os_ret = read_area(area, offset, chunk_size, dest_buf);
        if (os_ret) {
            ret = MBED_ERROR_READ_FAILED;
            goto end;
        }

        if (validate) {
            // calculate CRC on current read chunk
            crc = crc32(crc, chunk_size, dest_buf);
        }

        if (key_size) {
            // We're on key part. May need to calculate hash or check whether key is the expected one
            if (check_expected_key) {
                if (memcmp(user_key_ptr, dest_buf, chunk_size)) {
                    ret = MBED_ERROR_ITEM_NOT_FOUND;
                }
            }

            if (calc_hash) {
                hash = crc32(hash, chunk_size, dest_buf);
            }

            user_key_ptr += chunk_size;
            key_size -= chunk_size;
        }

        total_size -= chunk_size;
        offset += chunk_size;
    }

    if (validate && (crc != header.crc)) {
        ret = MBED_ERROR_INVALID_DATA_DETECTED;
        goto end;
    }

    next_offset = align_up(offset, _prog_size);

end:
    return ret;
}

int TDBStore::find_record(uint8_t area, const char *key, uint32_t& offset,
                            uint32_t& ram_table_ind, uint32_t& hash)
{
    ram_table_entry_t *ram_table = (ram_table_entry_t *) _ram_table;
    ram_table_entry_t *entry;
    int ret = MBED_ERROR_ITEM_NOT_FOUND;
    uint32_t actual_data_size;
    uint32_t flags, dummy_hash, next_offset;


    hash = crc32(initial_crc, strlen(key), key);

    for (ram_table_ind = 0; ram_table_ind < _num_keys; ram_table_ind++) {
        entry = &ram_table[ram_table_ind];
        offset = entry->bd_offset;
        if (hash < entry->hash)  {
            continue;
        }
        if (hash > entry->hash)  {
            return MBED_ERROR_ITEM_NOT_FOUND;
        }
        ret = read_record(_active_area, offset, const_cast<char *> (key), 0, 0, actual_data_size, 0,
                            false, false, true, false, dummy_hash, flags, next_offset);
        // not found return code here means that hash doesn't belong to name. Continue searching.
        if (ret != MBED_ERROR_ITEM_NOT_FOUND) {
            break;
        }
    }

    return ret;
}

uint32_t TDBStore::record_size(const char *key, uint32_t data_size)
{
    return align_up(sizeof(record_header_t), _prog_size) +
            align_up(strlen(key) + data_size, _prog_size);
}


int TDBStore::set_start(set_handle_t *handle, const char *key, size_t final_data_size,
                        uint32_t create_flags)
{
    int os_ret, ret;
    uint32_t offset;
    uint32_t hash, ram_table_ind;
    inc_set_handle_t *ih;

    *handle = reinterpret_cast<set_handle_t> (_inc_set_handle);
    ih = reinterpret_cast<inc_set_handle_t *> (*handle);

    if (!strcmp(key, master_rec_key)) {
        // Master record - special case
        ih->bd_base_offset = _master_record_offset;
        ih->new_key = false;
    } else {

        _mutex.lock();

        // A valid magic in the header means that this function has been called after an aborted
        // incremental set process. This means that our media may be in a bad state - call GC.
        if (ih->header.magic) {
            ret = garbage_collection();
            if (ret) {
                goto fail;
            }
        }

        // If we have no room for the record, perform garbage collection
        uint32_t rec_size = record_size(key, final_data_size);
        if (_free_space_offset + rec_size > _size) {
            ret = garbage_collection();
            if (ret) {
                goto fail;
            }
        }

        // If even after GC we have no room for the record, return error
        if (_free_space_offset + rec_size > _size) {
            ret = MBED_ERROR_MEDIA_FULL;
            goto fail;
        }

        ret = find_record(_active_area, key, offset, ram_table_ind, hash);

        if (ret == MBED_SUCCESS) {
            os_ret = read_area(_active_area, offset, sizeof(ih->header), &ih->header);
            if (os_ret) {
                ret = MBED_ERROR_READ_FAILED;
                goto fail;
            }
            if (ih->header.flags & WRITE_ONCE_FLAG) {
                ret = MBED_ERROR_WRITE_PROTECTED;
                goto fail;
            }
            ih->new_key = false;
        } else if (ret == MBED_ERROR_ITEM_NOT_FOUND) {
            if (create_flags & delete_flag) {
                goto fail;
            }
            if (_num_keys >= _max_keys) {
                increment_max_keys();
            }
            ih->new_key = true;
        } else {
            goto fail;
        }
        ih->bd_base_offset = _free_space_offset;

        check_erase_before_write(_active_area, ih->bd_base_offset, rec_size);
    }

    ret = MBED_SUCCESS;

    // Fill handle and header fields
    // Jump to offset after header (header will be written at finalize phase)
    ih->bd_curr_offset = ih->bd_base_offset + align_up(sizeof(record_header_t), _prog_size);
    ih->offset_in_data = 0;
    ih->hash = hash;
    ih->ram_table_ind = ram_table_ind;
    ih->header.magic = tdbstore_magic;
    ih->header.header_size = sizeof(record_header_t);
    ih->header.revision = tdbstore_revision;
    ih->header.flags = create_flags;
    ih->header.key_size = strlen(key);
    ih->header.reserved = 0;
    ih->header.data_size = final_data_size;
    // Calculate CRC on header and key
    ih->header.crc = crc32(initial_crc, sizeof(record_header_t) - sizeof(ih->header.crc), &ih->header);
    ih->header.crc = crc32(ih->header.crc, ih->header.key_size, key);

    // Write key now
    os_ret = write_area(_active_area, ih->bd_curr_offset, ih->header.key_size, key);
    if (os_ret) {
        ret = MBED_ERROR_WRITE_FAILED;
        goto fail;
    }
    ih->bd_curr_offset += ih->header.key_size;
    goto end;

fail:
    // mark handle as invalid by clearing magic field in header
    ih->header.magic = 0;
    _mutex.unlock();

end:
    return ret;
}

int TDBStore::set_add_data(set_handle_t handle, const void *value_data, size_t data_size)
{
    int os_ret, ret = MBED_SUCCESS;
    inc_set_handle_t *ih;

    if (handle != _inc_set_handle) {
        return MBED_ERROR_INVALID_ARGUMENT;
    }

    ih = reinterpret_cast<inc_set_handle_t *> (handle);

    if (!ih->header.magic) {
        return MBED_ERROR_INVALID_ARGUMENT;
    }

    if (ih->offset_in_data + data_size > ih->header.data_size) {
        ret = MBED_ERROR_INVALID_SIZE;
        // Need GC as otherwise our storage is kept in a non-usable state
        garbage_collection();
        goto fail;
    }

    // Update CRC with data chunk
    ih->header.crc = crc32(ih->header.crc, data_size, value_data);

    // Write data chunk
    os_ret = write_area(_active_area, ih->bd_curr_offset, data_size, value_data);
    if (os_ret) {
        ret = MBED_ERROR_WRITE_FAILED;
        goto fail;
    }
    ih->bd_curr_offset += data_size;
    ih->offset_in_data += data_size;
    goto end;

fail:
    // mark handle as invalid by clearing magic field in header
    ih->header.magic = 0;
    _mutex.unlock();

end:
    return ret;
}

int TDBStore::set_finalize(set_handle_t handle)
{
    int os_ret, ret = MBED_SUCCESS;
    inc_set_handle_t *ih;
    ram_table_entry_t *ram_table = (ram_table_entry_t *) _ram_table;
    ram_table_entry_t *entry;

    if (handle != _inc_set_handle) {
        return MBED_ERROR_INVALID_ARGUMENT;
    }

    ih = reinterpret_cast<inc_set_handle_t *> (handle);

    if (!ih->header.magic) {
        return MBED_ERROR_INVALID_ARGUMENT;
    }

    if (ih->offset_in_data != ih->header.data_size) {
        ret = MBED_ERROR_INVALID_SIZE;
        // Need GC as otherwise our storage is left in a non-usable state
        garbage_collection();
        goto end;
    }

    // Write header
    os_ret = write_area(_active_area, ih->bd_base_offset, sizeof(record_header_t), &ih->header);
    if (os_ret) {
        ret = MBED_ERROR_WRITE_FAILED;
        goto end;
    }

    // Need to flush buffered BD as our record is totally written now
    _buff_bd->sync();

    // In master record case we don't update RAM table
    if (ih->bd_base_offset == _master_record_offset) {
        goto end;
    }

    // Update RAM table
    if (ih->header.flags & delete_flag) {
        _num_keys--;
        if (ih->ram_table_ind < _num_keys) {
            memmove(&ram_table[ih->ram_table_ind], &ram_table[ih->ram_table_ind + 1],
                    sizeof(ram_table_entry_t) * (_num_keys - ih->ram_table_ind));
        }
    } else {
        if (ih->new_key) {
            if (ih->ram_table_ind < _num_keys) {
                memmove(&ram_table[ih->ram_table_ind + 1], &ram_table[ih->ram_table_ind],
                        sizeof(ram_table_entry_t) * (_num_keys - ih->ram_table_ind));
            }
            _num_keys++;
        }
        entry = &ram_table[ih->ram_table_ind];
        entry->hash = ih->hash;
        entry->bd_offset = ih->bd_base_offset;
    }

    _free_space_offset = align_up(ih->bd_curr_offset, _prog_size);

end:
    // mark handle as invalid by clearing magic field in header
    ih->header.magic = 0;

    if (ih->bd_base_offset != _master_record_offset) {
        _mutex.unlock();
    }
    return ret;
}

int TDBStore::set(const char *key, const void *buffer, size_t size, uint32_t create_flags)
{
    int ret;
    set_handle_t handle;

    ret = set_start(&handle, key, size, create_flags);
    if (ret) {
        return ret;
    }

    ret = set_add_data(handle, buffer, size);
    if (ret) {
        return ret;
    }

    ret = set_finalize(handle);
    return ret;
}

int TDBStore::remove(const char *key)
{
    return set(key, 0, 0, delete_flag);
}

int TDBStore::get(const char *key, void *buffer, size_t buffer_size, size_t *actual_size, size_t offset)
{
    int ret;
    uint32_t actual_data_size;
    uint32_t bd_offset, next_bd_offset;
    uint32_t flags, hash, ram_table_ind;

    _mutex.lock();

    ret = find_record(_active_area, key, bd_offset, ram_table_ind, hash);

    if (ret != MBED_SUCCESS) {
        goto end;
    }

    ret = read_record(_active_area, bd_offset, const_cast<char *>(key), buffer, buffer_size,
                        actual_data_size, offset, false, true, false, false, hash, flags, next_bd_offset);

    if (actual_size) {
        *actual_size = actual_data_size;
    }

end:
    _mutex.unlock();
    return ret;
}

int TDBStore::get_info(const char *key, info_t *info)
{
    int ret;
    uint32_t bd_offset, next_bd_offset;
    uint32_t flags, hash, ram_table_ind;
    uint32_t actual_data_size;

    _mutex.lock();

    ret = find_record(_active_area, key, bd_offset, ram_table_ind, hash);

    if (ret != MBED_SUCCESS) {
        goto end;
    }

    // Give a large dummy buffer size in order to achieve actual data size
    // (as copy_data flag is not set, data won't be copied anywhere)
    ret = read_record(_active_area, bd_offset, const_cast<char *>(key), 0, (uint32_t) -1,
                        actual_data_size, 0, false, false, false, false, hash, flags,
                        next_bd_offset);

    if (ret != MBED_SUCCESS) {
        goto end;
    }

    info->flags = flags;
    info->size = actual_data_size;

end:
    _mutex.unlock();
    return ret;
}

int TDBStore::write_master_record(uint8_t area, uint16_t version, uint32_t& next_offset)
{
    master_record_data_t master_rec;

    master_rec.version = version;
    master_rec.tdbstore_revision = tdbstore_revision;
    master_rec.reserved = 0;
    next_offset = _master_record_offset + record_size(master_rec_key, sizeof(master_rec));
    return set(master_rec_key, &master_rec, sizeof(master_rec), 0);
}

int TDBStore::copy_record(uint8_t from_area, uint32_t from_offset, uint32_t to_offset,
                             uint32_t& to_next_offset)
{
    int os_ret, ret;
    record_header_t header;
    uint32_t total_size;
    uint16_t chunk_size;

    os_ret = read_area(from_area, from_offset, sizeof(header), &header);
    if (os_ret) {
        return MBED_ERROR_READ_FAILED;
    }

    total_size = align_up(sizeof(record_header_t), _prog_size) +
                    align_up(header.key_size + header.data_size, _prog_size);;


    ret = check_erase_before_write(1 - from_area, to_offset, total_size);
    if (ret) {
        return ret;
    }

    chunk_size = align_up(sizeof(record_header_t), _prog_size);
    os_ret = write_area(1 - from_area, to_offset, chunk_size, &header);
    if (os_ret) {
        return MBED_ERROR_WRITE_FAILED;
    }

    from_offset += chunk_size;
    to_offset += chunk_size;
    total_size -= chunk_size;

    while (total_size) {
        chunk_size = std::min(total_size, work_buf_size);
        os_ret = read_area(from_area, from_offset, chunk_size, _work_buf);
        if (os_ret) {
            return MBED_ERROR_READ_FAILED;
        }

        os_ret = write_area(1 - from_area, to_offset, chunk_size, _work_buf);
        if (os_ret) {
            return MBED_ERROR_WRITE_FAILED;
        }

        from_offset += chunk_size;
        to_offset += chunk_size;
        total_size -= chunk_size;
    }

    to_next_offset = align_up(to_offset, _prog_size);
    return MBED_SUCCESS;
}

int TDBStore::copy_all_records(uint8_t from_area, uint32_t to_offset,
                                  uint32_t& to_next_offset)
{
    return MBED_SUCCESS;
}

int TDBStore::garbage_collection()
{
    ram_table_entry_t *ram_table = (ram_table_entry_t *) _ram_table;
    uint32_t to_offset, to_next_offset;
    uint32_t chunk_size, reserved_size;
    int ret, os_ret;
    size_t ind;

    ret = check_erase_before_write(1 - _active_area, 0, _master_record_offset);
    if (ret) {
        return ret;
    }

    // Copy reserved data
    to_offset = 0;
    reserved_size = RESERVED_AREA_SIZE;

    while (reserved_size) {
        chunk_size = std::min(work_buf_size, reserved_size);
        os_ret = read_area(_active_area, to_offset, chunk_size, _work_buf + to_offset);
        if (os_ret) {
            return MBED_ERROR_READ_FAILED;
        }
        os_ret = read_area(1 - _active_area, to_offset, chunk_size, _work_buf + to_offset);
        if (os_ret) {
            return MBED_ERROR_WRITE_FAILED;
        }
        to_offset += chunk_size;
        reserved_size -= chunk_size;
    }


    to_offset = _master_record_offset + record_size(master_rec_key, sizeof(master_record_data_t));

    // Initialize in case table is empty
    to_next_offset = to_offset;

    // Go over ram table and copy all entries to opposite area
    for (ind = 0; ind < _num_keys; ind++) {
        uint32_t from_offset = ram_table[ind].bd_offset;
        ret = copy_record(_active_area, from_offset, to_offset, to_next_offset);
        if (ret != MBED_SUCCESS) {
            return ret;
        }
        // Update RAM table
        ram_table[ind].bd_offset = to_offset;
        to_offset = to_next_offset;
    }

    to_offset = to_next_offset;
    _free_space_offset = to_next_offset;

    // Now we can switch to the new active area
    _active_area = 1 - _active_area;

    // Now write master record, with version incremented by 1.
    _active_area_version++;
    ret = write_master_record(_active_area, _active_area_version, to_offset);
    if (ret) {
        return ret;
    }

    // Now reset standby area
    os_ret = reset_area(1 - _active_area);
    if (os_ret) {
        return MBED_ERROR_WRITE_FAILED;
    }

    return 0;
}


int TDBStore::build_ram_table()
{
    ram_table_entry_t *ram_table = (ram_table_entry_t *) _ram_table;
    uint32_t offset, next_offset = 0, dummy;
    int ret = MBED_SUCCESS;
    uint32_t hash;
    uint32_t flags;
    uint32_t actual_data_size;
    uint32_t ram_table_ind;

    _num_keys = 0;
    offset = _master_record_offset;

    while (offset < _free_space_offset) {
        ret = read_record(_active_area, offset, _key_buf, 0, 0, actual_data_size, 0,
                          true, false, false, true, hash, flags, next_offset);

        if (ret != MBED_SUCCESS) {
            goto end;
        }

        ret = find_record(_active_area, _key_buf, dummy, ram_table_ind, hash);

        if ((ret != MBED_SUCCESS) && (ret != MBED_ERROR_ITEM_NOT_FOUND)) {
            goto end;
        }

        uint32_t save_offset = offset;
        offset = next_offset;

        if (ret == MBED_ERROR_ITEM_NOT_FOUND) {
            // Key doesn't exist, need to add it to RAM table

            if (flags & delete_flag) {
                 continue;
            }
            if (_num_keys >= _max_keys) {
                // In order to avoid numerous reallocations of ram table,
                // Add a chunk of entries now
                increment_max_keys(reinterpret_cast<void **>(&ram_table));
            }
            memmove(&ram_table[ram_table_ind + 1], &ram_table[ram_table_ind],
                    sizeof(ram_table_entry_t) * (_num_keys - ram_table_ind));

            _num_keys++;
        } else if (flags & delete_flag) {
            _num_keys--;
            memmove(&ram_table[ram_table_ind], &ram_table[ram_table_ind + 1],
                    sizeof(ram_table_entry_t) * (_num_keys - ram_table_ind));

            continue;
        }

        // update record parameters
        ram_table[ram_table_ind].hash = hash;
        ram_table[ram_table_ind].bd_offset = save_offset;
    }

end:
    _free_space_offset = next_offset;
    return ret;
}

int TDBStore::increment_max_keys(void **ram_table)
{
    // Reallocate ram table with new size
    ram_table_entry_t *old_ram_table = (ram_table_entry_t *) _ram_table;
    ram_table_entry_t *new_ram_table = new ram_table_entry_t[_max_keys + 1];

    // Copy old content to new table
    memcpy(new_ram_table, old_ram_table, sizeof(ram_table_entry_t) * _max_keys);
    _max_keys++;

    _ram_table = new_ram_table;
    delete[] old_ram_table;

    if (ram_table) {
        *ram_table = _ram_table;
    }
    return MBED_SUCCESS;
}


int TDBStore::init()
{
    ram_table_entry_t *ram_table;
    area_state_e area_state[_num_areas];
    uint32_t next_offset;
    uint32_t flags, hash;
    uint32_t actual_data_size;
    int os_ret;
    int ret = MBED_SUCCESS;
    uint16_t versions[_num_areas];

    _mutex.lock();

    _max_keys = initial_max_keys;

    ram_table = new ram_table_entry_t[_max_keys];
    _ram_table = ram_table;
    _num_keys = 0;

    // Underlying BD size must fit into 32 bits
    MBED_ASSERT((uint32_t)_bd->size() == _bd->size());

    // Underlying BD must have flash attributes, i.e. have an erase value
    MBED_ASSERT(_bd->get_erase_value() != -1);

    _size = (size_t) -1;

    _buff_bd = new BufferedBlockDevice(_bd);
    _buff_bd->init();

    _prog_size = _bd->get_program_size();
    _work_buf = new uint8_t[work_buf_size];
    _key_buf = new char[MAX_KEY_SIZE];
    _inc_set_handle = new inc_set_handle_t;
    memset(_inc_set_handle, 0, sizeof(inc_set_handle_t));

    _master_record_offset = align_up(RESERVED_AREA_SIZE, _prog_size);

    calc_area_params();

    for (uint8_t area = 0; area < _num_areas; area++) {
        area_state[area] = TDBSTORE_AREA_STATE_NONE;
        versions[area] = 0;

        _size = std::min(_size, _area_params[area].size);

        // Check validity of master record
        master_record_data_t master_rec;
        ret = read_record(area, _master_record_offset, const_cast<char *> (master_rec_key),
                            &master_rec, sizeof(master_rec), actual_data_size, 0, false, true, true, false,
                            hash, flags, next_offset);
        MBED_ASSERT((ret == MBED_SUCCESS) || (ret == MBED_ERROR_INVALID_DATA_DETECTED));

        // Master record may be corrupt, but it can be erased. Now check if its entire erase unit is erased
        if (ret == MBED_ERROR_INVALID_DATA_DETECTED) {
            bool erased;
            os_ret = is_erase_unit_erased(area, _master_record_offset / _bd->get_erase_size(_master_record_offset),
                                            erased);
            MBED_ASSERT(!os_ret);
            if (erased) {
                area_state[area] = TDBSTORE_AREA_STATE_EMPTY;
                continue;
            }
        }

        // We have a non valid master record, just reset the area.
        if (ret == MBED_ERROR_INVALID_DATA_DETECTED) {
            os_ret = reset_area(area);
            MBED_ASSERT(!os_ret);
            area_state[area] = TDBSTORE_AREA_STATE_EMPTY;
            continue;
        }
        versions[area] = master_rec.version;

        area_state[area] = TDBSTORE_AREA_STATE_VALID;

        // Unless both areas are valid (a case handled later), getting here means
        // that we found our active area.
        _active_area = area;
        _active_area_version = versions[area];
    }

    // In case we have two empty areas, arbitrarily use area 0 as the active one.
    if ((area_state[0] == TDBSTORE_AREA_STATE_EMPTY) && (area_state[1] == TDBSTORE_AREA_STATE_EMPTY)) {
        _active_area = 0;
        _active_area_version = 1;
        ret = write_master_record(_active_area, _active_area_version, _free_space_offset);
        MBED_ASSERT(ret == MBED_SUCCESS);
        // Nothing more to do here if active area is empty
        goto end;
    }

    // In case we have two valid areas, choose the one having the higher version (or 0
    // in case of wrap around). Erase the other one.
    if ((area_state[0] == TDBSTORE_AREA_STATE_VALID) && (area_state[1] == TDBSTORE_AREA_STATE_VALID)) {
        if ((versions[0] > versions[1]) || (!versions[0])) {
            _active_area = 0;
        } else {
            _active_area = 1;
        }
        _active_area_version = versions[_active_area];
        os_ret = erase_erase_unit(1 - _active_area, 0);
        MBED_ASSERT(!os_ret);
    }

    // Currently set free space offset pointer to the end of free space.
    // Ram table build process needs it, but will update it.
    _free_space_offset = _size;
    ret = build_ram_table();

    MBED_ASSERT((ret == MBED_SUCCESS) || (ret == MBED_ERROR_INVALID_DATA_DETECTED));

    if ((ret == MBED_ERROR_INVALID_DATA_DETECTED) && (_free_space_offset < _size)) {
        // Space after last valid record may be erased, hence "corrupt". Now check if it really is erased.
        bool erased;
        os_ret = is_erase_unit_erased(_active_area, _free_space_offset, erased);
        MBED_ASSERT(!os_ret);
        if (erased) {
            // Erased - all good
            ret = MBED_SUCCESS;
        }
    }

    // If we have a corrupt record somewhere, perform garbage collection to salvage
    // all preceding records
    if (ret == MBED_ERROR_INVALID_DATA_DETECTED) {
        ret = garbage_collection();
        MBED_ASSERT(ret == MBED_SUCCESS);
        _buff_bd->sync();
    }

end:
    _is_initialized = true;
    _mutex.unlock();
    return ret;
}

int TDBStore::deinit()
{
    _mutex.lock();
    if (_is_initialized) {
        _buff_bd->deinit();
        delete _buff_bd;

        ram_table_entry_t *ram_table = (ram_table_entry_t *) _ram_table;
        delete[] ram_table;
        delete[] _work_buf;
        delete[] _key_buf;
    }

    _is_initialized = false;
    _mutex.unlock();

    return MBED_SUCCESS;
}

int TDBStore::reset_area(uint8_t area)
{
    int os_ret;
    uint32_t bd_offset = 0;

    // Erase reserved area and master record
    do {
        os_ret = erase_erase_unit(area, bd_offset);
        if (os_ret) {
            return MBED_ERROR_WRITE_FAILED;
        }
        bd_offset += _buff_bd->get_erase_size(bd_offset);
    } while (bd_offset <= _master_record_offset);

    return 0;
}

int TDBStore::reset()
{
    uint8_t area;
    int os_ret, ret;

    if (!_is_initialized) {
        return MBED_ERROR_NOT_READY;
    }

    _mutex.lock();

    // Reset both areas
    for (area = 0; area < _num_areas; area++) {
        os_ret = reset_area(area);
        if (os_ret) {
            goto end;
        }
    }

    _active_area = 0;
    _num_keys = 0;
    _free_space_offset = _master_record_offset;
    _active_area_version = 1;

    // Write an initial master record on active area
    ret = write_master_record(_active_area, _active_area_version, _free_space_offset);
    if (ret) {
        goto end;
    }

end:
    _mutex.unlock();
    return ret;
}

int TDBStore::iterator_open(iterator_t *it, const char *prefix)
{
    key_iterator_handle_t *handle;

    if (!_is_initialized) {
        return MBED_ERROR_NOT_READY;
    }

    _mutex.lock();

    handle = new key_iterator_handle_t;
    *it = reinterpret_cast<iterator_t>(handle);

    if (prefix && strcmp(prefix, "")) {
        handle->prefix = strdup(prefix);
    } else {
        handle->prefix = 0;
    }
    handle->ram_table_ind = 0;

    _mutex.unlock();

    return MBED_SUCCESS;
}

int TDBStore::iterator_next(iterator_t it, char *key, size_t key_size)
{
    ram_table_entry_t *ram_table = (ram_table_entry_t *) _ram_table;
    key_iterator_handle_t *handle;
    int ret;
    uint32_t actual_data_size, hash, flags, next_offset;

    if (!_is_initialized) {
        return MBED_ERROR_NOT_READY;
    }

    _mutex.lock();

    handle = reinterpret_cast<key_iterator_handle_t *>(it);

    ret = MBED_ERROR_ITEM_NOT_FOUND;

    while (ret && (handle->ram_table_ind < _num_keys)) {
        ret = read_record(_active_area, ram_table[handle->ram_table_ind].bd_offset, _key_buf,
                            0, 0, actual_data_size, 0, true, false, false, false, hash, flags, next_offset);
        if (ret) {
            goto end;
        }
        if (!handle->prefix || (strstr(_key_buf, handle->prefix) == _key_buf)) {
            if (strlen(_key_buf) >= key_size) {
                ret = MBED_ERROR_INVALID_SIZE;
                goto end;
            }
            strcpy(key, _key_buf);
        } else {
            ret = MBED_ERROR_ITEM_NOT_FOUND;
        }
        handle->ram_table_ind++;
    }

end:
    _mutex.unlock();
    return ret;
}

int TDBStore::iterator_close(iterator_t it)
{
    key_iterator_handle_t *handle;

    if (!_is_initialized) {
        return MBED_ERROR_NOT_READY;
    }

    _mutex.lock();

    handle = reinterpret_cast<key_iterator_handle_t *>(it);
    delete handle->prefix;
    delete handle;

    _mutex.unlock();

    return MBED_SUCCESS;
}

int TDBStore::reserved_data_set(const void *reserved_data, size_t reserved_data_buf_size)
{
    uint32_t check_size = RESERVED_AREA_SIZE, chunk_size, offset = 0;
    uint8_t blank = _buff_bd->get_erase_value();
    int os_ret, ret = MBED_SUCCESS;

    if (reserved_data_buf_size > RESERVED_AREA_SIZE) {
        return MBED_ERROR_INVALID_SIZE;
    }

    _mutex.lock();

    while (check_size) {
        chunk_size = std::min(work_buf_size, (uint32_t) check_size);
        os_ret = read_area(_active_area, offset, chunk_size, _work_buf + offset);
        if (os_ret) {
            ret = MBED_ERROR_READ_FAILED;
            goto end;
        }
        for (uint32_t i = 0; i < chunk_size; i++) {
            if (_work_buf[i] != blank) {
                ret = MBED_ERROR_WRITE_FAILED;
                goto end;
            }
        }
        offset += chunk_size;
        check_size -= chunk_size;
    }

    os_ret = write_area(_active_area, 0, reserved_data_buf_size, reserved_data);
    if (os_ret) {
        ret = MBED_ERROR_WRITE_FAILED;
        goto end;
    }

    ret = _buff_bd->sync();

end:
    _mutex.unlock();
    return ret;
}

int TDBStore::reserved_data_get(void *reserved_data, size_t reserved_data_buf_size)
{
    int ret = MBED_SUCCESS;

    _mutex.lock();
    if (reserved_data_buf_size > RESERVED_AREA_SIZE) {
        reserved_data_buf_size = RESERVED_AREA_SIZE;
    }

    int os_ret = read_area(_active_area, 0, reserved_data_buf_size, reserved_data);
    if (os_ret) {
        ret = MBED_ERROR_READ_FAILED;
    }

    _mutex.unlock();
    return ret;
}


void TDBStore::offset_in_erase_unit(uint8_t area, uint32_t offset,
                                       uint32_t& offset_from_start, uint32_t& dist_to_end)
{
    uint32_t bd_offset = _area_params[area].address + offset;
    if (!_variant_bd_erase_unit_size) {
        uint32_t eu_size = _bd->get_erase_size();
        offset_from_start = bd_offset % eu_size;
        dist_to_end = eu_size - offset_from_start;
        return;
    }

    uint32_t agg_offset = 0;
    while (bd_offset < agg_offset + _bd->get_erase_size(agg_offset)) {
        agg_offset += _bd->get_erase_size(agg_offset);
    }
    offset_from_start = bd_offset - agg_offset;
    dist_to_end = _bd->get_erase_size(agg_offset) - offset_from_start;

}

int TDBStore::is_erase_unit_erased(uint8_t area, uint32_t offset, bool& erased)
{
    uint32_t offset_from_start, dist;
    offset_in_erase_unit(area, offset, offset_from_start, dist);
    uint8_t buf[sizeof(record_header_t)], blanks[sizeof(record_header_t)];
    memset(blanks, _bd->get_erase_value(), sizeof(blanks));

    while (dist) {
        uint32_t chunk = std::min(dist, (uint32_t) sizeof(buf));
        int ret = read_area(area, offset, chunk, buf);
        if (ret) {
            return MBED_ERROR_READ_FAILED;
        }
        if (memcmp(buf, blanks, chunk)) {
            erased = false;
            return MBED_SUCCESS;
        }
        offset += chunk;
        dist -= chunk;
    }
    erased = true;
    return MBED_SUCCESS;
}

int TDBStore::check_erase_before_write(uint8_t area, uint32_t offset, uint32_t size)
{
    // In order to save init time, we don't check that the entire area is erased.
    // Instead, whenever reaching an erase unit start, check that it's erased, and if not -
    // erase it. This is very not likely to happen (assuming area was initialized
    // by TDBStore). This can be achieved as all records (except for the master record
    // in offset 0) are written in an ascending order.

    if (!offset) {
        // Master record in offset 0 is a special case - don't check it
        return MBED_SUCCESS;
    }

    while (size) {
        uint32_t dist, offset_from_start;
        int ret;
        offset_in_erase_unit(area, offset, offset_from_start, dist);
        uint32_t chunk = std::min(size, dist);

        if (!offset_from_start) {
            // We're at the start of an erase unit. Here (and only here), check if it's erased.
            bool erased;
            ret = is_erase_unit_erased(area, offset, erased);
            if (ret) {
                return MBED_ERROR_WRITE_FAILED;
            }
            if (!erased) {
                ret = erase_erase_unit(area, offset);
                if (ret) {
                    return MBED_ERROR_WRITE_FAILED;
                }
            }
        }
        offset += chunk;
        size -= chunk;
    }
    return MBED_SUCCESS;
}

