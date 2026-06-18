#include "psram.h"

#include <string.h>

#include "app_log.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "sfe_psram.h"

#define PSRAM_CS_PIN 47

#define PSRAM_XIP_BASE (XIP_BASE + 0x1000000u)

#define PSRAM_OFFSET_SEP_RACER  0x00000000u
#define PSRAM_OFFSET_OVERLAY    0x00200000u
#define PSRAM_OFFSET_UNION      0x00300000u
#define PSRAM_OFFSET_KPF        0x00500000u
#define PSRAM_OFFSET_CPF        0x00520000u
#define PSRAM_OFFSET_PTEBLOCK   0x00540000u
#define PSRAM_OFFSET_FAT16      0x00541000u

#define FLASH_OFFSET_KPF        0x140000u
#define FLASH_OFFSET_CPF        0x160000u
#define FLASH_OFFSET_SEP_RACER  0x180000u
#define FLASH_OFFSET_OVERLAY    0x300000u
#define FLASH_OFFSET_UNION      0x400000u
#define FLASH_OFFSET_PTEBLOCK   0x600000u

#define RESOURCE_SIZE_SEP_RACER 1211608u
#define RESOURCE_SIZE_KPF       92248u
#define RESOURCE_SIZE_CPF       67672u
#define RESOURCE_SIZE_OVERLAY   1048576u
#define RESOURCE_SIZE_UNION     2097152u
#define PTEBLOCK_MAX_SIZE       4096u
#define FAT16_DISK_SIZE         65536u

#define PTEBLOCK_MAGIC          0x50544542u
#define PTEBLOCK_SLOT_SIZE      (FLASH_SECTOR_SIZE * 2)
#define PTEBLOCK_MAX_SLOTS      16u

#define XIP_BASE_ADDR           0x10000000u

typedef struct {
    uint8_t const *ptr;
    size_t len;
} resource_entry_t;

static resource_entry_t resources[RESOURCE_COUNT];
static size_t pteblock_len;
static uint64_t pteblock_ecid;
static bool psram_initialized;
static uint8_t pteblock_sram[PTEBLOCK_MAX_SIZE];

static uint8_t *psram_ptr(uint32_t offset) {
    return (uint8_t *)(uintptr_t)(PSRAM_XIP_BASE + offset);
}

static uint8_t const *flash_xip_ptr(uint32_t flash_offset) {
    return (uint8_t const *)(uintptr_t)(XIP_BASE_ADDR + flash_offset);
}

static uint32_t simple_checksum(uint8_t const *data, size_t len) {
    uint32_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
        sum = (sum << 1) | (sum >> 31);
    }
    return sum;
}

bool psram_init(void) {
    size_t sz = sfe_setup_psram(PSRAM_CS_PIN);
    if (sz == 0) {
        app_log_enqueue("psram: not detected\r\n");
        return false;
    }

    psram_initialized = true;
    app_logf("psram: init OK (%lu bytes)\r\n", (unsigned long)sz);
    return true;
}

bool psram_load_resources(void) {
    if (!psram_initialized) {
        return false;
    }

    struct {
        uint32_t flash_offset;
        uint32_t psram_offset;
        size_t size;
        resource_id_t id;
        char const *name;
    } load_table[] = {
        { FLASH_OFFSET_SEP_RACER, PSRAM_OFFSET_SEP_RACER, RESOURCE_SIZE_SEP_RACER, RESOURCE_SEP_RACER, "sep_racer" },
        { FLASH_OFFSET_KPF,       PSRAM_OFFSET_KPF,       RESOURCE_SIZE_KPF,       RESOURCE_KPF,       "kpf" },
        { FLASH_OFFSET_CPF,       PSRAM_OFFSET_CPF,       RESOURCE_SIZE_CPF,       RESOURCE_CPF,       "cpf" },
        { FLASH_OFFSET_OVERLAY,   PSRAM_OFFSET_OVERLAY,   RESOURCE_SIZE_OVERLAY,   RESOURCE_OVERLAY,   "overlay" },
        { FLASH_OFFSET_UNION,     PSRAM_OFFSET_UNION,     RESOURCE_SIZE_UNION,     RESOURCE_UNION,     "union" },
    };

    unsigned count = sizeof(load_table) / sizeof(load_table[0]);
    for (unsigned i = 0; i < count; i++) {
        uint8_t *dst = psram_ptr(load_table[i].psram_offset);
        uint8_t const *src = flash_xip_ptr(load_table[i].flash_offset);

        memcpy(dst, src, load_table[i].size);
        resources[load_table[i].id].ptr = dst;
        resources[load_table[i].id].len = load_table[i].size;

        app_status_set_progress((uint8_t)((i + 1) * 100 / count));
        app_logf("psram: loaded %s (%lu bytes)\r\n",
                 load_table[i].name, (unsigned long)load_table[i].size);
    }

    psram_pteblock_load_from_flash();
    return true;
}

