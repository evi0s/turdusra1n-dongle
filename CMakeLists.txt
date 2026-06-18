cmake_minimum_required(VERSION 3.13)

set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/third_party/pico-sdk" CACHE PATH "Local Pico SDK")
set(PICO_BOARD_HEADER_DIRS "${CMAKE_CURRENT_LIST_DIR}/boards" CACHE STRING "Local board definitions")
set(PICO_BOARD "tethered_booter_rp2350b" CACHE STRING "Target board")

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(tethered_booter_rp2350 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()


set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PONGO_PAYLOAD_ASM "${GENERATED_DIR}/pongo_payload.S")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/src/pongo_payload.S.in"
    "${PONGO_PAYLOAD_ASM}"
    @ONLY
)

add_executable(tethered_booter
    src/app_log.c
    src/checkm8.c
    src/main.c
    src/msc_disk.c
    src/oled_sh1106.c
    src/pongo_loader.c
    src/pongo_shell.c
    src/psram.c
    src/usb_descriptors.c
    src/sfe_psram.c
    "${PONGO_PAYLOAD_ASM}"
)

target_include_directories(tethered_booter PRIVATE
    src
    src/include
)

target_link_libraries(tethered_booter PRIVATE
    pico_stdlib
    pico_multicore
    pico_unique_id
    pico_util
    pico_flash
    hardware_adc
    hardware_clocks
    hardware_flash
    hardware_i2c
    hardware_spi
    hardware_sync
    hardware_xip_cache
    tinyusb_device
    tinyusb_host
)

pico_enable_stdio_uart(tethered_booter 1)
pico_enable_stdio_usb(tethered_booter 0)
pico_add_extra_outputs(tethered_booter)
