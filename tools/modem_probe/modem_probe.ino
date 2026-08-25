// Temporary hardware probe for the onboard SIM7670G modem (not part of the
// firmware). Auto-detects the board variant (Classic vs Standard pin maps),
// powers the modem up over UART1, and dumps identification plus network/SMS
// capability status as structured events on USB CDC.
//
// Upload: mise exec -- arduino-cli upload tools/modem_probe
// Monitor: mise exec -- arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200

#include <Arduino.h>

#define SerialAT Serial1

constexpr uint32_t kModemBaud = 115200;
constexpr unsigned long kUsbWaitMs = 5000;
constexpr unsigned long kVariantPollMs = 20000;

struct BoardVariant {
    const char* name;
    int rxPin;
    int txPin;
    int pwrKeyPin;
    int dtrPin;
};

// LilyGo utilities.h pin maps for both T-SIM7670G-S3 revisions.
constexpr BoardVariant kVariants[] = {
    {"standard", /*rx*/ 5, /*tx*/ 4, /*pwrkey*/ 46, /*dtr*/ 7},
    {"classic", /*rx*/ 10, /*tx*/ 11, /*pwrkey*/ 18, /*dtr*/ 9},
};

String modemReplyBuffer;

String readModemResponse(unsigned long timeoutMs) {
    String response;
    const unsigned long deadline = millis() + timeoutMs;
    while (millis() < deadline) {
        while (SerialAT.available()) {
            response += static_cast<char>(SerialAT.read());
        }
        if (response.endsWith("\r\nOK\r\n") ||
            response.indexOf("ERROR") >= 0) {
            break;
        }
        delay(10);
    }
    return response;
}

bool modemAlive() {
    for (int attempt = 0; attempt < 10; ++attempt) {
        SerialAT.print("AT\r\n");
        if (readModemResponse(1000).indexOf("OK") >= 0) {
            return true;
        }
        delay(200);
    }
    return false;
}

void query(const __FlashStringHelper* event, const char* command) {
    SerialAT.print(command);
    SerialAT.print("\r\n");
    String response = readModemResponse(3000);
    response.replace("\r", "");
    response.trim();
    response.replace("\n", " | ");
    Serial.print("event=");
    Serial.print(event);
    Serial.print(" cmd=");
    Serial.print(command);
    Serial.print(" reply=\"");
    Serial.print(response);
    Serial.println("\"");
}

