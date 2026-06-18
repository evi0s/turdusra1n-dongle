#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "app_log.h"
#include "checkm8.h"
#include "hardware/clocks.h"
#include "hardware/flash.h"
#include "hardware/irq.h"
#include "hardware/structs/usb.h"
#include "hardware/structs/usb_dpram.h"
#include "hardware/address_mapped.h"

#define usb_hw_set   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))
#include "msc_disk.h"
#include "oled_sh1106.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pongo_loader.h"
#include "pongo_shell.h"
#include "psram.h"
#include "host/hcd.h"
#include "tusb.h"

#define SIE_CTRL_BASE (USB_SIE_CTRL_SOF_EN_BITS | USB_SIE_CTRL_KEEP_ALIVE_EN_BITS | \
                      USB_SIE_CTRL_PULLDOWN_EN_BITS | USB_SIE_CTRL_EP0_INT_1BUF_BITS)

#define USB_HOST_RHPORT 0
#define ATTEMPT_RETRY_DELAY_MS 5000

#define BUTTON_PIN      TETHERED_BOOTER_BUTTON_PIN
#define DEBOUNCE_MS     50
#define LONG_PRESS_MS   2000

typedef enum {
    BTN_IDLE,
    BTN_PRESSED,
    BTN_LONG_DETECTED,
} button_state_t;

typedef enum {
    BTN_EVENT_NONE,
    BTN_EVENT_SHORT_PRESS,
    BTN_EVENT_LONG_PRESS,
} button_event_t;

typedef enum {
    USB_MODE_HOST,
    USB_MODE_DEVICE,
} usb_mode_t;

static volatile uint8_t mounted_dfu_addr;
static volatile uint8_t mounted_recovery_addr;
static volatile uint8_t mounted_pongo_addr;
static volatile uint16_t mounted_vid;
static volatile uint16_t mounted_pid;
static volatile uint32_t dfu_mount_epoch;
static volatile uint32_t pongo_mount_epoch;
static volatile bool suppress_status_log;

static usb_mode_t current_mode = USB_MODE_HOST;
static button_state_t btn_state = BTN_IDLE;
static uint32_t btn_press_start_ms;
static volatile bool core1_pause_requested;
static volatile bool core1_paused;

static void ui_core_main(void);
void core1_halt(void);
void core1_resume(void);

static char const *speed_name(tusb_speed_t speed) {
    switch (speed) {
    case TUSB_SPEED_LOW:
        return "low";
    case TUSB_SPEED_FULL:
        return "full";
    case TUSB_SPEED_HIGH:
        return "high";
    default:
        return "unknown";
    }
}

static bool is_apple_dfu(uint16_t vid, uint16_t pid) {
    return vid == 0x05ac && (pid == 0x1227 || pid == 0x1222);
}

static bool is_apple_recovery(uint16_t vid, uint16_t pid) {
    return vid == 0x05ac && pid == 0x1281;
}

static bool is_pongo(uint16_t vid, uint16_t pid) {
    return vid == 0x05ac && pid == 0x4141;
}

static void host_task_poll(void) {
    tuh_task_ext(0, false);
}

static app_stage_t app_stage_from_checkm8(checkm8_stage_t stage) {
    switch (stage) {
    case CHECKM8_STAGE_RESET:
        return APP_STAGE_RESET;
    case CHECKM8_STAGE_SETUP:
        return APP_STAGE_SETUP;
    case CHECKM8_STAGE_SPRAY:
        return APP_STAGE_SPRAY;
    case CHECKM8_STAGE_PATCH:
        return APP_STAGE_PATCH;
    case CHECKM8_STAGE_PWND:
        return APP_STAGE_PWND;
    default:
        return APP_STAGE_DFU;
    }
}

static void host_delay_ms(uint32_t ms) {
    absolute_time_t deadline = make_timeout_time_ms(ms);

    while (!time_reached(deadline)) {
        host_task_poll();
        sleep_ms(1);
    }
}

static bool host_has_device(void) {
    return mounted_dfu_addr || mounted_recovery_addr || mounted_pongo_addr;
}

