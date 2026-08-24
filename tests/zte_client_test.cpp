// Host test for the ZTE MF79RU goform dialog (ADR-0003): scripts a fake
// modem through the ZteChannel interface and asserts the exact request
// contract, session handling, AD token, paging/scan semantics, decoding,
// and record validation.
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "../sms_gate/codec.h"
#include "../sms_gate/zte_client.h"
#include "../sms_gate/zte_record.h"

namespace {

// #region CLASS_FakeZteChannel
// PURPOSE: Scripts the modem side of the dialog as a byte stream and
// records every byte the client writes, so tests assert the exact request
// contract of the goform API.
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
// PURPOSE: Builds one complete modem HTTP response with optional extra
// headers (Set-Cookie) before Connection: close.
std::string httpResponse(const std::string& body, const std::string& extraHeaders = "") {
  return "HTTP/1.1 200 OK\r\n" + extraHeaders + "Content-Length: " +
         std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}
// #endregion FUNC_httpResponse

// #region FUNC_loginResponse
// PURPOSE: Builds the successful LOGIN response that installs the stok
// cookie the later requests must send.
std::string loginResponse() {
  return httpResponse("{\"result\":\"0\"}", "Set-Cookie: stok=ABC123; Path=/\r\n");
}
// #endregion FUNC_loginResponse

// #region FUNC_versionsResponse
// PURPOSE: Builds the firmware-version answer whose concatenation feeds
// the AD token.
std::string versionsResponse() {
  return httpResponse("{\"cr_version\":\"\",\"wa_inner_version\":\"BD_MF79RUV1.0.0B02\"}");
}
// #endregion FUNC_versionsResponse

// #region FUNC_smsResponse
// PURPOSE: Builds one sms_data_total answer; each entry is a raw JSON
// object fragment so tests control id, tag, and content precisely.
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
// PURPOSE: Builds one messages[] element; content defaults to a short
// UCS-2 hex text ("OK") unless overridden.
std::string smsEntry(const char* id, const char* tag, const char* content = "004F004B",
                     const char* receivedAll = "1", const char* concatTotal = "1",
                     const char* concatReceived = "1") {
  std::string entry = "{\"id\":\"";
  entry += id;
  entry += "\",\"number\":\"+70001234567\",\"content\":\"";
  entry += content;
  entry += "\",\"tag\":\"";
  entry += tag;
  entry += "\",\"date\":\"26,01,02,03,04,05,+12\",\"draft_group_id\":\"\","
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
// PURPOSE: Counts substring occurrences to assert how often the dialog
// relogged in.
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
// PURPOSE: Builds one known-good ZTE record as the baseline for every
// mutation.
ZteConfigRecord makeRecord() {
  ZteConfigRecord record{};
  record.magic = kZteConfigMagic;
  record.version = kZteConfigVersion;
  record.enabled = 1;
  strcpy(record.host, "192.168.0.1");
  strcpy(record.password, "modem-pass");
  record.checksum = calculateZteConfigChecksum(record);
  return record;
}
// #endregion FUNC_makeRecord

// #region FUNC_testCodecVectors
// PURPOSE: Proves the shared MD5 and base64 encoders against RFC vectors
// before any dialog output can depend on them.
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

// #region FUNC_makeV1Record
// PURPOSE: Builds one known-good pre-label v1 record as the baseline for
// the migration-path assertions.
ZteConfigRecordV1 makeV1Record() {
  ZteConfigRecordV1 record{};
  record.magic = kZteConfigMagic;
  record.version = 1;
  record.enabled = 1;
  strcpy(record.host, "192.168.0.1");
  strcpy(record.password, "modem-pass");
  const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < offsetof(ZteConfigRecordV1, checksum); ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  record.checksum = hash;
  return record;
}
// #endregion FUNC_makeV1Record

// #region FUNC_testRecordValidation
// PURPOSE: Gates load/save on the shared predicate: corrupt, foreign, and
// non-printable records never reach the modem dialog; the label is optional
// but must be printable when present.
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
  record.enabled = 7;
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
  record.enabled = 0;  // Disabled still keeps complete credentials.
  record.checksum = calculateZteConfigChecksum(record);
  assert(isZteConfigRecordValid(record));
  puts("testRecordValidation ok");
}
// #endregion FUNC_testRecordValidation

// #region FUNC_testV1MigrationValidation
// PURPOSE: Proves the load-time v1 recognition accepts an original record
// and rejects corrupted or foreign blobs before any field is carried over.
void testV1MigrationValidation() {
  assert(isZteConfigRecordV1Valid(makeV1Record()));

  ZteConfigRecordV1 record = makeV1Record();
  record.checksum ^= 1;
  assert(!isZteConfigRecordV1Valid(record));

  record = makeV1Record();
  record.version = 2;
  assert(!isZteConfigRecordV1Valid(record));

  record = makeV1Record();
  record.password[0] = '\0';
  {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t hash = 2166136261UL;
    for (size_t index = 0; index < offsetof(ZteConfigRecordV1, checksum); ++index) {
      hash ^= bytes[index];
      hash *= 16777619UL;
    }
    record.checksum = hash;
  }
  assert(!isZteConfigRecordV1Valid(record));
  puts("testV1MigrationValidation ok");
}
// #endregion FUNC_testV1MigrationValidation

// #region FUNC_testLoginSuccess
// PURPOSE: Proves the LOGIN request contract (base64 password, Referer,
// cookie capture) and the version concatenation the AD token needs.
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
  assert(countOccurrences(channel.written, "GET /goform/goform_get_cmd_process?isTest=false&"
                                           "cmd=cr_version,wa_inner_version&multi_data=1") == 1);
  puts("testLoginSuccess ok");
}
// #endregion FUNC_testLoginSuccess

