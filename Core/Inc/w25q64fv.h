/**
 * @file    w25q64fv.h
 * @brief   W25Q64FV 64Mb SPI Flash driver for STM32F407VGT (HAL)
 * @author Saikishen
 *
 * Device  : Winbond W25Q64FV
 * Capacity: 64 Mbit / 8 MB
 * Interface: Standard SPI (Mode 0 / Mode 3), up to 104 MHz
 *
 *
 * Usage
 *   W25Q64FV_Handle flash;
 *   W25Q64FV_Init(&flash, &hspi1, FLASH_CS_GPIO_Port, FLASH_CS_Pin);
 *   W25Q64FV_SectorErase(&flash, 0x000000);
 *   W25Q64FV_Write(&flash, 0x000000, data, sizeof(data));
 *   W25Q64FV_Read(&flash, 0x000000, buf, sizeof(buf));
 */

#ifndef W25Q64FV_H
#define W25Q64FV_H

#include "stm32f4xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

//Memory constants
#define W25Q64FV_FLASH_SIZE        (8UL * 1024 * 1024)
#define W25Q64FV_PAGE_SIZE         256U
#define W25Q64FV_SECTOR_SIZE       (4U   * 1024)
#define W25Q64FV_BLOCK32_SIZE      (32U  * 1024)
#define W25Q64FV_BLOCK64_SIZE      (64U  * 1024)
#define W25Q64FV_PAGE_COUNT        32768U
#define W25Q64FV_SECTOR_COUNT      2048U
#define W25Q64FV_BLOCK_COUNT       128U

//ID
#define W25Q64FV_MANUFACTURER_ID   0xEFU
#define W25Q64FV_JEDEC_MEM_TYPE    0x40U
#define W25Q64FV_JEDEC_CAPACITY    0x17U
#define W25Q64FV_JEDEC_FULL        0x4017U

//SPI instructions
#define W25Q64FV_CMD_WRITE_ENABLE          0x06U
#define W25Q64FV_CMD_VOL_SR_WRITE_ENABLE   0x50U
#define W25Q64FV_CMD_WRITE_DISABLE         0x04U
#define W25Q64FV_CMD_READ_SR1              0x05U
#define W25Q64FV_CMD_READ_SR2              0x35U
#define W25Q64FV_CMD_WRITE_SR              0x01U
#define W25Q64FV_CMD_READ_DATA             0x03U
#define W25Q64FV_CMD_FAST_READ             0x0BU
#define W25Q64FV_CMD_PAGE_PROGRAM          0x02U
#define W25Q64FV_CMD_SECTOR_ERASE_4KB      0x20U
#define W25Q64FV_CMD_BLOCK_ERASE_32KB      0x52U
#define W25Q64FV_CMD_BLOCK_ERASE_64KB      0xD8U
#define W25Q64FV_CMD_CHIP_ERASE            0xC7U
#define W25Q64FV_CMD_ERASE_SUSPEND         0x75U
#define W25Q64FV_CMD_ERASE_RESUME          0x7AU
#define W25Q64FV_CMD_POWER_DOWN            0xB9U
#define W25Q64FV_CMD_RELEASE_POWER_DOWN    0xABU
#define W25Q64FV_CMD_READ_MFR_DEVICE_ID    0x90U
#define W25Q64FV_CMD_JEDEC_ID              0x9FU
#define W25Q64FV_CMD_READ_UNIQUE_ID        0x4BU
#define W25Q64FV_CMD_READ_SFDP             0x5AU
#define W25Q64FV_CMD_ERASE_SECURITY_REG    0x44U
#define W25Q64FV_CMD_PROG_SECURITY_REG     0x42U
#define W25Q64FV_CMD_READ_SECURITY_REG     0x48U
#define W25Q64FV_CMD_ENABLE_QPI            0x38U
#define W25Q64FV_CMD_ENABLE_RESET          0x66U
#define W25Q64FV_CMD_RESET                 0x99U

//Status Register 1
#define W25Q64FV_SR1_BUSY          (1U << 0)
#define W25Q64FV_SR1_WEL           (1U << 1)
#define W25Q64FV_SR1_BP0           (1U << 2)
#define W25Q64FV_SR1_BP1           (1U << 3)
#define W25Q64FV_SR1_BP2           (1U << 4)
#define W25Q64FV_SR1_TB            (1U << 5)
#define W25Q64FV_SR1_SEC           (1U << 6)
#define W25Q64FV_SR1_SRP0          (1U << 7)

//SR 2
#define W25Q64FV_SR2_SRP1          (1U << 0)
#define W25Q64FV_SR2_QE            (1U << 1)
#define W25Q64FV_SR2_LB1           (1U << 3)
#define W25Q64FV_SR2_LB2           (1U << 4)
#define W25Q64FV_SR2_LB3           (1U << 5)
#define W25Q64FV_SR2_CMP           (1U << 6)
#define W25Q64FV_SR2_SUS           (1U << 7)

