// GPS probe for LilyGO T-SIM7670G-S3 (SIM7670G internal GNSS).
// Tries to power on the modem, enable active antenna bias, start GNSS
// and continuously logs every AT step as structured Serial events.
//
// Board: auto-detects Classic vs Standard pin maps (same as modem_probe).
// Antenna: etecl25t6a-n3-v1 active patch on GNSS IPEX / pogo pin.
// GNSS is inside SIM7670G — no external module needed.
//
// Upload: mise exec -- arduino-cli upload tools/gps_probe
// Monitor: mise exec -- arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
//          (or: arduino-cli monitor, tio, etc.)
//
// Expected quick log on success:
//   event=probe_boot
//   event=variant_start variant=classic ...
//   event=modem_ready variant=classic
//   event=antenna_power cmd=AT+CGDRT=... reply=OK
//   event=gnss_power_set cmd=AT+CGNSSPWR=1 reply=OK
//   event=gnss_poll fix=0 sats=0 ...  (every 3s)
//   event=gnss_fix lat=... lon=... sats=... ttff_ms=...
//
// If fix never appears: antenna without bias, indoors, or 2374B05 issue #394.
// Passthrough stays active in loop() — type AT commands manually.

#include <Arduino.h>

#define SerialAT Serial1

constexpr uint32_t kModemBaud = 115200;
constexpr unsigned long kUsbWaitMs = 5000;
constexpr unsigned long kVariantPollMs = 20000;
constexpr unsigned long kGnssPollIntervalMs = 3000;
constexpr unsigned long kAtTimeoutMs = 3000;
constexpr unsigned long kGnssPowerOnWaitMs = 2000;

struct BoardVariant {
  const char* name;
  int rxPin;
  int txPin;
  int pwrKeyPin;
  int dtrPin;
  int gpsAntGpio; // modem GPIO driving active antenna LNA bias
};

// LilyGo utilities.h pin maps. GPS ant GPIO per
// docs/en/esp32s3/sim7670g-s3/README.MD (Classic=4) and
// docs/en/esp32s3/sim7670g-s3-standard/README.MD (Standard=1).
constexpr BoardVariant kVariants[] = {
    {"standard", /*rx*/ 5,  /*tx*/ 4,  /*pwrkey*/ 46, /*dtr*/ 7, /*ant*/ 1},
    {"classic",  /*rx*/ 10, /*tx*/ 11, /*pwrkey*/ 18, /*dtr*/ 9, /*ant*/ 4},
};

const BoardVariant* gActiveVariant = nullptr;
unsigned long gGnssPowerOnMs = 0;
unsigned long gLastPollMs = 0;
unsigned long gPollCount = 0;
bool gGnssFixAnnounced = false;
bool gGnssPowered = false;

// --- helpers ---------------------------------------------------------------

String readModemResponse(unsigned long timeoutMs) {
  String response;
  const unsigned long deadline = millis() + timeoutMs;
  while (millis() < deadline) {
    while (SerialAT.available()) {
      response += static_cast<char>(SerialAT.read());
    }
    if (response.endsWith("\r\nOK\r\n") ||
        response.indexOf("\r\nERROR\r\n") >= 0 ||
        response.indexOf("+CME ERROR") >= 0 ||
        response.indexOf("+CMS ERROR") >= 0) {
      // drain remainder for 100ms
      const unsigned long tail = millis() + 100;
      while (millis() < tail) {
        while (SerialAT.available()) response += static_cast<char>(SerialAT.read());
        delay(10);
      }
      break;
    }
    delay(10);
  }
  return response;
}

String sanitizeReply(const String& raw) {
  String s = raw;
  s.replace("\r", "");
  s.replace("\n", " | ");
  s.trim();
  // single line, limit length for Serial event
  if (s.length() > 400) s = s.substring(0, 400) + " ...truncated";
  // escape quotes
  s.replace("\"", "'");
  return s;
}

