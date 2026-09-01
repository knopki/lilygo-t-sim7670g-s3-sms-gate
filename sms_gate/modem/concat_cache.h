// #region MODULE_CONTRACT
// PURPOSE: Delays deletion until multipart SMS assembly and delivery succeed.
// SCOPE:
// - Identity/ref/part bookkeeping, complete/expired-set selection
// - volatile SMTP-acceptance/CMGD progress for retry-safe cleanup.
// - NOT: AT dialogs, SMTP, persistence, or email rendering.
// INVARIANTS:
// - At most two sets and five parts per set;
// - parts are keyed by ref+ref-width+sender+total;
// - SMTP acceptance is recorded before a delete;
// - a set remains until every present source record has a successful CMGD.
// #endregion MODULE_CONTRACT

#pragma once
#ifndef MODEM_CONCAT_CACHE_H
#define MODEM_CONCAT_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "modem/modem_client.h"

// #region CLASS_ModemConcatCache
// PURPOSE: Supplies deterministic RAM reassembly state to ModemService,
// so multipart SMS can be forwarded once complete or marked incomplete
// after a bounded polling window without adding persistent state.
class ModemConcatCache {
 public:
  static constexpr size_t kMaxSets = 2;
  static constexpr size_t kMaxParts = kMaxSmsMultipartParts;
  static constexpr uint8_t kExpirePollCycles = 20;

  // #region METHOD_ModemConcatCache_containsId
  // PURPOSE: Prevents a cached source record from being read twice.
  bool containsId(const char* id, const char* storage) const {
    if (id == nullptr || storage == nullptr) return false;
    for (size_t set = 0; set < kMaxSets; ++set) {
      if (!sets_[set].used) continue;
      for (size_t part = 0; part < sets_[set].total; ++part) {
        if (sets_[set].have[part] && strcmp(sets_[set].parts[part].id, id) == 0 &&
            strcmp(sets_[set].parts[part].storage, storage) == 0)
          return true;
      }
    }
    return false;
  }
  // #endregion METHOD_ModemConcatCache_containsId

  // Stores one confirmed concat part. False means malformed metadata or no
  // free set; the caller leaves the source SMS untouched for a later retry.
  // #region METHOD_ModemConcatCache_store
  // PURPOSE: Retains one validated concat part for later delivery or retry.
  bool store(const ModemSms& sms, const ModemConcatInfo& concat) {
    if (!concat.present || concat.total < 2 || concat.total > kMaxParts || concat.seq == 0 ||
        concat.seq > concat.total || sms.id[0] == '\0' || sms.number[0] == '\0')
      return false;
    size_t setIndex = kMaxSets;
    for (size_t i = 0; i < kMaxSets; ++i) {
      if (sets_[i].used && sets_[i].ref == concat.ref && sets_[i].refIs16Bit == concat.refIs16Bit &&
          sets_[i].total == concat.total && strcmp(sets_[i].number, sms.number) == 0) {
        setIndex = i;
        break;
      }
      if (!sets_[i].used && setIndex == kMaxSets) setIndex = i;
    }
    if (setIndex == kMaxSets) return false;
    Set& set = sets_[setIndex];
    if (!set.used) {
      set = Set{};
      set.used = true;
      set.ref = concat.ref;
      set.refIs16Bit = concat.refIs16Bit;
      set.total = concat.total;
      strncpy(set.number, sms.number, sizeof(set.number) - 1);
    }
    const size_t part = concat.seq - 1;
    if (!set.have[part]) {
      set.parts[part] = sms;
      set.have[part] = true;
    }
    set.age = 0;
    return true;
  }
  // #endregion METHOD_ModemConcatCache_store

  // #region METHOD_ModemConcatCache_advanceCycle
  // PURPOSE: Ages incomplete sets so bounded polling can eventually release them.
  void advanceCycle() {
    for (size_t set = 0; set < kMaxSets; ++set) {
      if (sets_[set].used && sets_[set].age < kExpirePollCycles) ++sets_[set].age;
    }
  }
  // #endregion METHOD_ModemConcatCache_advanceCycle

