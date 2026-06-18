#ifndef TETHERED_BOOTER_MSC_DISK_H
#define TETHERED_BOOTER_MSC_DISK_H

#include <stdbool.h>
#include <stdint.h>

void msc_disk_init(void);
bool msc_disk_pteblock_written(void);
void msc_disk_clear_written_flag(void);
void msc_disk_do_persist(void);
bool msc_disk_erase_requested(void);

#endif
