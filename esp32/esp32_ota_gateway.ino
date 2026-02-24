/*
 * Author: Saiikishen
 *
 * Firmware files stored in LittleFS:
 *   /fw_v1.bin  → STM32 V1 firmware binary
 *   /fw_v2.bin  → STM32 V2 firmware binary
 */

#include <Arduino.h>
#include <LittleFS.h>
#include <HardwareSerial.h>


#define STM32_UART_TX    17     // ESP32 TX  → STM32 PA9  
#define STM32_UART_RX    16     // ESP32 RX  ← STM32 PA10 
#define STM32_OTA_TRIG   25     // ESP32 OUT → STM32 PA0  
#define STM32_NRST       26     // ESP32 OUT → STM32 NRST 


#define OTA_PACKET_HEADER     0xA5
#define OTA_CMD_START         0x01
#define OTA_CMD_DATA          0x02
#define OTA_CMD_END           0x03
#define OTA_ACK               0x06
#define OTA_NAK               0x15
#define OTA_PACKET_DATA_SIZE  256
#define OTA_UART_TIMEOUT_MS   5000
#define APP_MAX_SIZE          (960UL * 1024UL)
#define OTA_READY_SIGNAL  0x55
#define OTA_READY_TIMEOUT 90000


#define FW_V1_PATH   "/fw_v1.bin"
#define FW_V2_PATH   "/fw_v2.bin"

HardwareSerial stm32(2);   

static uint32_t crc32_init() { return 0xFFFFFFFFUL; }

static void crc32_update(uint32_t &crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
}

static uint32_t crc32_final(uint32_t crc) { return ~crc; }

static uint32_t crc32_buf(const uint8_t *data, size_t len)
{
    uint32_t crc = crc32_init();
    crc32_update(crc, data, len);
    return crc32_final(crc);
}


static void stm32_flush_rx()
{
    while (stm32.available()) stm32.read();
}

static bool wait_ack(uint32_t timeout_ms = OTA_UART_TIMEOUT_MS)
{
    uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        if (stm32.available()) {
            uint8_t b = stm32.read();
            if (b == OTA_ACK) return true;
            if (b == OTA_NAK) {
                Serial.println("  [NAK received]");
                return false;
            }
        }
    }
    Serial.println("  [TIMEOUT — no response]");
    return false;
}


static bool send_start(uint32_t file_size, uint32_t file_crc)
{
    uint8_t payload[8];
    /* Size — big-endian */
    payload[0] = (file_size >> 24) & 0xFF;
    payload[1] = (file_size >> 16) & 0xFF;
    payload[2] = (file_size >>  8) & 0xFF;
    payload[3] = (file_size >>  0) & 0xFF;
    /* Expected image CRC — big-endian */
    payload[4] = (file_crc  >> 24) & 0xFF;
    payload[5] = (file_crc  >> 16) & 0xFF;
    payload[6] = (file_crc  >>  8) & 0xFF;
    payload[7] = (file_crc  >>  0) & 0xFF;

    uint32_t pkt_crc = crc32_buf(payload, 8);

    stm32.write((uint8_t)OTA_PACKET_HEADER);
    stm32.write((uint8_t)OTA_CMD_START);
    stm32.write(payload, 8);
    stm32.write((uint8_t)((pkt_crc >> 24) & 0xFF));
    stm32.write((uint8_t)((pkt_crc >> 16) & 0xFF));
    stm32.write((uint8_t)((pkt_crc >>  8) & 0xFF));
    stm32.write((uint8_t)((pkt_crc >>  0) & 0xFF));

    Serial.printf("  → START  size=%u  img_crc=0x%08X  pkt_crc=0x%08X\n",
                  file_size, file_crc, pkt_crc);
    return wait_ack();
}