static button_event_t button_poll(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    bool raw_pressed = !gpio_get(BUTTON_PIN);

    switch (btn_state) {
    case BTN_IDLE:
        if (raw_pressed) {
            btn_press_start_ms = now;
            btn_state = BTN_PRESSED;
        }
        break;

    case BTN_PRESSED:
        if (!raw_pressed) {
            if ((now - btn_press_start_ms) >= DEBOUNCE_MS) {
                btn_state = BTN_IDLE;
                return BTN_EVENT_SHORT_PRESS;
            }
            btn_state = BTN_IDLE;
        } else if ((now - btn_press_start_ms) >= LONG_PRESS_MS) {
            btn_state = BTN_LONG_DETECTED;
            return BTN_EVENT_LONG_PRESS;
        }
        break;

    case BTN_LONG_DETECTED:
        if (!raw_pressed) {
            btn_state = BTN_IDLE;
        }
        break;
    }

    return BTN_EVENT_NONE;
}

static uint8_t wait_for_dfu(uint32_t timeout_ms) {
    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);

    while (true) {
        host_task_poll();
        if (mounted_dfu_addr) {
            return mounted_dfu_addr;
        }
        if (timeout_ms != 0 && time_reached(deadline)) {
            return 0;
        }
        sleep_ms(1);
    }
}

static uint8_t wait_for_dfu_after_reset(uint8_t previous_addr, uint32_t previous_epoch) {
    absolute_time_t deadline = make_timeout_time_ms(5000);
    (void)previous_addr;

    while (!time_reached(deadline)) {
        host_task_poll();

        if (mounted_dfu_addr && dfu_mount_epoch != previous_epoch) {
            return mounted_dfu_addr;
        }

        sleep_ms(1);
    }

    return 0;
}

static void reset_target_bus(void) {
    app_log_enqueue("host: USB bus reset + re-enumerate\r\n");
    bool had_device = mounted_dfu_addr || mounted_pongo_addr || mounted_recovery_addr;
    mounted_dfu_addr = 0;
    mounted_pongo_addr = 0;
    mounted_recovery_addr = 0;
    mounted_vid = 0;
    mounted_pid = 0;

     if (had_device) {
        hcd_event_device_remove(USB_HOST_RHPORT, false);
        host_delay_ms(10);
    }

    usbh_dpram->epx_buf_ctrl = 0;
    for (int i = 0; i < USB_HOST_INTERRUPT_ENDPOINTS; i++) {
        usbh_dpram->int_ep_buffer_ctrl[i].ctrl = 0;
    }

    usb_hw->sie_ctrl = SIE_CTRL_BASE | USB_SIE_CTRL_RESET_BUS_BITS;
    host_delay_ms(50);
    usb_hw->sie_ctrl = SIE_CTRL_BASE;
    host_delay_ms(20);
}

static void update_wait_status(void) {
    if (mounted_dfu_addr || mounted_pongo_addr || mounted_recovery_addr) {
        app_status_set(APP_STAGE_WAIT_DFU, checkm8_current_cpid(), "WAITING", "DETECTING");
    } else {
        app_status_set(APP_STAGE_WAIT_DFU, 0, "NO DEVICE", "BTN: MSC");
    }
}