static const struct {
    uint32_t flash_offset;
    size_t size;
} flash_fallback[RESOURCE_COUNT] = {
    [RESOURCE_SEP_RACER] = { FLASH_OFFSET_SEP_RACER, RESOURCE_SIZE_SEP_RACER },
    [RESOURCE_KPF]       = { FLASH_OFFSET_KPF,       RESOURCE_SIZE_KPF },
    [RESOURCE_CPF]       = { FLASH_OFFSET_CPF,       RESOURCE_SIZE_CPF },
    [RESOURCE_OVERLAY]   = { FLASH_OFFSET_OVERLAY,   RESOURCE_SIZE_OVERLAY },
    [RESOURCE_UNION]     = { FLASH_OFFSET_UNION,     RESOURCE_SIZE_UNION },
    [RESOURCE_PTEBLOCK]  = { 0, 0 },
};

bool psram_get_resource(resource_id_t id, uint8_t const **ptr, size_t *len) {
    if (id >= RESOURCE_COUNT) {
        return false;
    }
    if (id == RESOURCE_PTEBLOCK) {
        if (pteblock_len == 0) {
            return false;
        }
        if (ptr) *ptr = pteblock_sram;
        if (len) *len = pteblock_len;
        return true;
    }
    if (resources[id].ptr != NULL && resources[id].len != 0) {
        if (ptr) *ptr = resources[id].ptr;
        if (len) *len = resources[id].len;
        return true;
    }
    if (flash_fallback[id].size != 0) {
        if (ptr) *ptr = flash_xip_ptr(flash_fallback[id].flash_offset);
        if (len) *len = flash_fallback[id].size;
        return true;
    }
    return false;
}

bool psram_pteblock_valid(void) {
    return pteblock_len > 0 && pteblock_len <= PTEBLOCK_MAX_SIZE;
}

bool psram_pteblock_store(uint64_t ecid, uint8_t const *data, size_t len) {
    if (len == 0 || len > PTEBLOCK_MAX_SIZE) {
        return false;
    }

    memcpy(pteblock_sram, data, len);
    pteblock_len = len;
    pteblock_ecid = ecid;
    resources[RESOURCE_PTEBLOCK].ptr = pteblock_sram;
    resources[RESOURCE_PTEBLOCK].len = len;

    app_logf("psram: pteblock stored (%lu bytes)\r\n", (unsigned long)len);
    return true;
}

static int find_pteblock_slot(uint64_t ecid) {
    uint64_t key = ecid % 1000000;
    for (unsigned i = 0; i < PTEBLOCK_MAX_SLOTS; i++) {
        uint8_t const *slot = flash_xip_ptr(FLASH_OFFSET_PTEBLOCK + i * PTEBLOCK_SLOT_SIZE);
        uint32_t magic;
        uint64_t slot_ecid;
        memcpy(&magic, slot, 4);
        memcpy(&slot_ecid, slot + 4, 8);
        if (magic == PTEBLOCK_MAGIC && (slot_ecid % 1000000) == key) {
            return (int)i;
        }
    }
    return -1;
}

static int find_empty_slot(void) {
    for (unsigned i = 0; i < PTEBLOCK_MAX_SLOTS; i++) {
        uint8_t const *slot = flash_xip_ptr(FLASH_OFFSET_PTEBLOCK + i * PTEBLOCK_SLOT_SIZE);
        uint32_t magic;
        memcpy(&magic, slot, 4);
        if (magic != PTEBLOCK_MAGIC) {
            return (int)i;
        }
    }
    return -1;
}

