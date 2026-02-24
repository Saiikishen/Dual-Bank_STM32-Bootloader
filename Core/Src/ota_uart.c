/**
 * @file ota_uart.c
 * author: Saiikishen
 */
#include "ota_uart.h"
#include "bootloader_utils.h"
#include "stm32f4xx_hal.h"
#include <string.h>

#define OTA_READY_SIGNAL    0x55U

/* ── Internal helpers ───────────────────────────────────────────── */
static int uart_rx_byte(UART_HandleTypeDef *h, uint8_t *b, uint32_t timeout_ms)
{
    return (HAL_UART_Receive(h, b, 1U, timeout_ms) == HAL_OK) ? 0 : -1;
}

static int uart_tx_byte(UART_HandleTypeDef *h, uint8_t b)
{
    return (HAL_UART_Transmit(h, &b, 1U, OTA_UART_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
}

static int recv_exact(UART_HandleTypeDef *h, uint8_t *buf, uint16_t len)
{
    return (HAL_UART_Receive(h, buf, len, OTA_UART_TIMEOUT_MS) == HAL_OK) ? 0 : -1;
}

/* ── Main receive function ──────────────────────────────────────── */
OtaRxStatus_t OTA_ReceiveFirmware(UART_HandleTypeDef *huart,
                                  W25Q64FV_Handle    *ext,
                                  uint32_t           *out_size,
                                  uint32_t           *out_crc)
{
    uint8_t  hdr, cmd;
    uint8_t  pkt_buf[OTA_PACKET_DATA_SIZE + 8U];
    uint16_t pkt_len;
    uint32_t image_size    = 0;
    uint32_t image_crc_exp = 0;
    uint32_t write_addr    = EXT_OTA_ADDR;
    uint32_t bytes_written = 0;

    for (uint8_t blk = 0; blk < 15U; blk++) {
        if (W25Q64FV_BlockErase64K(ext,
            EXT_OTA_ADDR + (uint32_t)blk * W25Q64FV_BLOCK64_SIZE)
            != W25Q64FV_OK)
            return OTA_RX_ERR_FLASH;
    }

    uint8_t ready = OTA_READY_SIGNAL;
    HAL_UART_Transmit(huart, &ready, 1U, 1000U);


    for (;;) {
        if (uart_rx_byte(huart, &hdr, OTA_UART_TIMEOUT_MS) != 0)
            return OTA_RX_ERR_TIMEOUT;
        if (hdr != OTA_PACKET_HEADER) continue;
        if (uart_rx_byte(huart, &cmd, OTA_UART_TIMEOUT_MS) != 0)
            return OTA_RX_ERR_TIMEOUT;
        if (cmd == OTA_CMD_START) break;
    }
    uint8_t start_buf[12];
    if (recv_exact(huart, start_buf, 12U) != 0) return OTA_RX_ERR_TIMEOUT;


    uint32_t pkt_crc = (uint32_t)start_buf[8]  << 24U |
                       (uint32_t)start_buf[9]  << 16U |
                       (uint32_t)start_buf[10] <<  8U |
                       (uint32_t)start_buf[11];
    if (BL_CRC32(start_buf, 8U) != pkt_crc) {
        uart_tx_byte(huart, OTA_NAK);
        return OTA_RX_ERR_CRC;
    }

    image_size    = (uint32_t)start_buf[0] << 24U | (uint32_t)start_buf[1] << 16U |
                   (uint32_t)start_buf[2] <<  8U | (uint32_t)start_buf[3];
    image_crc_exp = (uint32_t)start_buf[4] << 24U | (uint32_t)start_buf[5] << 16U |
                   (uint32_t)start_buf[6] <<  8U | (uint32_t)start_buf[7];

    if (image_size == 0U || image_size > APP_MAX_SIZE) {
        uart_tx_byte(huart, OTA_NAK);
        return OTA_RX_ERR_SIZE;
    }
    uart_tx_byte(huart, OTA_ACK);


    while (bytes_written < image_size) {
        if (uart_rx_byte(huart, &hdr, OTA_UART_TIMEOUT_MS) != 0)
            return OTA_RX_ERR_TIMEOUT;
        if (hdr != OTA_PACKET_HEADER) continue;

        if (uart_rx_byte(huart, &cmd, OTA_UART_TIMEOUT_MS) != 0)
            return OTA_RX_ERR_TIMEOUT;
        if (cmd == OTA_CMD_END) break;
        if (cmd != OTA_CMD_DATA) { uart_tx_byte(huart, OTA_NAK); continue; }

        uint8_t len_buf[2];
        if (recv_exact(huart, len_buf, 2U) != 0) return OTA_RX_ERR_TIMEOUT;
        pkt_len = ((uint16_t)len_buf[0] << 8U) | len_buf[1];
        if (pkt_len == 0U || pkt_len > OTA_PACKET_DATA_SIZE) {
            uart_tx_byte(huart, OTA_NAK);
            return OTA_RX_ERR_PROTO;
        }


        if (recv_exact(huart, pkt_buf, pkt_len + 4U) != 0) return OTA_RX_ERR_TIMEOUT;

        uint32_t rx_crc = (uint32_t)pkt_buf[pkt_len + 0] << 24U |
                          (uint32_t)pkt_buf[pkt_len + 1] << 16U |
                          (uint32_t)pkt_buf[pkt_len + 2] <<  8U |
                          (uint32_t)pkt_buf[pkt_len + 3];
        if (BL_CRC32(pkt_buf, pkt_len) != rx_crc) {
            uart_tx_byte(huart, OTA_NAK);
            continue;
        }

        if (W25Q64FV_Write(ext, write_addr, pkt_buf, pkt_len) != W25Q64FV_OK) {
            uart_tx_byte(huart, OTA_NAK);
            return OTA_RX_ERR_FLASH;
        }
        write_addr    += pkt_len;
        bytes_written += pkt_len;
        uart_tx_byte(huart, OTA_ACK);
    }


    {
        uint8_t verify_buf[256];
        uint32_t crc_acc = 0xFFFFFFFFUL;
        uint32_t remaining = bytes_written;
        uint32_t rd_addr = EXT_OTA_ADDR;


        crc_acc = 0xFFFFFFFFUL;
        remaining = bytes_written;
        rd_addr = EXT_OTA_ADDR;
        while (remaining > 0U) {
            uint32_t chunk = (remaining > 256U) ? 256U : remaining;
            if (W25Q64FV_Read(ext, rd_addr, verify_buf, chunk) != W25Q64FV_OK)
                return OTA_RX_ERR_FLASH;

            for (uint32_t i = 0; i < chunk; i++) {
                crc_acc ^= verify_buf[i];
                for (uint8_t b = 0; b < 8U; b++)
                    crc_acc = (crc_acc & 1U) ? ((crc_acc >> 1U) ^ 0xEDB88320UL)
                                             : (crc_acc >> 1U);
            }
            rd_addr   += chunk;
            remaining -= chunk;
        }
        uint32_t final_crc = ~crc_acc;
        if (final_crc != image_crc_exp) return OTA_RX_ERR_CRC;
    }

    *out_size = bytes_written;
    *out_crc  = image_crc_exp;
    return OTA_RX_OK;
}