static void run_checkm8_session(void) {
    static uint32_t attempt = 0;
    static bool pause_before_next_attempt;

    while (true) {
        uint8_t daddr = wait_for_dfu(0);
        checkm8_probe_t probe;
        char line1[24];
        char line2[24];

        if (!daddr) {
            suppress_status_log = false;
            update_wait_status();
            continue;
        }

        suppress_status_log = true;
        host_delay_ms(100);
        app_status_set(APP_STAGE_DFU, checkm8_current_cpid(), "APPLE DFU", "PROBING");
        if (!checkm8_probe_device(daddr, &probe)) {
            app_log_enqueue("checkm8: probe failed\r\n");
            app_status_set(APP_STAGE_ERROR, checkm8_current_cpid(), "PROBE FAILED", "REPLUG DFU");
            host_delay_ms(500);
            continue;
        }

        if (!probe.supported) {
            app_log_enqueue("checkm8: unsupported SRTG/iBoot build\r\n");
            app_status_set(APP_STAGE_ERROR, 0, "UNSUPPORTED", "SRTG BUILD");
            while (mounted_dfu_addr == daddr) {
                host_task_poll();
                sleep_ms(10);
            }
            checkm8_reset_state();
            return;
        }

        app_status_set_ecid(probe.ecid);
        if (probe.ecid && !probe.pwned) {
            if (!psram_pteblock_select(probe.ecid)) {
                app_logf("checkm8: no pteblock for ECID %llu (last6: %llu)\r\n",
                         (unsigned long long)probe.ecid,
                         (unsigned long long)(probe.ecid % 1000000));
                app_status_set(APP_STAGE_ERROR, probe.cpid, "NO PTEBLOCK", "UPLOAD VIA USB");
                suppress_status_log = true;
                while (mounted_dfu_addr == daddr) {
                    host_task_poll();
                    sleep_ms(10);
                }
                suppress_status_log = false;
                checkm8_reset_state();
                return;
            }
        }

        if (probe.pwned) {
            app_logf("checkm8: pwned CPID=0x%04x ECID=%llu\r\n", probe.cpid, (unsigned long long)probe.ecid);
            app_status_set(APP_STAGE_PONGO, probe.cpid, "PONGO", "SENDING");
            app_status_set_ecid(probe.ecid);
            host_delay_ms(500);

            if (!pongo_send_image(daddr)) {
                app_log_enqueue("pongo: compressed image send failed\r\n");
                app_status_set(APP_STAGE_ERROR, probe.cpid, "PONGO", "SEND FAIL");
                suppress_status_log = false;
                checkm8_reset_state();
                return;
            }

            app_status_set(APP_STAGE_PONGO, probe.cpid, "PONGO", "BOOTING");

            uint32_t pongo_epoch = pongo_mount_epoch;

            absolute_time_t pongo_deadline = make_timeout_time_ms(10000);
            while (!time_reached(pongo_deadline)) {
                host_task_poll();
                if (mounted_pongo_addr && pongo_mount_epoch != pongo_epoch) {
                    break;
                }
                sleep_ms(10);
            }

            uint8_t pongo_addr = mounted_pongo_addr;
            if (pongo_addr && pongo_mount_epoch != pongo_epoch) {
                app_log_enqueue("pongo: mode device ready\r\n");
                app_status_set(APP_STAGE_TBOOT, probe.cpid, "TETHERED", "BOOTING");

                if (probe.ecid && !psram_pteblock_valid()) {
                    psram_pteblock_select(probe.ecid);
                }

                pongo_boot_result_t boot_ret = pongo_shell_tethered_boot(pongo_addr);
                if (boot_ret == PONGO_BOOT_OK) {
                    app_status_set(APP_STAGE_TBOOT, probe.cpid, "TETHERED", "BOOT DONE");
                    app_log_enqueue("tboot: tethered boot success\r\n");
                    suppress_status_log = true;
                    return;
                } else {
                    app_logf("tboot: failed (code=%d)\r\n", boot_ret);
                    app_status_set(APP_STAGE_ERROR, probe.cpid, "TBOOT", "FAILED");
                }
            } else {
                app_log_enqueue("pongo: did not reappear after PongoOS send\r\n");
                app_status_set(APP_STAGE_ERROR, probe.cpid, "PONGO", "NO DEVICE");
            }

            suppress_status_log = false;
            checkm8_reset_state();
            return;
        }

        checkm8_stage_t stage = checkm8_current_stage();
        if (stage == CHECKM8_STAGE_RESET) {
            if (pause_before_next_attempt) {
            app_logf("\r\ncheckm8: attempt failed; retry in %u ms\r\n\r\n",
                         ATTEMPT_RETRY_DELAY_MS);
                host_delay_ms(ATTEMPT_RETRY_DELAY_MS);
                pause_before_next_attempt = false;
            }

            attempt++;
            app_logf("\r\ncheckm8: ===== attempt %lu begin =====\r\n",
                     (unsigned long)attempt);
        }

        snprintf(line1, sizeof(line1), "CPID %04X", probe.cpid);
        snprintf(line2, sizeof(line2), "%s", checkm8_stage_name(stage));
        app_status_set(app_stage_from_checkm8(stage), probe.cpid, line1, "RUNNING");
        app_status_set_progress((uint8_t)((stage + 1) * 100 / 4));
        app_logf("checkm8: CPID=0x%04x stage=%s\r\n", probe.cpid, checkm8_stage_name(stage));

        bool ok = checkm8_run_current_stage(daddr);
        app_logf("checkm8: stage %s ret=%s\r\n", checkm8_stage_name(stage), ok ? "true" : "false");

        if (!ok) {
            snprintf(line2, sizeof(line2), "%s FAIL", checkm8_stage_name(stage));
            app_status_set(APP_STAGE_ERROR, probe.cpid, line1, line2);
        }

        if (stage == CHECKM8_STAGE_PATCH) {
            app_log_enqueue("checkm8: PATCH done, waiting for reboot...\r\n");

            uint32_t patch_epoch = dfu_mount_epoch;

            absolute_time_t reboot_deadline = make_timeout_time_ms(8000);
            while (!time_reached(reboot_deadline)) {
                host_task_poll();
                if (mounted_dfu_addr && dfu_mount_epoch != patch_epoch) {
                    break;
                }
                sleep_ms(10);
            }

            if (!mounted_dfu_addr || dfu_mount_epoch == patch_epoch) {
                app_log_enqueue("checkm8: device did not reappear, bus reset...\r\n");
                reset_target_bus();

                patch_epoch = dfu_mount_epoch;
                reboot_deadline = make_timeout_time_ms(5000);
                while (!time_reached(reboot_deadline)) {
                    host_task_poll();
                    if (mounted_dfu_addr && dfu_mount_epoch != patch_epoch) {
                        break;
                    }
                    sleep_ms(10);
                }
            }

            if (!mounted_dfu_addr || dfu_mount_epoch == patch_epoch) {
                app_log_enqueue("checkm8: PATCH re-discovery pending, returning to main loop\r\n");
                suppress_status_log = false;
                checkm8_reset_state();
                return;
            }
            host_delay_ms(500);
            pause_before_next_attempt = true;
        } else {
            uint32_t epoch = dfu_mount_epoch;
            reset_target_bus();

            if (!wait_for_dfu_after_reset(daddr, epoch)) {
                app_log_enqueue("checkm8: DFU did not reappear after reset\r\n");
                app_status_set(APP_STAGE_WAIT_DFU, probe.cpid, "REPLUG DFU", "NO DEVICE");
                suppress_status_log = false;
                checkm8_reset_state();
                return;
            }
        }
    }
}

