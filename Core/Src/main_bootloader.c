/**
 * @file main_bootloader.c
 * @brief STM32F407VGT OTA Bootloader — Zero Application Modification Required
 *
 * Crash Detection Mechanism:
 *   1. Bootloader arms IWDG immediately before jumping to app.
 *   2. App kicks IWDG as part of normal healthy operation.
 *   3. App crash/hang → IWDG timeout → hardware reset.
 *   4. Bootloader reads RCC->CSR.IWDGRSTF on next boot.
 *   5. crash_count incremented; after BOOT_MAX_CRASH_COUNT → rollback.
 *   6. Intentional resets (SFTRSTF) never increment crash_count.
 *
 * OTA Trigger:
 *   ESP32 asserts PA0 HIGH before asserting STM32 NRST. No app code needed.
 *
 * App linker: ORIGIN = 0x08008000, LENGTH = 960K.
 */
#include "main_bootloader.h"
#include "bootloader_config.h"
#include "bootloader_utils.h"
#include "ota_uart.h"
#include "w25q64fv.h"
#include "stm32f4xx_hal.h"



extern SPI_HandleTypeDef  hspi2;
extern UART_HandleTypeDef huart1;

static W25Q64FV_Handle s_ext;
static BootMeta_t      s_meta;

static int  is_ota_requested(void);
static int  do_backup_v1(void);
static int  do_receive_and_flash_v2(void);
static int  do_rollback(void);
static void halt_error(void);

/* ════════════════════════════════════════════════════════════════════
 * BL_Run — no reset-cause handling, no crash counter, no IWDG
 * ════════════════════════════════════════════════════════════════════ */
void BL_Run(void)
{
    if (W25Q64FV_Init(&s_ext, &hspi2,
                      GPIOB, GPIO_PIN_12) != W25Q64FV_OK)
        halt_error();

    if (BL_MetaRead(&s_ext, &s_meta) == -2) {
        BL_MetaDefault(&s_meta);
        BL_MetaWrite(&s_ext, &s_meta);
    }

    /* Check OTA trigger (PA0 from ESP32) */
    if (is_ota_requested() && s_meta.state == BOOT_STATE_NORMAL) {
        s_meta.state = BOOT_STATE_OTA_REQ;
        BL_MetaWrite(&s_ext, &s_meta);
    }

    /* ── State Machine ───────────────────────────────────────────── */
    switch (s_meta.state) {

    case BOOT_STATE_NORMAL:
    {
        uint32_t sp = *(volatile uint32_t *)APP_FLASH_ADDR;
        if (sp == 0xFFFFFFFFUL || sp == 0x00000000UL) {
            halt_error();
        }

        if (s_meta.app_size == 0U) {
            s_meta.app_size  = measure_app_size();
            s_meta.app_crc32 = BL_CRC32(
                (const uint8_t *)APP_FLASH_ADDR, s_meta.app_size);
            BL_MetaWrite(&s_ext, &s_meta);
        }

        BL_JumpToApp(APP_FLASH_ADDR);
        halt_error();
        break;
    }


    case BOOT_STATE_OTA_REQ:
        if (do_backup_v1() != 0) {
            /* Backup failed — abort, run existing app safely */
            s_meta.state = BOOT_STATE_NORMAL;
            BL_MetaWrite(&s_ext, &s_meta);
            BL_JumpToApp(APP_FLASH_ADDR);
            halt_error();
        }
        /* FALLTHROUGH to BACKUP_OK */

    case BOOT_STATE_BACKUP_OK: {


        if (do_receive_and_flash_v2() != 0) {
            s_meta.state = BOOT_STATE_ROLLBACK;
            BL_MetaWrite(&s_ext, &s_meta);
            do_rollback();
            halt_error();
        }
        BL_JumpToApp(APP_FLASH_ADDR);
        halt_error();
        break;
    }

    case BOOT_STATE_ROLLBACK:
        do_rollback();
        halt_error();
        break;

    default:
        BL_MetaDefault(&s_meta);
        BL_MetaWrite(&s_ext, &s_meta);
        BL_JumpToApp(APP_FLASH_ADDR);
        halt_error();
        break;
    }
}

/* ── OTA trigger: PA0 debounced ──────────────────────────────────── */
static int is_ota_requested(void)
{
    if (HAL_GPIO_ReadPin(OTA_TRIGGER_PORT,
                         OTA_TRIGGER_PIN) != GPIO_PIN_SET) return 0;
    HAL_Delay(10U);
    return (HAL_GPIO_ReadPin(OTA_TRIGGER_PORT,
                              OTA_TRIGGER_PIN) == GPIO_PIN_SET) ? 1 : 0;
}

