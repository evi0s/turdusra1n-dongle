#include "pongo_shell.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "pico/stdlib.h"
#include "psram.h"
#include "tusb.h"

#define PONGO_CMD_SEND   3u
#define PONGO_CMD_BULK   1u
#define PONGO_CMD_DATA   2u
#define PONGO_CMD_CLEAR  4u
#define PONGO_CHUNK_SZ   2048u
#define PONGO_XFER_TIMEOUT_MS 2000u

typedef struct {
    volatile bool done;
    volatile xfer_result_t result;
    volatile uint32_t actual_len;
    uint16_t requested_len;
} pongo_wait_t;

static void host_task_poll(void) {
    tuh_task_ext(0, false);
}

static void host_delay_ms(uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!time_reached(deadline)) {
        host_task_poll();
        sleep_ms(1);
    }
}

static void pongo_control_cb(tuh_xfer_t *xfer) {
    pongo_wait_t *wait = (pongo_wait_t *)(uintptr_t)xfer->user_data;
    wait->result = xfer->result;
    wait->actual_len = xfer->actual_len;
    if (xfer->result == XFER_RESULT_SUCCESS && wait->requested_len == 0) {
        wait->actual_len = 0;
    }
    wait->done = true;
}

static bool pongo_control(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                          uint16_t w_value, uint16_t w_index, void *data, size_t len,
                          xfer_result_t *out_result) {
    tusb_control_request_t request = {
        .bmRequestType = bm_request_type,
        .bRequest = b_request,
        .wValue = w_value,
        .wIndex = w_index,
        .wLength = (uint16_t)len,
    };
    pongo_wait_t wait = {
        .done = false,
        .result = XFER_RESULT_INVALID,
        .actual_len = 0,
        .requested_len = (uint16_t)len,
    };
    tuh_xfer_t xfer = {
        .daddr = daddr,
        .ep_addr = 0,
        .setup = &request,
        .buffer = data,
        .complete_cb = pongo_control_cb,
        .user_data = (uintptr_t)&wait,
    };
    absolute_time_t deadline = make_timeout_time_ms(PONGO_XFER_TIMEOUT_MS);

    if (!tuh_control_xfer(&xfer)) {
        if (out_result) *out_result = XFER_RESULT_FAILED;
        return false;
    }

    while (!wait.done) {
        host_task_poll();
        if (time_reached(deadline)) {
            uint8_t ep_addr = tu_edpt_addr(0, (bm_request_type & 0x80u) ? TUSB_DIR_IN : TUSB_DIR_OUT);
            tuh_edpt_abort_xfer(daddr, ep_addr);
            if (out_result) *out_result = XFER_RESULT_TIMEOUT;
            return false;
        }
    }

    if (out_result) *out_result = wait.result;
    return wait.result == XFER_RESULT_SUCCESS;
}

static bool pongo_drain_output(uint8_t daddr) {
    uint8_t in_progress = 1;
    uint8_t buf[0x100];
    uint32_t total_drained = 0;
    absolute_time_t deadline = make_timeout_time_ms(3000);

    while (in_progress) {
        xfer_result_t result;
        if (!pongo_control(daddr, 0xA1, 2, 0, 0, &in_progress, 1, &result)) {
            break;
        }
        if (!pongo_control(daddr, 0xA1, 1, 0, 0, buf, sizeof(buf), &result)) {
            break;
        }
        total_drained += sizeof(buf);
        if (time_reached(deadline)) {
            break;
        }
    }

    if (total_drained > 0) {
        app_logf("pongo_shell: drained %lu bytes output\r\n", (unsigned long)total_drained);
    }
    return true;
}

static bool pongo_clear(uint8_t daddr) {
    xfer_result_t result;
    bool ok = pongo_control(daddr, 0x21, PONGO_CMD_CLEAR, 0xFFFF, 0, NULL, 0, &result);
    if (!ok) {
        app_logf("pongo_shell: clear ret=%u\r\n", (unsigned)result);
    }
    return ok;
}

static bool pongo_wait_done(uint8_t daddr, uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        uint8_t in_progress = 1;
        xfer_result_t result;
        if (!pongo_control(daddr, 0xA1, 2, 0, 0, &in_progress, 1, &result)) {
            break;
        }
        if (!in_progress) {
            return true;
        }
        sleep_ms(5);
    }
    return false;
}

