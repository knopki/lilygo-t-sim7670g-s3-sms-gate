// #region MODULE_CONTRACT
// PURPOSE: Runs the proven ZTE MF79RU HiLink goform dialog (LOGIN, inbox
// paging, DELETE_SMS and SEND_SMS with the AD token; see ADR-0003) over an
// abstract channel so the protocol logic stays host-testable.
// SCOPE:
// - One HTTP/1.1 request per command with Connection: close, the mandatory
// Referer header, the stok session cookie, a lenient fixed-shape JSON
// scanner, UCS-2-hex SMS decoding and encoding, modem timestamp formatting,
// one stale-session relogin per command, bounded device-storage paging with
// order auto-detection, delete verification, outgoing-record cleanup, and
// one send-status query sample.
// - NOT: Sockets, TLS, NVS persistence, SMTP delivery, send-status polling
// pacing, and HTTP route handling.
// INVARIANTS: Credentials are never copied into error paths or stage names;
// the channel is stopped before every return; incoming SMS stay on the modem
// until SMTP acceptance; cleanup deletes only final outgoing tags 2 and 3;
// outgoing message bodies never exceed the modem web UI's own send limit.
// DEPENDENCIES: Pure C++ (zte_record.h field limits, codec.h base64/MD5);
// the device channel lives in zte_transport.h.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef ZTE_ZTE_CLIENT_H
#define ZTE_ZTE_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#include "system/sms_validate.h"
#include "zte/zte_record.h"

constexpr size_t kZteSmsIdLength = 15;
constexpr size_t kZteNumberLength = 40;
constexpr size_t kZteDateLength = 31;
constexpr size_t kZteConcatLength = 8;
constexpr size_t kMaxZteSmsTextBytes = 4096;
constexpr uint16_t kZteHttpPort = 80;
constexpr size_t kZtePageSize = 5;
constexpr uint8_t kZteMaxPages = 21;  // 21 x 5 covers the 100-message device inbox
// Send limit of the modem's own web UI for UNICODE messages: five
// concatenated UCS-2 parts of 70 code units, minus the part headers.
// Kept for compatibility; canonical limit lives in sms_validate.h.
constexpr size_t kMaxZteSmsSendUnits = kMaxSmsSendUnits;
// Returned by zteSmsUtf16Units for malformed UTF-8 input.
constexpr size_t kZteSmsInvalidUnits = kSmsInvalidUnits;

// #region CLASS_ZteChannel
// PURPOSE: Abstracts the byte transport so tests can script a modem and the
// device can bind the bundled NetworkClient.
class ZteChannel {
 public:
  virtual ~ZteChannel() = default;

  // Opens a plain TCP connection to the modem.
  virtual bool connect(const char* host, uint16_t port) = 0;
  // Writes exactly length bytes or returns false.
  virtual bool write(const char* data, size_t length) = 0;
  // Reads one CRLF-terminated header line without the terminator.
  // Returns the line length, or a negative value on timeout or failure.
  virtual int readLine(char* buffer, size_t size) = 0;
  // Reads up to size bytes; returns the count read, or a negative value on
  // error or clean end of stream.
  virtual int read(char* buffer, size_t size) = 0;
  virtual void stop() = 0;
};
// #endregion CLASS_ZteChannel

// #region ENUM_ZteResult
// PURPOSE: Gives the caller one stable outcome per failure class for Serial
// events and the web send/test routes.
enum class ZteResult {
  kSuccess,
  kConnectFailed,
  kHttpFailed,
  kLoginRejected,
  kStaleSession,
  kProtocolError,
  kSendRejected,
};
// #endregion ENUM_ZteResult

// #region CLASS_ZteSms
// PURPOSE: Carries one complete incoming SMS out of the dialog so the
// caller can build an email and then delete exactly this message.
struct ZteSms {
  char id[kZteSmsIdLength + 1];
  char number[kZteNumberLength + 1];
  char dateRaw[kZteDateLength + 1];
  char concatReceived[kZteConcatLength + 1];
  char concatTotal[kZteConcatLength + 1];
  bool concatComplete;
  char textUtf8[kMaxZteSmsTextBytes + 1];
};
// #endregion CLASS_ZteSms

