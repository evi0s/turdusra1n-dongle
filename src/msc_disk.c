#include "msc_disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_log.h"
#include "psram.h"
#include "tusb.h"

#define DISK_BLOCK_SIZE  512u
#define DISK_BLOCK_COUNT 128u
#define DISK_SIZE        (DISK_BLOCK_SIZE * DISK_BLOCK_COUNT)

#define FAT16_RESERVED_SECTORS 1u
#define FAT16_FAT_SECTORS      1u
#define FAT16_ROOT_DIR_ENTRIES  16u
#define FAT16_ROOT_DIR_SECTORS  1u
#define FAT16_DATA_START       (FAT16_RESERVED_SECTORS + FAT16_FAT_SECTORS + FAT16_ROOT_DIR_SECTORS)

static uint64_t parse_ecid_from_fat_name(uint8_t const *name);

static uint8_t disk_buf[DISK_SIZE];
static uint8_t *disk_base;
static volatile bool pteblock_written;
static volatile bool erase_requested;
static volatile bool ejected;

static void format_fat16(void) {
    memset(disk_buf, 0, DISK_SIZE);

    uint8_t *boot = &disk_buf[0];
    boot[0] = 0xEB; boot[1] = 0x3C; boot[2] = 0x90;
    memcpy(&boot[3], "MSDOS5.0", 8);
    boot[11] = (uint8_t)(DISK_BLOCK_SIZE & 0xFF);
    boot[12] = (uint8_t)(DISK_BLOCK_SIZE >> 8);
    boot[13] = 1;
    boot[14] = (uint8_t)(FAT16_RESERVED_SECTORS & 0xFF);
    boot[15] = (uint8_t)(FAT16_RESERVED_SECTORS >> 8);
    boot[16] = 1;
    boot[17] = (uint8_t)(FAT16_ROOT_DIR_ENTRIES & 0xFF);
    boot[18] = (uint8_t)(FAT16_ROOT_DIR_ENTRIES >> 8);
    boot[19] = (uint8_t)(DISK_BLOCK_COUNT & 0xFF);
    boot[20] = (uint8_t)(DISK_BLOCK_COUNT >> 8);
    boot[21] = 0xF8;
    boot[22] = (uint8_t)(FAT16_FAT_SECTORS & 0xFF);
    boot[23] = (uint8_t)(FAT16_FAT_SECTORS >> 8);
    boot[24] = 1; boot[25] = 0;
    boot[26] = 1; boot[27] = 0;
    boot[36] = 0x80;
    boot[38] = 0x29;
    boot[39] = 0x12; boot[40] = 0x34; boot[41] = 0x56; boot[42] = 0x78;
    memcpy(&boot[43], "TETHERED   ", 11);
    memcpy(&boot[54], "FAT16   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;

    uint8_t *fat = &disk_buf[FAT16_RESERVED_SECTORS * DISK_BLOCK_SIZE];
    fat[0] = 0xF8; fat[1] = 0xFF;
    fat[2] = 0xFF; fat[3] = 0xFF;

    uint8_t *root_dir = &disk_buf[(FAT16_RESERVED_SECTORS + FAT16_FAT_SECTORS) * DISK_BLOCK_SIZE];
    memcpy(&root_dir[0], "TETHERED   ", 11);
    root_dir[11] = 0x08;
}

void msc_disk_init(void) {
    format_fat16();

    uint8_t *root_dir = &disk_buf[(FAT16_RESERVED_SECTORS + FAT16_FAT_SECTORS) * DISK_BLOCK_SIZE];
    uint8_t *fat = &disk_buf[FAT16_RESERVED_SECTORS * DISK_BLOCK_SIZE];
    uint16_t next_cluster = 2;
    int dir_idx = 1;

    for (int slot = 0; slot < 16 && dir_idx < (int)FAT16_ROOT_DIR_ENTRIES; slot++) {
        uint64_t ecid;
        uint8_t const *data;
        size_t len;

        if (!psram_pteblock_get_slot(slot, &ecid, &data, &len)) continue;

        uint32_t data_offset = (FAT16_DATA_START + (next_cluster - 2)) * DISK_BLOCK_SIZE;
        if (data_offset + len > DISK_SIZE) break;

        char fname[9];
        snprintf(fname, sizeof(fname), "%06llu", (unsigned long long)(ecid % 1000000));

        uint8_t *entry = &root_dir[dir_idx * 32];
        memset(entry, ' ', 11);
        memcpy(entry, fname, strlen(fname));
        memcpy(entry + 8, "BIN", 3);
        entry[11] = 0x20;
        entry[26] = (uint8_t)(next_cluster & 0xFF);
        entry[27] = (uint8_t)(next_cluster >> 8);
        entry[28] = (uint8_t)(len & 0xFF);
        entry[29] = (uint8_t)((len >> 8) & 0xFF);
        entry[30] = (uint8_t)((len >> 16) & 0xFF);
        entry[31] = (uint8_t)((len >> 24) & 0xFF);

        uint16_t clusters_needed = (uint16_t)((len + DISK_BLOCK_SIZE - 1) / DISK_BLOCK_SIZE);
        for (uint16_t c = next_cluster; c < next_cluster + clusters_needed; c++) {
            uint16_t next = (c + 1 < next_cluster + clusters_needed) ? (c + 1) : 0xFFFF;
            fat[c * 2] = (uint8_t)(next & 0xFF);
            fat[c * 2 + 1] = (uint8_t)(next >> 8);
        }

        memcpy(&disk_buf[data_offset], data, len);
        next_cluster += clusters_needed;
        dir_idx++;

        app_logf("msc: showing %s.BIN (%lu bytes)\r\n", fname, (unsigned long)len);
    }

    __compiler_memory_barrier();
    disk_base = disk_buf;
    pteblock_written = false;
    erase_requested = false;
    ejected = false;
}

bool msc_disk_pteblock_written(void) {
    return pteblock_written;
}

void msc_disk_clear_written_flag(void) {
    pteblock_written = false;
}

void msc_disk_do_persist(void) {
    if (!pteblock_written || !disk_base) return;

    uint8_t *root_dir = &disk_base[(FAT16_RESERVED_SECTORS + FAT16_FAT_SECTORS) * DISK_BLOCK_SIZE];

    for (unsigned i = 0; i < FAT16_ROOT_DIR_ENTRIES; i++) {
        uint8_t *entry = &root_dir[i * 32];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) continue;
        if (entry[11] & 0x08) continue;
        if (entry[11] & 0x10) continue;

        if (memcmp(entry + 8, "BIN", 3) != 0) continue;

        uint64_t ecid = parse_ecid_from_fat_name(entry);
        if (ecid == 0) continue;

        uint32_t file_size = (uint32_t)entry[28] | ((uint32_t)entry[29] << 8) |
                             ((uint32_t)entry[30] << 16) | ((uint32_t)entry[31] << 24);
        uint16_t cluster = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);

        if (file_size == 0 || file_size > 4096 || cluster < 2) continue;

        uint32_t data_offset = (FAT16_DATA_START + (cluster - 2)) * DISK_BLOCK_SIZE;
        if (data_offset + file_size > DISK_SIZE) continue;

        uint8_t *file_data = &disk_base[data_offset];
        psram_pteblock_store(ecid, file_data, file_size);
        psram_pteblock_persist_to_flash();
        app_logf("msc: persisted ECID %llu (%lu bytes)\r\n",
                 (unsigned long long)ecid, (unsigned long)file_size);
    }

    pteblock_written = false;
}

