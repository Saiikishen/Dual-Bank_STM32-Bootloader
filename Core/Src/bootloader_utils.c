/* Author: Saiikishen*/



#include "bootloader_utils.h"
#include "stm32f4xx_hal.h"
#include <string.h>


/* ── CRC32 ───────────────────────────────────────────────────────── */
uint32_t BL_CRC32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0; b < 8U; b++)
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320UL) : (crc >> 1U);
    }
    return ~crc;
}

/* ── Internal Flash Erase ────────────────────────────────────────── */
int BL_EraseAppSectors(void)
{
    HAL_FLASH_Unlock();
    FLASH_EraseInitTypeDef erase = {
        .TypeErase    = FLASH_TYPEERASE_SECTORS,
        .VoltageRange = FLASH_VOLTAGE_RANGE_3,
        .Sector       = APP_FIRST_SECTOR,
        .NbSectors    = APP_SECTOR_COUNT,
    };
    uint32_t err = 0xFFFFFFFFUL;
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&erase, &err);
    HAL_FLASH_Lock();
    return (st == HAL_OK && err == 0xFFFFFFFFUL) ? 0 : -1;
}

/* ── Internal Flash Write ────────────────────────────────────────── */
int BL_WriteInternalFlash(uint32_t dest_addr, const uint8_t *src, uint32_t len)
{
    HAL_FLASH_Unlock();
    int ret = 0;
    for (uint32_t i = 0; i < len; i++) {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_BYTE,
                              dest_addr + i,
                              (uint64_t)src[i]) != HAL_OK) {
            ret = -1;
            break;
        }
    }
    HAL_FLASH_Lock();
    return ret;
}

/* ── Metadata ────────────────────────────────────────────────────── */
int BL_MetaRead(W25Q64FV_Handle *ext, BootMeta_t *meta)
{
    if (W25Q64FV_Read(ext, EXT_META_ADDR,
                      (uint8_t *)meta, sizeof(BootMeta_t)) != W25Q64FV_OK)
        return -1;
    if (meta->magic != BOOT_META_MAGIC) {
        BL_MetaDefault(meta);
        return -2;
    }
    return 0;
}

int BL_MetaWrite(W25Q64FV_Handle *ext, const BootMeta_t *meta)
{
    if (W25Q64FV_SectorErase(ext, EXT_META_ADDR) != W25Q64FV_OK) return -1;
    if (W25Q64FV_Write(ext, EXT_META_ADDR,
                       (const uint8_t *)meta, sizeof(BootMeta_t)) != W25Q64FV_OK)
        return -1;
    return 0;
}

void BL_MetaDefault(BootMeta_t *meta)
{
    memset(meta, 0, sizeof(BootMeta_t));
    meta->magic = BOOT_META_MAGIC;
    meta->state = BOOT_STATE_NORMAL;
}

/* ── Jump to Application ─────────────────────────────────────────── */
void BL_JumpToApp(uint32_t app_addr)
{
    uint32_t sp = *(volatile uint32_t *)app_addr;
    if ((sp & 0x2FF00000UL) != 0x20000000UL) return;

    typedef void (*pFunc)(void);
    uint32_t reset_handler = *(volatile uint32_t *)(app_addr + 4U);
    pFunc jump = (pFunc)reset_handler;


    HAL_RCC_DeInit();
    HAL_DeInit();
    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL  = 0U;

    for (uint8_t i = 0; i < 8U; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }

    SCB->VTOR = app_addr;
    __DSB();
    __ISB();
    __enable_irq();

    __set_MSP(sp);
    jump();
}


/* Scan flash backwards in 256B chunks to find last non-0xFF byte */
uint32_t measure_app_size(void)
{
    const uint8_t *flash = (const uint8_t *)APP_FLASH_ADDR;
    uint32_t size = APP_MAX_SIZE;

    while (size > 256U) {
        const uint8_t *chunk = flash + size - 256U;
        int all_ff = 1;
        for (int i = 0; i < 256; i++) {
            if (chunk[i] != 0xFFU) { all_ff = 0; break; }
        }
        if (!all_ff) break;
        size -= 256U;
    }
    /* Round up to nearest 4KB sector boundary */
    size = ((size + 0xFFFU) & ~0xFFFU);
    return size;
}