  // #region METHOD_ModemConcatCache_findComplete
  // PURPOSE: Selects a fully assembled set ready for one combined delivery.
  bool findComplete(size_t& setIndex) const {
    for (size_t set = 0; set < kMaxSets; ++set) {
      if (!sets_[set].used) continue;
      bool complete = true;
      for (size_t part = 0; part < sets_[set].total; ++part)
        complete = complete && sets_[set].have[part];
      if (complete) {
        setIndex = set;
        return true;
      }
    }
    return false;
  }
  // #endregion METHOD_ModemConcatCache_findComplete

  // True only while a complete set still needs its one SMTP submission.
  // #region METHOD_ModemConcatCache_completeReadyForSmtp
  // PURPOSE: Prevents duplicate SMTP submission of an assembled set.
  bool completeReadyForSmtp(size_t setIndex) const {
    return valid(setIndex) && findSetComplete(setIndex) && !sets_[setIndex].completeSmtpAccepted;
  }
  // #endregion METHOD_ModemConcatCache_completeReadyForSmtp

  // Records SMTP 250 before any CMGD, so a failed cleanup cannot resend a
  // complete message during this boot.
  // #region METHOD_ModemConcatCache_markCompleteSmtpAccepted
  // PURPOSE: Records SMTP acceptance before any source deletion can begin.
  bool markCompleteSmtpAccepted(size_t setIndex) {
    if (!completeReadyForSmtp(setIndex)) return false;
    sets_[setIndex].completeSmtpAccepted = true;
    return true;
  }
  // #endregion METHOD_ModemConcatCache_markCompleteSmtpAccepted

  // Joins a complete set only while it still needs SMTP. After acceptance,
  // callers must skip SMTP and use partNeedsDelete() for cleanup retries.
  // #region METHOD_ModemConcatCache_buildComplete
  // PURPOSE: Joins ordered parts into one bounded SMS for SMTP forwarding.
  bool buildComplete(size_t setIndex, ModemSms& out) const {
    if (!completeReadyForSmtp(setIndex)) return false;
    const Set& set = sets_[setIndex];
    out = set.parts[0];
    out.text[0] = '\0';
    out.concatComplete = true;
    snprintf(out.concatReceived, sizeof(out.concatReceived), "%u",
             static_cast<unsigned>(set.total));
    snprintf(out.concatTotal, sizeof(out.concatTotal), "%u", static_cast<unsigned>(set.total));
    for (size_t partIndex = 0; partIndex < set.total; ++partIndex) {
      const size_t used = strlen(out.text);
      const size_t partLength = strlen(set.parts[partIndex].text);
      if (used + partLength >= sizeof(out.text)) return false;
      memcpy(out.text + used, set.parts[partIndex].text, partLength + 1);
    }
    return true;
  }
  // #endregion METHOD_ModemConcatCache_buildComplete

  // #region METHOD_ModemConcatCache_findExpired
  // PURPOSE: Selects a stalled set for bounded incomplete-message delivery.
  bool findExpired(size_t& setIndex) const {
    for (size_t set = 0; set < kMaxSets; ++set) {
      if (sets_[set].used && sets_[set].age >= kExpirePollCycles) {
        setIndex = set;
        return true;
      }
    }
    return false;
  }
  // #endregion METHOD_ModemConcatCache_findExpired

  // #region METHOD_ModemConcatCache_total
  // PURPOSE: Gives cleanup and status logic the set size without exposing cache state.
  uint8_t total(size_t setIndex) const { return valid(setIndex) ? sets_[setIndex].total : 0; }
  // #endregion METHOD_ModemConcatCache_total

  // #region METHOD_ModemConcatCache_part
  // PURPOSE: Exposes one present source record for delivery or cleanup.
  const ModemSms* part(size_t setIndex, size_t partIndex) const {
    if (!hasPart(setIndex, partIndex)) return nullptr;
    return &sets_[setIndex].parts[partIndex];
  }
  // #endregion METHOD_ModemConcatCache_part

