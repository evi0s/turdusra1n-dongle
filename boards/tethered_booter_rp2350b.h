/*
 * Local board definition for the RP2350B tethered booter dongle.
 *
 * The SDK includes generic RP2350 boards, but this project needs fixed pins
 * for SH1106 status I2C and the Pico-PIO-USB full-speed host port.
 */

#ifndef _BOARDS_TETHERED_BOOTER_RP2350B_H
#define _BOARDS_TETHERED_BOOTER_RP2350B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

#define TETHERED_BOOTER_RP2350B 1

/* RP2350B, not RP2350A. */
#define PICO_RP2350A 0

/* UART0 debug output: GP30=TX, GP29=RX, 115200 baud. */
#ifndef PICO_DEFAULT_UART
#define PICO_DEFAULT_UART 0
#endif
#ifndef PICO_DEFAULT_UART_TX_PIN
#define PICO_DEFAULT_UART_TX_PIN 30
#endif
#ifndef PICO_DEFAULT_UART_RX_PIN
#define PICO_DEFAULT_UART_RX_PIN 29
#endif
#ifndef PICO_DEFAULT_UART_BAUD_RATE
#define PICO_DEFAULT_UART_BAUD_RATE 115200
#endif

#ifndef PICO_DEFAULT_LED_PIN
#define PICO_DEFAULT_LED_PIN 25
#endif

/* SH1106 128x64 OLED: GP10=SDA, GP7=SCL (I2C1), addr=0x3c. */
#ifndef PICO_DEFAULT_I2C
#define PICO_DEFAULT_I2C 1
#endif
#ifndef PICO_DEFAULT_I2C_SDA_PIN
#define PICO_DEFAULT_I2C_SDA_PIN 10
#endif
#ifndef PICO_DEFAULT_I2C_SCL_PIN
#define PICO_DEFAULT_I2C_SCL_PIN 7
#endif
#define TETHERED_BOOTER_OLED_I2C_ADDR 0x3c

/* Button: GP17, active-low with internal pull-up. */
#define TETHERED_BOOTER_BUTTON_PIN 17

/* Native USB used for both host (iPhone) and device (MSC) modes. */

/* External flash. */
#define PICO_BOOT_STAGE2_CHOOSE_W25Q080 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (16 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (16 * 1024 * 1024)
#endif

/* PSRAM: 8MB QSPI PSRAM on CS1, GP47. */
#define TETHERED_BOOTER_PSRAM_PRESENT 1
#define TETHERED_BOOTER_PSRAM_SIZE_BYTES (8 * 1024 * 1024)
#define TETHERED_BOOTER_PSRAM_CS_PIN 47

pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

#endif
