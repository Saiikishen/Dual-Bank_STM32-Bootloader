/**
 * @file    w25q64fv.c
 * @brief   W25Q64FV 64Mb SPI Flash driver for STM32F407VGT (HAL)
 * @author  Saikishen
 */

#include "w25q64fv.h"
#include <string.h>



static inline void prv_cs_assert(const W25Q64FV_Handle *dev)
{
    HAL_GPIO_WritePin(dev->cs_port, dev->cs_pin, GPIO_PIN_RESET);
}

static inline void prv_cs_deassert(const W25Q64FV_Handle *dev)
{
    HAL_GPIO_WritePin(dev->cs_port, dev->cs_pin, GPIO_PIN_SET);
}


static W25Q64FV_Status prv_spi_tx(W25Q64FV_Handle *dev,
                                   const uint8_t *buf, uint16_t len)
{
    if (HAL_SPI_Transmit(dev->hspi, (uint8_t *)buf, len,
                         W25Q64FV_TIMEOUT_SPI) != HAL_OK)
        return W25Q64FV_ERR;
    return W25Q64FV_OK;
}


static W25Q64FV_Status prv_spi_rx(W25Q64FV_Handle *dev,
                                   uint8_t *buf, uint16_t len)
{
    if (HAL_SPI_Receive(dev->hspi, buf, len,
                        W25Q64FV_TIMEOUT_SPI) != HAL_OK)
        return W25Q64FV_ERR;
    return W25Q64FV_OK;
}


static W25Q64FV_Status prv_send_cmd_addr(W25Q64FV_Handle *dev,
                                          uint8_t cmd, uint32_t addr)
{
    uint8_t buf[4] = {
        cmd,
        (uint8_t)((addr >> 16) & 0xFFU),
        (uint8_t)((addr >>  8) & 0xFFU),
        (uint8_t)((addr      ) & 0xFFU),
    };
    return prv_spi_tx(dev, buf, 4U);
}


static W25Q64FV_Status prv_write_enable(W25Q64FV_Handle *dev)
{
    uint8_t cmd = W25Q64FV_CMD_WRITE_ENABLE;
    W25Q64FV_Status ret;

    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);
    return ret;
}


W25Q64FV_Status W25Q64FV_Init(W25Q64FV_Handle *dev,SPI_HandleTypeDef *hspi,GPIO_TypeDef *cs_port, uint16_t cs_pin)
{
    if (!dev || !hspi || !cs_port) return W25Q64FV_PARAM_ERR;

    dev->hspi    = hspi;
    dev->cs_port = cs_port;
    dev->cs_pin  = cs_pin;

    prv_cs_deassert(dev);   /* Idle CS high                                   */
    HAL_Delay(5U);          /* tVSL: VCC stable to CS low, min 5 ms          */

    W25Q64FV_WakeUp(dev);   /* sends 0xAB, waits tRES1 ~1ms                 */
    W25Q64FV_SoftReset(dev); /* sends 0x66 + 0x99, waits tRST ~1ms */

    return W25Q64FV_VerifyID(dev);
}


W25Q64FV_Status W25Q64FV_ReadJEDECID(W25Q64FV_Handle *dev,uint8_t *mfr, uint16_t *dev_id)
{
    uint8_t cmd = W25Q64FV_CMD_JEDEC_ID;
    uint8_t rx[3];
    W25Q64FV_Status ret;

    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    if (ret == W25Q64FV_OK) ret = prv_spi_rx(dev, rx, 3U);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK) {
        if (mfr)    *mfr    = rx[0];
        if (dev_id) *dev_id = ((uint16_t)rx[1] << 8U) | rx[2];
    }
    return ret;
}


