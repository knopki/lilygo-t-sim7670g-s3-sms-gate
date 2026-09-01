// #region MODULE_CONTRACT
// PURPOSE: Keeps the ZTE modem dialog and record rules reproducible without hardware.
// SCOPE:
// - Tests ZTE record migration and validation
// - Tests scripted HTTP login, SMS, deletion, cleanup, and send-status flows.
// INVARIANTS:
// - Requests follow scripted wire order;
// - malformed modem responses yield explicit failures;
// - successful deletion is verified by a later listing.
// #endregion MODULE_CONTRACT
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../sms_gate/codec.h"
#include "../sms_gate/zte/zte_client.h"
#include "../sms_gate/zte/zte_json.h"
#include "../sms_gate/zte/zte_record.h"

namespace {

// #region CLASS_FakeZteChannel
// PURPOSE: Keeps one deterministic modem stream so exact goform requests remain testable.
class FakeZteChannel : public ZteChannel {
 public:
  void enqueue(const std::string& response) { stream += response; }

  int connectCalls = 0;
  int stopCalls = 0;
  int connectFailures = 0;
  std::string written;

  bool connect(const char* host, uint16_t port) override {
    (void)host;
    (void)port;
    ++connectCalls;
    if (connectFailures > 0) {
      --connectFailures;
      return false;
    }
    return true;
  }

  bool write(const char* data, size_t length) override {
    written.append(data, length);
    return true;
  }

  int readLine(char* buffer, size_t size) override {
    size_t used = 0;
    while (readPosition < stream.size()) {
      const char ch = stream[readPosition++];
      if (ch == '\n') {
        if (used > 0 && buffer[used - 1] == '\r') {
          --used;
        }
        buffer[used] = '\0';
        return static_cast<int>(used);
      }
      if (used + 1 >= size) {
        return -1;  // Header line overflow is a protocol failure.
      }
      buffer[used++] = ch;
    }
    return -1;
  }

  int read(char* buffer, size_t size) override {
    const size_t available = stream.size() - readPosition;
    if (available == 0) {
      return -1;
    }
    const size_t take = size < available ? size : available;
    memcpy(buffer, stream.data() + readPosition, take);
    readPosition += take;
    return static_cast<int>(take);
  }

  void stop() override { ++stopCalls; }