  // Expired fragments use per-part acceptance because each produces a
  // separate incomplete email. Complete sets use completeSmtpAccepted.
  // #region METHOD_ModemConcatCache_partReadyForSmtp
  // PURPOSE: Prevents duplicate delivery of one expired fragment.
  bool partReadyForSmtp(size_t setIndex, size_t partIndex) const {
    return hasPart(setIndex, partIndex) && !sets_[setIndex].smtpAccepted[partIndex];
  }
  // #endregion METHOD_ModemConcatCache_partReadyForSmtp

  // #region METHOD_ModemConcatCache_markPartSmtpAccepted
  // PURPOSE: Records fragment acceptance before its source record is deleted.
  bool markPartSmtpAccepted(size_t setIndex, size_t partIndex) {
    if (!partReadyForSmtp(setIndex, partIndex)) return false;
    sets_[setIndex].smtpAccepted[partIndex] = true;
    return true;
  }
  // #endregion METHOD_ModemConcatCache_markPartSmtpAccepted

  // A present source record needs CMGD after either its individual expired
  // submission or the complete set's submission has received SMTP 250.
  // #region METHOD_ModemConcatCache_partNeedsDelete
  // PURPOSE: Identifies accepted fragments whose source cleanup is still pending.
  bool partNeedsDelete(size_t setIndex, size_t partIndex) const {
    return hasPart(setIndex, partIndex) && !sets_[setIndex].deleted[partIndex] &&
           (sets_[setIndex].completeSmtpAccepted || sets_[setIndex].smtpAccepted[partIndex]);
  }
  // #endregion METHOD_ModemConcatCache_partNeedsDelete

  // #region METHOD_ModemConcatCache_markPartDeleted
  // PURPOSE: Records successful CMGD so cleanup retries remain idempotent.
  bool markPartDeleted(size_t setIndex, size_t partIndex) {
    if (!partNeedsDelete(setIndex, partIndex)) return false;
    sets_[setIndex].deleted[partIndex] = true;
    return true;
  }
  // #endregion METHOD_ModemConcatCache_markPartDeleted

  // A cache set may be discarded only when every present source record has
  // received successful CMGD. Missing expired parts require no cleanup.
  // #region METHOD_ModemConcatCache_removable
  // PURPOSE: Allows discarded state only after every present record is deleted.
  bool removable(size_t setIndex) const {
    if (!valid(setIndex)) return false;
    for (size_t partIndex = 0; partIndex < sets_[setIndex].total; ++partIndex) {
      if (sets_[setIndex].have[partIndex] && !sets_[setIndex].deleted[partIndex]) return false;
    }
    return true;
  }
  // #endregion METHOD_ModemConcatCache_removable

  // #region METHOD_ModemConcatCache_remove
  // PURPOSE: Releases a fully cleaned set without risking message loss.
  void remove(size_t setIndex) {
    if (removable(setIndex)) sets_[setIndex] = Set{};
  }
  // #endregion METHOD_ModemConcatCache_remove

 private:
  struct Set {
    bool used = false;
    uint16_t ref = 0;
    bool refIs16Bit = false;
    uint8_t total = 0;
    uint8_t age = 0;
    char number[32] = "";
    bool have[kMaxParts] = {};
    bool completeSmtpAccepted = false;
    bool smtpAccepted[kMaxParts] = {};
    bool deleted[kMaxParts] = {};
    ModemSms parts[kMaxParts] = {};
  };

  bool valid(size_t setIndex) const { return setIndex < kMaxSets && sets_[setIndex].used; }
  bool hasPart(size_t setIndex, size_t partIndex) const {
    return valid(setIndex) && partIndex < sets_[setIndex].total && sets_[setIndex].have[partIndex];
  }
  bool findSetComplete(size_t setIndex) const {
    if (!valid(setIndex)) return false;
    for (size_t partIndex = 0; partIndex < sets_[setIndex].total; ++partIndex) {
      if (!sets_[setIndex].have[partIndex]) return false;
    }
    return true;
  }
  Set sets_[kMaxSets] = {};
};
// #endregion CLASS_ModemConcatCache

#endif  // MODEM_CONCAT_CACHE_H