W25Q64FV_Status W25Q64FV_ReadMfrDeviceID(W25Q64FV_Handle *dev,uint8_t *mfr, uint8_t *dev_id)
{
    /* 90h + 3 dummy bytes (address 0x000000) → MFR ID + Device ID */
    uint8_t tx[4] = { W25Q64FV_CMD_READ_MFR_DEVICE_ID, 0x00U, 0x00U, 0x00U };
    uint8_t rx[2];
    W25Q64FV_Status ret;

    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, tx, 4U);
    if (ret == W25Q64FV_OK) ret = prv_spi_rx(dev, rx, 2U);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK) {
        if (mfr)    *mfr    = rx[0];
        if (dev_id) *dev_id = rx[1];
    }
    return ret;
}


W25Q64FV_Status W25Q64FV_VerifyID(W25Q64FV_Handle *dev)
{
    uint8_t  mfr;
    uint16_t dev_id;
    W25Q64FV_Status ret = W25Q64FV_ReadJEDECID(dev, &mfr, &dev_id);
    if (ret != W25Q64FV_OK) return ret;

    if (mfr != W25Q64FV_MANUFACTURER_ID || dev_id != W25Q64FV_JEDEC_FULL)
        return W25Q64FV_ID_MISMATCH;
    return W25Q64FV_OK;
}


W25Q64FV_Status W25Q64FV_ReadUniqueID(W25Q64FV_Handle *dev, uint8_t uid[8])
{
    uint8_t tx[5] = { W25Q64FV_CMD_READ_UNIQUE_ID,
                      0xFFU, 0xFFU, 0xFFU, 0xFFU };
    W25Q64FV_Status ret;

    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, tx, 5U);
    if (ret == W25Q64FV_OK) ret = prv_spi_rx(dev, uid, 8U);
    prv_cs_deassert(dev);
    return ret;
}



/** @brief Read Status Register-1 (05h). */
W25Q64FV_Status W25Q64FV_ReadSR1(W25Q64FV_Handle *dev, uint8_t *sr1)
{
    uint8_t cmd = W25Q64FV_CMD_READ_SR1;
    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    if (ret == W25Q64FV_OK) ret = prv_spi_rx(dev, sr1, 1U);
    prv_cs_deassert(dev);
    return ret;
}

/** @brief Read Status Register-2 (35h). */
W25Q64FV_Status W25Q64FV_ReadSR2(W25Q64FV_Handle *dev, uint8_t *sr2)
{
    uint8_t cmd = W25Q64FV_CMD_READ_SR2;
    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    if (ret == W25Q64FV_OK) ret = prv_spi_rx(dev, sr2, 1U);
    prv_cs_deassert(dev);
    return ret;
}

/**
 * @brief  Write both status registers (01h).
 *         Issues Write Enable first; waits for completion.
 *         WARNING: Do not set SR2.QE=1 if WP/HOLD are tied to GND/VCC.
 */
W25Q64FV_Status W25Q64FV_WriteSR(W25Q64FV_Handle *dev,
                                   uint8_t sr1, uint8_t sr2)
{
    uint8_t buf[3] = { W25Q64FV_CMD_WRITE_SR, sr1, sr2 };
    W25Q64FV_Status ret;

    ret = prv_write_enable(dev);
    if (ret != W25Q64FV_OK) return ret;

    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, buf, 3U);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK)
        ret = W25Q64FV_WaitBusy(dev, W25Q64FV_TIMEOUT_WRITE_SR);
    return ret;
}

/**
 * @brief  Poll the BUSY bit in SR1 until clear or timeout.
 * @param  timeout_ms  Maximum wait time in milliseconds.
 */
W25Q64FV_Status W25Q64FV_WaitBusy(W25Q64FV_Handle *dev, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t sr1;
    W25Q64FV_Status ret;

    do {
        ret = W25Q64FV_ReadSR1(dev, &sr1);
        if (ret != W25Q64FV_OK)            return ret;
        if (!(sr1 & W25Q64FV_SR1_BUSY))    return W25Q64FV_OK;
    } while ((HAL_GetTick() - start) < timeout_ms);

    return W25Q64FV_TIMEOUT;
}