 private:
  std::string stream;
  size_t readPosition = 0;
};
// #endregion CLASS_FakeZteChannel

// #region FUNC_httpResponse
// PURPOSE: Supplies realistic HTTP framing so response parsing stays covered.
std::string httpResponse(const std::string& body, const std::string& extraHeaders = "") {
  return "HTTP/1.1 200 OK\r\n" + extraHeaders + "Content-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}
// #endregion FUNC_httpResponse

// #region FUNC_loginResponse
// PURPOSE: Supplies a session cookie so authenticated requests remain testable.
std::string loginResponse() {
  return httpResponse("{\"result\":\"0\"}", "Set-Cookie: stok=ABC123; Path=/\r\n");
}
// #endregion FUNC_loginResponse

// #region FUNC_versionsResponse
// PURPOSE: Supplies the version input required to make AD-token checks deterministic.
std::string versionsResponse() {
  return httpResponse("{\"cr_version\":\"\",\"wa_inner_version\":\"BD_MF79RUV1.0.0B02\"}");
}
// #endregion FUNC_versionsResponse

// #region FUNC_smsResponse
// PURPOSE: Keeps SMS listing fixtures controllable so paging decisions remain testable.
std::string smsResponse(const std::vector<std::string>& entries) {
  std::string body = "{\"messages\":[";
  for (size_t index = 0; index < entries.size(); ++index) {
    if (index > 0) {
      body += ',';
    }
    body += entries[index];
  }
  body += "]}";
  return httpResponse(body);
}
// #endregion FUNC_smsResponse

// #region FUNC_smsEntry
// PURPOSE: Keeps message fixtures concise while allowing precise field mutations.
std::string smsEntry(const char* id, const char* tag, const char* content = "004F004B",
                     const char* receivedAll = "1", const char* concatTotal = "1",
                     const char* concatReceived = "1") {
  std::string entry = "{\"id\":\"";
  entry += id;
  entry += "\",\"number\":\"+70001234567\",\"content\":\"";
  entry += content;
  entry += "\",\"tag\":\"";
  entry += tag;
  entry +=
      "\",\"date\":\"26,01,02,03,04,05,+12\",\"draft_group_id\":\"\","
      "\"received_all_concat_sms\":\"";
  entry += receivedAll;
  entry += "\",\"concat_sms_total\":\"";
  entry += concatTotal;
  entry += "\",\"concat_sms_received\":\"";
  entry += concatReceived;
  entry += "\",\"sms_class\":\"1\"}";
  return entry;
}
// #endregion FUNC_smsEntry

// #region FUNC_countOccurrences
// PURPOSE: Makes repeated request behavior directly assertable in the wire trace.
int countOccurrences(const std::string& text, const std::string& needle) {
  int count = 0;
  for (size_t position = text.find(needle); position != std::string::npos;
       position = text.find(needle, position + needle.size())) {
    ++count;
  }
  return count;
}
// #endregion FUNC_countOccurrences

// #region FUNC_makeRecord
// PURPOSE: Supplies a valid baseline so each record mutation isolates one rule.
ZteConfigRecord makeRecord() {
  ZteConfigRecord record{};
  record.magic = kZteConfigMagic;
  record.version = kZteConfigVersion;
  record.moduleEnabled = 1;
  record.forwardEnabled = 1;
  strcpy(record.host, "192.168.0.1");
  strcpy(record.password, "modem-pass");
  strcpy(record.label, "");
  record.pollIntervalSec = kDefaultZtePollSec;
  record.checksum = calculateZteConfigChecksum(record);
  return record;
}
// #endregion FUNC_makeRecord

// #region FUNC_testCodecVectors
// PURPOSE: Prevents codec regressions from invalidating later dialog assertions.
void testCodecVectors() {
  char hex[33];
  codec::md5Hex("", 0, hex);
  assert(strcmp(hex, "d41d8cd98f00b204e9800998ecf8427e") == 0);
  codec::md5Hex("abc", 3, hex);
  assert(strcmp(hex, "900150983cd24fb0d6963f7d28e17f72") == 0);
  const char* fox = "The quick brown fox jumps over the lazy dog";
  codec::md5Hex(fox, strlen(fox), hex);
  assert(strcmp(hex, "9e107d9d372bb6826bd81d3542a419d6") == 0);

  char encoded[16];
  assert(codec::encodeBase64("admin", 5, encoded, sizeof(encoded)) == 8 &&
         strcmp(encoded, "YWRtaW4=") == 0);
  assert(codec::encodeBase64("a", 1, encoded, sizeof(encoded)) == 4 &&
         strcmp(encoded, "YQ==") == 0);
  assert(codec::encodeBase64("abc", 3, encoded, sizeof(encoded)) == 4 &&
         strcmp(encoded, "YWJj") == 0);
  assert(codec::encodeBase64("admin", 5, encoded, 8) == 0);  // Needs 9 bytes.
  puts("testCodecVectors ok");
}
// #endregion FUNC_testCodecVectors

// #region FUNC_testParseUint32String
// PURPOSE: Rejects out-of-range modem IDs before they can alias valid IDs.
void testParseUint32String() {
  uint32_t value = 0;
  assert(parseUint32String("4294967295", value));
  assert(value == UINT32_MAX);
  assert(!parseUint32String("4294967296", value));
  assert(!parseUint32String("4294967297", value));
  assert(!parseUint32String("42949672960", value));
  assert(!parseUint32String("4294967295x", value));
  puts("testParseUint32String ok");
}
// #endregion FUNC_testParseUint32String

// #region FUNC_makeV3Record
// PURPOSE: Supplies a legacy blob so migration validation remains reproducible.
ZteConfigRecordV3 makeV3Record() {
  ZteConfigRecordV3 record{};
  record.magic = kZteConfigMagic;
  record.version = 3;
  record.enabled = 1;
  strcpy(record.host, "192.168.0.1");
  strcpy(record.password, "modem-pass");
  strcpy(record.label, "+79990000000");
  record.pollIntervalSec = kDefaultZtePollSec;
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ZteConfigRecordV3, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  record.checksum = hash;
  return record;
}
// #endregion FUNC_makeV3Record

// #region FUNC_testRecordValidation
// PURPOSE: Prevents invalid records from reaching the modem dialog.
void testRecordValidation() {
  ZteConfigRecord record = makeRecord();
  assert(isZteConfigRecordValid(record));

  strcpy(record.label, "+79990000000 (ZTE)");
  record.checksum = calculateZteConfigChecksum(record);
  assert(isZteConfigRecordValid(record));

  strcpy(record.label, "bad\x01label");
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.checksum ^= 1;
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.version = static_cast<uint16_t>(kZteConfigVersion + 1);  // Foreign future version.
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.magic = 0x11111111;
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.moduleEnabled = 7;
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.forwardEnabled = 7;
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.host[0] = '\0';
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.host[3] = '\x01';
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.password[0] = '\0';
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.moduleEnabled = 0;  // Disabled module still keeps complete credentials.
  record.forwardEnabled = 0;
  record.checksum = calculateZteConfigChecksum(record);
  assert(isZteConfigRecordValid(record));

  record = makeRecord();
  record.moduleEnabled = 0;
  record.forwardEnabled = 1;  // module off but forward on is still valid record
  record.checksum = calculateZteConfigChecksum(record);
  assert(isZteConfigRecordValid(record));

  record = makeRecord();
  record.pollIntervalSec = kMinZtePollSec;
  record.checksum = calculateZteConfigChecksum(record);
  assert(isZteConfigRecordValid(record));

  record = makeRecord();
  record.pollIntervalSec = kMaxZtePollSec;
  record.checksum = calculateZteConfigChecksum(record);
  assert(isZteConfigRecordValid(record));

  record = makeRecord();
  record.pollIntervalSec = kMinZtePollSec - 1;
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.pollIntervalSec = kMaxZtePollSec + 1;
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  record = makeRecord();
  record.pollIntervalSec = 0;
  record.checksum = calculateZteConfigChecksum(record);
  assert(!isZteConfigRecordValid(record));

  assert(isValidZtePollInterval(kDefaultZtePollSec));
  assert(isValidZtePollInterval(kMinZtePollSec));
  assert(isValidZtePollInterval(kMaxZtePollSec));
  assert(!isValidZtePollInterval(kMinZtePollSec - 1));
  assert(!isValidZtePollInterval(kMaxZtePollSec + 1));
  puts("testRecordValidation ok");
}
// #endregion FUNC_testRecordValidation

// #region FUNC_testV3MigrationValidation
// PURPOSE: Protects legacy-record recognition before migration to v4.
void testV3MigrationValidation() {
  assert(isZteConfigRecordV3Valid(makeV3Record()));
  ZteConfigRecordV3 record = makeV3Record();
  record.checksum ^= 1;
  assert(!isZteConfigRecordV3Valid(record));
  record = makeV3Record();
  record.version = 4;
  assert(!isZteConfigRecordV3Valid(record));
  record = makeV3Record();
  record.label[0] = '\x01';
  {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t hash = 2166136261UL;
    for (size_t index = 0; index < offsetof(ZteConfigRecordV3, checksum); ++index) {
      hash ^= bytes[index];
      hash *= 16777619UL;
    }
    record.checksum = hash;
  }
  assert(!isZteConfigRecordV3Valid(record));
  puts("testV3MigrationValidation ok");
}
// #endregion FUNC_testV3MigrationValidation

// #region FUNC_testLoginSuccess
// PURPOSE: Protects session setup and AD-token inputs from request regressions.
void testLoginSuccess() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(strcmp(modem.waVersion(), "BD_MF79RUV1.0.0B02") == 0);
  assert(modem.failedStage()[0] == '\0');

  assert(countOccurrences(channel.written, "POST /goform/goform_set_cmd_process HTTP/1.1") == 1);
  assert(countOccurrences(channel.written, "isTest=false&goformId=LOGIN&password=YWRtaW4=") == 1);
  assert(countOccurrences(channel.written, "Referer: http://192.168.0.1/index.html") == 2);
  assert(countOccurrences(channel.written, "Host: 192.168.0.1") == 2);

  // The version GET carries the captured session cookie.
  assert(countOccurrences(channel.written, "Cookie: stok=ABC123") == 1);
  assert(countOccurrences(channel.written,
                          "GET /goform/goform_get_cmd_process?isTest=false&"
                          "cmd=cr_version,wa_inner_version&multi_data=1") == 1);
  puts("testLoginSuccess ok");
}
// #endregion FUNC_testLoginSuccess

// #region FUNC_testLoginRejected
// PURPOSE: Keeps password and session-cookie failures diagnostically distinct.
void testLoginRejected() {
  FakeZteChannel channel;
  channel.enqueue(httpResponse("{\"result\":\"1\"}", "Set-Cookie: stok=ABC123; Path=/\r\n"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "wrong") == ZteResult::kLoginRejected);
  assert(strcmp(modem.failedStage(), "login") == 0);

  FakeZteChannel cookieless;
  cookieless.enqueue(httpResponse("{\"result\":\"0\"}"));
  ZteModem second(cookieless, scratch.data(), scratch.size());
  assert(second.login("192.168.0.1", "admin") == ZteResult::kLoginRejected);
  assert(strcmp(second.failedStage(), "login_cookie") == 0);
  puts("testLoginRejected ok");
}
// #endregion FUNC_testLoginRejected

// #region FUNC_testFindOldestAscending
// PURPOSE: Protects oldest-message selection and decoded listing semantics.
void testFindOldestAscending() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // Entries: sent, unread incoming 101, draft, read incoming 103, sent.
  channel.enqueue(smsResponse({smsEntry("100", "2"), smsEntry("101", "1"), smsEntry("102", "4"),
                               smsEntry("103", "0"), smsEntry("104", "2")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "101") == 0);
  assert(strcmp(sms.number, "+70001234567") == 0);
  assert(strcmp(sms.textUtf8, "OK") == 0);
  assert(sms.concatComplete);
  assert(strcmp(sms.concatReceived, "1") == 0);
  assert(strcmp(sms.concatTotal, "1") == 0);
  assert(strcmp(sms.dateRaw, "26,01,02,03,04,05,+12") == 0);
  assert(countOccurrences(channel.written,
                          "cmd=sms_data_total&page=0&data_per_page=5&mem_store=1&tags=10&"
                          "order_by=order+by+id+asc") == 1);
  puts("testFindOldestAscending ok");
}
// #endregion FUNC_testFindOldestAscending

// #region FUNC_testFindOldestDescending
// PURPOSE: Protects oldest-message selection when firmware reverses page order.
void testFindOldestDescending() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // Descending page 0 (5 entries, count equals page size, so the scan must
  // continue) and an empty final page.
  channel.enqueue(smsResponse({smsEntry("104", "2"), smsEntry("103", "1"), smsEntry("102", "2"),
                               smsEntry("101", "0"), smsEntry("100", "2")}));
  channel.enqueue(smsResponse({}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "101") == 0);  // Oldest incoming, not newest.
  assert(countOccurrences(channel.written, "page=1&") == 1);
  puts("testFindOldestDescending ok");
}
// #endregion FUNC_testFindOldestDescending