static void erase_all_pteblocks(void) {
    app_status_set(APP_STAGE_DEVICE, 0, "ERASING", "PTEBLOCKS");
    app_log_enqueue("device: erasing all pteblocks from flash...\r\n");

    tud_deinit(0);
    sleep_ms(50);

    core1_halt();

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(0x600000, 16 * 4096 * 2);
    restore_interrupts(ints);

    core1_resume();

    app_log_enqueue("device: all pteblocks erased\r\n");

    sleep_ms(50);
    msc_disk_init();
    tud_init(0);

    app_status_set(APP_STAGE_DEVICE, 0, "ERASED", "REBOOT");
}

static void enter_device_mode(void) {
    app_log_enqueue("mode: switching to USB device (MSC)\r\n");

    mounted_dfu_addr = 0;
    mounted_recovery_addr = 0;
    mounted_pongo_addr = 0;
    mounted_vid = 0;
    mounted_pid = 0;

    tuh_deinit(0);
    sleep_ms(100);

    msc_disk_init();
    tud_init(0);

    current_mode = USB_MODE_DEVICE;
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    int pte_count = psram_pteblock_count();
    if (pte_count > 0) {
        char line[24];
        snprintf(line, sizeof(line), "%d BLOCK[S]", pte_count);
        app_status_set(APP_STAGE_DEVICE, 0, line, "LONG: ERASE");
    } else {
        app_status_set(APP_STAGE_DEVICE, 0, "MSC READY", "UPLOAD FILE");
    }
}

