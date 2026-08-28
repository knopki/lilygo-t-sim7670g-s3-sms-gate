// #region MODULE_CONTRACT
// PURPOSE: Verifies the SIM7670G GNSS startup dialog powers the active antenna
// on the production Classic board before using the receiver, including when
// the GNSS engine was already left running.
// #endregion MODULE_CONTRACT

#include "../sms_gate/gps/gps_client.h"
#include "../sms_gate/modem/modem_client.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// #region CLASS_FakeGpsChannel
// PURPOSE: Enforces the exact GNSS AT command order and returns scripted lines.
class FakeGpsChannel : public ModemChannel {
 public:
  struct Transaction {
    std::string command;
    std::vector<std::string> replyLines;
  };

  explicit FakeGpsChannel(std::vector<Transaction> transactions)
      : transactions_(std::move(transactions)) {}

  bool write(const char* data, size_t len) override {
    assert(transactionIndex_ < transactions_.size());
    std::string command(data, len);
    while (!command.empty() && (command.back() == '\r' || command.back() == '\n')) {
      command.pop_back();
    }
    assert(command == transactions_[transactionIndex_].command);
    lineIndex_ = 0;
    return true;
  }

  int readLine(char* buffer, size_t size, unsigned long) override {
    assert(transactionIndex_ < transactions_.size());
    const auto& lines = transactions_[transactionIndex_].replyLines;
    assert(lineIndex_ < lines.size());
    const std::string& line = lines[lineIndex_++];
    assert(line.size() + 1 <= size);
    memcpy(buffer, line.c_str(), line.size() + 1);
    if (lineIndex_ == lines.size()) {
      ++transactionIndex_;
      lineIndex_ = 0;
    }
    return static_cast<int>(line.size());
  }

  void purge() override {}

  bool complete() const { return transactionIndex_ == transactions_.size(); }

 private:
  std::vector<Transaction> transactions_;
  size_t transactionIndex_ = 0;
  size_t lineIndex_ = 0;
};
// #endregion CLASS_FakeGpsChannel

// #region FUNC_testRestartReordersPersistedPowerState
// PURPOSE: Guarantees a receiver left on by the previous firmware is stopped
// before Classic antenna bias is applied and GNSS is started again.
void testRestartReordersPersistedPowerState() {
  FakeGpsChannel channel({
      {"AT+CGNSSPWR?", {"+CGNSSPWR: 1", "OK"}},
      {"AT+CGNSSPWR=0", {"OK"}},
      {"AT+CGDRT=4,1", {"OK"}},
      {"AT+CGSETV=4,1", {"OK"}},
      {"AT+CGNSSPWR?", {"+CGNSSPWR: 0", "OK"}},
      {"AT+CGNSSPWR=1", {"OK"}},
      {"AT+CGNSSMODE=3", {"OK"}},
  });
  char scratch[256];
  GpsClient client(channel, scratch, sizeof(scratch));

  assert(client.restart() == GpsResult::kSuccess);
  assert(channel.complete());
}
// #endregion FUNC_testRestartReordersPersistedPowerState

// #region FUNC_testClassicAntennaBiasBeforeColdPowerOn
// PURPOSE: Prevents Standard GPIO1 from being accepted while the Classic
// antenna on modem GPIO4 remains unpowered.
void testClassicAntennaBiasBeforeColdPowerOn() {
  FakeGpsChannel channel({
      {"AT+CGDRT=4,1", {"OK"}},
      {"AT+CGSETV=4,1", {"OK"}},
      {"AT+CGNSSPWR?", {"+CGNSSPWR: 0", "OK"}},
      {"AT+CGNSSPWR=1", {"OK"}},
      {"AT+CGNSSMODE=3", {"OK"}},
  });
  char scratch[256];
  GpsClient client(channel, scratch, sizeof(scratch));

  assert(client.ensurePowered() == GpsResult::kSuccess);
  assert(channel.complete());
}
// #endregion FUNC_testClassicAntennaBiasBeforeColdPowerOn

// #region FUNC_testClassicAntennaBiasRestoredForRunningEngine
// PURPOSE: Restores antenna bias after an ESP/task restart even when the modem
// reports that its GNSS engine is already powered.
void testClassicAntennaBiasRestoredForRunningEngine() {
  FakeGpsChannel channel({
      {"AT+CGDRT=4,1", {"OK"}},
      {"AT+CGSETV=4,1", {"OK"}},
      {"AT+CGNSSPWR?", {"+CGNSSPWR: 1", "OK"}},
      {"AT+CGNSSMODE?", {"+CGNSSMODE: 3", "OK"}},
  });
  char scratch[256];
  GpsClient client(channel, scratch, sizeof(scratch));

  assert(client.ensurePowered() == GpsResult::kSuccess);
  assert(channel.complete());
}
// #endregion FUNC_testClassicAntennaBiasRestoredForRunningEngine

// #region FUNC_testPollReportsAntennaBiasFailure
// PURPOSE: Makes a failed antenna route visible to GpsService instead of
// publishing the misleading "powered, no fix" state indefinitely.
void testPollReportsAntennaBiasFailure() {
  FakeGpsChannel channel({
      {"AT", {"OK"}},
      {"AT+CGDRT=4,1", {"ERROR"}},
  });
  char scratch[256];
  GpsClient client(channel, scratch, sizeof(scratch));
  GpsStatus status;

  assert(client.poll(status) == GpsResult::kProtocolError);
  assert(strcmp(client.failedStage(), "antenna_bias_route") == 0);
  assert(status.present);
  assert(!status.powered);
  assert(channel.complete());
}
// #endregion FUNC_testPollReportsAntennaBiasFailure

