/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : STM32F407 Dual-Bank Bootloader with UART OTA
  * @version        : 2.0 - Production Ready
  * @date           : February 17, 2026
  ******************************************************************************

  ******************************************************************************
  */

#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
/* ==================== Memory Map Configuration ==================== */
#define BOOTLOADER_START    0x08000000
#define BOOTLOADER_SIZE     0x00008000  // 32KB (Sectors 0-1)
#define APP_BANK_A_ADDR     0x08008000  // Bank A start (Sectors 2-7)
#define APP_BANK_B_ADDR     0x08080000  // Bank B start (Sectors 8-11)
#define APP_SIZE            0x00078000  // 480KB per bank
#define CONFIG_SECTOR_ADDR  0x080E0000  // Last 128KB for metadata (Sector 11)

/* ==================== Bootloader Protocol Commands ==================== */
#define CMD_FLASH_START     0x55
#define CMD_FLASH_DATA      0x56
#define CMD_FLASH_END       0x57
#define CMD_GET_STATUS      0x58

/* ==================== Protocol Response Codes ==================== */
#define ACK                 0x79
#define NACK                0x1F

/* ==================== Configuration ==================== */
#define BOOTLOADER_TIMEOUT_MS   5000    // Wait 5s for OTA command
#define MAX_CHUNK_SIZE          1024    // Maximum data chunk size
#define DEBUG_UART_ENABLED      1       // Enable debug messages

/* ==================== OTA Metadata Structure ==================== */
typedef struct {

    uint32_t magic;           // 0x12345678 - validity marker
    uint32_t active_bank;     // 0 = Bank A, 1 = Bank B
    uint32_t bank_a_crc;
    uint32_t bank_a_size;
    uint32_t bank_a_version;
    uint32_t bank_b_crc;
    uint32_t bank_b_size;
    uint32_t bank_b_version;
    uint32_t ota_in_progress; // 1 = OTA ongoing
    uint32_t target_bank;     // Bank being updated
    uint32_t reserved[6];     // Reserved for future use
} OTA_Metadata_t;

/* ==================== Global Variables ==================== */
UART_HandleTypeDef huart1;
CRC_HandleTypeDef hcrc;
OTA_Metadata_t ota_metadata;

uint8_t rx_buffer[MAX_CHUNK_SIZE];
uint32_t flash_dest_addr = 0;
uint32_t total_bytes_received = 0;
uint32_t expected_fw_size = 0;

/* ==================== Function Prototypes ==================== */
void SystemClock_Config(void);
void MX_GPIO_Init(void);
void MX_UART1_Init(void);
void MX_CRC_Init(void);
void Error_Handler(void);

bool Load_Metadata(void);
void Save_Metadata(void);
void Init_Metadata(void);
bool Verify_Bank(uint32_t bank_addr, uint32_t size, uint32_t expected_crc);
void Jump_To_Application(uint32_t app_addr);
void Bootloader_UART_Handler(void);

bool Flash_Erase_Bank(uint32_t bank_addr);
bool Flash_Write_Data_Verified(uint32_t addr, uint8_t *data, uint32_t len);
uint32_t Calculate_CRC32(uint32_t addr, uint32_t size);
uint32_t Calculate_CRC32_Safe(uint8_t *data, uint32_t size);
void Send_Response(uint8_t response);

void Debug_Print(const char* msg);
void Debug_Printf(const char* format, ...);