// Returns true when the modem answers AT on this variant's pins.
bool tryVariant(const BoardVariant& variant) {
    pinMode(variant.dtrPin, OUTPUT); // keep modem awake
    digitalWrite(variant.dtrPin, LOW);

    // Power-on pulse per LilyGo sequence.
    pinMode(variant.pwrKeyPin, OUTPUT);
    digitalWrite(variant.pwrKeyPin, LOW);
    delay(100);
    digitalWrite(variant.pwrKeyPin, HIGH);
    delay(100);
    digitalWrite(variant.pwrKeyPin, LOW);

    Serial1.begin(kModemBaud, SERIAL_8N1, variant.rxPin, variant.txPin);

    Serial.printf("event=variant_start variant=%s rx=%d tx=%d "
                  "pwrkey=%d dtr=%d\n",
                  variant.name, variant.rxPin, variant.txPin,
                  variant.pwrKeyPin, variant.dtrPin);

    const unsigned long deadline = millis() + kVariantPollMs;
    unsigned long lastHeartbeat = 0;
    bool alive = false;
    while (millis() < deadline) {
        SerialAT.print("AT\r\n");
        String reply = readModemResponse(700);
        if (reply.indexOf("OK") >= 0) {
            alive = true;
            break;
        }
        // Dump anything non-empty as hex so garbage/baud mismatches are
        // visible even without a decodable answer.
        modemReplyBuffer += reply;
        while (modemReplyBuffer.length() > 0) {
            Serial.print("event=uart_noise variant=");
            Serial.print(variant.name);
            Serial.print(" bytes_hex=");
            const size_t chunk =
                min(modemReplyBuffer.length(), static_cast<size_t>(32));
            for (size_t i = 0; i < chunk; ++i) {
                char byteHex[4];
                snprintf(byteHex, sizeof(byteHex), "%02X",
                         static_cast<unsigned char>(modemReplyBuffer[i]));
                Serial.print(byteHex);
            }
            Serial.println();
            modemReplyBuffer.remove(0, chunk);
        }
        if (millis() - lastHeartbeat >= 2000) {
            lastHeartbeat = millis();
            Serial.printf("event=polling variant=%s elapsed_ms=%lu\n",
                          variant.name,
                          static_cast<unsigned long>(kVariantPollMs -
                                                     (deadline - millis())));
        }
    }

    if (alive) {
        Serial.printf("event=modem_ready variant=%s\n", variant.name);
        SerialAT.print("ATE0\r\n");
        readModemResponse(1000);
        query(F("modem_identity"), "ATI");
        query(F("modem_model"), "AT+CGMM");
        query(F("modem_firmware"), "AT+CGMR"); // decisive for SMS-over-SGS
        query(F("modem_imei"), "AT+GSN");
        query(F("sim_status"), "AT+CPIN?");
        query(F("signal_rssi"), "AT+CSQ");
        query(F("signal_lte"), "AT+CESQ");
        query(F("reg_eps"), "AT+CEREG?");
        query(F("reg_cs"), "AT+CREG?");
        query(F("operator"), "AT+COPS?");
        query(F("ps_attach"), "AT+CGATT?");
        query(F("network_clock"), "AT+CCLK?");
        query(F("sms_storage_support"), "AT+CPMS=?");
        query(F("sms_format_support"), "AT+CMGF=?");
        query(F("sms_indication_support"), "AT+CNMI=?");
        query(F("sms_csdh_support"), "AT+CSDH=?");
        query(F("sms_csdh_current"), "AT+CSDH?");
        query(F("sms_cscs_support"), "AT+CSCS=?");
        query(F("sms_cscs_current"), "AT+CSCS?");
        query(F("sms_cpms_current"), "AT+CPMS?");
        query(F("sms_cnmi_current"), "AT+CNMI?");
        // SMS setup for poll-forward-delete cycle (ADR-0004):
        // TEXT + UCS2 + CSDH=1 (show concat header) + ME storage.
        SerialAT.print("AT+CMGF=1\r\n");
        readModemResponse(2000);
        SerialAT.print("AT+CSCS=\"UCS2\"\r\n");
        readModemResponse(2000);
        SerialAT.print("AT+CSDH=1\r\n");
        readModemResponse(2000);
        SerialAT.print("AT+CPMS=\"ME\",\"ME\",\"ME\"\r\n");
        readModemResponse(3000);
        SerialAT.print("AT+CNMI=2,1,0,0,0\r\n");
        readModemResponse(2000);
        query(F("sms_cmgl_unread"), "AT+CMGL=\"REC UNREAD\"");
        query(F("sms_cmgl_all"), "AT+CMGL=\"ALL\"");
        // PDU alternative (for UDH fallback): list via PDU if TEXT hides concat.
        SerialAT.print("AT+CMGF=0\r\n");
        readModemResponse(2000);
        query(F("sms_cmgl_pdu_all"), "AT+CMGL=4");
        SerialAT.print("AT+CMGF=1\r\n");
        readModemResponse(2000);
        Serial.println("event=probe_done");
        return true;
    }
    Serial.printf("event=variant_failed variant=%s\n", variant.name);
    Serial1.end();
    return false;
}

void setup() {
    Serial.begin(115200);
    const unsigned long usbDeadline = millis() + kUsbWaitMs;
    while (!Serial && millis() < usbDeadline) {
        delay(50);
    }
    Serial.println("event=probe_boot");

    for (const auto& variant : kVariants) {
        if (tryVariant(variant)) {
            return; // loop() keeps passthrough after success
        }
    }
    Serial.println(
        "event=modem_unreachable variants_tried=standard,classic");
}

void loop() {
    if (SerialAT.available()) {
        Serial.write(SerialAT.read());
    }
    if (Serial.available()) {
        SerialAT.write(Serial.read());
    }
    delay(1);
}
