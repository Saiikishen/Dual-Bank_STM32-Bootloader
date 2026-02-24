# STM32F407VGT OTA Bootloader with W25Q64FV Backup & Rollback

Production-ready **OTA bootloader** for STM32F407VGT6 using external W25Q64FV (8MB SPI NOR flash) for automatic V1 backup and V2 staging. Supports full rollback on OTA failure. No application code changes required.

## Features

- **Zero App Changes**: App links at `0x08008000` — bootloader sets VTOR before jump.
- **Automatic V1 Backup**: Copies running app to external flash before OTA.
- **Robust UART Protocol**: 256B packets from ESP32, per-packet + full-image CRC32, ACK/NAK retransmit.
- **Power-Loss Safe**: Persistent metadata state machine — resumes from any point.
- **OTA Trigger**: ESP32 GPIO → PA0 + NRST pulse.
- **Error Recovery**: UART timeout/CRC/flash fail → rollback to last good backup.
- **Verified CRC32**: Identical algorithm both sides (`0xEDB88320` poly, `~crc` final).
- **Flash Protected**: RDP Level 1 on bootloader sectors 0-1.

## Memory Map

### Internal Flash (1MB)
| Region | Address | Size | Sectors | Content |
|--------|---------|------|---------|---------|
| Bootloader | `0x08000000` | 32 KB | 0-1 | OTA logic  [Saiikishen/WinbondW25Q64FV_STM32F407VGTdriver](https://github.com/Saiikishen/WinbondW25Q64FV_STM32F407VGTdriver) |
| App Slot | `0x08008000` | 960 KB | 2-11 | V1/V2 active image |

### External W25Q64FV (8MB)
| Region | Address | Size | Purpose |
|--------|---------|------|---------|
| V1 Backup | `0x000000` | 960 KB | Rollback source |
| V2 Staging | `0x100000` | 960 KB | Incoming OTA image |
| Metadata | `0x200000` | 4 KB | State, sizes, CRCs [Saiikishen/WinbondW25Q64FV_STM32F407VGTdriver](https://github.com/Saiikishen/WinbondW25Q64FV_STM32F407VGTdriver) |

## Hardware Wiring (ESP32 → STM32)

| ESP32 Pin | STM32 Pin | Signal | Notes |
|-----------|-----------|--------|-------|
| GPIO17 | PA9 (USART1_TX) | UART TX | ESP → STM32 |
| GPIO16 | PA10 (USART1_RX) | UART RX | STM32 → ESP ACK |
| GPIO25 | PA0 | OTA Trigger | Assert HIGH + NRST |
| GPIO26 | NRST | Reset | 200ms LOW pulse |
| GND | GND | Ground | Common |

**SPI2 (PB12 NSS/PB13 SCK/PB14 MISO/PB15 MOSI)** → W25Q64FV (STM32 side only).

## Protocol (ESP32 → STM32)

```
START:  0xA5 0x01 [size:4BE][img_crc:4BE][pkt_crc:4BE]
DATA:   0xA5 0x02 [len:2BE][data:≤256][pkt_crc:4BE]
END:    0xA5 0x03

STM32:  ACK=0x06 / NAK=0x15 per packet
        0x55 after V1 backup + OTA erase ready
```

CRC32: Poly=`0xEDB88320`, Init=`0xFFFFFFFF`, Final=`~crc` (both sides identical).

## Software Files

| File | Purpose |
|------|---------|
| `bootloader_config.h` | Constants, states, BootMeta_t |
| `bootloader_utils.[hc]` | CRC32, int flash R/W, metadata, JumpToApp |
| `ota_uart.[hc]` | UART protocol, packet handling |
| `main_bootloader.[hc]` | State machine, PA0 trigger |
| `w25q64fv.[hc]` | External SPI flash driver (user-provided) [Saiikishen/WinbondW25Q64FV_STM32F407VGTdriver](https://github.com/Saiikishen/WinbondW25Q64FV_STM32F407VGTdriver) |

**App V1/V2**: Standard projects, ORIGIN=`0x08008000`, `VECT_TAB_OFFSET=0x8000`.

## Setup & Build

### STM32CubeIDE (Bootloader)
```
1. CubeMX: SPI2 PB12-15, USART1 PA9/PA10, PA0 input pull-down
2. Edit linker: FLASH LENGTH=32K
3. Add files to Core/Src/Inc
4. Build → BootloaderWithFlash.bin
5. ST-LINK Utility: 0x08000000
```


### ESP32 Arduino
```
1. Install LittleFS plugin
2. data/fw_v1.bin + fw_v2.bin → Upload LittleFS
3. Upload ESP32_OTA_Sender.ino
4. Serial Monitor: '1'/'2' → OTA
```

## Testing

```
1. Flash bootloader → power cycle → jumps to V1 (LED ✓)
2. ESP32 '2' → OTA V2 (LED pattern changes ✓)
3. ESP32 '1' → rollback V1 (LED pattern reverts ✓)
4. Power-loss mid-OTA → resumes ✓
```
## App V1/V2 Changes

### 1. `STM32F407VGTX_FLASH.ld` (Core → Linker Script)

```ld
MEMORY {
  FLASH (rx) : ORIGIN = 0x08008000, LENGTH = 960K   /* App slot */
  RAM   (xrw): ORIGIN = 0x20000000, LENGTH = 128K
}
```

### 2. `Core/Src/system_stm32f4xx.c` — Enable + Set Offset

```c
#if defined(USER_VECT_TAB_ADDRESS)
/* #define VECT_TAB_SRAM */  /* Keep commented */
#define VECT_TAB_BASE_ADDRESS   FLASH_BASE

#if !defined(VECT_TAB_OFFSET)
#define VECT_TAB_OFFSET         0x00008000U   /* 32KB offset */
#endif /* VECT_TAB_OFFSET */
#endif /* USER_VECT_TAB_ADDRESS */
```

**Project Defines** (Properties → C/C++ Build → MCU GCC Compiler → Preprocessor → Add):
```
USER_VECT_TAB_ADDRESS
```


| Symptom | Cause | Fix |
|---------|-------|-----|
| No V1 after bootloader | App linker `0x08000000` | ORIGIN=`0x08008000` |
| No V2 after OTA | `VECT_TAB_OFFSET=0x0` | `0x00008000U` |
| START timeout | ESP32 sends too early | `0x55` READY handshake |
| CRC mismatch | Poly/init/final differ | Self-test `0xCBF43926` both sides |
| SPI init fail | PB12-15 wrong | CubeMX SPI2 NSS pin shall be configured as  GPIO_Output with the user label FLASH_CS.|

