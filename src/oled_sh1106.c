#include "oled_sh1106.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "app_log.h"
#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define BATTERY_ADC_PIN 42
#define BATTERY_ADC_CHANNEL 2

#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_PAGES (OLED_HEIGHT / 8)
#define OLED_COLUMN_OFFSET 2

#if PICO_DEFAULT_I2C == 0
#define OLED_I2C i2c0
#else
#define OLED_I2C i2c1
#endif

static uint8_t framebuffer[OLED_WIDTH * OLED_PAGES];
static bool oled_ready;

static bool oled_write(uint8_t control, uint8_t const *data, size_t len) {
    uint8_t buf[17];

    while (len != 0) {
        size_t chunk = len;
        if (chunk > sizeof(buf) - 1) {
            chunk = sizeof(buf) - 1;
        }

        buf[0] = control;
        memcpy(&buf[1], data, chunk);
        if (i2c_write_blocking(OLED_I2C, TETHERED_BOOTER_OLED_I2C_ADDR, buf, (int)(chunk + 1), false) < 0) {
            return false;
        }

        data += chunk;
        len -= chunk;
    }

    return true;
}

static bool oled_cmd(uint8_t cmd) {
    return oled_write(0x00, &cmd, 1);
}

static bool oled_cmd2(uint8_t a, uint8_t b) {
    uint8_t cmd[2] = {a, b};
    return oled_write(0x00, cmd, sizeof(cmd));
}

static void fb_clear(void) {
    memset(framebuffer, 0, sizeof(framebuffer));
}

static void fb_pixel(int x, int y, bool on) {
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    uint8_t *p = &framebuffer[(y / 8) * OLED_WIDTH + x];
    uint8_t mask = (uint8_t)(1u << (y & 7));
    if (on) {
        *p |= mask;
    } else {
        *p &= (uint8_t)~mask;
    }
}

static void glyph_rows(char ch, uint8_t rows[5]) {
    memset(rows, 0, 5);

    switch ((char)toupper((unsigned char)ch)) {
    case 'A': rows[0] = 2; rows[1] = 5; rows[2] = 7; rows[3] = 5; rows[4] = 5; break;
    case 'B': rows[0] = 6; rows[1] = 5; rows[2] = 6; rows[3] = 5; rows[4] = 6; break;
    case 'C': rows[0] = 3; rows[1] = 4; rows[2] = 4; rows[3] = 4; rows[4] = 3; break;
    case 'D': rows[0] = 6; rows[1] = 5; rows[2] = 5; rows[3] = 5; rows[4] = 6; break;
    case 'E': rows[0] = 7; rows[1] = 4; rows[2] = 6; rows[3] = 4; rows[4] = 7; break;
    case 'F': rows[0] = 7; rows[1] = 4; rows[2] = 6; rows[3] = 4; rows[4] = 4; break;
    case 'G': rows[0] = 3; rows[1] = 4; rows[2] = 5; rows[3] = 5; rows[4] = 3; break;
    case 'H': rows[0] = 5; rows[1] = 5; rows[2] = 7; rows[3] = 5; rows[4] = 5; break;
    case 'I': rows[0] = 7; rows[1] = 2; rows[2] = 2; rows[3] = 2; rows[4] = 7; break;
    case 'J': rows[0] = 1; rows[1] = 1; rows[2] = 1; rows[3] = 5; rows[4] = 2; break;
    case 'K': rows[0] = 5; rows[1] = 5; rows[2] = 6; rows[3] = 5; rows[4] = 5; break;
    case 'L': rows[0] = 4; rows[1] = 4; rows[2] = 4; rows[3] = 4; rows[4] = 7; break;
    case 'M': rows[0] = 5; rows[1] = 7; rows[2] = 7; rows[3] = 5; rows[4] = 5; break;
    case 'N': rows[0] = 5; rows[1] = 7; rows[2] = 7; rows[3] = 7; rows[4] = 5; break;
    case 'O': rows[0] = 2; rows[1] = 5; rows[2] = 5; rows[3] = 5; rows[4] = 2; break;
    case 'P': rows[0] = 6; rows[1] = 5; rows[2] = 6; rows[3] = 4; rows[4] = 4; break;
    case 'Q': rows[0] = 2; rows[1] = 5; rows[2] = 5; rows[3] = 7; rows[4] = 3; break;
    case 'R': rows[0] = 6; rows[1] = 5; rows[2] = 6; rows[3] = 5; rows[4] = 5; break;
    case 'S': rows[0] = 3; rows[1] = 4; rows[2] = 2; rows[3] = 1; rows[4] = 6; break;
    case 'T': rows[0] = 7; rows[1] = 2; rows[2] = 2; rows[3] = 2; rows[4] = 2; break;
    case 'U': rows[0] = 5; rows[1] = 5; rows[2] = 5; rows[3] = 5; rows[4] = 7; break;
    case 'V': rows[0] = 5; rows[1] = 5; rows[2] = 5; rows[3] = 5; rows[4] = 2; break;
    case 'W': rows[0] = 5; rows[1] = 5; rows[2] = 7; rows[3] = 7; rows[4] = 5; break;
    case 'X': rows[0] = 5; rows[1] = 5; rows[2] = 2; rows[3] = 5; rows[4] = 5; break;
    case 'Y': rows[0] = 5; rows[1] = 5; rows[2] = 2; rows[3] = 2; rows[4] = 2; break;
    case 'Z': rows[0] = 7; rows[1] = 1; rows[2] = 2; rows[3] = 4; rows[4] = 7; break;
    case '0': rows[0] = 7; rows[1] = 5; rows[2] = 5; rows[3] = 5; rows[4] = 7; break;
    case '1': rows[0] = 2; rows[1] = 6; rows[2] = 2; rows[3] = 2; rows[4] = 7; break;
    case '2': rows[0] = 6; rows[1] = 1; rows[2] = 2; rows[3] = 4; rows[4] = 7; break;
    case '3': rows[0] = 6; rows[1] = 1; rows[2] = 2; rows[3] = 1; rows[4] = 6; break;
    case '4': rows[0] = 5; rows[1] = 5; rows[2] = 7; rows[3] = 1; rows[4] = 1; break;
    case '5': rows[0] = 7; rows[1] = 4; rows[2] = 6; rows[3] = 1; rows[4] = 6; break;
    case '6': rows[0] = 3; rows[1] = 4; rows[2] = 6; rows[3] = 5; rows[4] = 7; break;
    case '7': rows[0] = 7; rows[1] = 1; rows[2] = 2; rows[3] = 4; rows[4] = 4; break;
    case '8': rows[0] = 7; rows[1] = 5; rows[2] = 7; rows[3] = 5; rows[4] = 7; break;
    case '9': rows[0] = 7; rows[1] = 5; rows[2] = 7; rows[3] = 1; rows[4] = 6; break;
    case ':': rows[1] = 2; rows[3] = 2; break;
    case '.': rows[4] = 2; break;
    case '-': rows[2] = 7; break;
    case '_': rows[4] = 7; break;
    case '/': rows[0] = 1; rows[1] = 1; rows[2] = 2; rows[3] = 4; rows[4] = 4; break;
    case '[': rows[0] = 6; rows[1] = 4; rows[2] = 4; rows[3] = 4; rows[4] = 6; break;
    case ']': rows[0] = 3; rows[1] = 1; rows[2] = 1; rows[3] = 1; rows[4] = 3; break;
    case '%': rows[0] = 5; rows[1] = 1; rows[2] = 2; rows[3] = 4; rows[4] = 5; break;
    default: break;
    }
}

