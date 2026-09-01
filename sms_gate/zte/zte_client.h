// #region MODULE_CONTRACT
// PURPOSE: Keeps ZTE SMS dialogs testable and bounded off hardware.
// SCOPE:
// - HTTP/goform session, bounded JSON/SMS parsing, inbox cleanup, and
// send/status operations.
// - NOT: sockets, TLS, NVS, SMTP, or HTTP routes.
// INVARIANTS:
// - Channel stops on return;
// - incoming SMS survive until SMTP acceptance;
// - cleanup removes only terminal outgoing records.
// DEPENDENCIES: zte_record.h, codec.h; device channel in zte_transport.h.
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

// #region STRUCT_ZteSms
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
// #endregion STRUCT_ZteSms

// #region FUNC_formatZteDate
// PURPOSE: Keeps modem timestamps readable in operator-facing messages.
bool formatZteDate(const char* raw, char* out, size_t outSize);
// #endregion FUNC_formatZteDate

// Counts the UTF-16 code units a UTF-8 text occupies once encoded as the
// modem's UCS-2-hex MessageBody; returns kZteSmsInvalidUnits on malformed
// UTF-8 so both the web form and sendSms share one length rule.
// #region FUNC_zteSmsUtf16Units
// PURPOSE: Keeps ZTE message limits aligned with the shared send validation.
size_t zteSmsUtf16Units(const char* utf8);
// #endregion FUNC_zteSmsUtf16Units

// #region STRUCT_ZteInboxStatus
// PURPOSE: Carries modem storage occupancy to the status API.
struct ZteInboxStatus {
  uint16_t used;
  uint16_t total;
};
// #endregion STRUCT_ZteInboxStatus

// #region ENUM_ZteSendStatus
// PURPOSE: Lets callers distinguish accepted, failed, and still-pending sends.
enum class ZteSendStatus { kInProgress, kDone, kFailed };
// #endregion ENUM_ZteSendStatus

// #region CLASS_ZteModem
// PURPOSE: Owns one modem session (login, cookie, and firmware version) and
// sequences the goform commands, so callers only provide a channel, a
// scratch buffer, and credentials, and always learn which stage failed.
class ZteModem {
 public:
  // #region METHOD_ZteModem_ZteModem
  // PURPOSE: Binds the dialog to a channel and bounded scratch buffer.
  ZteModem(ZteChannel& channel, char* scratch, size_t scratchSize);
  // #endregion METHOD_ZteModem_ZteModem

  // #region METHOD_ZteModem_login
  // PURPOSE: Establishes the session required by all goform operations.
  ZteResult login(const char* host, const char* password);
  // #endregion METHOD_ZteModem_login

  // #region METHOD_ZteModem_findOldestIncoming
  // PURPOSE: Finds the oldest incoming SMS without deleting it.
  ZteResult findOldestIncoming(ZteSms& out, bool& found);
  // #endregion METHOD_ZteModem_findOldestIncoming

  // #region METHOD_ZteModem_deleteSms
  // PURPOSE: Deletes one incoming SMS only after forwarding succeeds.
  ZteResult deleteSms(const ZteSms& sms);
  // #endregion METHOD_ZteModem_deleteSms

  // #region METHOD_ZteModem_cleanupOutgoing
  // PURPOSE: Reclaims terminal outgoing records without touching inbox data.
  ZteResult cleanupOutgoing(uint16_t& deleted);
  // #endregion METHOD_ZteModem_cleanupOutgoing

  // #region METHOD_ZteModem_readInboxStatus
  // PURPOSE: Gives operators bounded storage health without exposing modem internals.
  ZteResult readInboxStatus(ZteInboxStatus& out);
  // #endregion METHOD_ZteModem_readInboxStatus

  // #region METHOD_ZteModem_sendSms
  // PURPOSE: Submits one bounded SMS through the modem web UI.
  ZteResult sendSms(const char* number, const char* textUtf8);
  // #endregion METHOD_ZteModem_sendSms

  // #region METHOD_ZteModem_readSendStatus
  // PURPOSE: Samples the asynchronous send result after submission.
  ZteResult readSendStatus(ZteSendStatus& out);
  // #endregion METHOD_ZteModem_readSendStatus

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