/* ==================== Main Entry Point ==================== */
int main(void)
{
    /* Disable watchdog if possible */
    __HAL_RCC_WWDG_CLK_DISABLE();

    /* Initialize HAL */
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_UART1_Init();
    MX_CRC_Init();

    /* Startup banner */
    Debug_Print("\r\n\r\n");
    Debug_Print("================================================\r\n");
    Debug_Print("  STM32F407 Dual-Bank Bootloader v2.0\r\n");
    Debug_Print("  Build: " __DATE__ " " __TIME__ "\r\n");
    Debug_Print("================================================\r\n");

    /* Load and validate OTA metadata */
    bool metadata_valid = Load_Metadata();

    if (!metadata_valid) {
        Debug_Print("INFO: First boot or corrupted metadata\r\n");
        Init_Metadata();
        Save_Metadata();
        Debug_Print("INFO: Metadata initialized\r\n");
    } else {
        Debug_Print("INFO: Metadata loaded successfully\r\n");
    }

    /* Display current status */
    Debug_Printf("Active Bank: %c\r\n", ota_metadata.active_bank == 0 ? 'A' : 'B');
    Debug_Printf("Bank A: %lu bytes, v%lu, CRC 0x%08lX\r\n",
                 ota_metadata.bank_a_size,
                 ota_metadata.bank_a_version,
                 ota_metadata.bank_a_crc);
    Debug_Printf("Bank B: %lu bytes, v%lu, CRC 0x%08lX\r\n",
                 ota_metadata.bank_b_size,
                 ota_metadata.bank_b_version,
                 ota_metadata.bank_b_crc);

    /* Check for OTA trigger */
    Debug_Printf("INFO: Waiting for OTA command (%dms)...\r\n", BOOTLOADER_TIMEOUT_MS);

    uint32_t boot_delay = HAL_GetTick() + BOOTLOADER_TIMEOUT_MS;
    bool enter_bootloader = false;

    while (HAL_GetTick() < boot_delay) {
        uint8_t cmd;
        if (HAL_UART_Receive(&huart1, &cmd, 1, 100) == HAL_OK) {
            if (cmd == CMD_FLASH_START) {
                enter_bootloader = true;
                Debug_Print("INFO: CMD_FLASH_START received\r\n");
                Send_Response(ACK);
                break;
            } else if (cmd == CMD_GET_STATUS) {
                enter_bootloader = true;
                Debug_Print("INFO: CMD_GET_STATUS received\r\n");
                break;
            }
        }
    }

    /* No OTA command - try to boot application */
    if (!enter_bootloader) {
        Debug_Print("INFO: No OTA command - checking application\r\n");

        uint32_t active_addr = (ota_metadata.active_bank == 0) ?
                                APP_BANK_A_ADDR : APP_BANK_B_ADDR;
        uint32_t active_size = (ota_metadata.active_bank == 0) ?
                                ota_metadata.bank_a_size : ota_metadata.bank_b_size;
        uint32_t active_crc = (ota_metadata.active_bank == 0) ?
                               ota_metadata.bank_a_crc : ota_metadata.bank_b_crc;

        Debug_Printf("INFO: Checking Bank %c...\r\n",
                     ota_metadata.active_bank == 0 ? 'A' : 'B');

        /* Verify active bank */
        if (active_size > 0 && active_size <= APP_SIZE) {
            if (Verify_Bank(active_addr, active_size, active_crc)) {
                Debug_Print("SUCCESS: Application valid - jumping!\r\n");
                HAL_Delay(100);  // Let UART finish
                Jump_To_Application(active_addr);
            } else {
                Debug_Print("ERROR: Active bank CRC failed!\r\n");

                /* Try fallback bank */
                uint32_t fallback_addr = (ota_metadata.active_bank == 0) ?
                                          APP_BANK_B_ADDR : APP_BANK_A_ADDR;
                uint32_t fallback_size = (ota_metadata.active_bank == 0) ?
                                          ota_metadata.bank_b_size : ota_metadata.bank_a_size;
                uint32_t fallback_crc = (ota_metadata.active_bank == 0) ?
                                         ota_metadata.bank_b_crc : ota_metadata.bank_a_crc;

                Debug_Printf("INFO: Trying fallback Bank %c...\r\n",
                            ota_metadata.active_bank == 0 ? 'B' : 'A');

                if (fallback_size > 0 && fallback_size <= APP_SIZE &&
                    Verify_Bank(fallback_addr, fallback_size, fallback_crc)) {

                    Debug_Print("SUCCESS: Fallback bank valid - switching!\r\n");
                    ota_metadata.active_bank = 1 - ota_metadata.active_bank;
                    Save_Metadata();
                    HAL_Delay(100);
                    Jump_To_Application(fallback_addr);
                } else {
                    Debug_Print("ERROR: Both banks invalid!\r\n");
                }
            }
        } else {
            Debug_Print("INFO: No valid application (size=0)\r\n");
        }
    }

    /* Enter bootloader mode */
    Debug_Print("INFO: Entering bootloader mode\r\n");
    Debug_Print("INFO: Ready for OTA commands\r\n");

    Bootloader_UART_Handler();

    /* Should never reach here */
    Error_Handler();
}

