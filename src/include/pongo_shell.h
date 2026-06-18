#ifndef TETHERED_BOOTER_PONGO_SHELL_H
#define TETHERED_BOOTER_PONGO_SHELL_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PONGO_BOOT_OK = 0,
    PONGO_BOOT_NO_PTEBLOCK,
    PONGO_BOOT_XFER_FAIL,
    PONGO_BOOT_CMD_FAIL,
    PONGO_BOOT_TIMEOUT,
} pongo_boot_result_t;

pongo_boot_result_t pongo_shell_tethered_boot(uint8_t daddr);

#endif