static void core1_wait_if_paused(void) {
    if (core1_pause_requested) {
        core1_paused = true;
        while (core1_pause_requested) {
            tight_loop_contents();
        }
        core1_paused = false;
    }
}

void core1_halt(void) {
    core1_pause_requested = true;
    while (!core1_paused) {
        tight_loop_contents();
    }
}

void core1_resume(void) {
    core1_pause_requested = false;
    while (core1_paused) {
        tight_loop_contents();
    }
}

static void ui_core_main(void) {
    bool oled_ok = oled_sh1106_init();
    uint32_t last_oled_ms = 0;

    app_logf("oled: i2c%u sda=%u scl=%u addr=0x%02x %s\r\n",
             PICO_DEFAULT_I2C,
             PICO_DEFAULT_I2C_SDA_PIN,
             PICO_DEFAULT_I2C_SCL_PIN,
             TETHERED_BOOTER_OLED_I2C_ADDR,
             oled_ok ? "online" : "not detected");

    while (true) {
        core1_wait_if_paused();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - last_oled_ms >= 250) {
            last_oled_ms = now;
            oled_sh1106_draw_status();
        }
        sleep_ms(10);
    }
}

static void top_level_loop(void) {
    update_wait_status();
    app_log_enqueue("host: native USB host running\r\n");

    while (true) {
        button_event_t evt = button_poll();

        if (current_mode == USB_MODE_HOST) {
            if (evt == BTN_EVENT_SHORT_PRESS && !host_has_device()) {
                enter_device_mode();
                continue;
            }

            host_task_poll();

            if (mounted_dfu_addr) {
                run_checkm8_session();
                if (!mounted_pongo_addr && !suppress_status_log) {
                    update_wait_status();
                }
            } else if (mounted_recovery_addr) {
                app_log_enqueue("host: recovery mode - enter DFU\r\n");

                for (int i = 3; i > 0; i--) {
                    char msg[24];
                    snprintf(msg, sizeof(msg), "STARTING %d", i);
                    app_status_set(APP_STAGE_ENTER_DFU, 0, "GET READY", msg);
                    host_delay_ms(1000);
                    if (mounted_dfu_addr) break;
                }

                if (!mounted_dfu_addr) {
                    uint32_t step_start = to_ms_since_boot(get_absolute_time());
                    int step = 0;

                    while (!mounted_dfu_addr) {
                        host_task_poll();
                        uint32_t now = to_ms_since_boot(get_absolute_time());
                        uint32_t elapsed = now - step_start;

                        if (step == 0) {
                            char countdown[24];
                            int secs = 10 - (int)(elapsed / 1000);
                            if (secs < 0) secs = 0;
                            snprintf(countdown, sizeof(countdown), "PWR+HOME %ds", secs);
                            app_status_set(APP_STAGE_ENTER_DFU, 0, "HOLD BUTTONS", countdown);
                            if (elapsed >= 10000) {
                                step = 1;
                                step_start = now;
                            }
                        } else {
                            uint32_t blink = (elapsed / 500) % 2;
                            if (blink) {
                                app_status_set(APP_STAGE_ENTER_DFU, 0, "RELEASE PWR", "KEEP HOME");
                            } else {
                                app_status_set(APP_STAGE_ENTER_DFU, 0, "RELEASE PWR", "");
                            }
                            if (elapsed >= 8000) {
                                break;
                            }
                        }
                        sleep_ms(50);
                    }
                }

                app_status_set(APP_STAGE_WAIT_DFU, 0, "DFU ENTERED", "DETECTING");
                app_log_enqueue("host: DFU animation done\r\n");
                mounted_recovery_addr = 0;
            } else if (mounted_pongo_addr) {
                uint8_t pongo_addr = mounted_pongo_addr;
                app_status_set(APP_STAGE_TBOOT, checkm8_current_cpid(), "TETHERED", "BOOTING");
                app_logf("host: PongoOS detected addr=%u, starting tethered boot\r\n", pongo_addr);

                pongo_boot_result_t boot_ret = pongo_shell_tethered_boot(pongo_addr);
                if (boot_ret == PONGO_BOOT_OK) {
                    app_status_set(APP_STAGE_TBOOT, checkm8_current_cpid(), "TETHERED", "BOOT DONE");
                    app_log_enqueue("tboot: tethered boot success\r\n");
                    suppress_status_log = true;
                    while (true) { sleep_ms(1000); }
                } else {
                    app_logf("tboot: failed (code=%d)\r\n", boot_ret);
                    app_status_set(APP_STAGE_ERROR, checkm8_current_cpid(), "TBOOT", "FAILED");
                    host_delay_ms(3000);
                }
            } else {
                if (!suppress_status_log) {
                    app_status_set(APP_STAGE_WAIT_DFU, 0, "NO DEVICE", "BTN: MSC");
                }
                sleep_ms(10);
            }
        } else {
            if (evt == BTN_EVENT_LONG_PRESS) {
                erase_all_pteblocks();
                msc_disk_init();
            }

            tud_task_ext(10, false);

            if (msc_disk_pteblock_written()) {
                tud_deinit(0);
                sleep_ms(50);

                msc_disk_do_persist();

                sleep_ms(50);
                tud_init(0);

                app_status_set(APP_STAGE_DEVICE, 0, "PTEBLOCK", "SAVED OK");
                msc_disk_clear_written_flag();
            }

            if (msc_disk_erase_requested()) {
                erase_all_pteblocks();
                msc_disk_init();
            }
        }
    }
}