/* ==================== Bootloader UART Command Handler ==================== */
void Bootloader_UART_Handler(void)
{
    uint8_t cmd;
    uint32_t crc_received, crc_calculated;
    uint8_t retry_count = 0;

    while (1) {
        /* Receive command */
        if (HAL_UART_Receive(&huart1, &cmd, 1, HAL_MAX_DELAY) != HAL_OK) {
            continue;
        }

        switch (cmd) {
            case CMD_FLASH_START: {
                /* Format: [CMD][SIZE:4][TARGET_BANK:1][CRC:4] */
                uint8_t header[9];
                if (HAL_UART_Receive(&huart1, header, 9, 5000) != HAL_OK) {
                    Debug_Print("ERROR: FLASH_START timeout\r\n");
                    Send_Response(NACK);
                    break;
                }

                expected_fw_size = *(uint32_t*)&header[0];
                ota_metadata.target_bank = header[4];

                Debug_Printf("CMD: FLASH_START - Size=%lu, Bank=%c\r\n",
                            expected_fw_size,
                            ota_metadata.target_bank == 0 ? 'A' : 'B');

                /* Validate parameters */
                if (expected_fw_size == 0 || expected_fw_size > APP_SIZE) {
                    Debug_Print("ERROR: Invalid firmware size\r\n");
                    Send_Response(NACK);
                    break;
                }

                if (ota_metadata.target_bank > 1) {
                    Debug_Print("ERROR: Invalid bank number\r\n");
                    Send_Response(NACK);
                    break;
                }

                /* Determine target address */
                flash_dest_addr = (ota_metadata.target_bank == 0) ?
                                   APP_BANK_A_ADDR : APP_BANK_B_ADDR;

                /* Erase target bank */
                Debug_Print("INFO: Erasing flash (20-30s)...\r\n");
                if (!Flash_Erase_Bank(flash_dest_addr)) {
                    Debug_Print("ERROR: Flash erase failed\r\n");
                    Send_Response(NACK);
                    break;
                }

                Debug_Print("SUCCESS: Flash erased\r\n");

                total_bytes_received = 0;
                ota_metadata.ota_in_progress = 1;
                Save_Metadata();

                Send_Response(ACK);
                retry_count = 0;
                break;
            }

            case CMD_FLASH_DATA: {
                /* Format: [CMD][LEN:2][DATA][CRC:4] */
                uint8_t len_buf[2];
                if (HAL_UART_Receive(&huart1, len_buf, 2, 2000) != HAL_OK) {
                    Send_Response(NACK);
                    retry_count++;
                    break;
                }

                uint16_t chunk_len = *(uint16_t*)len_buf;

                if (chunk_len == 0 || chunk_len > MAX_CHUNK_SIZE) {
                    Debug_Printf("ERROR: Invalid chunk size %u\r\n", chunk_len);
                    Send_Response(NACK);
                    break;
                }

                /* Receive data */
                if (HAL_UART_Receive(&huart1, rx_buffer, chunk_len, 5000) != HAL_OK) {
                    Debug_Print("ERROR: Data receive timeout\r\n");
                    Send_Response(NACK);
                    retry_count++;
                    break;
                }

                /* Receive CRC */
                if (HAL_UART_Receive(&huart1, (uint8_t*)&crc_received, 4, 1000) != HAL_OK) {
                    Debug_Print("ERROR: CRC receive timeout\r\n");
                    Send_Response(NACK);
                    retry_count++;
                    break;
                }

                /* Verify chunk CRC */
                crc_calculated = Calculate_CRC32_Safe(rx_buffer, chunk_len);

                if (crc_calculated != crc_received) {
                    Debug_Printf("ERROR: CRC mismatch (exp=0x%08lX, got=0x%08lX)\r\n",
                                crc_received, crc_calculated);
                    Send_Response(NACK);
                    retry_count++;
                    break;
                }

                /* Write to flash with verification */
                if (!Flash_Write_Data_Verified(flash_dest_addr + total_bytes_received,
                                                rx_buffer, chunk_len)) {
                    Debug_Print("ERROR: Flash write failed\r\n");
                    Send_Response(NACK);
                    retry_count++;
                    break;
                }

                total_bytes_received += chunk_len;
                retry_count = 0;

                /* Progress indicator (every 10 chunks) */
                static uint32_t chunk_counter = 0;
                chunk_counter++;
                if (chunk_counter % 10 == 0) {
                    Debug_Printf("PROGRESS: %lu/%lu bytes (%.1f%%)\r\n",
                                total_bytes_received, expected_fw_size,
                                (total_bytes_received * 100.0f) / expected_fw_size);
                }

                Send_Response(ACK);
                break;
            }

            case CMD_FLASH_END: {
                /* Format: [CMD][EXPECTED_CRC:4] */
                uint32_t expected_full_crc;
                if (HAL_UART_Receive(&huart1, (uint8_t*)&expected_full_crc,
                                    4, 2000) != HAL_OK) {
                    Debug_Print("ERROR: Final CRC receive timeout\r\n");
                    Send_Response(NACK);
                    break;
                }

                Debug_Print("CMD: FLASH_END - Verifying firmware...\r\n");

                /* Verify size matches */
                if (total_bytes_received != expected_fw_size) {
                    Debug_Printf("ERROR: Size mismatch (exp=%lu, got=%lu)\r\n",
                                expected_fw_size, total_bytes_received);
                    Send_Response(NACK);
                    break;
                }

                /* Verify entire firmware CRC */
                uint32_t actual_crc = Calculate_CRC32(flash_dest_addr,
                                                       total_bytes_received);

                if (actual_crc != expected_full_crc) {
                    Debug_Printf("ERROR: Final CRC mismatch (exp=0x%08lX, got=0x%08lX)\r\n",
                                expected_full_crc, actual_crc);
                    Send_Response(NACK);
                    break;
                }

                Debug_Print("SUCCESS: Firmware verified!\r\n");

                /* Update metadata */
                if (ota_metadata.target_bank == 0) {
                    ota_metadata.bank_a_crc = actual_crc;
                    ota_metadata.bank_a_size = total_bytes_received;
                    ota_metadata.bank_a_version++;
                    ota_metadata.active_bank = 0;
                    Debug_Printf("INFO: Bank A updated to v%lu\r\n", ota_metadata.bank_a_version);
                } else {
                    ota_metadata.bank_b_crc = actual_crc;
                    ota_metadata.bank_b_size = total_bytes_received;
                    ota_metadata.bank_b_version++;
                    ota_metadata.active_bank = 1;
                    Debug_Printf("INFO: Bank B updated to v%lu\r\n", ota_metadata.bank_b_version);
                }

                ota_metadata.ota_in_progress = 0;
                Save_Metadata();

                Send_Response(ACK);

                Debug_Print("SUCCESS: OTA complete! Rebooting...\r\n");
                HAL_Delay(1000);
                NVIC_SystemReset();
                break;
            }

            case CMD_GET_STATUS: {
                /* Send status: [ACK][ACTIVE_BANK][VERSION][BYTES_RECEIVED:4] */
                Send_Response(ACK);

                uint8_t status[6];
                status[0] = ota_metadata.active_bank;
                status[1] = (ota_metadata.active_bank == 0) ?
                            (uint8_t)ota_metadata.bank_a_version :
                            (uint8_t)ota_metadata.bank_b_version;
                *(uint32_t*)&status[2] = total_bytes_received;

                HAL_UART_Transmit(&huart1, status, 6, 1000);

                Debug_Printf("CMD: GET_STATUS - Bank %c, v%u, %lu bytes\r\n",
                            status[0] == 0 ? 'A' : 'B',
                            status[1],
                            *(uint32_t*)&status[2]);
                break;
            }

            default:
                Debug_Printf("ERROR: Unknown command 0x%02X\r\n", cmd);
                Send_Response(NACK);
                break;
        }

        /* Safety: Abort if too many errors */
        if (retry_count > 10) {
            Debug_Print("ERROR: Too many retries - aborting OTA\r\n");
            ota_metadata.ota_in_progress = 0;
            Save_Metadata();
            retry_count = 0;
        }
    }
}

