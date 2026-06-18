#include "pongo_loader.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "pico/stdlib.h"
#include "tusb.h"

#define DFU_DNLOAD 1u
#define DFU_GET_STATUS 3u
#define DFU_CLR_STATUS 4u
#define CHUNK_SZ 0x800u
#define XFER_TIMEOUT_MS 1000u

extern uint8_t const pongo_lz4_bin[];
extern uint8_t const pongo_lz4_bin_end[];

typedef struct {
    volatile bool done;
    volatile xfer_result_t result;
    volatile uint32_t actual_len;
    uint16_t requested_len;
} control_wait_t;

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

static char const *xfer_name(xfer_result_t result) {
    switch (result) {
    case XFER_RESULT_SUCCESS: return "ok";
    case XFER_RESULT_FAILED: return "fail";
    case XFER_RESULT_STALLED: return "stall";
    case XFER_RESULT_TIMEOUT: return "timeout";
    default: return "?";
    }
}

static void control_complete_cb(tuh_xfer_t *xfer) {
    control_wait_t *wait = (control_wait_t *)(uintptr_t)xfer->user_data;
    wait->result = xfer->result;
    wait->actual_len = xfer->actual_len;
    if (xfer->result == XFER_RESULT_SUCCESS && wait->requested_len == 0) {
        wait->actual_len = 0;
    }
    wait->done = true;
}

static bool ctrl_xfer(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                      uint16_t w_value, uint16_t w_index, void *data, size_t len,
                      xfer_result_t *out_result, uint32_t *out_actual) {
    tusb_control_request_t request = {
        .bmRequestType = bm_request_type,
        .bRequest = b_request,
        .wValue = w_value,
        .wIndex = w_index,
        .wLength = (uint16_t)len,
    };
    control_wait_t wait = {
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
        .complete_cb = control_complete_cb,
        .user_data = (uintptr_t)&wait,
    };
    absolute_time_t deadline = make_timeout_time_ms(XFER_TIMEOUT_MS);

    if (!tuh_control_xfer(&xfer)) {
        if (out_result) *out_result = XFER_RESULT_FAILED;
        if (out_actual) *out_actual = 0;
        return false;
    }

    while (!wait.done) {
        host_task_poll();
        if (time_reached(deadline)) {
            uint8_t ep_addr = tu_edpt_addr(0, (bm_request_type & 0x80u) ? TUSB_DIR_IN : TUSB_DIR_OUT);
            tuh_edpt_abort_xfer(daddr, ep_addr);
            if (out_result) *out_result = XFER_RESULT_TIMEOUT;
            if (out_actual) *out_actual = wait.actual_len;
            return false;
        }
    }

    if (out_result) *out_result = wait.result;
    if (out_actual) *out_actual = wait.actual_len;
    return wait.result == XFER_RESULT_SUCCESS;
}

#define SIE_CTRL_BASE_PONGO (USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS | \
                             USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS)

#define usb_hw_set_pl   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear_pl ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))

static bool raw_poll_complete_pl(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!time_reached(deadline)) {
        uint32_t status = usb_hw->sie_status;
        if (status & USB_SIE_STATUS_STALL_REC_BITS) {
            usb_hw_clear_pl->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
            return false;
        }
        if ((status & USB_SIE_STATUS_TRANS_COMPLETE_BITS) || (usb_hw->buf_status & 1u)) {
            usb_hw_clear_pl->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
            usb_hw_clear_pl->buf_status = 1u;
            return true;
        }
        tight_loop_contents();
    }
    return false;
}

static bool raw_send_setup_pl(uint8_t daddr, uint8_t const setup[8], uint32_t timeout_ms) {
    memcpy((void *)usbh_dpram->setup_packet, setup, 8);
    usb_hw->dev_addr_ctrl = daddr;

    usb_hw_clear_pl->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                  USB_SIE_STATUS_STALL_REC_BITS;

    uint32_t flags = SIE_CTRL_BASE_PONGO | USB_SIE_CTRL_SEND_SETUP_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = flags;

    return raw_poll_complete_pl(timeout_ms);
}

static bool raw_data_out_pl(uint8_t daddr, void *data, uint16_t len, uint8_t *pid, uint32_t timeout_ms) {
    if (len > 0) {
        memcpy(usbh_dpram->epx_data, data, len);
    }

    usb_hw->dev_addr_ctrl = daddr;

    uint32_t buf_ctrl = USB_BUF_CTRL_FULL | USB_BUF_CTRL_LAST |
                        (*pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID) |
                        (len & USB_BUF_CTRL_LEN_MASK);

    usbh_dpram->epx_buf_ctrl = buf_ctrl;
    busy_wait_at_least_cycles(12);
    usbh_dpram->epx_buf_ctrl = buf_ctrl | USB_BUF_CTRL_AVAIL;

    usb_hw_clear_pl->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                  USB_SIE_STATUS_STALL_REC_BITS |
                                  USB_SIE_STATUS_NAK_REC_BITS;
    usb_hw_clear_pl->buf_status = 1u;

    uint32_t flags = SIE_CTRL_BASE_PONGO | USB_SIE_CTRL_SEND_DATA_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = flags;

    if (!raw_poll_complete_pl(timeout_ms)) {
        return false;
    }
    *pid ^= 1;
    return true;
}