bool msc_disk_erase_requested(void) {
    return erase_requested;
}

static uint64_t parse_ecid_from_fat_name(uint8_t const *name) {
    char digits[9];
    int len = 0;
    for (int i = 0; i < 8 && name[i] != ' '; i++) {
        uint8_t c = name[i];
        if (c >= '0' && c <= '9') {
            digits[len++] = (char)c;
        } else {
            return 0;
        }
    }
    digits[len] = 0;
    if (len < 1 || len > 6) return 0;
    return strtoull(digits, NULL, 10);
}

static void check_for_pteblock(void) {
    uint8_t *root_dir = &disk_base[(FAT16_RESERVED_SECTORS + FAT16_FAT_SECTORS) * DISK_BLOCK_SIZE];

    for (unsigned i = 0; i < FAT16_ROOT_DIR_ENTRIES; i++) {
        uint8_t *entry = &root_dir[i * 32];
        if (entry[0] == 0x00) break;
        if (entry[0] == 0xE5) continue;
        if (entry[11] & 0x08) continue;
        if (entry[11] & 0x10) continue;

        char name[12];
        memcpy(name, entry, 11);
        name[11] = 0;

        if (memcmp(name, "ERASE      ", 11) == 0 ||
            memcmp(name, "ERASE   BIN", 11) == 0) {
            erase_requested = true;
            app_log_enqueue("msc: ERASE file detected\r\n");
            return;
        }

        uint64_t ecid = 0;
        bool is_pteblock = false;

        if (memcmp(&name[8], "BIN", 3) == 0) {
            ecid = parse_ecid_from_fat_name(entry);
            if (ecid != 0) {
                is_pteblock = true;
            }
        }
        if (!is_pteblock && (memcmp(name, "PTEBLOCK   ", 11) == 0 ||
                             memcmp(name, "PTEBLOCKBIN", 11) == 0)) {
            is_pteblock = true;
            ecid = 0;
        }

        if (!is_pteblock) continue;

        uint32_t file_size = (uint32_t)entry[28] | ((uint32_t)entry[29] << 8) |
                             ((uint32_t)entry[30] << 16) | ((uint32_t)entry[31] << 24);
        uint16_t cluster = (uint16_t)entry[26] | ((uint16_t)entry[27] << 8);

        if (file_size == 0 || file_size > 4096 || cluster < 2) {
            continue;
        }

        uint32_t data_offset = (FAT16_DATA_START + (cluster - 2)) * DISK_BLOCK_SIZE;
        if (data_offset + file_size > DISK_SIZE) {
            continue;
        }

        uint8_t *file_data = &disk_base[data_offset];
        if (psram_pteblock_store(ecid, file_data, file_size)) {
            pteblock_written = true;
            app_logf("msc: pteblock for ECID %llu (%lu bytes)\r\n",
                     (unsigned long long)ecid, (unsigned long)file_size);
        }
    }
}

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "TURDUS  ", 8);
    memcpy(product_id, "TETHERED BOOTER ", 16);
    memcpy(product_rev, "1.00", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (ejected) return false;
    return disk_base != NULL;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = DISK_BLOCK_COUNT;
    *block_size = DISK_BLOCK_SIZE;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun;
    (void)power_condition;
    if (load_eject && !start) {
        check_for_pteblock();
        ejected = true;
    }
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!disk_base) {
        memset(buffer, 0, bufsize);
        return (int32_t)bufsize;
    }

    uint32_t addr = lba * DISK_BLOCK_SIZE + offset;
    if (addr + bufsize > DISK_SIZE) {
        memset(buffer, 0, bufsize);
        return (int32_t)bufsize;
    }

    memcpy(buffer, &disk_base[addr], bufsize);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (!disk_base) return (int32_t)bufsize;

    uint32_t addr = lba * DISK_BLOCK_SIZE + offset;
    if (addr + bufsize > DISK_SIZE) return (int32_t)bufsize;

    memcpy(&disk_base[addr], buffer, bufsize);
    return (int32_t)bufsize;
}

void tud_msc_write10_complete_cb(uint8_t lun) {
    (void)lun;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize) {
    (void)lun;
    (void)buffer;
    (void)bufsize;

    switch (scsi_cmd[0]) {
    case 0x1E:
        return 0;
    default:
        tud_msc_set_sense(lun, 0x05, 0x20, 0x00);
        return -1;
    }
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return true;
}