// #region FUNC_testLoginRejected
// PURPOSE: Separates a wrong password (rejected) from a missing cookie
// (also rejected) so the operator sees the true cause.
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
// PURPOSE: Captures the oldest incoming SMS on an ascending page (the
// requested order), including decoded text and request query shape.
void testFindOldestAscending() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // Entries: sent, unread incoming 101, draft, read incoming 103, sent.
  channel.enqueue(smsResponse({smsEntry("100", "2"), smsEntry("101", "1"),
                               smsEntry("102", "4"), smsEntry("103", "0"),
                               smsEntry("104", "2")}));
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
// PURPOSE: Auto-detects a firmware that ignores the ascending request and
// still selects the oldest incoming SMS after the final page.
void testFindOldestDescending() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // Descending page 0 (5 entries, count equals page size, so the scan must
  // continue) and an empty final page.
  channel.enqueue(smsResponse({smsEntry("104", "2"), smsEntry("103", "1"),
                               smsEntry("102", "2"), smsEntry("101", "0"),
                               smsEntry("100", "2")}));
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
// PURPOSE: Skips a page without incoming entries and finds the target on
// the next page when entries are ascending.
void testFindOldestContinuesPages() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(smsResponse({smsEntry("100", "2"), smsEntry("101", "2"),
                               smsEntry("102", "2"), smsEntry("103", "2"),
                               smsEntry("104", "2")}));
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
// PURPOSE: Reports an empty inbox without any error so the poll cycle can
// stay quiet.
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
// PURPOSE: Carries the incomplete-concat warning fields and proves the
// UCS-2 decoder (Cyrillic, surrogate pair, and non-hex fallback).
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
  const unsigned char expected[] = {0xD0, 0x92, 0x20, 0xD1, 0x81, 0xD0, 0xB2, 0xD1,
                                    0x8F, 0xD0, 0xB7, 0xD0, 0xB8, 0xF0, 0x9F, 0x98, 0x80};
  assert(strlen(sms.textUtf8) == sizeof(expected));
  assert(memcmp(sms.textUtf8, expected, sizeof(expected)) == 0);
  puts("testIncompleteConcatAndDecode ok");
}
// #endregion FUNC_testIncompleteConcatAndDecode

// #region FUNC_testNonHexContentFallback
// PURPOSE: Falls back to the raw string when the content is not UCS-2 hex,
// matching the reference forwarder.
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
// PURPOSE: Keeps raw control characters inside string values scannable
// (B02 firmware behavior a strict parser rejects).
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
// PURPOSE: Recognizes the stale empty sms_data_total answer, relogs in
// exactly once, and completes the scan.
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
// PURPOSE: Proves the DELETE_SMS request contract (msg_id with %3B, fresh
// AD token) and the verified absence afterwards.
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
  assert(countOccurrences(channel.written, "GET /goform/goform_get_cmd_process?isTest=false&cmd=RD") ==
         1);
  assert(countOccurrences(channel.written,
                          "isTest=false&goformId=DELETE_SMS&msg_id=600%3B&notCallback=true&"
                          "AD=02bb862c133c79826efbe952c8a57c34") == 1);
  puts("testDeleteFlow ok");
}
// #endregion FUNC_testDeleteFlow