String atTransaction(const char* cmd, unsigned long timeoutMs = kAtTimeoutMs) {
  SerialAT.print(cmd);
  SerialAT.print("\r\n");
  String reply = readModemResponse(timeoutMs);
  String clean = sanitizeReply(reply);
  Serial.printf("event=at_reply cmd=\"%s\" reply=\"%s\"\n", cmd, clean.c_str());
  return reply;
}

bool atOk(const String& reply) {
  return reply.indexOf("\r\nOK\r\n") >= 0 || reply.endsWith("OK") || reply.indexOf(" OK") >= 0;
}

// Try one variant: pulse PWRKEY, open Serial1, poll AT.
bool tryVariant(const BoardVariant& v) {
  pinMode(v.dtrPin, OUTPUT);
  digitalWrite(v.dtrPin, LOW); // keep modem awake

  pinMode(v.pwrKeyPin, OUTPUT);
  digitalWrite(v.pwrKeyPin, LOW);
  delay(100);
  digitalWrite(v.pwrKeyPin, HIGH);
  delay(100);
  digitalWrite(v.pwrKeyPin, LOW);

  Serial1.begin(kModemBaud, SERIAL_8N1, v.rxPin, v.txPin);
  // flush stale bytes
  delay(300);
  while (SerialAT.available()) SerialAT.read();

  Serial.printf("event=variant_start variant=%s rx=%d tx=%d pwrkey=%d dtr=%d ant_gpio=%d\n",
                v.name, v.rxPin, v.txPin, v.pwrKeyPin, v.dtrPin, v.gpsAntGpio);

  const unsigned long deadline = millis() + kVariantPollMs;
  unsigned long lastHeartbeat = 0;
  String noiseBuf;
  while (millis() < deadline) {
    SerialAT.print("AT\r\n");
    String reply = readModemResponse(700);
    if (reply.indexOf("OK") >= 0) {
      Serial.printf("event=modem_ready variant=%s\n", v.name);
      return true;
    }
    if (reply.length() > 0) {
      noiseBuf += reply;
      while (noiseBuf.length() > 0) {
        Serial.printf("event=uart_noise variant=%s bytes_hex=", v.name);
        size_t chunk = min(noiseBuf.length(), (size_t)32);
        for (size_t i = 0; i < chunk; ++i) {
          Serial.printf("%02X", (unsigned char)noiseBuf[i]);
        }
        Serial.println();
        noiseBuf.remove(0, chunk);
      }
    }
    if (millis() - lastHeartbeat >= 2000) {
      lastHeartbeat = millis();
      Serial.printf("event=polling variant=%s elapsed_ms=%lu\n", v.name,
                    (unsigned long)(kVariantPollMs - (deadline - millis())));
    }
  }
  Serial.printf("event=variant_failed variant=%s\n", v.name);
  Serial1.end();
  return false;
}

void queryAndLog(const char* label, const char* cmd, unsigned long timeoutMs = kAtTimeoutMs) {
  Serial.printf("event=at_send cmd=\"%s\" label=%s\n", cmd, label);
  String reply = atTransaction(cmd, timeoutMs);
  (void)reply;
}

