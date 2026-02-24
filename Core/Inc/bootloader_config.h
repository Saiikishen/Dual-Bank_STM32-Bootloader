// Author: Saiikishen



#ifndef BOOTLOADER_CONFIG_H
#define BOOTLOADER_CONFIG_H

#include <stdint.h>

/* ─── Internal Flash ─────────────────────────────────────────────── */
#define APP_FLASH_ADDR          0x08008000UL
#define APP_MAX_SIZE            (960UL * 1024UL)
#define APP_FIRST_SECTOR        FLASH_SECTOR_2
#define APP_SECTOR_COUNT        10U

/* ─── External Flash Layout ──────────────────────────────────────── */
#define EXT_BACKUP_ADDR         0x000000UL
#define EXT_OTA_ADDR            0x100000UL
#define EXT_META_ADDR           0x200000UL

/* ─── OTA UART Protocol ──────────────────────────────────────────── */
#define OTA_PACKET_HEADER       0xA5U
#define OTA_CMD_START           0x01U
#define OTA_CMD_DATA            0x02U
#define OTA_CMD_END             0x03U
#define OTA_ACK                 0x06U
#define OTA_NAK                 0x15U
#define OTA_UART_TIMEOUT_MS     5000U
#define OTA_PACKET_DATA_SIZE    256U

/* ─── OTA Trigger ────────────────────────────────────────────────── */
#define OTA_TRIGGER_PORT        GPIOA
#define OTA_TRIGGER_PIN         GPIO_PIN_0

/* ─── Metadata Magic ─────────────────────────────────────────────── */
#define BOOT_META_MAGIC         0xB007AB1EUL

/* ─── Boot States ────────────────────────────────────────────────── */
typedef enum {
    BOOT_STATE_NORMAL    = 0x00U,  /* Healthy — jump to app           */
    BOOT_STATE_OTA_REQ   = 0x01U,  /* OTA triggered, backup pending   */
    BOOT_STATE_BACKUP_OK = 0x02U,  /* V1 backed up, receive V2        */
    BOOT_STATE_ROLLBACK  = 0x04U,  /* Restore V1 to internal flash    */
} BootState_t;

/* ─── Metadata (persisted to ext flash) ──────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t    magic;
    BootState_t state;
    uint32_t    app_size;
    uint32_t    app_crc32;
    uint32_t    backup_size;
    uint32_t    backup_crc32;
    uint8_t     reserved[8];
} BootMeta_t;

#endif /* BOOTLOADER_CONFIG_H */
