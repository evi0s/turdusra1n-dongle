#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

#define USB_VID 0xcafe
#define USB_PID 0x4001
#define USB_BCD 0x0200

tusb_desc_device_t const desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = USB_BCD,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_TOTAL
};

#define EPNUM_MSC_OUT 0x01
#define EPNUM_MSC_IN 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 4, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},
    "tethered-booter",
    "RP2350B MSC",
    NULL,
    "PTE Block Drive",
};

static uint16_t desc_str[32];
static char serial_string[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    uint8_t chr_count;

    if (index == 0) {
        memcpy(&desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index == 3) {
            pico_get_unique_board_id_string(serial_string, sizeof(serial_string));
            string_desc_arr[3] = serial_string;
        }

        if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }

        char const *str = string_desc_arr[index];
        if (!str) {
            return NULL;
        }

        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }

        for (uint8_t i = 0; i < chr_count; i++) {
            desc_str[1 + i] = str[i];
        }
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return desc_str;
}