static bool pongo_send_cmd(uint8_t daddr, char const *cmd) {
    size_t len = strlen(cmd);
    app_logf("pongo_shell: cmd \"%.*s\"\r\n", (int)(len > 0 && cmd[len-1] == '\n' ? len-1 : len), cmd);

    if (!pongo_clear(daddr)) {
        app_log_enqueue("pongo_shell: clear failed\r\n");
        return false;
    }

    xfer_result_t result;
    if (!pongo_control(daddr, 0x21, PONGO_CMD_SEND, 0, 0, (void *)cmd, len, &result)) {
        app_logf("pongo_shell: cmd send failed (ret=%u)\r\n", (unsigned)result);
        return false;
    }

    pongo_wait_done(daddr, 5000);
    return true;
}

#define SIE_CTRL_BASE (USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS | \
                       USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS)

#define usb_hw_set   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))

static uint8_t bulk_pid;

static bool pongo_bulk_xfer(uint8_t daddr, uint8_t *data, size_t len) {
    irq_set_enabled(USBCTRL_IRQ, false);

    usbh_dpram->epx_buf_ctrl = 0;
    busy_wait_at_least_cycles(12);
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS;
    usb_hw_clear->buf_status = usb_hw->buf_status;

    size_t off = 0;
    while (off < len) {
        uint16_t chunk = (uint16_t)((len - off) > 64 ? 64 : (len - off));

        memcpy(usbh_dpram->epx_data, data + off, chunk);

        usb_hw->dev_addr_ctrl = (uint32_t)(daddr | (2u << USB_ADDR_ENDP_ENDPOINT_LSB));

        usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                   USB_SIE_STATUS_STALL_REC_BITS |
                                   USB_SIE_STATUS_NAK_REC_BITS;
        usb_hw_clear->buf_status = usb_hw->buf_status;

        uint32_t buf_ctrl = USB_BUF_CTRL_FULL | USB_BUF_CTRL_LAST |
                            (bulk_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID) |
                            (chunk & USB_BUF_CTRL_LEN_MASK);

        usbh_dpram->epx_buf_ctrl = buf_ctrl;
        busy_wait_at_least_cycles(12);
        usbh_dpram->epx_buf_ctrl = buf_ctrl | USB_BUF_CTRL_AVAIL;

        uint32_t flags = SIE_CTRL_BASE | USB_SIE_CTRL_SEND_DATA_BITS | USB_SIE_CTRL_START_TRANS_BITS;
        usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
        busy_wait_at_least_cycles(12);
        usb_hw->sie_ctrl = flags;

        absolute_time_t deadline = make_timeout_time_ms(PONGO_XFER_TIMEOUT_MS);
        bool done = false;
        while (!done) {
            uint32_t status = usb_hw->sie_status;
            if (status & USB_SIE_STATUS_STALL_REC_BITS) {
                usb_hw_clear->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
                goto fail;
            }
            if ((status & USB_SIE_STATUS_TRANS_COMPLETE_BITS) || (usb_hw->buf_status & 1u)) {
                usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
                usb_hw_clear->buf_status = 1u;
                done = true;
            }
            if (!done && time_reached(deadline)) {
                goto fail;
            }
        }

        bulk_pid ^= 1;
        off += chunk;
    }

    usb_hw->sie_ctrl = SIE_CTRL_BASE;
    usbh_dpram->epx_buf_ctrl = 0;
    busy_wait_at_least_cycles(12);
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS;
    usb_hw_clear->buf_status = usb_hw->buf_status;
    irq_set_enabled(USBCTRL_IRQ, true);
    return true;

fail:
    usb_hw->sie_ctrl = SIE_CTRL_BASE;
    usbh_dpram->epx_buf_ctrl = 0;
    busy_wait_at_least_cycles(12);
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS;
    usb_hw_clear->buf_status = usb_hw->buf_status;
    irq_set_enabled(USBCTRL_IRQ, true);
    app_log_enqueue("pongo_shell: bulk raw xfer failed\r\n");
    return false;
}