// #region FUNC_testFindOldestContinuesPages
// PURPOSE: Protects scans that must continue past pages without incoming SMS.
void testFindOldestContinuesPages() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({smsEntry("100", "2"), smsEntry("101", "2"), smsEntry("102", "2"),
                               smsEntry("103", "2"), smsEntry("104", "2")}));
  channel.enqueue(smsResponse({smsEntry("105", "1"), smsEntry("106", "2")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "105") == 0);
  puts("testFindOldestContinuesPages ok");
}
// #endregion FUNC_testFindOldestContinuesPages

// #region FUNC_testFindOldestEmpty
// PURPOSE: Keeps an empty inbox a quiet, successful poll result.
void testFindOldestEmpty() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = true;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(!found);
  puts("testFindOldestEmpty ok");
}
// #endregion FUNC_testFindOldestEmpty

// #region FUNC_testIncompleteConcatAndDecode
// PURPOSE: Protects incomplete-message diagnostics and UCS-2 decoding fallbacks.
void testIncompleteConcatAndDecode() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // Cyrillic text "В связи" plus U+1F600 written as a surrogate pair.
  const char* content = "0412002004410432044F04370438D83DDE00";
  channel.enqueue(smsResponse({smsEntry("200", "1", content, "0", "5", "1")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(!sms.concatComplete);
  assert(strcmp(sms.concatTotal, "5") == 0);
  assert(strcmp(sms.concatReceived, "1") == 0);
  const unsigned char expected[] = {0xD0, 0x92, 0x20, 0xD1, 0x81, 0xD0, 0xB2, 0xD1, 0x8F,
                                    0xD0, 0xB7, 0xD0, 0xB8, 0xF0, 0x9F, 0x98, 0x80};
  assert(strlen(sms.textUtf8) == sizeof(expected));
  assert(memcmp(sms.textUtf8, expected, sizeof(expected)) == 0);
  puts("testIncompleteConcatAndDecode ok");
}
// #endregion FUNC_testIncompleteConcatAndDecode

// #region FUNC_testNonHexContentFallback
// PURPOSE: Keeps non-hex modem content forwardable without lossy decoding.
void testNonHexContentFallback() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({smsEntry("300", "0", "plain text")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.textUtf8, "plain text") == 0);
  puts("testNonHexContentFallback ok");
}
// #endregion FUNC_testNonHexContentFallback

// #region FUNC_testControlCharactersInNumber
// PURPOSE: Preserves scanability of firmware replies containing raw controls.
void testControlCharactersInNumber() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  std::string entry = smsEntry("400", "1", "0041");
  entry.insert(entry.find("+70001234567") + 1, 1, '\x01');  // Raw control byte.
  channel.enqueue(smsResponse({entry}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "400") == 0);
  assert(sms.number[0] == '+');
  assert(sms.number[1] == '\x01');
  puts("testControlCharactersInNumber ok");
}
// #endregion FUNC_testControlCharactersInNumber