W25Q64FV_Status W25Q64FV_Read(W25Q64FV_Handle *dev, uint32_t addr,uint8_t *buf, uint32_t len)
{
    if (!buf || len == 0U || (addr + len) > W25Q64FV_FLASH_SIZE)
        return W25Q64FV_PARAM_ERR;

    uint8_t hdr[5] = {
        W25Q64FV_CMD_FAST_READ,
        (uint8_t)((addr >> 16) & 0xFFU),
        (uint8_t)((addr >>  8) & 0xFFU),
        (uint8_t)((addr      ) & 0xFFU),
        0xFFU
    };

    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, hdr, 5U);

    if (ret == W25Q64FV_OK) {
        uint32_t remaining = len;
        uint8_t *ptr       = buf;
        while (remaining > 0U && ret == W25Q64FV_OK) {
            uint16_t chunk = (remaining > 0xFFFFU) ? 0xFFFFU
                                                    : (uint16_t)remaining;
            ret        = prv_spi_rx(dev, ptr, chunk);
            ptr       += chunk;
            remaining -= chunk;
        }
    }
    prv_cs_deassert(dev);
    return ret;
}


W25Q64FV_Status W25Q64FV_PageProgram(W25Q64FV_Handle *dev, uint32_t addr,const uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0U || len > W25Q64FV_PAGE_SIZE)
        return W25Q64FV_PARAM_ERR;

    /* Verify write stays within the same page */
    uint32_t page_offset = addr & (W25Q64FV_PAGE_SIZE - 1U);
    if ((page_offset + len) > W25Q64FV_PAGE_SIZE)
        return W25Q64FV_PARAM_ERR;

    if ((addr + len) > W25Q64FV_FLASH_SIZE)
        return W25Q64FV_PARAM_ERR;

    W25Q64FV_Status ret = prv_write_enable(dev);
    if (ret != W25Q64FV_OK) return ret;

    prv_cs_assert(dev);
    ret = prv_send_cmd_addr(dev, W25Q64FV_CMD_PAGE_PROGRAM, addr);
    if (ret == W25Q64FV_OK) ret = prv_spi_tx(dev, buf, len);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK)
        ret = W25Q64FV_WaitBusy(dev, W25Q64FV_TIMEOUT_PAGE_PROG);
    return ret;
}


W25Q64FV_Status W25Q64FV_Write(W25Q64FV_Handle *dev, uint32_t addr,const uint8_t *buf, uint32_t len)
{
    if (!buf || len == 0U || (addr + len) > W25Q64FV_FLASH_SIZE)
        return W25Q64FV_PARAM_ERR;

    W25Q64FV_Status ret = W25Q64FV_OK;
    uint32_t offset = 0U;

    while (offset < len && ret == W25Q64FV_OK) {
        uint32_t cur_addr  = addr + offset;
        uint32_t page_off  = cur_addr & (W25Q64FV_PAGE_SIZE - 1U);
        uint16_t space     = (uint16_t)(W25Q64FV_PAGE_SIZE - page_off);
        uint32_t remaining = len - offset;
        uint16_t chunk     = (remaining < (uint32_t)space)
                             ? (uint16_t)remaining : space;

        ret     = W25Q64FV_PageProgram(dev, cur_addr, buf + offset, chunk);
        offset += chunk;
    }
    return ret;
}


W25Q64FV_Status W25Q64FV_SectorErase(W25Q64FV_Handle *dev, uint32_t addr)
{
    addr &= ~((uint32_t)W25Q64FV_SECTOR_SIZE - 1U);
    if (addr >= W25Q64FV_FLASH_SIZE) return W25Q64FV_PARAM_ERR;

    W25Q64FV_Status ret = prv_write_enable(dev);
    if (ret != W25Q64FV_OK) return ret;

    prv_cs_assert(dev);
    ret = prv_send_cmd_addr(dev, W25Q64FV_CMD_SECTOR_ERASE_4KB, addr);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK)
        ret = W25Q64FV_WaitBusy(dev, W25Q64FV_TIMEOUT_SECTOR);
    return ret;
}


