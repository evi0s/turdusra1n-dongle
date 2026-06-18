#ifndef TETHERED_BOOTER_APP_LOG_H
#define TETHERED_BOOTER_APP_LOG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef enum {
    APP_STAGE_BOOT = 0,
    APP_STAGE_WAIT_DFU,
    APP_STAGE_ENTER_DFU,
    APP_STAGE_DFU,
    APP_STAGE_RESET,
    APP_STAGE_SETUP,
    APP_STAGE_SPRAY,
    APP_STAGE_PATCH,
    APP_STAGE_PWND,
    APP_STAGE_PONGO,
    APP_STAGE_TBOOT,
    APP_STAGE_DEVICE,
    APP_STAGE_ERROR,
} app_stage_t;

typedef struct {
    app_stage_t stage;
    uint16_t cpid;
    uint64_t ecid;
    uint8_t progress;
    char line1[24];
    char line2[24];
} app_status_t;

void app_log_init(void);
void app_log_enqueue(char const *text);
void app_logf(char const *fmt, ...);
bool app_log_try_pop(char *dst, size_t dst_len);

void app_status_set(app_stage_t stage, uint16_t cpid, char const *line1, char const *line2);
void app_status_set_progress(uint8_t percent);
void app_status_set_ecid(uint64_t ecid);
void app_status_get(app_status_t *status);
char const *app_stage_name(app_stage_t stage);

#endif