// #region FUNC_testStaleSessionRelogin
// PURPOSE: Keeps stale sessions recoverable without duplicate login loops.
void testStaleSessionRelogin() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"sms_data_total\":\"\"}"));  // Stale shape.
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({smsEntry("500", "1", "0041")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "500") == 0);
  assert(countOccurrences(channel.written, "goformId=LOGIN") == 2);
  puts("testStaleSessionRelogin ok");
}
// #endregion FUNC_testStaleSessionRelogin

// #region FUNC_testDeleteFlow
// PURPOSE: Protects deletion requests and confirms the message is truly gone.
void testDeleteFlow() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({smsEntry("600", "1", "0041")}));
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  // After deletion the inbox starts above the deleted id.
  channel.enqueue(smsResponse({smsEntry("601", "1"), smsEntry("602", "2")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = false;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(found);
  assert(strcmp(sms.id, "600") == 0);

  // AD = md5(md5("BD_MF79RUV1.0.0B02") + "AB12CD34"), precomputed vector.
  assert(modem.deleteSms(sms) == ZteResult::kSuccess);
  assert(modem.failedStage()[0] == '\0');
  assert(countOccurrences(channel.written,
                          "GET /goform/goform_get_cmd_process?isTest=false&cmd=RD") == 1);
  assert(countOccurrences(channel.written,
                          "isTest=false&goformId=DELETE_SMS&msg_id=600%3B&notCallback=true&"
                          "AD=02bb862c133c79826efbe952c8a57c34") == 1);
  puts("testDeleteFlow ok");
}
// #endregion FUNC_testDeleteFlow

// #region FUNC_testDeleteRejected
// PURPOSE: Keeps refused deletions visible so messages remain retryable.
void testDeleteRejected() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"failure\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  strcpy(sms.id, "600");
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.deleteSms(sms) == ZteResult::kProtocolError);
  assert(strcmp(modem.failedStage(), "delete") == 0);
  puts("testDeleteRejected ok");
}
// #endregion FUNC_testDeleteRejected