static void draw_char(int x, int y, char ch, int scale) {
    uint8_t rows[5];

    glyph_rows(ch, rows);
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 3; col++) {
            if ((rows[row] & (1u << (2 - col))) == 0) {
                continue;
            }

            for (int yy = 0; yy < scale; yy++) {
                for (int xx = 0; xx < scale; xx++) {
                    fb_pixel(x + col * scale + xx, y + row * scale + yy, true);
                }
            }
        }
    }
}

static void draw_text(int x, int y, char const *text, int scale) {
    int step = 4 * scale;

    for (size_t i = 0; text && text[i] != 0; i++) {
        if (x > OLED_WIDTH - (3 * scale)) {
            break;
        }
        draw_char(x, y, text[i], scale);
        x += step;
    }
}

static bool oled_flush(void) {
    if (!oled_ready) {
        return false;
    }

    for (uint8_t page = 0; page < OLED_PAGES; page++) {
        if (!oled_cmd((uint8_t)(0xB0 | page)) ||
            !oled_cmd((uint8_t)(0x00 | (OLED_COLUMN_OFFSET & 0x0f))) ||
            !oled_cmd((uint8_t)(0x10 | (OLED_COLUMN_OFFSET >> 4))) ||
            !oled_write(0x40, &framebuffer[page * OLED_WIDTH], OLED_WIDTH)) {
            oled_ready = false;
            return false;
        }
    }

    return true;
}

static uint32_t battery_mv(void) {
    adc_select_input(BATTERY_ADC_CHANNEL);
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) {
        sum += adc_read();
    }
    uint32_t mv = sum * 3564 * 2 / (4095 * 16);
    return mv;
}

