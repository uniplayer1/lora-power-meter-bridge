#pragma once
// LoRa link: SX1262 driver, legal airtime gating, ACK/retry, pairing.
//
// Everything that leaves the antenna goes through DutyCycleBudget, and the
// airtime is computed up front from the modem parameters so a packet is never
// even attempted when the hour's 36 s are spent.

#include <Arduino.h>
#include <RadioLib.h>

#include "app_config.h"
#include "config.h"
#include "duty_cycle.h"
#include "meter_values.h"
#include "protocol.h"

enum class PairState : uint8_t {
  Unpaired = 0,  // never paired, nothing running
  Running = 1,   // broadcasting, waiting for an answer
  Done = 2,      // both ends know each other
  TimedOut = 3,  // nothing answered within the window
};

struct LinkStats {
  uint32_t txPackets = 0;
  uint32_t txFailures = 0;
  uint32_t txBlocked = 0;  // refused by the duty-cycle budget
  uint32_t rxPackets = 0;
  uint32_t rxCorrupt = 0;
  // A sealed frame whose tag did not match, and a reading that arrived in the
  // clear while encryption is on. Both mean "something the link did not send".
  uint32_t rxAuthFailures = 0;
  uint32_t rxPlaintextDropped = 0;
  uint32_t acksReceived = 0;
  uint32_t acksMissed = 0;
  uint32_t airtimeMs = 0;
  int lastRssi = 0;
  float lastSnr = 0.0f;
  int peerRssi = 0;
  float peerSnr = 0.0f;
  uint32_t lastPeerSeenMs = 0;
  // The last packet from ANY device, coupled or not. Readings arrive over the
  // link even while the two are unpaired, so "something is being received" has
  // to be distinguishable from "the peer answers" - otherwise a working link
  // shows a timestamp that never moves, or looks connected when it is not.
  uint32_t lastPacketSeenMs = 0;
  uint16_t lastPacketSrc = 0;
  bool peerOnline = false;
  bool radioOk = false;
  int radioError = 0;
  uint32_t pairingAttempts = 0;
  bool pairingActive = false;

  // Pairing has to be observable while it runs, otherwise a button that does
  // nothing and a link that cannot be made look identical.
  PairState pairState = PairState::Unpaired;
  uint32_t pairingStartedMs = 0;
  uint32_t pairingFinishedMs = 0;
  char pairingResult[40] = {0};
};

class RadioLink {
 public:
  bool begin(const DeviceConfig& config);
  // Re-tunes a running radio to the frequency, spreading factor and power in
  // `config`, so a change made in the web UI takes effect without a reboot.
  bool applySettings(const DeviceConfig& config);
  float frequencyMhz() const { return freq_; }
  uint8_t spreadingFactor() const { return sf_; }
  int8_t outputPowerDbm() const { return static_cast<int8_t>(power_); }
  void loop(uint32_t nowMs);

  // Sends one packet if the budget allows. Returns false when it was refused or
  // the radio rejected it - the caller simply tries again later.
  bool send(core::PacketType type, const uint8_t* payload, uint8_t payloadLen, uint16_t dst = 0,
            bool requireAck = false);

  // Telemetry convenience wrapper: packs the snapshot and remembers it until it
  // is acknowledged.
  bool sendTelemetry(const core::MeterTelemetry& telemetry, uint32_t nowMs);
  bool telemetryAwaitingAck() const { return awaitingAck_; }

  // Most recent packet handed up by the radio (type + payload + source).
  bool popReceived(core::Packet& out);

  const LinkStats& stats() const { return stats_; }
  const core::DutyCycleBudget& budget() const { return budget_; }
  uint32_t estimatedAirtimeMs(size_t payloadBytes) const;

  // Pairing: the gateway broadcasts a request, the node answers, both keep the
  // net id. Only used while unpaired.
  void startPairing(uint32_t nowMs);
  void stopPairing();
  // Forgets the peer locally so a fresh pairing can be started (the other end
  // simply answers the next request again).
  void clearPairing();
  bool paired() const;
  const char* pairStateName() const;
  uint32_t pairingTimeoutMs() const { return PAIRING_TIMEOUT_MS; }

  // True when a link secret is configured and a key was derived from it: every
  // reading then travels sealed, and an unsealed one is refused.
  bool encryptionEnabled() const { return linkEncrypted_; }
  // Re-derives the key. Called when the secret or the net id changes, both of
  // which are inputs to it.
  void updateLinkKey();

 private:
  void onReceive();
  void markPaired(uint16_t peerId);
  static void IRAM_ATTR rxIsr();
  void handlePacket(const core::Packet& packet, uint32_t nowMs);
  void refreshPeer(uint32_t nowMs);
  uint16_t nextSeq() { return ++seq_; }

  SX1262 radio_{new Module(RADIO_CS_PIN, RADIO_DIO1_PIN, RADIO_RST_PIN, RADIO_BUSY_PIN)};
  core::DutyCycleBudget budget_;

  // The link key is derived, not stored: it is recomputed whenever the secret or
  // the net id changes, so the two ends cannot drift apart without one of them
  // saying so.
  uint8_t linkKey_[core::kLinkKeySize] = {0};
  bool linkEncrypted_ = false;
  LinkStats stats_;

  uint8_t sf_ = LORA_DEFAULT_SF;
  uint8_t cr_ = LORA_DEFAULT_CR;
  uint8_t power_ = LORA_DEFAULT_POWER_DBM;
  float bw_ = LORA_DEFAULT_BW_KHZ;
  float freq_ = LORA_DEFAULT_FREQ_MHZ;
  size_t preamble_ = LORA_PREAMBLE;

  uint16_t netId_ = 0;
  uint16_t srcId_ = 0;
  uint16_t peerId_ = 0;
  uint32_t pairKey_ = 0;
  uint32_t lastPairTxMs_ = 0;
  uint32_t lastStatusTxMs_ = 0;
  uint16_t seq_ = 0;
  uint16_t awaitingAckSeq_ = 0;
  bool awaitingAck_ = false;
  uint32_t awaitingAckSinceMs_ = 0;

  static volatile bool rxFlag_;
  static RadioLink* instance_;
  core::Packet inbound_;
  volatile bool inboundReady_ = false;
};