// #region FUNC_testDeleteUnverified
// PURPOSE: Prevents a surviving message from being falsely treated as deleted.
void testDeleteUnverified() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  channel.enqueue(smsResponse({smsEntry("600", "1"), smsEntry("601", "2")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  strcpy(sms.id, "600");
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.deleteSms(sms) == ZteResult::kProtocolError);
  assert(strcmp(modem.failedStage(), "delete_unverified") == 0);
  puts("testDeleteUnverified ok");
}
// #endregion FUNC_testDeleteUnverified

// #region FUNC_testCleanupOutgoing
// PURPOSE: Keeps outgoing cleanup complete despite paging shifts and protects incoming SMS.
void testCleanupOutgoing() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // B02 ignores page, so each tag-specific page zero becomes the work
  // queue: deleting 701 exposes the next matching record on the next scan.
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("701", "2"), smsEntry("703", "4")}));
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("703", "4")}));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("703", "4")}));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("702", "3"), smsEntry("703", "4")}));
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("703", "4")}));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("703", "4")}));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  uint16_t deleted = 99;
  assert(modem.cleanupOutgoing(deleted) == ZteResult::kSuccess);
  assert(deleted == 2);
  assert(countOccurrences(channel.written, "tags=2") >= 3);
  assert(countOccurrences(channel.written, "tags=3") >= 3);
  assert(countOccurrences(channel.written, "goformId=DELETE_SMS&msg_id=701%3B") == 1);
  assert(countOccurrences(channel.written, "goformId=DELETE_SMS&msg_id=702%3B") == 1);
  assert(countOccurrences(channel.written, "goformId=DELETE_SMS&msg_id=700%3B") == 0);
  assert(countOccurrences(channel.written, "goformId=DELETE_SMS&msg_id=703%3B") == 0);
  puts("testCleanupOutgoing ok");
}
// #endregion FUNC_testCleanupOutgoing

// #region FUNC_testCleanupOutgoingDeleteRejected
// PURPOSE: Prevents failed outgoing cleanup from being reported as complete.
void testCleanupOutgoingDeleteRejected() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({}));  // tags=2 is empty.
  channel.enqueue(smsResponse({smsEntry("703", "3")}));
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"failure\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  uint16_t deleted = 99;
  assert(modem.cleanupOutgoing(deleted) == ZteResult::kProtocolError);
  assert(deleted == 0);
  assert(strcmp(modem.failedStage(), "delete") == 0);
  puts("testCleanupOutgoingDeleteRejected ok");
}
// #endregion FUNC_testCleanupOutgoingDeleteRejected