/* ==================== Flash Operations ==================== */
bool Flash_Erase_Bank(uint32_t bank_addr)
{
    HAL_FLASH_Unlock();

    FLASH_EraseInitTypeDef erase_cfg;
    uint32_t sector_error;
    HAL_StatusTypeDef status;

    if (bank_addr == APP_BANK_A_ADDR) {
        /* Erase sectors 2-7 (Bank A: 480KB) */
        erase_cfg.TypeErase = FLASH_TYPEERASE_SECTORS;
        erase_cfg.Sector = FLASH_SECTOR_2;
        erase_cfg.NbSectors = 6;
        erase_cfg.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    } else if (bank_addr == APP_BANK_B_ADDR) {
        /* Erase sectors 8-11 (Bank B: 512KB) */
        erase_cfg.TypeErase = FLASH_TYPEERASE_SECTORS;
        erase_cfg.Sector = FLASH_SECTOR_8;
        erase_cfg.NbSectors = 4;
        erase_cfg.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    } else {
        HAL_FLASH_Lock();
        return false;
    }

    status = HAL_FLASHEx_Erase(&erase_cfg, &sector_error);
    HAL_FLASH_Lock();

    if (status != HAL_OK) {
        return false;
    }

    /* Verify erase */
    uint32_t *verify = (uint32_t*)bank_addr;
    for (uint32_t i = 0; i < 256; i++) {
        if (verify[i] != 0xFFFFFFFF) {
            return false;
        }
    }

    return true;
}