static bool send_data_packet(const uint8_t *buf, uint16_t len)
{
    for (int attempt = 1; attempt <= 3; attempt++) {
        uint32_t pkt_crc = crc32_buf(buf, len);

        stm32.write((uint8_t)OTA_PACKET_HEADER);
        stm32.write((uint8_t)OTA_CMD_DATA);
        stm32.write((uint8_t)((len >> 8) & 0xFF));
        stm32.write((uint8_t)((len >> 0) & 0xFF));
        stm32.write(buf, len);
        stm32.write((uint8_t)((pkt_crc >> 24) & 0xFF));
        stm32.write((uint8_t)((pkt_crc >> 16) & 0xFF));
        stm32.write((uint8_t)((pkt_crc >>  8) & 0xFF));
        stm32.write((uint8_t)((pkt_crc >>  0) & 0xFF));

        if (wait_ack(2000)) return true;
        Serial.printf("  [RETRY %d/3]\n", attempt);
    }
    return false;
}

static void send_end()
{
    stm32.write((uint8_t)OTA_PACKET_HEADER);
    stm32.write((uint8_t)OTA_CMD_END);
    Serial.println("  → END sent");
}


static bool stm32_trigger_ota()
{
    Serial.println("[HW] Assert PA0 HIGH (OTA trigger)");
    digitalWrite(STM32_OTA_TRIG, HIGH);
    delay(50);

    Serial.println("[HW] Pulse NRST LOW → HIGH");
    stm32_flush_rx();           
    digitalWrite(STM32_NRST, LOW);
    delay(200);
    digitalWrite(STM32_NRST, HIGH);

    /* Wait for READY signal — STM32 sends 0x55 when backup is done */
    Serial.print("[HW] Waiting for STM32 READY signal (0x55)...");
    uint32_t start = millis();
    while ((millis() - start) < OTA_READY_TIMEOUT) {
        if (stm32.available()) {
            uint8_t b = stm32.read();
            if (b == OTA_READY_SIGNAL) {
                Serial.printf(" received after %lums ✓\n", millis() - start);
                return true;
            }
        }
        if ((millis() - start) % 5000 < 10) {
            Serial.printf(" %lus...", (millis() - start) / 1000);
        }
    }
    Serial.println("\n[ERROR] READY timeout — STM32 backup failed?");
    return false;
}

static void stm32_release_ota()
{
    digitalWrite(STM32_OTA_TRIG, LOW);
    Serial.println("[HW] PA0 released LOW");
}

static void stm32_reset()
{
    Serial.println("[HW] Resetting STM32 (normal boot)...");
    digitalWrite(STM32_NRST, LOW);
    delay(200);
    digitalWrite(STM32_NRST, HIGH);
    Serial.println("[HW] STM32 reset done");
}


static bool ota_flash(const char *path)
{
    Serial.printf("\n══ OTA Flash: %s ══\n", path);

    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("[ERROR] Cannot open %s — did you upload LittleFS?\n", path);
        return false;
    }

    uint32_t file_size = f.size();
    Serial.printf("[FILE] Size: %u bytes (%.1f KB)\n",
                  file_size, file_size / 1024.0f);

    if (file_size == 0 || file_size > APP_MAX_SIZE) {
        Serial.println("[ERROR] Invalid firmware size");
        f.close();
        return false;
    }


    Serial.print("[CRC] Computing... ");
    uint8_t  chunk_buf[256];
    uint32_t crc_acc  = crc32_init();
    uint32_t total_rd = 0;

    f.seek(0);
    while (f.available()) {
        int n = f.read(chunk_buf, sizeof(chunk_buf));
        if (n <= 0) break;
        crc32_update(crc_acc, chunk_buf, (size_t)n);
        total_rd += n;
    }
    uint32_t file_crc = crc32_final(crc_acc);
    Serial.printf("0x%08X\n", file_crc);


    if (!stm32_trigger_ota()) {
        Serial.println("[ERROR] STM32 not ready  aborting");
        f.close();
        stm32_release_ota();
        return false;
    }


    Serial.println("[OTA] Sending START...");
    if (!send_start(file_size, file_crc)) {
        Serial.println("[ERROR] START failed");
        f.close();
        stm32_release_ota();
        return false;
    }
    Serial.println("[OTA] START ACKed ✓");


    Serial.println("[OTA] Sending firmware data...");
    f.seek(0);
    uint32_t sent    = 0;
    uint32_t pkt_num = 0;

    while (f.available()) {
        int n = f.read(chunk_buf, OTA_PACKET_DATA_SIZE);
        if (n <= 0) break;

        if (!send_data_packet(chunk_buf, (uint16_t)n)) {
            Serial.printf("[ERROR] Packet %u failed\n", pkt_num);
            f.close();
            stm32_release_ota();
            return false;
        }

        sent += n;
        pkt_num++;

        if (pkt_num % 16 == 0 || sent == file_size) {
            int pct = (int)((uint64_t)sent * 100ULL / file_size);
            Serial.printf("  [%3d%%] %u / %u bytes  (%u packets)\r",
                          pct, sent, file_size, pkt_num);
        }
    }
    Serial.println();
    Serial.printf("[OTA] All %u packets sent ✓\n", pkt_num);

    /* ── Step 6: Send END ────────────────────────────────────────── */
    send_end();
    f.close();
    stm32_release_ota();

    Serial.println("[OTA] Waiting for STM32 to write internal flash (~10s)...");
    for (int i = 10; i > 0; i--) {
        Serial.printf("  %d...\r", i);
        delay(1000);
    }
    Serial.println("\n[OTA] Done! STM32 should be running new firmware. ✓");
    return true;
}