// #region FUNC_testDeleteRejected
// PURPOSE: Surfaces a refused delete as a protocol error so the message is
// retained for the next poll instead of assumed gone.
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
// PURPOSE: Detects an id that survived the delete request, which would
// otherwise re-forward the same message forever.
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
// PURPOSE: Reclaims both completed (tag 2) and failed (tag 3) outgoing
// records, re-scanning from page zero after each verified deletion so the
// modem's paging shift cannot skip either record; incoming SMS stays intact.
void testCleanupOutgoing() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  // B02 ignores page, so each tag-specific page zero becomes the work
  // queue: deleting 701 exposes the next matching record on the next scan.
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("701", "2"),
                               smsEntry("703", "4")}));
  channel.enqueue(httpResponse("{\"RD\":\"AB12CD34\"}"));
  channel.enqueue(httpResponse("{\"result\":\"success\"}"));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("703", "4")}));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("703", "4")}));
  channel.enqueue(smsResponse({smsEntry("700", "1"), smsEntry("702", "3"),
                               smsEntry("703", "4")}));
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
// PURPOSE: Keeps an outgoing record visible when the modem refuses its
// delete, reports the precise delete stage, and never claims a deletion.
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
// PURPOSE: Reads the device-storage occupancy for the operator test route.
void testInboxStatus() {
  FakeZteChannel channel;
  channel.enqueue(loginResponse());
  channel.enqueue(versionsResponse());
  channel.enqueue(httpResponse("{\"sms_nv_total\":\"100\",\"sms_sim_total\":\"15\","
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
// PURPOSE: Classifies transport and framing failures to one stable stage
// each so Serial events name the true cause.
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
// PURPOSE: Accepts a body terminated by connection close when the modem
// omits Content-Length.
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
// PURPOSE: Proves the SEND_SMS request contract (escaped Number and
// sms_time, UCS-2-hex body with surrogate pair, ID=-1, UNICODE, fresh AD
// token) and the send-status query shape.
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
  const char* text =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x98\x80";
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
  const std::string expectedTail =
      "&ID=-1&encode_type=UNICODE&AD=02bb862c133c79826efbe952c8a57c34";
  assert(countOccurrences(channel.written,
                          expectedTail + "GET /goform/goform_get_cmd_process?isTest=false&"
                                         "cmd=sms_cmd_status_info&sms_cmd=4") == 1);
  puts("testSendSmsFlow ok");
}
// #endregion FUNC_testSendSmsFlow

// #region FUNC_testSendSmsAsciiUsesGsm7
// PURPOSE: Mirrors the modem web UI's proven request: printable-ASCII text
// carries encode_type=GSM7_default with the same UTF-16-hex body (captured
// browser request for "test": 0074006500730074).
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
  assert(strstr(modem.lastSendForm(), "encode_type=GSM7_default") != nullptr);
  assert(strstr(modem.lastSendForm(), "Number=%2B79685557161") != nullptr);
  assert(countOccurrences(channel.written,
                          "Number=%2B79685557161&") == 1);
  assert(countOccurrences(channel.written,
                          "&MessageBody=0074006500730074&ID=-1&encode_type=GSM7_default&") == 1);
  // Exact boundary again for the ASCII path.
  assert(strlen(strstr(modem.lastSendForm(), "AD=")) == 35);  // AD= + 32 hex + '\0'.
  puts("testSendSmsAsciiUsesGsm7 ok");
}
// #endregion FUNC_testSendSmsAsciiUsesGsm7

// #region FUNC_testSendSmsEmptyReply
// PURPOSE: Names the modem's empty-200-body rejection signature distinctly
// so a hardware log identifies a malformed form without guessing.
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
// PURPOSE: Separates a refused send command from transport and protocol
// failures so the operator sees the true cause.
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
// PURPOSE: Maps every sms_cmd_status_info answer onto one sampled outcome,
// treating the field's absence as still in progress.
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
// PURPOSE: Rejects empty, oversize, non-printable, and malformed-UTF-8
// input before any byte reaches the modem.
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
// PURPOSE: Proves the shared length rule: BMP characters count one unit,
// astral codepoints two, and malformed UTF-8 is rejected.
void testZteSmsUtf16Units() {
  assert(zteSmsUtf16Units("") == 0);
  assert(zteSmsUtf16Units("abc") == 3);
  assert(zteSmsUtf16Units("\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82") == 6);
  assert(zteSmsUtf16Units("\xF0\x9F\x98\x80") == 2);
  assert(zteSmsUtf16Units("hi \xF0\x9F\x98\x80!") == 6);
  assert(zteSmsUtf16Units("\xFF") == kZteSmsInvalidUnits);
  assert(zteSmsUtf16Units("\xC3\x28") == kZteSmsInvalidUnits);   // Bad continuation.
  assert(zteSmsUtf16Units("\xC0\xAF") == kZteSmsInvalidUnits);   // Overlong '/'.
  assert(zteSmsUtf16Units("\xED\xA0\x80") == kZteSmsInvalidUnits);  // Encoded surrogate.
  assert(zteSmsUtf16Units("\xF0\x9F\x98") == kZteSmsInvalidUnits);  // Truncated.
  puts("testZteSmsUtf16Units ok");
}
// #endregion FUNC_testZteSmsUtf16Units

// #region FUNC_testFormatZteDate
// PURPOSE: Covers the timestamp rendering, quarter-hour offsets, and the
// verbatim fallback for unexpected firmware formats.
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
  testRecordValidation();
  testV1MigrationValidation();
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