static bool pongo_send_bulk(uint8_t daddr, uint8_t const *data, size_t len) {
    uint32_t size32 = (uint32_t)len;
    xfer_result_t result;

    if (!pongo_clear(daddr)) {
        app_log_enqueue("pongo_shell: bulk clear failed\r\n");
        return false;
    }
    app_log_enqueue("pongo_shell: clear ok\r\n");

    if (!pongo_control(daddr, 0x21, PONGO_CMD_BULK, 0, 0, &size32, 4, &result)) {
        app_logf("pongo_shell: bulk setup (size=%lu) failed (ret=%u)\r\n",
                 (unsigned long)len, (unsigned)result);
        return false;
    }
    app_logf("pongo_shell: bulk setup ok, sending %lu bytes\r\n", (unsigned long)len);

    size_t off = 0;
    uint32_t last_pct = 0;
    while (off < len) {
        size_t chunk = len - off;
        if (chunk > PONGO_CHUNK_SZ) {
            chunk = PONGO_CHUNK_SZ;
        }

        if (!pongo_bulk_xfer(daddr, (uint8_t *)(data + off), chunk)) {
            app_logf("pongo_shell: bulk data failed at off=%lu\r\n", (unsigned long)off);
            return false;
        }

        off += chunk;

        uint32_t pct = (uint32_t)(off * 100 / len);
        if (pct != last_pct) {
            app_status_set_progress((uint8_t)pct);
            last_pct = pct;
        }

        if ((off % (64u * PONGO_CHUNK_SZ)) == 0 || off == len) {
            app_logf("pongo_shell: bulk %lu/%lu\r\n", (unsigned long)off, (unsigned long)len);
        }
    }

    return true;
}

static bool pongo_upload_resource(uint8_t daddr, resource_id_t id, char const *name) {
    uint8_t const *data;
    size_t len;

    if (!psram_get_resource(id, &data, &len)) {
        app_logf("pongo_shell: resource %s not available\r\n", name);
        return false;
    }

    app_logf("pongo_shell: %s header: %02X %02X %02X %02X\r\n",
             name, data[0], data[1], data[2], data[3]);

    char line2[24];
    snprintf(line2, sizeof(line2), "SEND %s", name);
    app_status_set(APP_STAGE_TBOOT, 0, "UPLOADING", line2);

    app_logf("pongo_shell: uploading %s (%lu bytes)\r\n", name, (unsigned long)len);
    if (!pongo_send_bulk(daddr, data, len)) {
        app_logf("pongo_shell: upload %s failed\r\n", name);
        return false;
    }

    return true;
}

pongo_boot_result_t pongo_shell_tethered_boot(uint8_t daddr) {
    bulk_pid = 0;
    host_delay_ms(1000);

    pongo_drain_output(daddr);

    app_log_enqueue("pongo_shell: starting tethered boot sequence\r\n");

    if (!pongo_upload_resource(daddr, RESOURCE_SEP_RACER, "sep_racer")) {
        return PONGO_BOOT_XFER_FAIL;
    }
    if (!pongo_send_cmd(daddr, "modload\n")) {
        return PONGO_BOOT_CMD_FAIL;
    }

    if (psram_pteblock_valid()) {
        if (!pongo_upload_resource(daddr, RESOURCE_PTEBLOCK, "pteblock")) {
            return PONGO_BOOT_XFER_FAIL;
        }
        if (!pongo_send_cmd(daddr, "sep pte\n")) {
            return PONGO_BOOT_CMD_FAIL;
        }

        if (!pongo_send_cmd(daddr, "sep pwn_pte\n")) {
            return PONGO_BOOT_CMD_FAIL;
        }

        host_delay_ms(500);
        pongo_drain_output(daddr);
    } else {
        app_log_enqueue("pongo_shell: no pteblock, skipping SEP exploit\r\n");
    }

    if (!pongo_upload_resource(daddr, RESOURCE_KPF, "kpf_tethered")) {
        return PONGO_BOOT_XFER_FAIL;
    }
    if (!pongo_send_cmd(daddr, "modload\n")) {
        return PONGO_BOOT_CMD_FAIL;
    }

    if (!pongo_send_cmd(daddr, "kpf-tethered\n")) {
        return PONGO_BOOT_CMD_FAIL;
    }

    pongo_send_cmd(daddr, "bootux\n");

    app_log_enqueue("pongo_shell: tethered boot sequence complete\r\n");
    return PONGO_BOOT_OK;
}
