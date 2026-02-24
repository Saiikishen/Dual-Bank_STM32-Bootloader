/*
 * bootloader_utils.h
 *
 *  Created on: Feb 23, 2026
 *      Author: saiikishen
 *      @brief CRC32, internal flash write, and metadata helpers.
 */

#ifndef BOOTLOADER_UTILS_H
#define BOOTLOADER_UTILS_H

#include <stdint.h>
#include "bootloader_config.h"
#include "w25q64fv.h"

/* CRC32 */
uint32_t BL_CRC32(const uint8_t *data, uint32_t len);

/* Internal Flash */
int BL_EraseAppSectors(void);
int BL_WriteInternalFlash(uint32_t dest_addr, const uint8_t *src, uint32_t len);

/* External Flash Metadata */
int  BL_MetaRead(W25Q64FV_Handle *ext, BootMeta_t *meta);
int  BL_MetaWrite(W25Q64FV_Handle *ext, const BootMeta_t *meta);
void BL_MetaDefault(BootMeta_t *meta);

/* Jump */
void BL_JumpToApp(uint32_t app_addr);

uint32_t measure_app_size(void);


/* ❌ BL_ArmIWDG / BL_RefreshIWDG / BL_WasIWDGReset — removed */

#endif /* BOOTLOADER_UTILS_H */
