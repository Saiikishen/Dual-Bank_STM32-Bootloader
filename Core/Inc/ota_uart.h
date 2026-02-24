/**
 * @file ota_uart.h
 * @brief UART OTA receive protocol (ESP32 → STM32).
 *  Author: saiikishen
 *
 * Packet format (from ESP32):
 *  [0xA5][CMD][LEN_H][LEN_L][DATA...][CRC32_B3..B0]
 *
 * CMD_START payload: 4-byte total image size + 4-byte image CRC32
 * CMD_DATA  payload: up to 256 bytes of firmware
 * CMD_END   payload: empty (0 bytes)
 */
#ifndef OTA_UART_H
#define OTA_UART_H

#include <stdint.h>
#include "w25q64fv.h"
#include "bootloader_config.h"

typedef enum {
    OTA_RX_OK         = 0,
    OTA_RX_ERR_TIMEOUT,
    OTA_RX_ERR_CRC,
    OTA_RX_ERR_FLASH,
    OTA_RX_ERR_SIZE,
    OTA_RX_ERR_PROTO,
} OtaRxStatus_t;

/**
 * @brief  Receive OTA firmware from ESP32 via UART and write to ext. OTA slot.
 * @param  huart   UART handle connected to ESP32
 * @param  ext     Initialized W25Q64FV handle
 * @param  out_size  Returns the number of bytes received
 * @param  out_crc   Returns the CRC32 of the received image
 * @return OTA_RX_OK on success, error code otherwise.
 */
OtaRxStatus_t OTA_ReceiveFirmware(UART_HandleTypeDef *huart,
                                  W25Q64FV_Handle    *ext,
                                  uint32_t           *out_size,
                                  uint32_t           *out_crc);

#endif /* OTA_UART_H */