int main(void) {
    set_sys_clock_khz(120000, true);
    stdio_init_all();
    app_log_init();

    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);

    multicore_launch_core1(ui_core_main);
    sleep_ms(50);

    app_status_set(APP_STAGE_BOOT, 0, "PSRAM", "INIT");
    if (psram_init()) {
        app_status_set(APP_STAGE_BOOT, 0, "LOADING RES", "");
        app_status_set_progress(1);
        psram_load_resources();
        app_status_set(APP_STAGE_BOOT, 0, "RESOURCES", "LOADED");
    } else {
        app_status_set(APP_STAGE_BOOT, 0, "NO PSRAM", "FLASH XIP");
        psram_pteblock_load_from_flash();
    }

    app_log_enqueue("boot: starting native USB host\r\n");
    tuh_init(USB_HOST_RHPORT);
    checkm8_init();

    top_level_loop();
}

void tuh_mount_cb(uint8_t daddr) {
    uint16_t vid = 0;
    uint16_t pid = 0;

    tuh_vid_pid_get(daddr, &vid, &pid);

    app_logf("host: device mounted addr=%u speed=%s vid=0x%04x pid=0x%04x%s\r\n",
             daddr,
             speed_name(tuh_speed_get(daddr)),
             vid,
             pid,
             is_apple_dfu(vid, pid) ? " Apple DFU candidate" : "");

    if (is_apple_dfu(vid, pid)) {
        mounted_vid = vid;
        mounted_pid = pid;
        mounted_dfu_addr = daddr;
        dfu_mount_epoch++;
    } else if (is_apple_recovery(vid, pid)) {
        mounted_vid = vid;
        mounted_pid = pid;
        mounted_recovery_addr = daddr;
    } else if (is_pongo(vid, pid)) {
        mounted_vid = vid;
        mounted_pid = pid;
        mounted_pongo_addr = daddr;
        pongo_mount_epoch++;
    }
}

void tuh_umount_cb(uint8_t daddr) {
    app_logf("host: device removed addr=%u\r\n", daddr);

    usbh_dpram->epx_buf_ctrl = 0;

    if (mounted_dfu_addr == daddr) {
        mounted_dfu_addr = 0;
        mounted_vid = 0;
        mounted_pid = 0;
    }
    if (mounted_recovery_addr == daddr) {
        mounted_recovery_addr = 0;
        mounted_vid = 0;
        mounted_pid = 0;
    }
    if (mounted_pongo_addr == daddr) {
        mounted_pongo_addr = 0;
        mounted_vid = 0;
        mounted_pid = 0;
    }
}