// #region FUNC_testInboxStatus
// PURPOSE: Keeps storage occupancy reporting aligned with the operator route.
void testInboxStatus() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(
      httpResponse("{\"sms_nv_total\":\"100\",\"sms_sim_total\":\"15\","
                   "\"sms_nvused_total\":\"3\",\"sms_nv_rev_total\":\"3\","
                   "\"sms_nv_send_total\":\"0\",\"sms_nv_draftbox_total\":\"0\","
                   "\"sms_sim_rev_total\":\"0\",\"sms_sim_send_total\":\"0\","
                   "\"sms_sim_draftbox_total\":\"0\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  ZteInboxStatus status{};
  assert(modem.readInboxStatus(status) == ZteResult::kSuccess);
  assert(status.used == 3);
  assert(status.total == 100);
  puts("testInboxStatus ok");
}
// #endregion FUNC_testInboxStatus

// #region FUNC_testHttpFailures
// PURPOSE: Keeps transport and framing failures diagnostically distinct.
void testHttpFailures() {
  std::vector<char> scratch(4096);

  FakeZteChannel connectFailure;
  connectFailure.connectFailures = 1;
  ZteModem first(connectFailure, scratch.data(), scratch.size());
  assert(first.login("192.168.0.1", "admin") == ZteResult::kConnectFailed);
  assert(strcmp(first.failedStage(), "connect") == 0);

  FakeZteChannel badStatus;
  badStatus.enqueue("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
  ZteModem second(badStatus, scratch.data(), scratch.size());
  assert(second.login("192.168.0.1", "admin") == ZteResult::kHttpFailed);
  assert(strcmp(second.failedStage(), "http_status") == 0);

  FakeZteChannel garbage;
  garbage.enqueue("not-http\r\n\r\n");
  ZteModem third(garbage, scratch.data(), scratch.size());
  assert(third.login("192.168.0.1", "admin") == ZteResult::kHttpFailed);
  assert(strcmp(third.failedStage(), "http_status") == 0);

  FakeZteChannel truncated;
  truncated.enqueue(loginResponse() + "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  ZteModem fourth(truncated, scratch.data(), scratch.size());
  assert(fourth.login("192.168.0.1", "admin") == ZteResult::kHttpFailed);
  assert(strcmp(fourth.failedStage(), "http_body") == 0);

  FakeZteChannel chunked;
  chunked.enqueue("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
  ZteModem fifth(chunked, scratch.data(), scratch.size());
  assert(fifth.login("192.168.0.1", "admin") == ZteResult::kProtocolError);
  assert(strcmp(fifth.failedStage(), "http_chunked") == 0);
  puts("testHttpFailures ok");
}
// #endregion FUNC_testHttpFailures

// #region FUNC_testBodyWithoutContentLength
// PURPOSE: Preserves compatibility with modem responses lacking Content-Length.
void testBodyWithoutContentLength() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n{\"messages\":[]}");
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  ZteSms sms{};
  bool found = true;
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.findOldestIncoming(sms, found) == ZteResult::kSuccess);
  assert(!found);
  puts("testBodyWithoutContentLength ok");
}
// #endregion FUNC_testBodyWithoutContentLength

// #region FUNC_testSendSmsFlow
// PURPOSE: Protects byte-exact SMS submission and status polling requests.
void testSendSmsFlow() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  channel.enqueue(httpResponse("{\"sms_cmd_status_result\":\"3\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);

  // "Привет " + U+1F600 as raw UTF-8 bytes.
  const char* text = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80";
  assert(modem.sendSms("+79990000000", text) == ZteResult::kSuccess);
  assert(modem.failedStage()[0] == '\0');
  assert(countOccurrences(channel.written,
                          "goformId=SEND_SMS&notCallback=true&Number=%2B79990000000") == 1);
  // UCS-2 hex: 041F 0440 0438 0432 0435 0442 0020 D83D DE00.
  assert(countOccurrences(channel.written,
                          "&MessageBody=041F044004380432043504420020D83DDE00"
                          "&ID=-1&encode_type=UNICODE&AD=02bb862c133c79826efbe952c8a57c34") == 1);
  // sms_time shape: six 2-digit fields and six %3B separators, then the
  // UNPADDED "+0" offset ("%2B0" escaped) exactly like the browser's "+3".
  const size_t start = channel.written.find("sms_time=") + strlen("sms_time=");
  const size_t end = channel.written.find("&MessageBody=", start);
  const std::string smsTime = channel.written.substr(start, end - start);
  assert(smsTime.length() == 34);  // 6 x 2 digits + 6 x "%3B" + "%2B0".
  assert(countOccurrences(smsTime, "%3B") == 6);
  assert(smsTime.substr(smsTime.length() - 4) == "%2B0");

  ZteSendStatus status = ZteSendStatus::kFailed;
  assert(modem.readSendStatus(status) == ZteResult::kSuccess);
  assert(status == ZteSendStatus::kDone);
  assert(countOccurrences(channel.written,
                          "GET /goform/goform_get_cmd_process?isTest=false&"
                          "cmd=sms_cmd_status_info&sms_cmd=4") == 1);
  // The request must END exactly with the form: no bytes may follow the
  // AD before the next request begins (an unterminated body once leaked
  // stack bytes onto the wire).
  const std::string expectedTail = "&ID=-1&encode_type=UNICODE&AD=02bb862c133c79826efbe952c8a57c34";
  assert(countOccurrences(channel.written, expectedTail +
                                               "GET /goform/goform_get_cmd_process?isTest=false&"
                                               "cmd=sms_cmd_status_info&sms_cmd=4") == 1);
  puts("testSendSmsFlow ok");
}
// #endregion FUNC_testSendSmsFlow

// #region FUNC_testSendSmsAsciiUsesGsm7
// PURPOSE: Protects the UI-compatible GSM7 request shape used by the modem.
void testSendSmsAsciiUsesGsm7() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.sendSms("+79685557161", "test") == ZteResult::kSuccess);
  assert(countOccurrences(channel.written, "Number=%2B79685557161&") == 1);
  assert(countOccurrences(channel.written,
                          "&MessageBody=0074006500730074&ID=-1&encode_type=GSM7_default&") == 1);
  // Exact boundary again for the ASCII path: the form is the final request.
  const size_t ad = channel.written.rfind("AD=");
  assert(ad != std::string::npos);
  assert(channel.written.size() - ad == 35);  // AD= + 32 hex.
  puts("testSendSmsAsciiUsesGsm7 ok");
}
// #endregion FUNC_testSendSmsAsciiUsesGsm7

// #region FUNC_testSendSmsEmptyReply
// PURPOSE: Keeps empty successful HTTP bodies diagnostically distinct.
void testSendSmsEmptyReply() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse(""));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.sendSms("+79990000000", "hello") == ZteResult::kSendRejected);
  assert(strcmp(modem.failedStage(), "send_reply_empty") == 0);
  assert(modem.lastBodyLength() == 0);
  puts("testSendSmsEmptyReply ok");
}
// #endregion FUNC_testSendSmsEmptyReply

