#ifndef TETHERED_BOOTER_PSRAM_H
#define TETHERED_BOOTER_PSRAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RESOURCE_SEP_RACER = 0,
    RESOURCE_KPF,
    RESOURCE_CPF,
    RESOURCE_OVERLAY,
    RESOURCE_UNION,
    RESOURCE_PTEBLOCK,
    RESOURCE_COUNT,
} resource_id_t;

bool psram_init(void);
bool psram_load_resources(void);
bool psram_get_resource(resource_id_t id, uint8_t const **ptr, size_t *len);

bool psram_pteblock_valid(void);
bool psram_pteblock_store(uint64_t ecid, uint8_t const *data, size_t len);
bool psram_pteblock_select(uint64_t ecid);
bool psram_pteblock_load_from_flash(void);
bool psram_pteblock_persist_to_flash(void);
int psram_pteblock_count(void);
bool psram_pteblock_get_slot(int slot, uint64_t *ecid, uint8_t const **data, size_t *len);

uint8_t *psram_fat16_base(void);

#endif