//Operational Timeout (ms)
#define W25Q64FV_TIMEOUT_SPI       100U
#define W25Q64FV_TIMEOUT_PAGE_PROG 3U
#define W25Q64FV_TIMEOUT_SECTOR    400U
#define W25Q64FV_TIMEOUT_BLOCK32   1600U
#define W25Q64FV_TIMEOUT_BLOCK64   2000U
#define W25Q64FV_TIMEOUT_CHIP      200000U
#define W25Q64FV_TIMEOUT_WRITE_SR  15U

//Return status
typedef enum {
    W25Q64FV_OK          = 0,
    W25Q64FV_ERR         = 1,
    W25Q64FV_TIMEOUT     = 2,
    W25Q64FV_BUSY        = 3,
    W25Q64FV_PARAM_ERR   = 4,
    W25Q64FV_ID_MISMATCH = 5,
} W25Q64FV_Status;

//Driver Handle
typedef struct {
    SPI_HandleTypeDef *hspi;
    GPIO_TypeDef      *cs_port;
    uint16_t           cs_pin;
} W25Q64FV_Handle;


/* Initialisation & identification */
W25Q64FV_Status W25Q64FV_Init(W25Q64FV_Handle *dev,SPI_HandleTypeDef *hspi,GPIO_TypeDef *cs_port, uint16_t cs_pin);
W25Q64FV_Status W25Q64FV_VerifyID(W25Q64FV_Handle *dev);
W25Q64FV_Status W25Q64FV_ReadJEDECID(W25Q64FV_Handle *dev,uint8_t *mfr, uint16_t *dev_id);
W25Q64FV_Status W25Q64FV_ReadMfrDeviceID(W25Q64FV_Handle *dev,uint8_t *mfr, uint8_t *dev_id);
W25Q64FV_Status W25Q64FV_ReadUniqueID(W25Q64FV_Handle *dev, uint8_t uid[8]);

/* Status register access */
W25Q64FV_Status W25Q64FV_ReadSR1(W25Q64FV_Handle *dev, uint8_t *sr1);
W25Q64FV_Status W25Q64FV_ReadSR2(W25Q64FV_Handle *dev, uint8_t *sr2);
W25Q64FV_Status W25Q64FV_WriteSR(W25Q64FV_Handle *dev,
                                   uint8_t sr1, uint8_t sr2);
W25Q64FV_Status W25Q64FV_WaitBusy(W25Q64FV_Handle *dev, uint32_t timeout_ms);

/* Read */
W25Q64FV_Status W25Q64FV_Read(W25Q64FV_Handle *dev, uint32_t addr,uint8_t *buf, uint32_t len);

/* Write (handles page-boundary splitting automatically) */
W25Q64FV_Status W25Q64FV_PageProgram(W25Q64FV_Handle *dev, uint32_t addr,const uint8_t *buf, uint16_t len);
W25Q64FV_Status W25Q64FV_Write(W25Q64FV_Handle *dev, uint32_t addr,const uint8_t *buf, uint32_t len);

/* Erase */
W25Q64FV_Status W25Q64FV_SectorErase(W25Q64FV_Handle *dev, uint32_t addr);
W25Q64FV_Status W25Q64FV_BlockErase32K(W25Q64FV_Handle *dev, uint32_t addr);
W25Q64FV_Status W25Q64FV_BlockErase64K(W25Q64FV_Handle *dev, uint32_t addr);
W25Q64FV_Status W25Q64FV_ChipErase(W25Q64FV_Handle *dev);

/* Erase/Program suspend & resume */
W25Q64FV_Status W25Q64FV_EraseSuspend(W25Q64FV_Handle *dev);
W25Q64FV_Status W25Q64FV_EraseResume(W25Q64FV_Handle *dev);

/* Power management */
W25Q64FV_Status W25Q64FV_PowerDown(W25Q64FV_Handle *dev);
W25Q64FV_Status W25Q64FV_WakeUp(W25Q64FV_Handle *dev);

/* Software reset */
W25Q64FV_Status W25Q64FV_SoftReset(W25Q64FV_Handle *dev);

//inline address helpers
static inline uint32_t W25Q64FV_PageToAddr(uint32_t page)
    { return page * W25Q64FV_PAGE_SIZE; }

static inline uint32_t W25Q64FV_SectorToAddr(uint32_t sector)
    { return sector * W25Q64FV_SECTOR_SIZE; }

static inline uint32_t W25Q64FV_BlockToAddr(uint32_t block)
    { return block * W25Q64FV_BLOCK64_SIZE; }

static inline uint32_t W25Q64FV_AddrToPage(uint32_t addr)
    { return addr / W25Q64FV_PAGE_SIZE; }

static inline uint32_t W25Q64FV_AddrToSector(uint32_t addr)
    { return addr / W25Q64FV_SECTOR_SIZE; }

static inline uint32_t W25Q64FV_AddrToBlock(uint32_t addr)
    { return addr / W25Q64FV_BLOCK64_SIZE; }

#ifdef __cplusplus
}
#endif

#endif /* W25Q64FV_H */