W25Q64FV_Status W25Q64FV_BlockErase32K(W25Q64FV_Handle *dev, uint32_t addr)
{
    addr &= ~((uint32_t)W25Q64FV_BLOCK32_SIZE - 1U);
    if (addr >= W25Q64FV_FLASH_SIZE) return W25Q64FV_PARAM_ERR;

    W25Q64FV_Status ret = prv_write_enable(dev);
    if (ret != W25Q64FV_OK) return ret;

    prv_cs_assert(dev);
    ret = prv_send_cmd_addr(dev, W25Q64FV_CMD_BLOCK_ERASE_32KB, addr);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK)
        ret = W25Q64FV_WaitBusy(dev, W25Q64FV_TIMEOUT_BLOCK32);
    return ret;
}


W25Q64FV_Status W25Q64FV_BlockErase64K(W25Q64FV_Handle *dev, uint32_t addr)
{
    addr &= ~((uint32_t)W25Q64FV_BLOCK64_SIZE - 1U);
    if (addr >= W25Q64FV_FLASH_SIZE) return W25Q64FV_PARAM_ERR;

    W25Q64FV_Status ret = prv_write_enable(dev);
    if (ret != W25Q64FV_OK) return ret;

    prv_cs_assert(dev);
    ret = prv_send_cmd_addr(dev, W25Q64FV_CMD_BLOCK_ERASE_64KB, addr);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK)
        ret = W25Q64FV_WaitBusy(dev, W25Q64FV_TIMEOUT_BLOCK64);
    return ret;
}


W25Q64FV_Status W25Q64FV_ChipErase(W25Q64FV_Handle *dev)
{
    uint8_t cmd = W25Q64FV_CMD_CHIP_ERASE;
    W25Q64FV_Status ret = prv_write_enable(dev);
    if (ret != W25Q64FV_OK) return ret;

    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);

    if (ret == W25Q64FV_OK)
        ret = W25Q64FV_WaitBusy(dev, W25Q64FV_TIMEOUT_CHIP);
    return ret;
}

/* ======================================================================== */
/*  Erase / Program suspend & resume                                          */
/* ======================================================================== */

/** @brief Suspend an ongoing sector/block erase to allow a read (75h). */
W25Q64FV_Status W25Q64FV_EraseSuspend(W25Q64FV_Handle *dev)
{
    uint8_t cmd = W25Q64FV_CMD_ERASE_SUSPEND;
    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);
    HAL_Delay(1U);   /* tSUS: suspend latency ~20 µs, 1 ms safe margin */
    return ret;
}

/** @brief Resume a suspended erase operation (7Ah). */
W25Q64FV_Status W25Q64FV_EraseResume(W25Q64FV_Handle *dev)
{
    uint8_t cmd = W25Q64FV_CMD_ERASE_RESUME;
    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);
    return ret;
}



W25Q64FV_Status W25Q64FV_PowerDown(W25Q64FV_Handle *dev)
{
    uint8_t cmd = W25Q64FV_CMD_POWER_DOWN;
    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);
    HAL_Delay(1U);
    return ret;
}


W25Q64FV_Status W25Q64FV_WakeUp(W25Q64FV_Handle *dev)
{
    uint8_t cmd = W25Q64FV_CMD_RELEASE_POWER_DOWN;
    W25Q64FV_Status ret;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);
    HAL_Delay(1U);
    return ret;
}


/**
 * @brief  Software reset sequence: Enable Reset (66h) then Reset (99h).
 *         Device returns to power-on defaults in ~30 µs (tRST).
 *         WARNING: Data corruption may occur if an erase/program is ongoing.
 *                  Check BUSY and SUS bits before calling.
 */
W25Q64FV_Status W25Q64FV_SoftReset(W25Q64FV_Handle *dev)
{
    uint8_t cmd;
    W25Q64FV_Status ret;

    cmd = W25Q64FV_CMD_ENABLE_RESET;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);
    if (ret != W25Q64FV_OK) return ret;

    cmd = W25Q64FV_CMD_RESET;
    prv_cs_assert(dev);
    ret = prv_spi_tx(dev, &cmd, 1U);
    prv_cs_deassert(dev);

    HAL_Delay(1U);
    return ret;
}