/* ── Print file info with CRC ────────────────────────────────────── */
static void print_file_info(const char *path)
{
    File f = LittleFS.open(path, "r");
    if (!f) {
        Serial.printf("  %-20s  [NOT FOUND]\n", path);
        return;
    }
    uint32_t sz      = f.size();
    uint32_t crc_acc = crc32_init();
    uint8_t  buf[256];
    while (f.available()) {
        int n = f.read(buf, sizeof(buf));
        crc32_update(crc_acc, buf, n);
    }
    f.close();
    Serial.printf("  %-20s  size=%-8u  CRC32=0x%08X\n",
                  path, sz, crc32_final(crc_acc));
}

/* ─── List all files in LittleFS ─────────────────────────────────── */
static void list_files()
{
    Serial.println("\n── LittleFS Contents ──────────────────────────────");
    File root = LittleFS.open("/");
    File f    = root.openNextFile();
    while (f) {
        Serial.printf("  %-30s  %u bytes\n", f.name(), f.size());
        f = root.openNextFile();
    }
    Serial.println("───────────────────────────────────────────────────\n");
}


void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println("║  ESP32 → STM32 OTA Test Bench    ║");

    const uint8_t tv[] = {'1','2','3','4','5','6','7','8','9'};
    uint32_t result = crc32_buf(tv, 9);
    Serial.printf("[CRC] Self-test: 0x%08X  %s\n",
                  result,
                  result == 0xCBF43926UL ? "PASS ✓" : "FAIL ✗");
    if (result != 0xCBF43926UL) { while(1); }

    /* GPIO init — safe defaults */
    pinMode(STM32_OTA_TRIG, OUTPUT);
    pinMode(STM32_NRST,     OUTPUT);
    digitalWrite(STM32_OTA_TRIG, LOW);
    digitalWrite(STM32_NRST,     HIGH);   /* NRST idle = HIGH */

    /* STM32 UART */
    stm32.begin(115200, SERIAL_8N1, STM32_UART_RX, STM32_UART_TX);
    Serial.println("[UART] STM32 serial on GPIO16 (RX) / GPIO17 (TX) @ 115200");

    /* LittleFS */
    if (!LittleFS.begin(true)) {
        Serial.println("[ERROR] LittleFS mount FAILED");
        return;
    }
    Serial.println("[FS]   LittleFS mounted OK");
    list_files();

    Serial.println("Commands (send via Serial Monitor):");
    Serial.println("  1 → Flash fw_v1.bin to STM32");
    Serial.println("  2 → Flash fw_v2.bin to STM32");
    Serial.println("  l → List LittleFS files");
    Serial.println("  i → File info + CRC32");
    Serial.println("  r → Reset STM32 (normal boot, no OTA)");
    Serial.println();
}

void loop()
{
    if (!Serial.available()) return;

    char cmd = Serial.read();
    switch (cmd) {
        case '1':
            ota_flash(FW_V1_PATH);
            break;
        case '2':
            ota_flash(FW_V2_PATH);
            break;
        case 'l':
            list_files();
            break;
        case 'i':
            print_file_info(FW_V1_PATH);
            print_file_info(FW_V2_PATH);
            break;
        case 'r':
            stm32_reset();
            break;
        default:
            break;
    }
}
