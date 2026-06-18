#include "app_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pico/sync.h"
#include "pico/util/queue.h"

typedef struct {
    char text[160];
} log_msg_t;

static queue_t log_queue;
static critical_section_t status_lock;
static app_status_t current_status;

static void copy_str(char *dst, size_t dst_len, char const *src) {
    if (dst_len == 0) {
        return;
    }

    if (!src) {
        dst[0] = 0;
        return;
    }

    size_t len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1;
    }

    memcpy(dst, src, len);
    dst[len] = 0;
}

void app_log_init(void) {
    queue_init(&log_queue, sizeof(log_msg_t), 48);
    critical_section_init(&status_lock);
    app_status_set(APP_STAGE_BOOT, 0, "BOOT", "USB HOST INIT");
}

void app_log_enqueue(char const *text) {
    log_msg_t msg;

    copy_str(msg.text, sizeof(msg.text), text);
    queue_try_add(&log_queue, &msg);
    printf("%s", text);
}

void app_logf(char const *fmt, ...) {
    log_msg_t msg;
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg.text, sizeof(msg.text), fmt, ap);
    va_end(ap);

    queue_try_add(&log_queue, &msg);
    printf("%s", msg.text);
}

bool app_log_try_pop(char *dst, size_t dst_len) {
    log_msg_t msg;

    if (!queue_try_remove(&log_queue, &msg)) {
        return false;
    }

    copy_str(dst, dst_len, msg.text);
    return true;
}

void app_status_set(app_stage_t stage, uint16_t cpid, char const *line1, char const *line2) {
    critical_section_enter_blocking(&status_lock);
    current_status.stage = stage;
    current_status.cpid = cpid;
    current_status.progress = 0;
    copy_str(current_status.line1, sizeof(current_status.line1), line1);
    copy_str(current_status.line2, sizeof(current_status.line2), line2);
    critical_section_exit(&status_lock);
}

void app_status_set_progress(uint8_t percent) {
    critical_section_enter_blocking(&status_lock);
    current_status.progress = percent > 100 ? 100 : percent;
    critical_section_exit(&status_lock);
}

void app_status_set_ecid(uint64_t ecid) {
    critical_section_enter_blocking(&status_lock);
    current_status.ecid = ecid;
    critical_section_exit(&status_lock);
}

void app_status_get(app_status_t *status) {
    critical_section_enter_blocking(&status_lock);
    *status = current_status;
    critical_section_exit(&status_lock);
}

char const *app_stage_name(app_stage_t stage) {
    switch (stage) {
    case APP_STAGE_BOOT:
        return "BOOT";
    case APP_STAGE_WAIT_DFU:
        return "WAIT DFU";
    case APP_STAGE_ENTER_DFU:
        return "ENTER DFU";
    case APP_STAGE_DFU:
        return "DFU";
    case APP_STAGE_RESET:
        return "RESET";
    case APP_STAGE_SETUP:
        return "SETUP";
    case APP_STAGE_SPRAY:
        return "SPRAY";
    case APP_STAGE_PATCH:
        return "PATCH";
    case APP_STAGE_PWND:
        return "PWND";
    case APP_STAGE_PONGO:
        return "PONGO";
    case APP_STAGE_TBOOT:
        return "TBOOT";
    case APP_STAGE_DEVICE:
        return "DEVICE";
    case APP_STAGE_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}
