# turdusra1n boot dongle

A checkm8 tethered boot dongle for A9 iPhones (iPhone 6s/6s+/SE 1st gen) built on the RP2350B microcontroller.

Performs the full tethered boot sequence autonomously: checkm8 exploit, PongoOS upload, kernel patchfinder, SEP exploit, and XNU boot — no computer required.

Demo on [reddit](https://www.reddit.com/r/LegacyJailbreak/comments/1u719e4/i_made_a_tethered_boot_dongle/)

> [!WARNING]
> Vibe-coded, use at your own risk!
> 
> Code is provided “AS IS” without warranty

## Features

- Standalone tethered boot — plug in and go
- SH1106 OLED status display with progress bars and battery indicator
- 8MB PSRAM for resource caching (fast re-boots)
- 16MB flash for firmware + all boot resources
- Button for mode switching (host/device)
- Device mode: FAT16 mass storage for multiple pteblock upload

## Hardware

### MCU

- **RP2350B** (QFN-80, dual Cortex-M33 @ 150MHz)
- 16MB QSPI Flash (W25Q128)
- 8MB QSPI PSRAM (APS6404L, CS1 on GP47)
- The PoC uses [Waveshare Core2350B](https://www.waveshare.com/wiki/Core2350B0)

### GPIO Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| **USB D+** | Native | RP2350 native USB |
| **USB D-** | Native | RP2350 native USB |
| **OLED SDA** | GP10 | I2C1, 400kHz |
| **OLED SCL** | GP7 | I2C1, 400kHz |
| **UART TX** | GP30 | UART0, 115200 baud |
| **UART RX** | GP29 | UART0, 115200 baud |
| **Button** | GP17 | Active-low, internal pull-up |
| **Battery ADC** | GP42 | ADC channel 2, via 220K/220K voltage divider |
| **PSRAM CS** | GP47 | QMI CS1 |

### Power

A lithium battery charge/boost converter module is required to manage charging and boost the Li-ion cell output to 5V for the USB VBUS (to power the iPhone in DFU mode).

### Battery Monitoring

Li-ion battery connected via a 220K/220K resistive voltage divider to GP42 (ADC2). The ADC reads half the battery voltage; firmware applies a calibrated multiplier to compensate for the high-impedance source.

### OLED Display

SH1106 128x64 I2C OLED at address `0x3C`. Shows:
- Current stage and status text
- Graphical progress bar during uploads
- Battery icon and percentage (top-right)

### Wiring Diagram

```
                 RP2350B
              +-----------+
    OLED SDA -| GP10      |
    OLED SCL -| GP7       |
      Button -| GP17      |
    UART  TX -| GP30      |
    UART  RX -| GP29      |
    Batt ADC -| GP42      |
              |       USB |-- iPhone (host mode) / PC or Mac (MSC mode)
              +-----------+

    Battery ----[220K]----+----[220K]---- GND
                          |
                        GP42
```

## Pre-built Firmware

CI builds run automatically on every push/PR via GitHub Actions. Download the latest `tethered_booter.uf2` from the [Actions](../../actions) tab — no local toolchain needed.

You still need to flash runtime resources separately (see [Flash Resources](#flash-resources) below).

## Building from Source

### Prerequisites

- [ARM GCC toolchain](https://developer.arm.com/downloads/-/gnu-rm) (`arm-none-eabi-gcc`)
- CMake >= 3.13
- [picotool](https://github.com/raspberrypi/picotool) (for flashing resources)

### Clone

```bash
git clone --recursive https://github.com/evi0s/turdusra1n-dongle.git
cd turdusra1n-dongle
```

If you already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

> **Note:** Only the `lib/tinyusb` submodule inside pico-sdk is required for this project. You can selectively initialize it with:
> ```bash
> cd third_party/pico-sdk && git submodule update --init lib/tinyusb
> ```

### Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Output: `build/tethered_booter.uf2`

### Flash Firmware

1. Hold BOOTSEL on the RP2350B and connect to PC via USB
2. Copy `build/tethered_booter.uf2` to the mounted UF2 drive

### Flash Resources

Runtime resources are stored in dedicated flash regions. Download them from [sep.lol](https://sep.lol):

- `kpf.bin` — Kernel patchfinder
- `cpf.bin` — CoreTrust patchfinder
- `sep_racer.bin` — SEP exploit
- `overlay.bin` — Filesystem overlay
- `union.bin` — Union filesystem

Place them in the `resource/` directory, then flash with the device in BOOTSEL mode:

```bash
./flash_resources.sh
```

Requires `picotool`. See the script for exact flash offsets.

## Operating Modes

### Host Mode (default)

On power-up, the dongle enables USB host mode and waits for an iPhone in DFU mode. Once detected:

1. **checkm8** — exploit stages (reset, setup, spray, overwrite)
2. **PongoOS** — upload and boot the PongoOS payload
3. **Tethered boot** — upload sep_racer, kpf, overlay, union; execute boot sequence

### Device Mode

Press the button and the dongle will switch to USB device mode and exposes a FAT16 mass storage drive. Drop a pteblock file onto it to persist the SEP pairing ticket for subsequent boots.

Multiple pteblocks can be stored — the dongle automatically selects the correct one by matching the ECID of the connected device.

The pteblock file is produced by turdusra1n during the tethered downgrade process. The filename must be the last 6 digits of the device ECID with a `.BIN` extension:

```bash
# Example: ECID is 123456789012345, file is 123456789012345-iPhone8,2-current-pteblock2.bin
cp 123456789012345-iPhone8,2-current-pteblock2.bin /Volumes/TETHERED/012345.BIN
```

To erase all stored pteblocks, long-press the button (2s) while in device mode.

## Project Structure

```
.
├── CMakeLists.txt
├── boards/
│   └── tethered_booter_rp2350b.h    # Board pin definitions
├── src/
│   ├── main.c                        # State machine, mode detection
│   ├── checkm8.c                     # checkm8 exploit implementation
│   ├── pongo_loader.c                # DFU image upload (PongoOS)
│   ├── pongo_shell.c                 # Tethered boot sequence
│   ├── psram.c                       # PSRAM init + resource preloading
│   ├── msc_disk.c                    # FAT16 RAM disk (device mode)
│   ├── oled_sh1106.c                 # OLED display driver
│   ├── app_log.c                     # Logging + status (cross-core safe)
│   ├── usb_descriptors.c             # USB device descriptors
│   ├── sfe_psram.c                   # PSRAM QMI driver (from SparkFun)
│   └── tusb_config.h                 # TinyUSB configuration
├── resource/                          # Boot resources (not in git)
├── scripts/                           # Development/debug scripts
└── third_party/
    └── pico-sdk/                      # Raspberry Pi Pico SDK (submodule)
```


## Credits

This project would not exist without **[turdus merula](https://sep.lol)**. It provides the complete tethered downgrade toolchain including turdusra1n (the host-side exploit and boot tool), PongoOS modules (kpf, cpf), the SEP exploit (sep_racer), and all runtime resources needed for tethered boot. This dongle is essentially a standalone hardware reimplementation of the turdusra1n boot flow.

- [checkm8](https://github.com/axi0mX/ipwndfu) — A9 SecureROM exploit
- [PongoOS](https://github.com/checkra1n/PongoOS) — Pre-boot execution environment
- [Pico SDK](https://github.com/raspberrypi/pico-sdk) — RP2350 SDK
- [SparkFun Pico](https://github.com/sparkfun/sparkfun-pico) — PSRAM driver (MIT license)
- [gaster](https://github.com/0x7ff/gaster) — Reference checkm8 implementation

## License

This project is dual-licensed:

Noncommercial use: [PolyForm Noncommercial 1.0.0](LICENSE) — hobbyists, students, makerspaces, community projects, academic research, and personal tinkering can use, modify, and share this freely.

Commercial use is not permitted without a separate written license from the author.

Commercial use includes, but is not limited to:
- manufacturing or selling devices using this firmware
- bundling this firmware with a paid product
- using this firmware in a revenue-generating service
- using this firmware internally to support a commercial product or service
- modifying the project to create a commercial derivative product

### Third-party licenses

| Component | License | Path / Notes |
|-----------|---------|--------------|
| [Pico SDK](https://github.com/raspberrypi/pico-sdk) | BSD 3-Clause | `third_party/pico-sdk/` (submodule) |
| [TinyUSB](https://github.com/hathach/tinyusb) | MIT | `third_party/pico-sdk/lib/tinyusb/` (SDK submodule) |
| [SparkFun PSRAM driver](https://github.com/sparkfun/sparkfun-pico) | MIT | `src/sfe_psram.c`, `src/sfe_psram.h` |