bool Flash_Write_Data_Verified(uint32_t addr, uint8_t *data, uint32_t len)
{
    HAL_StatusTypeDef status;
    uint32_t words = (len + 3) / 4;

    uint32_t write_buffer[256];
    memset(write_buffer, 0xFF, sizeof(write_buffer));
    memcpy(write_buffer, data, len);

    HAL_FLASH_Unlock();

    for (uint32_t i = 0; i < words; i++) {
        uint32_t write_addr = addr + (i * 4);
        uint32_t write_data = write_buffer[i];

        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, write_addr, write_data);

        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return false;
        }

        /* Verify */
        uint32_t read_back = *(volatile uint32_t*)write_addr;
        if (read_back != write_data) {
            HAL_FLASH_Lock();
            return false;
        }
    }

    HAL_FLASH_Lock();
    return true;
}

/* ==================== CRC32 Calculation ==================== */
uint32_t Calculate_CRC32_Safe(uint8_t *data, uint32_t size)
{
    __HAL_CRC_DR_RESET(&hcrc);

    uint32_t word_count = size / 4;
    uint32_t remaining = size % 4;

    uint32_t crc = 0xFFFFFFFF;

    if (word_count > 0) {
        crc = HAL_CRC_Accumulate(&hcrc, (uint32_t*)data, word_count);
    }

    if (remaining > 0) {
        uint32_t last_word = 0xFFFFFFFF;
        uint8_t *last_bytes = (uint8_t*)&last_word;
        for (uint32_t i = 0; i < remaining; i++) {
            last_bytes[i] = data[word_count * 4 + i];
        }
        crc = HAL_CRC_Accumulate(&hcrc, &last_word, 1);
    }

    return crc;
}