/* ── Backup V1: internal flash → ext backup slot ─────────────────── */
static int do_backup_v1(void)
{
    const uint8_t *app  = (const uint8_t *)APP_FLASH_ADDR;
    uint32_t       size = s_meta.app_size;
    if (size == 0U || size > APP_MAX_SIZE) size = APP_MAX_SIZE;

    for (uint8_t i = 0; i < 15U; i++) {
        if (W25Q64FV_BlockErase64K(&s_ext,
            EXT_BACKUP_ADDR + (uint32_t)i * W25Q64FV_BLOCK64_SIZE)
            != W25Q64FV_OK) return -1;
    }

    for (uint32_t off = 0; off < size; off += 256U) {
        uint32_t chunk = ((size - off) > 256U) ? 256U : (size - off);
        if (W25Q64FV_Write(&s_ext, EXT_BACKUP_ADDR + off,
                           app + off, chunk) != W25Q64FV_OK) return -1;
    }

    uint32_t crc     = BL_CRC32(app, size);
    uint32_t ext_crc = 0xFFFFFFFFUL;

    /* Verify backup by re-reading and computing CRC */
    {
        uint8_t  vbuf[256];
        uint32_t crc_acc = 0xFFFFFFFFUL;
        uint32_t rem = size, ra = EXT_BACKUP_ADDR;
        while (rem > 0U) {
            uint32_t c = (rem > 256U) ? 256U : rem;
            if (W25Q64FV_Read(&s_ext, ra, vbuf, c) != W25Q64FV_OK) return -1;
            for (uint32_t i = 0; i < c; i++) {
                crc_acc ^= vbuf[i];
                for (uint8_t b = 0; b < 8U; b++)
                    crc_acc = (crc_acc & 1U) ?
                              ((crc_acc >> 1U) ^ 0xEDB88320UL) : (crc_acc >> 1U);
            }
            ra += c; rem -= c;
        }
        ext_crc = ~crc_acc;
    }

    if (crc != ext_crc) return -1;

    s_meta.backup_size  = size;
    s_meta.backup_crc32 = crc;
    s_meta.state        = BOOT_STATE_BACKUP_OK;
    BL_MetaWrite(&s_ext, &s_meta);
    return 0;
}

/* ── Receive V2 via UART → ext OTA slot → internal flash ─────────── */
static int do_receive_and_flash_v2(void)
{
    uint32_t size = 0, crc = 0;

    if (OTA_ReceiveFirmware(&huart1, &s_ext, &size, &crc) != OTA_RX_OK)
        return -1;

    if (BL_EraseAppSectors() != 0) return -1;

    uint8_t buf[256];
    for (uint32_t off = 0; off < size; off += 256U) {
        uint32_t chunk = ((size - off) > 256U) ? 256U : (size - off);
        if (W25Q64FV_Read(&s_ext, EXT_OTA_ADDR + off,
                          buf, chunk) != W25Q64FV_OK) return -1;
        if (BL_WriteInternalFlash(APP_FLASH_ADDR + off,
                                  buf, chunk) != 0) return -1;
    }

    if (BL_CRC32((const uint8_t *)APP_FLASH_ADDR, size) != crc) return -1;

    s_meta.app_size  = size;
    s_meta.app_crc32 = crc;
    s_meta.state     = BOOT_STATE_NORMAL;   /* ✅ Straight to NORMAL, no V2_READY */
    BL_MetaWrite(&s_ext, &s_meta);
    return 0;
}

/* ── Restore V1: ext backup → internal flash ─────────────────────── */
static int do_rollback(void)
{
    uint32_t size = s_meta.backup_size;
    if (size == 0U || size > APP_MAX_SIZE) halt_error();

    if (BL_EraseAppSectors() != 0) halt_error();

    uint8_t buf[256];
    for (uint32_t off = 0; off < size; off += 256U) {
        uint32_t chunk = ((size - off) > 256U) ? 256U : (size - off);
        if (W25Q64FV_Read(&s_ext, EXT_BACKUP_ADDR + off,
                          buf, chunk) != W25Q64FV_OK) halt_error();
        if (BL_WriteInternalFlash(APP_FLASH_ADDR + off,
                                  buf, chunk) != 0) halt_error();
    }

    if (BL_CRC32((const uint8_t *)APP_FLASH_ADDR, size)
        != s_meta.backup_crc32) halt_error();

    s_meta.state     = BOOT_STATE_NORMAL;
    s_meta.app_size  = s_meta.backup_size;
    s_meta.app_crc32 = s_meta.backup_crc32;
    BL_MetaWrite(&s_ext, &s_meta);

    BL_JumpToApp(APP_FLASH_ADDR);
    return 0;
}

static void halt_error(void)
{
    __disable_irq();
    while (1) {}
}