void logGnssPoll(unsigned long elapsedMs) {
  // Raw queries — every reply already logged as at_reply.
  // Additionally emit a compact poll summary for quick scan.
  String r1 = atTransaction("AT+CGNSSPWR?", 1500);
  String r2 = atTransaction("AT+CGNSSMODE?", 1500);
  String r3 = atTransaction("AT+CGPSINFO", 3000);
  String r4 = atTransaction("AT+CGNSSINFO", 3000);
  // Optional extras that help diagnose antenna/power issues:
  String r5 = atTransaction("AT+CSQ", 1500);
  String r6 = atTransaction("AT+CBC", 1500);

  bool hasFix = false;
  // CGPSINFO: +CGPSINFO: lat,N,lon,E,date,time,alt,speed,course  — empty fields = no fix
  // CGNSSINFO: +CGNSSINFO: mode,fixSat,totalSat,... — first empty = no fix
  if (r3.indexOf("+CGPSINFO:") >= 0) {
    int idx = r3.indexOf("+CGPSINFO:");
    String payload = r3.substring(idx);
    // count commas after header: if payload contains ",,,"
    // quick heuristic: if payload has at least one digit before first comma
    hasFix = payload.indexOf(",,") == -1 && payload.length() > 20 && payload.indexOf(",,,,") == -1;
    // more robust: empty fix is "+CGPSINFO: ,,,,,,,"
    if (payload.indexOf(",,,,") >= 0) hasFix = false;
    else if (payload.indexOf(": ,") >= 0) hasFix = false;
  }

  gPollCount++;
  Serial.printf("event=gnss_poll count=%lu elapsed_ms=%lu fix_hint=%s heap=%u\n",
                gPollCount, elapsedMs, hasFix ? "maybe" : "no_fix",
                (unsigned)ESP.getFreeHeap());

  if (hasFix && !gGnssFixAnnounced) {
    gGnssFixAnnounced = true;
    unsigned long ttff = millis() - gGnssPowerOnMs;
    Serial.printf("event=gnss_fix poll=%lu ttff_ms=%lu raw_cgps=\"%s\" raw_cgnss=\"%s\"\n",
                  gPollCount, ttff,
                  sanitizeReply(r3).c_str(), sanitizeReply(r4).c_str());
    Serial.println("event=hint msg=\"PPS LED should now flash 1Hz if antenna has sky view\"");
  }

  // Hint for common failure
  if (gPollCount == 10 && !gGnssFixAnnounced) {
    Serial.println("event=hint msg=\"10 polls without fix: check (1) active antenna bias powered, (2) outdoors with sky view, (3) pogo pin / IPEX seated, (4) not indoors near window film\"");
  }
}

