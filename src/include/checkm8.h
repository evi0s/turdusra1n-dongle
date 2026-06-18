#ifndef TETHERED_BOOTER_CHECKM8_H
#define TETHERED_BOOTER_CHECKM8_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CHECKM8_STAGE_RESET = 0,
    CHECKM8_STAGE_SETUP,
    CHECKM8_STAGE_SPRAY,
    CHECKM8_STAGE_PATCH,
    CHECKM8_STAGE_PWND,
} checkm8_stage_t;

typedef struct {
    bool supported;
    bool pwned;
    uint16_t cpid;
    uint64_t ecid;
    char serial[160];
} checkm8_probe_t;

void checkm8_init(void);
void checkm8_reset_state(void);
checkm8_stage_t checkm8_current_stage(void);
uint16_t checkm8_current_cpid(void);
bool checkm8_probe_device(uint8_t daddr, checkm8_probe_t *probe);
bool checkm8_run_current_stage(uint8_t daddr);
char const *checkm8_stage_name(checkm8_stage_t stage);

#endif