bool formatZteDate(const char* raw, char* out, size_t outSize);

// Counts the UTF-16 code units a UTF-8 text occupies once encoded as the
// modem's UCS-2-hex MessageBody; returns kZteSmsInvalidUnits on malformed
// UTF-8 so both the web form and sendSms share one length rule.
size_t zteSmsUtf16Units(const char* utf8);

struct ZteInboxStatus {
  uint16_t used;
  uint16_t total;
};

enum class ZteSendStatus { kInProgress, kDone, kFailed };

// #region CLASS_ZteModem
// PURPOSE: Owns one modem session (login, cookie, and firmware version) and
// sequences the goform commands, so callers only provide a channel, a
// scratch buffer, and credentials, and always learn which stage failed.
class ZteModem {
 public:
  ZteModem(ZteChannel& channel, char* scratch, size_t scratchSize);

  // Stores the credentials, connects, and logs in with the base64 password,
  // then reads the firmware versions the AD token depends on.
  ZteResult login(const char* host, const char* password);

  // Finds the oldest incoming (tag 0 or 1) SMS with bounded paging; found
  // is false when the inbox holds no incoming messages.
  ZteResult findOldestIncoming(ZteSms& out, bool& found);

  // Deletes exactly one incoming message after a fresh RD token and verifies
  // the ID disappeared; the message is retained for retry on any failure.
  ZteResult deleteSms(const ZteSms& sms);

  // Deletes every final outgoing device record (tag 2 sent or tag 3 failed),
  // verifying each ID disappeared; incoming (0/1) and draft (4) messages
  // remain untouched. Returns the number of verified deletions.
  ZteResult cleanupOutgoing(uint16_t& deleted);

  // Reads sms_capacity_info for the device storage.
  ZteResult readInboxStatus(ZteInboxStatus& out);

  // Sends one SMS (SEND_SMS with a fresh AD token, UCS-2-hex body, always
  // UNICODE encoding); success means the modem accepted the command, and
  // the caller confirms delivery with readSendStatus samples.
  ZteResult sendSms(const char* number, const char* textUtf8);

  // Reads one send-command status sample in the current session.
  ZteResult readSendStatus(ZteSendStatus& out);

  // Exact form body of the last SEND_SMS attempt ("" before the first
  // send), so callers can log the request bytes for byte-level protocol
  // diagnosis against a known-good browser capture.
  const char* lastSendForm() const { return lastSendForm_; }

  const char* waVersion() const { return waVersion_; }
  // Stage at which the last operation failed ("" on success); stable token.
  const char* failedStage() const { return failedStage_; }
  // Read-only view of the last response body (the scratch buffer; possibly
  // stale after later requests), so callers can log what the modem replied.
  const char* lastBody() const { return scratch_ != nullptr ? scratch_ : ""; }
  // Length of the last response body (0 means the modem answered 200 with
  // an empty body — its rejection signature for malformed SEND_SMS forms).
  size_t lastBodyLength() const { return bodyLength_; }

 private:
  ZteResult loginSession();
  ZteResult openSession();
  ZteResult requestPost(const char* formBody);
  ZteResult requestGet(const char* query);
  ZteResult readResponse();
  ZteResult requestVersions();
  ZteResult fetchRd(char* rd, size_t rdSize);
  ZteResult fetchAd(char* ad, size_t adSize);
  ZteResult fetchSmsPage(unsigned int page, const char* tags);
  ZteResult scanOldest(ZteSms& out, bool& found);
  ZteResult findOutgoing(const char* tag, char* id, size_t idSize, bool& found);
  ZteResult deleteMessage(const char* id, const char* verifyTags);
  ZteResult verifyAbsent(const char* targetId, const char* tags);

  void fail(const char* stage);
  bool hasSession() const { return cookie_[0] != '\0'; }

  ZteChannel& channel_;
  char* scratch_;
  size_t scratchSize_;
  char host_[kMaxZteHostLength + 1];
  char password_[kMaxZtePasswordLength + 1];
  char cookie_[96];
  char waVersion_[96];
  size_t bodyLength_ = 0;
  const char* failedStage_ = "";
  char lastSendForm_[1792] = "";
};
// #endregion CLASS_ZteModem
#endif  // ZTE_ZTE_CLIENT_H