bool psram_pteblock_select(uint64_t ecid) {
    int slot = find_pteblock_slot(ecid);
    if (slot < 0) {
        pteblock_len = 0;
        return false;
    }

    uint8_t const *slot_ptr = flash_xip_ptr(FLASH_OFFSET_PTEBLOCK + (unsigned)slot * PTEBLOCK_SLOT_SIZE);
    uint32_t len;
    memcpy(&len, slot_ptr + 12, 4);

    if (len == 0 || len > PTEBLOCK_MAX_SIZE) {
        pteblock_len = 0;
        return false;
    }

    uint8_t const *data_ptr = slot_ptr + 16;
    uint32_t stored_checksum;
    memcpy(&stored_checksum, data_ptr + len, 4);

    if (simple_checksum(data_ptr, len) != stored_checksum) {
        pteblock_len = 0;
        return false;
    }

    memcpy(pteblock_sram, data_ptr, len);
    pteblock_len = len;
    pteblock_ecid = ecid;
    resources[RESOURCE_PTEBLOCK].ptr = pteblock_sram;
    resources[RESOURCE_PTEBLOCK].len = len;

    app_logf("psram: pteblock selected for ECID %llx (%lu bytes)\r\n",
             (unsigned long long)ecid, (unsigned long)len);
    return true;
}

int psram_pteblock_count(void) {
    int count = 0;
    for (unsigned i = 0; i < PTEBLOCK_MAX_SLOTS; i++) {
        uint8_t const *slot = flash_xip_ptr(FLASH_OFFSET_PTEBLOCK + i * PTEBLOCK_SLOT_SIZE);
        uint32_t magic;
        memcpy(&magic, slot, 4);
        if (magic == PTEBLOCK_MAGIC) count++;
    }
    return count;
}

bool psram_pteblock_get_slot(int slot, uint64_t *ecid, uint8_t const **data, size_t *len) {
    if (slot < 0 || slot >= (int)PTEBLOCK_MAX_SLOTS) return false;
    uint8_t const *s = flash_xip_ptr(FLASH_OFFSET_PTEBLOCK + (unsigned)slot * PTEBLOCK_SLOT_SIZE);
    uint32_t magic;
    memcpy(&magic, s, 4);
    if (magic != PTEBLOCK_MAGIC) return false;
    uint64_t e; uint32_t l;
    memcpy(&e, s + 4, 8);
    memcpy(&l, s + 12, 4);
    if (l == 0 || l > PTEBLOCK_MAX_SIZE) return false;
    if (ecid) *ecid = e;
    if (data) *data = s + 16;
    if (len) *len = l;
    return true;
}

bool psram_pteblock_load_from_flash(void) {
    int count = psram_pteblock_count();
    if (count > 0) {
        app_logf("psram: %d pteblock(s) in flash\r\n", count);
    }
    pteblock_len = 0;
    return count > 0;
}

bool psram_pteblock_persist_to_flash(void) {
    if (pteblock_len == 0 || pteblock_len > PTEBLOCK_MAX_SIZE || pteblock_ecid == 0) {
        return false;
    }

    int slot = find_pteblock_slot(pteblock_ecid);
    if (slot < 0) {
        slot = find_empty_slot();
        if (slot < 0) {
            app_log_enqueue("psram: no empty pteblock slot\r\n");
            return false;
        }
    }

    static uint8_t flash_buf[PTEBLOCK_SLOT_SIZE];

    memset(flash_buf, 0xFF, sizeof(flash_buf));
    uint32_t magic = PTEBLOCK_MAGIC;
    uint32_t len32 = (uint32_t)pteblock_len;
    uint32_t checksum = simple_checksum(pteblock_sram, pteblock_len);
    memcpy(&flash_buf[0], &magic, 4);
    memcpy(&flash_buf[4], &pteblock_ecid, 8);
    memcpy(&flash_buf[12], &len32, 4);
    memcpy(&flash_buf[16], pteblock_sram, pteblock_len);
    memcpy(&flash_buf[16 + pteblock_len], &checksum, 4);

    uint32_t flash_offset = FLASH_OFFSET_PTEBLOCK + (unsigned)slot * PTEBLOCK_SLOT_SIZE;

    extern void core1_halt(void);
    extern void core1_resume(void);
    core1_halt();

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(flash_offset, PTEBLOCK_SLOT_SIZE);
    flash_range_program(flash_offset, flash_buf, PTEBLOCK_SLOT_SIZE);
    restore_interrupts(ints);

    core1_resume();

    app_logf("psram: pteblock persisted to flash slot %d for ECID %llx (%lu bytes)\r\n",
             slot, (unsigned long long)pteblock_ecid, (unsigned long)pteblock_len);
    return true;
}

uint8_t *psram_fat16_base(void) {
    if (!psram_initialized) {
        return NULL;
    }
    return psram_ptr(PSRAM_OFFSET_FAT16);
}