uint32_t Calculate_CRC32(uint32_t addr, uint32_t size)
{
    __HAL_CRC_DR_RESET(&hcrc);

    uint32_t word_count = size / 4;
    uint32_t remaining = size % 4;
    uint32_t *data = (uint32_t*)addr;

    uint32_t crc = 0xFFFFFFFF;

    if (word_count > 0) {
        for (uint32_t i = 0; i < word_count; i++) {
            crc = HAL_CRC_Accumulate(&hcrc, &data[i], 1);
        }
    }

    if (remaining > 0) {
        uint32_t last_word = 0xFFFFFFFF;
        uint8_t *src = (uint8_t*)(addr + (word_count * 4));
        uint8_t *dst = (uint8_t*)&last_word;
        for (uint32_t i = 0; i < remaining; i++) {
            dst[i] = src[i];
        }
        crc = HAL_CRC_Accumulate(&hcrc, &last_word, 1);
    }

    return crc;
}

/* ==================== Bank Verification ==================== */
bool Verify_Bank(uint32_t bank_addr, uint32_t size, uint32_t expected_crc)
{
    /* Sanity checks */
    if (size == 0 || size > APP_SIZE) {
        return false;
    }

    /* Check if bank is erased */
    uint32_t *start = (uint32_t*)bank_addr;
    uint32_t sp = start[0];
    uint32_t reset = start[1];

    /* Check for blank flash */
    if (sp == 0xFFFFFFFF || sp == 0x00000000 ||
        reset == 0xFFFFFFFF || reset == 0x00000000) {
        return false;
    }

    /* Validate stack pointer in RAM range */
    if (sp < 0x20000000 || sp > 0x20020000) {
        return false;
    }

    /* Validate reset handler in flash range */
    if (reset < bank_addr || reset > (bank_addr + APP_SIZE)) {
        return false;
    }

    /* Calculate and verify CRC */
    uint32_t actual_crc = Calculate_CRC32(bank_addr, size);
    return (actual_crc == expected_crc);
}

/* ==================== Jump to Application ==================== */
void Jump_To_Application(uint32_t app_addr)
{
    /* Validate stack pointer */
    uint32_t sp = *(volatile uint32_t*)app_addr;
    if (sp < 0x20000000 || sp > 0x20020000) {
        Debug_Print("ERROR: Invalid stack pointer!\r\n");
        return;
    }

    /* Get reset handler */
    uint32_t reset_handler = *(volatile uint32_t*)(app_addr + 4);
    if (reset_handler < app_addr || reset_handler > (app_addr + APP_SIZE)) {
        Debug_Print("ERROR: Invalid reset handler!\r\n");
        return;
    }

    /* Deinitialize peripherals */
    HAL_UART_DeInit(&huart1);
    HAL_CRC_DeInit(&hcrc);
    HAL_RCC_DeInit();
    HAL_DeInit();

    /* Disable interrupts */
    __disable_irq();

    /* Disable SysTick */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;

    /* Set vector table */
    SCB->VTOR = app_addr;

    /* Set stack pointer */
    __set_MSP(sp);

    /* Jump to application */
    void (*app_reset_handler)(void) = (void*)reset_handler;
    app_reset_handler();
}