static bool raw_data_in_status_pl(uint8_t daddr, uint32_t timeout_ms) {
    usb_hw->dev_addr_ctrl = daddr;

    uint32_t buf_ctrl = USB_BUF_CTRL_LAST | USB_BUF_CTRL_DATA1_PID;

    usbh_dpram->epx_buf_ctrl = buf_ctrl;
    busy_wait_at_least_cycles(12);
    usbh_dpram->epx_buf_ctrl = buf_ctrl | USB_BUF_CTRL_AVAIL;

    usb_hw_clear_pl->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                  USB_SIE_STATUS_STALL_REC_BITS |
                                  USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
    usb_hw_clear_pl->buf_status = 1u;

    uint32_t flags = SIE_CTRL_BASE_PONGO | USB_SIE_CTRL_RECEIVE_DATA_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = flags;

    return raw_poll_complete_pl(timeout_ms);
}

bool pongo_send_image(uint8_t daddr) {
    size_t const pongo_len = (size_t)(pongo_lz4_bin_end - pongo_lz4_bin);
    if (pongo_len == 0) {
        app_log_enqueue("pongo: image is empty\r\n");
        return false;
    }

    app_logf("pongo: sending image len=%lu\r\n", (unsigned long)pongo_len);
    app_status_set(APP_STAGE_PONGO, 0, "SENDING", "PONGO");

    irq_set_enabled(USBCTRL_IRQ, false);
    usb_hw_clear_pl->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                  USB_SIE_STATUS_STALL_REC_BITS |
                                  USB_SIE_STATUS_NAK_REC_BITS |
                                  USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                  USB_SIE_STATUS_ACK_REC_BITS;
    usb_hw_clear_pl->buf_status = usb_hw->buf_status;

    size_t off = 0;
    uint32_t last_pct = 0;
    while (off < pongo_len) {
        size_t chunk = pongo_len - off;
        if (chunk > CHUNK_SZ) chunk = CHUNK_SZ;

        uint8_t setup[8] = {
            0x21, DFU_DNLOAD,
            0, 0,
            0, 0,
            (uint8_t)(chunk & 0xFF), (uint8_t)(chunk >> 8),
        };

        if (!raw_send_setup_pl(daddr, setup, XFER_TIMEOUT_MS)) {
            app_logf("pongo: image setup failed at off=%lu\r\n", (unsigned long)off);
            goto fail;
        }

        uint8_t pid = 1;
        size_t data_off = 0;
        while (data_off < chunk) {
            uint16_t pkt = (uint16_t)((chunk - data_off) > 64 ? 64 : (chunk - data_off));
            if (!raw_data_out_pl(daddr, (void *)(pongo_lz4_bin + off + data_off), pkt, &pid, XFER_TIMEOUT_MS)) {
                app_logf("pongo: image data failed at off=%lu\r\n", (unsigned long)(off + data_off));
                goto fail;
            }
            data_off += pkt;
        }

        off += chunk;

        uint32_t pct = (uint32_t)(off * 100 / pongo_len);
        if (pct != last_pct) {
            app_status_set_progress((uint8_t)pct);
            last_pct = pct;
        }

        if ((off % (32u * CHUNK_SZ)) == 0 || off == pongo_len) {
            app_logf("pongo: sent %lu/%lu bytes\r\n", (unsigned long)off, (unsigned long)pongo_len);
        }
    }

    usb_hw->sie_ctrl = SIE_CTRL_BASE_PONGO;
    usbh_dpram->epx_buf_ctrl = 0;
    busy_wait_at_least_cycles(12);
    usb_hw_clear_pl->sie_status = 0xFFFFFFFF;
    usb_hw_clear_pl->buf_status = usb_hw->buf_status;
    irq_set_enabled(USBCTRL_IRQ, true);

    host_delay_ms(50);

    xfer_result_t result;
    ctrl_xfer(daddr, 0x21, DFU_CLR_STATUS, 0, 0, NULL, 0, &result, NULL);
    app_logf("pongo: trigger ret=%s\r\n", xfer_name(result));

    app_log_enqueue("pongo: image sent, booting PongoOS\r\n");
    return true;

fail:
    usb_hw->sie_ctrl = SIE_CTRL_BASE_PONGO;
    usbh_dpram->epx_buf_ctrl = 0;
    busy_wait_at_least_cycles(12);
    usb_hw_clear_pl->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                                  USB_SIE_STATUS_STALL_REC_BITS |
                                  USB_SIE_STATUS_NAK_REC_BITS |
                                  USB_SIE_STATUS_RX_TIMEOUT_BITS |
                                  USB_SIE_STATUS_DATA_SEQ_ERROR_BITS |
                                  USB_SIE_STATUS_ACK_REC_BITS;
    usb_hw_clear_pl->buf_status = usb_hw->buf_status;
    irq_set_enabled(USBCTRL_IRQ, true);
    app_log_enqueue("pongo: compressed image send failed\r\n");
    return false;
}