// #region FUNC_testCgnssInfoMatchesManualExample
// PURPOSE: Maps fields by the SIM767xx manual V1.02 order so the GPS page
// shows per-constellation counts instead of GPS/GLONASS mislabeled as used.
void testCgnssInfoMatchesManualExample() {
  GpsSatsInfo sats;
  assert(
      parseCgnssInfoLine("+CGNSSINFO: 2,09,05,00,00,3113.330650,N,12121.262554,E,131117,091918.00,"
                         "32.9,0.0,255.0,1.1,0.8,0.7,14",
                         sats));
  assert(sats.gps == 9);
  assert(sats.glonass == 5);
  assert(sats.galileo == 0);
  assert(sats.beidou == 0);
  assert(sats.visible == 14);
  assert(sats.used == 14);  // trailing NoSV
}
// #endregion FUNC_testCgnssInfoMatchesManualExample

// #region FUNC_testCgnssInfoEmptyFieldsKeepPositions
// PURPOSE: Keeps positional meaning when a constellation reports an empty SV
// field; strtok-style splitting would shift BEIDOU/NoSV into earlier slots.
void testCgnssInfoEmptyFieldsKeepPositions() {
  GpsSatsInfo sats;
  assert(
      parseCgnssInfoLine("+CGNSSINFO: 3,13,10,,4,5545.012345,N,03737.654321,E,080425,101112.00,"
                         "150.0,0.0,0.0,1.0,0.7,0.6,21",
                         sats));
  assert(sats.gps == 13);
  assert(sats.glonass == 10);
  assert(sats.galileo == 0);  // empty GALILEO-SVs stays positional
  assert(sats.beidou == 4);
  assert(sats.visible == 27);
  assert(sats.used == 21);
}
// #endregion FUNC_testCgnssInfoEmptyFieldsKeepPositions

// #region FUNC_testCgnssInfoNoFixAllEmpty
// PURPOSE: Accepts the all-empty no-fix line and rejects foreign prefixes.
void testCgnssInfoNoFixAllEmpty() {
  GpsSatsInfo sats;
  sats.gps = 7;
  assert(parseCgnssInfoLine("+CGNSSINFO:,,,,,,,,,,,,,,,,,", sats));
  assert(sats.gps == 0 && sats.glonass == 0 && sats.galileo == 0 && sats.beidou == 0);
  assert(sats.used == 0 && sats.visible == 0);
  assert(!parseCgnssInfoLine("+CGPSINFO: ,,,,,,,,", sats));
}
// #endregion FUNC_testCgnssInfoNoFixAllEmpty

// #region FUNC_testPollMapsSatelliteCountsFromCgnssInfo
// PURPOSE: Publishes per-constellation counts with NoSV as used, while
// keeping status.mode sourced from AT+CGNSSMODE?.
void testPollMapsSatelliteCountsFromCgnssInfo() {
  FakeGpsChannel channel({
      {"AT", {"OK"}},
      {"AT+CGDRT=4,1", {"OK"}},
      {"AT+CGSETV=4,1", {"OK"}},
      {"AT+CGNSSPWR?", {"+CGNSSPWR: 1", "OK"}},
      {"AT+CGNSSMODE?", {"+CGNSSMODE: 3", "OK"}},
      {"AT+CGNSSPWR?", {"+CGNSSPWR: 1", "OK"}},
      {"AT+CGNSSMODE?", {"+CGNSSMODE: 3", "OK"}},
      {"AT+CGPSINFO", {"+CGPSINFO: ,,,,,,,,", "OK"}},
      {"AT+CGNSSINFO",
       {"+CGNSSINFO: 2,13,10,00,00,3113.330650,N,12121.262554,E,131117,091918.00,"
        "32.9,0.0,255.0,1.1,0.8,0.7,21",
        "OK"}},
  });
  char scratch[256];
  GpsClient client(channel, scratch, sizeof(scratch));
  GpsStatus status;

  assert(client.poll(status) == GpsResult::kSuccess);
  assert(status.sats.gps == 13);
  assert(status.sats.glonass == 10);
  assert(status.sats.visible == 23);
  assert(status.sats.used == 21);
  assert(status.mode == 3);  // CGNSSMODE value; CGNSSINFO fix mode (2) ignored
  assert(channel.complete());
}
// #endregion FUNC_testPollMapsSatelliteCountsFromCgnssInfo

int main() {
  testRestartReordersPersistedPowerState();
  testClassicAntennaBiasBeforeColdPowerOn();
  testClassicAntennaBiasRestoredForRunningEngine();
  testPollReportsAntennaBiasFailure();
  testCgnssInfoMatchesManualExample();
  testCgnssInfoEmptyFieldsKeepPositions();
  testCgnssInfoNoFixAllEmpty();
  testPollMapsSatelliteCountsFromCgnssInfo();
  puts("all gps_client tests passed");
  return 0;
}