/* ==================== Metadata Management ==================== */
bool Load_Metadata(void)
{
    OTA_Metadata_t *stored = (OTA_Metadata_t*)CONFIG_SECTOR_ADDR;

    /* Check if sector is erased */
    uint32_t *check = (uint32_t*)CONFIG_SECTOR_ADDR;
    bool is_erased = true;
    for (int i = 0; i < 8; i++) {
        if (check[i] != 0xFFFFFFFF) {
            is_erased = false;
            break;
        }
    }

    if (is_erased) {
        return false;
    }

    /* Validate magic number */
    if (stored->magic != 0x12345678) {
        return false;
    }

    /* Validate values */
    if (stored->active_bank > 1 ||
        stored->bank_a_size > APP_SIZE ||
        stored->bank_b_size > APP_SIZE) {
        return false;
    }

    memcpy(&ota_metadata, stored, sizeof(OTA_Metadata_t));
    return true;
}

void Save_Metadata(void)
{
    HAL_FLASH_Unlock();

    /* Erase config sector */
    FLASH_EraseInitTypeDef erase;
    uint32_t error;
    erase.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase.Sector = FLASH_SECTOR_11;
    erase.NbSectors = 1;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &error);
    if (status != HAL_OK) {
        HAL_FLASH_Lock();
        return;
    }

    /* Verify erase */
    uint32_t *check = (uint32_t*)CONFIG_SECTOR_ADDR;
    for (uint32_t i = 0; i < 32; i++) {
        if (check[i] != 0xFFFFFFFF) {
            HAL_FLASH_Lock();
            return;
        }
    }

    /* Write metadata */
    uint32_t *src = (uint32_t*)&ota_metadata;
    uint32_t words = (sizeof(OTA_Metadata_t) + 3) / 4;

    for (uint32_t i = 0; i < words; i++) {
        uint32_t addr = CONFIG_SECTOR_ADDR + (i * 4);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, src[i]);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            return;
        }

        /* Verify */
        if (*(volatile uint32_t*)addr != src[i]) {
            HAL_FLASH_Lock();
            return;
        }
    }

    HAL_FLASH_Lock();
}

void Init_Metadata(void)
{
    memset(&ota_metadata, 0, sizeof(OTA_Metadata_t));
    ota_metadata.magic = 0x12345678;
    ota_metadata.active_bank = 0;
    ota_metadata.ota_in_progress = 0;
}

/* ==================== UART Utilities ==================== */
void Send_Response(uint8_t response)
{
    HAL_UART_Transmit(&huart1, &response, 1, 100);
}

void Debug_Print(const char* msg)
{
#if DEBUG_UART_ENABLED
    HAL_UART_Transmit(&huart1, (uint8_t*)msg, strlen(msg), 1000);
#endif
}

void Debug_Printf(const char* format, ...)
{
#if DEBUG_UART_ENABLED
    char buffer[128];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    HAL_UART_Transmit(&huart1, (uint8_t*)buffer, strlen(buffer), 1000);
#endif
}

/* ==================== Peripheral Initialization ==================== */
void MX_UART1_Init(void)
{
    huart1.Instance = USART1;
    huart1.Init.BaudRate = 115200;
    huart1.Init.WordLength = UART_WORDLENGTH_8B;
    huart1.Init.StopBits = UART_STOPBITS_1;
    huart1.Init.Parity = UART_PARITY_NONE;
    huart1.Init.Mode = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;

    if (HAL_UART_Init(&huart1) != HAL_OK) {
        Error_Handler();
    }
}

void MX_CRC_Init(void)
{
    __HAL_RCC_CRC_CLK_ENABLE();
    hcrc.Instance = CRC;

    if (HAL_CRC_Init(&hcrc) != HAL_OK) {
        Error_Handler();
    }
}

void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    /* PA9: USART1_TX, PA10: USART1_RX */
    GPIO_InitStruct.Pin = GPIO_PIN_9 | GPIO_PIN_10;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    RCC_OscInitStruct.PLL.PLLM = 8;
    RCC_OscInitStruct.PLL.PLLN = 336;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = 7;

    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK) {
        Error_Handler();
    }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {
        /* Infinite loop on error */
    }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
    Error_Handler();
}
#endif