// #region FUNC_testSendSmsRejected
// PURPOSE: Keeps send refusal distinct from transport and protocol failures.
void testSendSmsRejected() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"failure\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  assert(modem.sendSms("+79990000000", "hello") == ZteResult::kSendRejected);
  assert(strcmp(modem.failedStage(), "send") == 0);
  assert(strcmp(modem.lastBody(), "{\"result\":\"failure\"}") == 0);
  puts("testSendSmsRejected ok");
}
// #endregion FUNC_testSendSmsRejected

// #region FUNC_testSendStatusSamples
// PURPOSE: Keeps sampled send status outcomes stable, including missing fields.
void testSendStatusSamples() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"sms_cmd_status_result\":\"1\"}"));
  channel.enqueue(httpResponse("{\"messages\":[]}"));
  channel.enqueue(httpResponse("{\"sms_cmd_status_result\":\"2\"}"));
  channel.enqueue(httpResponse("{\"sms_cmd_status_result\":\"3\"}"));
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.login("192.168.0.1", "admin") == ZteResult::kSuccess);
  ZteSendStatus status = ZteSendStatus::kDone;
  assert(modem.readSendStatus(status) == ZteResult::kSuccess);
  assert(status == ZteSendStatus::kInProgress);
  assert(modem.readSendStatus(status) == ZteResult::kSuccess);
  assert(status == ZteSendStatus::kInProgress);
  assert(modem.readSendStatus(status) == ZteResult::kSuccess);
  assert(status == ZteSendStatus::kFailed);
  assert(strcmp(modem.failedStage(), "send_status") == 0);
  assert(modem.readSendStatus(status) == ZteResult::kSuccess);
  assert(status == ZteSendStatus::kDone);
  puts("testSendStatusSamples ok");
}
// #endregion FUNC_testSendStatusSamples

