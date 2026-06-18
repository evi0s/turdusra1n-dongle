#ifndef TETHERED_BOOTER_TUSB_CONFIG_H
#define TETHERED_BOOTER_TUSB_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_TUSB_OS OPT_OS_PICO

/* Native USB: host for iPhone, device for MSC upload. Switched at runtime. */
#define CFG_TUD_ENABLED 1
#define CFG_TUH_ENABLED 1
#define CFG_TUH_RPI_PIO_USB 0

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

/* Device MSC for device mode. */
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_CDC 0
#define CFG_TUD_MSC 1
#define CFG_TUD_MSC_EP_BUFSIZE 512

/* Host enumeration. checkm8 will use EP0 control transfers directly. */
#define CFG_TUH_API_EDPT_XFER 1
#define CFG_TUH_ENUMERATION_BUFSIZE 512
#define CFG_TUH_HUB 0
#define CFG_TUH_DEVICE_MAX 1
#define CFG_TUH_INTERFACE_MAX 4

#define CFG_TUH_HID 0
#define CFG_TUH_CDC 0
#define CFG_TUH_MSC 0
#define CFG_TUH_VENDOR 0

#ifdef __cplusplus
}
#endif

#endif