void runGnssInitSequence() {
  Serial.println("event=gnss_init_begin");
  // Basic modem setup
  atTransaction("ATE0", 1000);
  atTransaction("ATV1", 1000);
  atTransaction("AT+CMEE=2", 1000);

  queryAndLog("modem_identity", "ATI");
  queryAndLog("modem_model", "AT+CGMM");
  queryAndLog("modem_firmware", "AT+CGMR");
  queryAndLog("modem_imei", "AT+GSN");
  queryAndLog("sim_status", "AT+CPIN?");
  queryAndLog("signal", "AT+CSQ");
  queryAndLog("gnss_pwr_before", "AT+CGNSSPWR?");
  queryAndLog("gnss_mode_before", "AT+CGNSSMODE?");
  queryAndLog("gnss_info_before", "AT+CGNSSINFO");

  // Active antenna bias — the crucial step for etecl25t6a-n3-v1.
  // Must be before CGNSSPWR=1.
  if (gActiveVariant) {
    char cmd1[32], cmd2[32];
    snprintf(cmd1, sizeof(cmd1), "AT+CGDRT=%d,1", gActiveVariant->gpsAntGpio);
    snprintf(cmd2, sizeof(cmd2), "AT+CGSETV=%d,1", gActiveVariant->gpsAntGpio);
    Serial.printf("event=antenna_power_begin gpio=%d variant=%s\n",
                  gActiveVariant->gpsAntGpio, gActiveVariant->name);
    String rA = atTransaction(cmd1, 2000);
    String rB = atTransaction(cmd2, 2000);
    bool okA = atOk(rA);
    bool okB = atOk(rB);
    Serial.printf("event=antenna_power_result gpio=%d ok=%s reply_a=\"%s\" reply_b=\"%s\"\n",
                  gActiveVariant->gpsAntGpio, (okA && okB) ? "true" : "false",
                  sanitizeReply(rA).c_str(), sanitizeReply(rB).c_str());
    if (!okA || !okB) {
      // Fallback: try the other GPIO (some boards wired differently)
      int alt = (gActiveVariant->gpsAntGpio == 1) ? 4 : 1;
      Serial.printf("event=antenna_power_fallback_try gpio=%d\n", alt);
      snprintf(cmd1, sizeof(cmd1), "AT+CGDRT=%d,1", alt);
      snprintf(cmd2, sizeof(cmd2), "AT+CGSETV=%d,1", alt);
      String rC = atTransaction(cmd1, 2000);
      String rD = atTransaction(cmd2, 2000);
      Serial.printf("event=antenna_power_fallback_result gpio=%d ok=%s\n",
                    alt, (atOk(rC) && atOk(rD)) ? "true" : "false");
    }
  }

  // Start GNSS engine
  Serial.println("event=gnss_power_set cmd=\"AT+CGNSSPWR=1\"");
  String pw = atTransaction("AT+CGNSSPWR=1", 5000);
  // Some firmwares need 1-2s after power on
  delay(kGnssPowerOnWaitMs);
  String pwCheck = atTransaction("AT+CGNSSPWR?", 1500);
  gGnssPowered = pwCheck.indexOf(": 1") >= 0;
  Serial.printf("event=gnss_power_status powered=%s reply=\"%s\"\n",
                gGnssPowered ? "true" : "false", sanitizeReply(pwCheck).c_str());
  if (!gGnssPowered) {
    Serial.println("event=gnss_error stage=cgnsspwr_not_on hint=\"CGNSSPWR=1 rejected; retry or check firmware 2374B05\"");
  }

  // Ensure GNSS mode allows GPS+GLONASS+BeiDou (mode 3). Read first, set if needed.
  String mode = atTransaction("AT+CGNSSMODE?", 1500);
  if (mode.indexOf(": 3") == -1 && mode.indexOf(":3") == -1) {
    Serial.println("event=gnss_mode_set cmd=\"AT+CGNSSMODE=3\"");
    atTransaction("AT+CGNSSMODE=3", 2000);
    atTransaction("AT+CGNSSMODE?", 1500);
  }

  // Cold start hint — uncomment if you need forced TTFF test:
  // atTransaction("AT+CGPSCOLD", 2000);

  gGnssPowerOnMs = millis();
  gLastPollMs = 0;
  Serial.printf("event=gnss_init_done elapsed_ms=%lu heap=%u\n",
                (unsigned long)(millis() - gGnssPowerOnMs), (unsigned)ESP.getFreeHeap());
  Serial.println("event=hint msg=\"Polling CGPSINFO/CGNSSINFO every 3s — put active antenna outdoors label-down with sky view. PPS LED: ON=startup, OFF=ready, 1Hz=fix\"");
}

void setup() {
  Serial.begin(115200);
  unsigned long usbDeadline = millis() + kUsbWaitMs;
  while (!Serial && millis() < usbDeadline) delay(50);
  Serial.println("event=probe_boot");
  Serial.println("event=hint msg=\"LilyGo T-SIM7670G-S3 GPS probe — SIM7670G internal GNSS + etecl25t6a active antenna\"");
  Serial.println("event=hint msg=\"GPS Ant Power must be HIGH (CGDRT/CGSETV) before CGNSSPWR=1\"");

  for (auto &v : kVariants) {
    if (tryVariant(v)) {
      gActiveVariant = &v;
      break;
    }
  }
  if (!gActiveVariant) {
    Serial.println("event=modem_unreachable variants_tried=standard,classic hint=\"Check ESP-USB cable, power >=2A, 115200 baud\"");
    return;
  }

  // variant is ready with Serial1 open — run GPS bring-up
  runGnssInitSequence();
}

void loop() {
  // Periodic GNSS poll
  if (gActiveVariant && gGnssPowered && millis() - gLastPollMs >= kGnssPollIntervalMs) {
    gLastPollMs = millis();
    unsigned long elapsed = millis() - gGnssPowerOnMs;
    logGnssPoll(elapsed);
  }

  // AT passthrough for manual debugging (same as modem_probe)
  if (SerialAT.available()) Serial.write(SerialAT.read());
  if (Serial.available()) SerialAT.write(Serial.read());

  // Keep DTR low (awake)
  // (no delay needed, but avoid busy loop)
  delay(1);
}