// #region FUNC_testSendSmsInputValidation
// PURPOSE: Prevents invalid SMS input from reaching the modem.
void testSendSmsInputValidation() {
  FakeZteChannel channel;
  std::vector<char> scratch(4096);
  ZteModem modem(channel, scratch.data(), scratch.size());
  assert(modem.sendSms("", "hello") == ZteResult::kProtocolError);
  assert(strcmp(modem.failedStage(), "send_input") == 0);
  assert(modem.sendSms("123", "") == ZteResult::kProtocolError);
  assert(strcmp(modem.failedStage(), "send_input") == 0);
  assert(modem.sendSms("12\x7F", "hello") == ZteResult::kProtocolError);
  assert(modem.sendSms("123", "\xFF") == ZteResult::kProtocolError);
  assert(modem.sendSms("123", "\xC3\x28") == ZteResult::kProtocolError);
  std::string tooLong(kMaxZteSmsSendUnits + 1, 'A');
  assert(modem.sendSms("123", tooLong.c_str()) == ZteResult::kProtocolError);
  std::string tooLongNumber(kZteNumberLength + 1, '1');
  assert(modem.sendSms(tooLongNumber.c_str(), "hello") == ZteResult::kProtocolError);
  assert(countOccurrences(channel.written, "goformId=SEND_SMS") == 0);
  assert(countOccurrences(channel.written, "goformId=LOGIN") == 0);
  puts("testSendSmsInputValidation ok");
}
// #endregion FUNC_testSendSmsInputValidation

// #region FUNC_testZteSmsUtf16Units
// PURPOSE: Protects the shared UTF-16 length boundary used for SMS limits.
void testZteSmsUtf16Units() {
  assert(zteSmsUtf16Units("") == 0);
  assert(zteSmsUtf16Units("abc") == 3);
  assert(zteSmsUtf16Units("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == 6);
  assert(zteSmsUtf16Units("\xF0\x9F\x98\x80") == 2);
  assert(zteSmsUtf16Units("hi \xF0\x9F\x98\x80!") == 6);
  assert(zteSmsUtf16Units("\xFF") == kZteSmsInvalidUnits);
  assert(zteSmsUtf16Units("\xC3\x28") == kZteSmsInvalidUnits);      // Bad continuation.
  assert(zteSmsUtf16Units("\xC0\xAF") == kZteSmsInvalidUnits);      // Overlong '/'.
  assert(zteSmsUtf16Units("\xED\xA0\x80") == kZteSmsInvalidUnits);  // Encoded surrogate.
  assert(zteSmsUtf16Units("\xF0\x9F\x98") == kZteSmsInvalidUnits);  // Truncated.
  puts("testZteSmsUtf16Units ok");
}
// #endregion FUNC_testZteSmsUtf16Units

// #region FUNC_testFormatZteDate
// PURPOSE: Keeps modem timestamps readable while preserving unexpected input.
void testFormatZteDate() {
  char out[64];
  assert(formatZteDate("26,08,24,13,48,05,+12", out, sizeof(out)));
  assert(strcmp(out, "2026-08-24 13:48:05 UTC+03:00") == 0);
  assert(formatZteDate("25,01,02,03,04,05,-8", out, sizeof(out)));
  assert(strcmp(out, "2025-01-02 03:04:05 UTC-02:00") == 0);
  assert(formatZteDate("25,12,31,23,59,59,+5", out, sizeof(out)));
  assert(strcmp(out, "2025-12-31 23:59:59 UTC+01:15") == 0);
  // Out-of-range fields fall back to the raw text.
  assert(!formatZteDate("26,13,24,13,48,05,+12", out, sizeof(out)));
  assert(strcmp(out, "26,13,24,13,48,05,+12") == 0);
  // Trailing garbage falls back to the raw text.
  assert(!formatZteDate("26,08,24,13,48,05,+12x", out, sizeof(out)));
  assert(strcmp(out, "26,08,24,13,48,05,+12x") == 0);
  // Empty input copies verbatim.
  assert(!formatZteDate("", out, sizeof(out)));
  assert(strcmp(out, "") == 0);
  puts("testFormatZteDate ok");
}
// #endregion FUNC_testFormatZteDate

}  // namespace

int main() {
  testCodecVectors();
  testParseUint32String();
  testRecordValidation();
  testV3MigrationValidation();
  testLoginSuccess();
  testLoginRejected();
  testFindOldestAscending();
  testFindOldestDescending();
  testFindOldestContinuesPages();
  testFindOldestEmpty();
  testIncompleteConcatAndDecode();
  testNonHexContentFallback();
  testControlCharactersInNumber();
  testStaleSessionRelogin();
  testDeleteFlow();
  testDeleteRejected();
  testDeleteUnverified();
  testCleanupOutgoing();
  testCleanupOutgoingDeleteRejected();
  testInboxStatus();
  testSendSmsFlow();
  testSendSmsAsciiUsesGsm7();
  testSendSmsEmptyReply();
  testSendSmsRejected();
  testSendStatusSamples();
  testSendSmsInputValidation();
  testZteSmsUtf16Units();
  testHttpFailures();
  testBodyWithoutContentLength();
  testFormatZteDate();
  puts("all zte tests passed");
  return 0;
}