static uint8_t battery_percent(void) {
    uint32_t mv = battery_mv();

    static const struct { uint16_t mv; uint8_t pct; } lut[] = {
        { 4200, 100 },
        { 4150,  95 },
        { 4110,  90 },
        { 4080,  85 },
        { 4020,  80 },
        { 3980,  70 },
        { 3950,  60 },
        { 3910,  50 },
        { 3870,  40 },
        { 3830,  30 },
        { 3790,  20 },
        { 3700,  15 },
        { 3600,  10 },
        { 3500,   5 },
        { 3300,   1 },
        { 3000,   0 },
    };
    static const int lut_size = sizeof(lut) / sizeof(lut[0]);

    if (mv >= lut[0].mv) return lut[0].pct;
    if (mv <= lut[lut_size - 1].mv) return lut[lut_size - 1].pct;

    for (int i = 0; i < lut_size - 1; i++) {
        if (mv >= lut[i + 1].mv) {
            uint32_t range_mv = lut[i].mv - lut[i + 1].mv;
            uint32_t range_pct = lut[i].pct - lut[i + 1].pct;
            return (uint8_t)(lut[i + 1].pct + (mv - lut[i + 1].mv) * range_pct / range_mv);
        }
    }
    return 0;
}

static void draw_battery_icon(int x, int y, uint8_t pct) {
    for (int i = 0; i < 14; i++) {
        fb_pixel(x + i, y, true);
        fb_pixel(x + i, y + 4, true);
    }
    for (int j = 0; j <= 4; j++) {
        fb_pixel(x, y + j, true);
        fb_pixel(x + 13, y + j, true);
    }
    fb_pixel(x + 14, y + 1, true);
    fb_pixel(x + 14, y + 2, true);
    fb_pixel(x + 14, y + 3, true);

    int fill = (int)pct * 12 / 100;
    for (int i = 0; i < fill; i++) {
        for (int j = 1; j <= 3; j++) {
            fb_pixel(x + 1 + i, y + j, true);
        }
    }
}

bool oled_sh1106_init(void) {
    adc_init();
    adc_gpio_init(BATTERY_ADC_PIN);

    i2c_init(OLED_I2C, 400000);
    gpio_set_function(PICO_DEFAULT_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(PICO_DEFAULT_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(PICO_DEFAULT_I2C_SDA_PIN);
    gpio_pull_up(PICO_DEFAULT_I2C_SCL_PIN);
    sleep_ms(50);

    oled_ready =
        oled_cmd(0xAE) &&
        oled_cmd2(0xD5, 0x80) &&
        oled_cmd2(0xA8, 0x3F) &&
        oled_cmd2(0xD3, 0x00) &&
        oled_cmd(0x40) &&
        oled_cmd2(0xAD, 0x8B) &&
        oled_cmd(0xA1) &&
        oled_cmd(0xC8) &&
        oled_cmd2(0xDA, 0x12) &&
        oled_cmd2(0x81, 0x7F) &&
        oled_cmd2(0xD9, 0xF1) &&
        oled_cmd2(0xDB, 0x40) &&
        oled_cmd(0xA4) &&
        oled_cmd(0xA6) &&
        oled_cmd(0xAF);

    if (oled_ready) {
        fb_clear();
        oled_flush();
    }

    return oled_ready;
}

void oled_sh1106_draw_status(void) {
    app_status_t status;
    char info_line[32];

    if (!oled_ready) {
        return;
    }

    app_status_get(&status);

    char line_cpid[24];
    char line_ecid[24];

    if (status.cpid != 0) {
        snprintf(line_cpid, sizeof(line_cpid), "CPID:%04X", status.cpid);
    } else {
        snprintf(line_cpid, sizeof(line_cpid), "USB MSC ");
    }

    if (status.ecid != 0) {
        snprintf(line_ecid, sizeof(line_ecid), "ECID:%llu",
                 (unsigned long long)(status.ecid % 1000000));
    } else {
        line_ecid[0] = 0;
    }

    fb_clear();
    draw_text(0, 0, "TETHERED BOOTER", 1);

    uint8_t batt = battery_percent();
    char batt_str[8];
    snprintf(batt_str, sizeof(batt_str), "%u%%", batt);
    draw_battery_icon(92, 0, batt);
    int batt_text_x = OLED_WIDTH - (int)strlen(batt_str) * 4;
    draw_text(batt_text_x, 0, batt_str, 1);

    draw_text(0, 10, app_stage_name(status.stage), 2);
    draw_text(0, 25, status.line1, 2);
    if (status.progress > 0) {
        for (int x = 0; x < 128; x++) {
            fb_pixel(x, 40, true);
            fb_pixel(x, 49, true);
        }
        for (int y = 41; y < 49; y++) {
            fb_pixel(0, y, true);
            fb_pixel(127, y, true);
        }
        int fill_w = (int)status.progress * 126 / 100;
        for (int y = 41; y < 49; y++) {
            for (int x = 1; x <= fill_w; x++) {
                fb_pixel(x, y, true);
            }
        }
    } else {
        draw_text(0, 40, status.line2, 2);
    }
    draw_text(0, 56, line_cpid, 1);
    if (line_ecid[0]) {
        draw_text(64, 56, line_ecid, 1);
    }
    oled_flush();
}
