#include "radio_link.h"

#include <SPI.h>
#include <string.h>

#include "config.h"
#include "lora_airtime.h"
#include "time_util.h"

volatile bool RadioLink::rxFlag_ = false;
RadioLink* RadioLink::instance_ = nullptr;

void IRAM_ATTR RadioLink::rxIsr() { rxFlag_ = true; }

bool RadioLink::begin(const DeviceConfig& config) {
  instance_ = this;
  freq_ = config.loraFreqMhz > 100.0f ? config.loraFreqMhz : LORA_DEFAULT_FREQ_MHZ;
  sf_ = config.loraSf >= 6 && config.loraSf <= 12 ? config.loraSf : LORA_DEFAULT_SF;
  cr_ = config.loraCr >= 5 && config.loraCr <= 8 ? config.loraCr : LORA_DEFAULT_CR;
  power_ = config.loraPowerDbm;
  // Hard ceiling: 14 dBm keeps the radiated power inside 25 mW ERP for
  // 868.0-868.6 MHz. A config value above it is clamped, never honoured.
  if (power_ > LORA_DEFAULT_POWER_DBM) power_ = LORA_DEFAULT_POWER_DBM;

  bw_ = LORA_DEFAULT_BW_KHZ;
  netId_ = config.netId;
  srcId_ = configStore.shortId();
  peerId_ = config.peerShortId;
  pairKey_ = config.pairKey;
  budget_.configure(LORA_DUTY_WINDOW_MS, LORA_DUTY_BUDGET_MS);

  // The sequence number starts at a random value. The per-frame nonce is built
  // from it together with the uptime and the source, so a reboot must not replay
  // a sequence the previous boot already used under the same key.
  seq_ = static_cast<uint16_t>(esp_random() & 0xFFFFu);

  updateLinkKey();
  refreshCheckCode();

  // The SX1262 is wired to the board's own SPI pins; RadioLib does not bring
  // the bus up by itself, so without this the radio init fails silently.
  SPI.begin(RADIO_SCLK_PIN, RADIO_MISO_PIN, RADIO_MOSI_PIN);

  const int state = radio_.begin();
  if (state != RADIOLIB_ERR_NONE) {
    stats_.radioOk = false;
    stats_.radioError = state;
    logEvent("radio init FAILED (%d)", state);
    return false;
  }

  radio_.setFrequency(freq_);
  radio_.setBandwidth(bw_);
  radio_.setSpreadingFactor(sf_);
  radio_.setCodingRate(cr_);
  radio_.setSyncWord(LORA_SYNC_WORD);
  radio_.setOutputPower(power_);
  radio_.setCurrentLimit(LORA_DEFAULT_CURRENT_LIMIT);
  radio_.setPreambleLength(preamble_);
  radio_.setCRC(true);
  radio_.setPacketReceivedAction(rxIsr);
  radio_.startReceive();
  lastRxIrqMs_ = millis();

  stats_.radioOk = true;
  logEvent("radio OK %.3f MHz SF%u CR%d %d dBm, airtime %lu ms/packet", freq_, sf_, cr_,
           power_, static_cast<unsigned long>(estimatedAirtimeMs(42)));
  return true;
}

bool RadioLink::applySettings(const DeviceConfig& config) {
  if (!stats_.radioOk) return false;
  freq_ = config.loraFreqMhz > 100.0f ? config.loraFreqMhz : LORA_DEFAULT_FREQ_MHZ;
  sf_ = config.loraSf >= 6 && config.loraSf <= 12 ? config.loraSf : LORA_DEFAULT_SF;
  power_ = config.loraPowerDbm;
  // Same hard ceiling as begin(): never above 14 dBm, whatever the config says.
  if (power_ > LORA_DEFAULT_POWER_DBM) power_ = LORA_DEFAULT_POWER_DBM;

  // Transmissions are blocking, so nothing is on the air while the web handler
  // runs; the modem only has to leave receive mode to be re-tuned.
  radio_.standby();
  int state = radio_.setFrequency(freq_);
  if (state == RADIOLIB_ERR_NONE) state = radio_.setSpreadingFactor(sf_);
  if (state == RADIOLIB_ERR_NONE) state = radio_.setOutputPower(power_);
  radio_.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    logEvent("radio re-tune FAILED (%d)", state);
    return false;
  }
  logEvent("radio re-tuned %.3f MHz SF%u %d dBm, airtime %lu ms/packet", freq_, sf_, power_,
           static_cast<unsigned long>(estimatedAirtimeMs(42)));
  return true;
}

uint32_t RadioLink::estimatedAirtimeMs(size_t payloadBytes) const {
  // +2 for the CRC the modem appends is already covered by the formula's crc
  // term; this is the wire length of one of our packets.
  return core::loraAirtimeMsCeil(sf_, bw_, cr_, static_cast<uint8_t>(preamble_), payloadBytes);
}

uint32_t RadioLink::minSendIntervalMs() const {
  // A sealed reading: header + telemetry + tag. 85 % of the budget goes to
  // readings; the rest covers heartbeats, pairing and the odd retry.
  const uint32_t airtime =
      estimatedAirtimeMs(core::kHeaderSize + core::MeterTelemetry::kWireSize + core::kLinkTagSize);
  const uint64_t budgetForReadings = static_cast<uint64_t>(LORA_DUTY_BUDGET_MS) * 85u / 100u;
  const uint64_t interval = static_cast<uint64_t>(airtime) * LORA_DUTY_WINDOW_MS / budgetForReadings + 1;
  return interval < SEND_INTERVAL_MIN_MS ? SEND_INTERVAL_MIN_MS : static_cast<uint32_t>(interval);
}

bool RadioLink::paired() const { return netId_ != 0 && peerId_ != 0; }

void RadioLink::updateLinkKey() {
  uint8_t secret[core::kLinkSecretSize];
  const bool hadKey = linkEncrypted_;

  if (configStore.linkSecret(secret) && netId_ != 0) {
    core::deriveLinkKey(secret, netId_, linkKey_);
    linkEncrypted_ = true;
  } else {
    linkEncrypted_ = false;
  }

  if (linkEncrypted_ != hadKey) {
    logEvent(linkEncrypted_ ? "link encryption on" : "link encryption off");
  }
}

bool RadioLink::send(core::PacketType type, const uint8_t* payload, uint8_t payloadLen,
                     uint16_t dst, bool requireAck) {
  if (!stats_.radioOk) return false;
  if (payloadLen > core::kMaxPayload) return false;

  core::Packet packet;
  packet.type = type;
  packet.netId = netId_;
  packet.src = srcId_;
  packet.dst = dst;
  packet.seq = nextSeq();
  packet.uptimeS = millis() / 1000;
  packet.payloadLen = payloadLen;
  if (payloadLen > 0 && payload != nullptr) memcpy(packet.payload, payload, payloadLen);

  const uint32_t now = millis();
  const uint32_t airtime = estimatedAirtimeMs(packet.wireSize());
  if (!budget_.canTransmit(now, airtime)) {
    ++stats_.txBlocked;
    budget_.noteBlocked();
    return false;
  }

  // The request and the answer of a pairing stay in the clear: they carry the
  // public keys the secret is computed from. Everything else is sealed once a
  // key exists, with the tag in place of the CRC - the confirmation included,
  // which is how the answering side learns that both computed the same key.
  const bool sealThis = linkEncrypted_ && payloadLen > 0 && type != core::PacketType::PairRequest &&
                        type != core::PacketType::PairAck;

  uint8_t wire[core::kHeaderSize + core::kMaxPayload + core::kLinkTagSize];
  const size_t len = core::encodePacket(packet, wire, sizeof(wire), sealThis ? linkKey_ : nullptr);
  if (len == 0) return false;

  const int state = radio_.transmit(wire, len);
  radio_.startReceive();
  // DIO1 also signals "transmission done", and the receive interrupt is still
  // attached while transmit() polls for it, so every transmission used to raise
  // rxFlag_. The next loop then read the buffer and restarted the receiver -
  // a few milliseconds after sending, which is exactly when the other side's
  // acknowledgement arrives. That restart cut most acknowledgements off.
  rxFlag_ = false;
  lastTxMs_ = millis();

  budget_.commit(now, airtime);
  stats_.airtimeMs += airtime;
  ++stats_.txPackets;

  if (state != RADIOLIB_ERR_NONE) {
    ++stats_.txFailures;
    logEvent("radio TX failed (%d)", state);
    return false;
  }

  if (requireAck) {
    awaitingAck_ = true;
    awaitingAckSeq_ = packet.seq;
    awaitingAckSinceMs_ = now;
  }
  return true;
}

bool RadioLink::sendTelemetry(const core::MeterTelemetry& telemetry, uint32_t nowMs) {
  uint8_t payload[core::MeterTelemetry::kWireSize];
  const size_t len = core::packTelemetry(telemetry, payload, sizeof(payload));
  if (len == 0) return false;
  if (!send(core::PacketType::Data, payload, static_cast<uint8_t>(len), peerId_, true)) return false;
  (void)nowMs;
  return true;
}

void RadioLink::newPairKeys() {
  for (size_t i = 0; i < core::kX25519Size; i += 4) {
    const uint32_t r = esp_random();
    memcpy(pairPriv_ + i, &r, 4);
  }
  core::x25519Public(pairPub_, pairPriv_);
}

void RadioLink::forgetPairKeys() {
  memset(pairPriv_, 0, sizeof(pairPriv_));
  memset(pairPub_, 0, sizeof(pairPub_));
  memset(initiatorPub_, 0, sizeof(initiatorPub_));
  memset(pendingSecret_, 0, sizeof(pendingSecret_));
  memset(pendingKey_, 0, sizeof(pendingKey_));
  pendingKeyValid_ = false;
}

void RadioLink::refreshCheckCode() {
  uint8_t secret[core::kLinkSecretSize];
  stats_.checkCodeValid = configStore.linkSecret(secret);
  stats_.checkCode = stats_.checkCodeValid ? core::pairingCheckCode(secret) : 0;
  memset(secret, 0, sizeof(secret));
}

void RadioLink::startPairing(uint32_t nowMs) {
  // Re-pairing while paired: the old peer is set aside so that an answer can be
  // accepted, and put back if nobody answers. Before, a paired device ignored
  // every answer - "Neu koppeln" always ended in "fehlgeschlagen", even with
  // the other device right next to it and the link working.
  if (!stats_.pairingActive && peerId_ != 0) previousPeer_ = peerId_;
  peerId_ = 0;
  initiating_ = true;
  pendingConfirmFrom_ = 0;
  forgetPairKeys();
  newPairKeys();
  stats_.pairingActive = true;
  stats_.pairState = PairState::Running;
  stats_.pairingStartedMs = nowMs;
  stats_.pairingAttempts = 0;
  stats_.pairingResult[0] = '\0';
  lastPairTxMs_ = nowMs - PAIRING_RETRY_MS;  // fire immediately
  logEvent("pairing started (timeout %lus)", static_cast<unsigned long>(PAIRING_TIMEOUT_MS / 1000));
}

void RadioLink::endPairing(PairState state, uint32_t nowMs) {
  initiating_ = false;
  pendingConfirmFrom_ = 0;
  forgetPairKeys();
  stats_.pairingActive = false;
  stats_.pairState = state;
  stats_.pairingFinishedMs = nowMs;
  if (peerId_ == 0 && previousPeer_ != 0) {
    peerId_ = previousPeer_;
    logEvent("pairing: keeping the previous peer %04X", peerId_);
  }
  previousPeer_ = 0;
}

void RadioLink::stopPairing() {
  if (!stats_.pairingActive) return;
  endPairing(PairState::Unpaired, millis());
  logEvent("pairing stopped");
}

void RadioLink::clearPairing() {
  peerId_ = 0;
  previousPeer_ = 0;
  initiating_ = false;
  pendingConfirmFrom_ = 0;
  confirmsLeft_ = 0;
  stats_.peerOnline = false;
  stats_.lastPeerSeenMs = 0;
  stats_.pairState = PairState::Unpaired;
  stats_.pairingActive = false;
  stats_.pairingResult[0] = '\0';
  forgetPairKeys();
  // The secret belongs to the pairing: without a peer there is nobody to share
  // it with, and the next pairing agrees a new one.
  configStore.clearLinkSecret();
  updateLinkKey();
  refreshCheckCode();
  configStore.mutableGet().peerShortId = 0;
  configStore.save();
  logEvent("pairing cleared");
}

const char* RadioLink::pairStateName() const {
  switch (stats_.pairState) {
    case PairState::Running: return "running";
    case PairState::Done: return "paired";
    case PairState::TimedOut: return "timeout";
    default: return "unpaired";
  }
}

void RadioLink::markPaired(uint16_t peerId, uint32_t nowMs) {
  const bool samePeer = peerId == configStore.get().peerShortId && netId_ == configStore.get().netId &&
                        pairKey_ == configStore.get().pairKey;
  const bool newPeer = peerId != previousPeer_ && peerId != configStore.get().peerShortId;
  peerId_ = peerId;
  previousPeer_ = 0;
  // Pairing can adopt the other end's net id, and the net id is part of the key
  // derivation: without this the two ends would derive different keys and every
  // frame would be refused.
  updateLinkKey();
  refreshCheckCode();
  forgetPairKeys();
  // "Online" needs a real, decodable packet from the peer, which a pairing
  // frame is not - a missing or different link secret would otherwise look
  // connected for two minutes and then drop. A heartbeat goes out shortly, so
  // the other side hears from us without waiting for the next reading.
  if (newPeer) {
    stats_.peerOnline = false;
    stats_.lastPeerSeenMs = 0;
    peerUptimeKnown_ = false;
  }
  heartbeatDueMs_ = nowMs + 1500;
  if (heartbeatDueMs_ == 0) heartbeatDueMs_ = 1;
  initiating_ = false;
  pendingConfirmFrom_ = 0;
  stats_.pairState = PairState::Done;
  stats_.pairingActive = false;
  stats_.pairingFinishedMs = nowMs;
  snprintf(stats_.pairingResult, sizeof(stats_.pairingResult), "Verbunden mit %04X, Code %04u", peerId,
           static_cast<unsigned>(stats_.checkCode));
  if (!samePeer) {
    configStore.mutableGet().peerShortId = peerId;
    configStore.mutableGet().netId = netId_;
    configStore.mutableGet().pairKey = pairKey_;
    configStore.save();
  }
  logEvent("paired with %04X on net %04X", peerId, netId_);
}

void RadioLink::checkReceiver(uint32_t nowMs) {
  // A receiver that stopped reporting packets is re-armed instead of staying
  // deaf until the board is unplugged. Two cases: a packet arrived but its
  // interrupt edge was missed (DIO1 stays high, so no new edge ever comes), or
  // the modem left receive mode. Both are cheap to check every so often.
  if (!stats_.radioOk) return;
  if (!olderThan(nowMs, lastRxIrqMs_, RX_WATCHDOG_MS)) return;
  if (!olderThan(nowMs, lastRxCheckMs_, RX_WATCHDOG_MS)) return;
  lastRxCheckMs_ = nowMs;
  const uint32_t irq = radio_.getIrqFlags();
  if (irq & RADIOLIB_SX126X_IRQ_RX_DONE) {
    logEvent("radio: packet without interrupt - reading it");
    onReceive();
    return;
  }
  radio_.startReceive();
}

void RadioLink::loop(uint32_t nowMs) {
  if (rxFlag_) {
    rxFlag_ = false;
    onReceive();
  }

  // The `now` handed down from main.cpp was taken before the WiFi and web work
  // of this iteration, and packet handling stamps with millis(): mixing the two
  // makes "later" look earlier. Everything below uses one fresh clock.
  nowMs = millis();
  refreshPeer(nowMs);
  checkReceiver(nowMs);

  if (awaitingAck_ && olderThan(nowMs, awaitingAckSinceMs_, 8000)) {
    awaitingAck_ = false;
    ++stats_.acksMissed;
  }

  if (confirmsLeft_ > 0 && peerId_ != 0 && static_cast<int32_t>(nowMs - nextConfirmMs_) >= 0) {
    --confirmsLeft_;
    nextConfirmMs_ = nowMs + 700;
    uint8_t confirm[4];
    core::putLe(confirm, srcId_, 4);
    send(core::PacketType::PairConfirm, confirm, sizeof(confirm), peerId_, false);
  }

  if (stats_.pairingActive) {
    if (olderThan(nowMs, stats_.pairingStartedMs, PAIRING_TIMEOUT_MS)) {
      const uint16_t kept = previousPeer_;
      endPairing(PairState::TimedOut, nowMs);
      if (kept != 0) {
        snprintf(stats_.pairingResult, sizeof(stats_.pairingResult),
                 "Keine Antwort - Kopplung mit %04X bleibt", kept);
      } else {
        snprintf(stats_.pairingResult, sizeof(stats_.pairingResult),
                 "Keine Gegenstelle geantwortet (%lu Versuche)",
                 static_cast<unsigned long>(stats_.pairingAttempts));
      }
      logEvent("pairing timed out after %lu attempts",
               static_cast<unsigned long>(stats_.pairingAttempts));
    } else if (initiating_ && olderThan(nowMs, lastPairTxMs_, PAIRING_RETRY_MS)) {
      lastPairTxMs_ = nowMs;
      ++stats_.pairingAttempts;
      if (!send(core::PacketType::PairRequest, pairPub_, sizeof(pairPub_), 0, false)) {
        // Duty cycle or the radio refused it; say so instead of looking idle.
        logEvent("pairing: send refused (Zeitbudget oder Funkfehler)");
      }
      if (stats_.pairingAttempts == 1) {
        snprintf(stats_.pairingResult, sizeof(stats_.pairingResult), "Suche laeuft ...");
      }
    }
  }

  // Presence, so both ends can tell "link down" from "meter quiet". Any frame
  // already says "I am here": readings and acknowledgements go out every send
  // interval, and an extra status frame each minute on top of them cost
  // airtime the meter did not have. So it only fills silences, plus one right
  // after pairing so that both ends see each other within seconds.
  const bool heartbeatForced = heartbeatDueMs_ != 0 && static_cast<int32_t>(nowMs - heartbeatDueMs_) >= 0;
  if (stats_.radioOk && confirmsLeft_ == 0 &&
      (heartbeatForced || lastTxMs_ == 0 || olderThan(nowMs, lastTxMs_, HEARTBEAT_MS))) {
    heartbeatDueMs_ = 0;
    uint8_t payload[2];
    core::putLe(payload, static_cast<uint64_t>(stats_.txPackets & 0xFFFF), 2);
    if (!send(core::PacketType::Status, payload, sizeof(payload), peerId_ ? peerId_ : 0, false)) {
      // Refused by the budget: try again in a heartbeat, not on every loop.
      lastTxMs_ = nowMs;
    }
  }
}

void RadioLink::onReceive() {
  // Only a finished reception is read. Anything else (a stray edge from the
  // transmitter) must not touch the modem: restarting receive mode in the middle
  // of an incoming frame throws that frame away.
  const uint32_t irq = radio_.getIrqFlags();
  if ((irq & RADIOLIB_SX126X_IRQ_RX_DONE) == 0) return;
  lastRxIrqMs_ = millis();

  const size_t available = radio_.getPacketLength();
  constexpr size_t kMaxFrame = core::kHeaderSize + core::kMaxPayload + core::kLinkTagSize;
  static_assert(core::kLinkTagSize >= core::kCrcSize, "frame buffer sized for the larger trailer");
  if (available == 0 || available > kMaxFrame) {
    radio_.startReceive();
    return;
  }

  uint8_t buffer[kMaxFrame];
  const int state = radio_.readData(buffer, available);
  const int rssi = radio_.getRSSI();
  const float snr = radio_.getSNR();
  radio_.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    ++stats_.rxCorrupt;
    return;
  }

  // A pairing confirmation is sealed with the key that pairing just agreed,
  // which this side holds as pending until that very frame proves it.
  const uint8_t* key = linkEncrypted_ ? linkKey_ : nullptr;
  if (pendingKeyValid_ && available > 2 &&
      buffer[2] == static_cast<uint8_t>(core::PacketType::PairConfirm)) {
    key = pendingKey_;
  }
  core::Packet packet;
  if (!core::decodePacket(buffer, available, packet, key)) {
    // Distinguish "the air was noisy" from "this frame was not written by our
    // peer": a sealed frame that fails its tag is the second kind.
    if ((buffer[3] & core::kFlagEncrypted) != 0) {
      ++stats_.rxAuthFailures;
    } else {
      ++stats_.rxCorrupt;
    }
    return;
  }

  // A packet addressed to someone else on the same channel is not ours.
  if (packet.dst != 0 && packet.dst != srcId_) return;
  if (packet.src == srcId_) return;  // our own transmission

  ++stats_.rxPackets;
  stats_.lastRssi = rssi;
  stats_.lastSnr = snr;
  handlePacket(packet, millis());
}

void RadioLink::handlePacket(const core::Packet& packet, uint32_t nowMs) {
  // With a key configured, a reading that arrives in the clear is not a reading:
  // anyone in range could have written it. Pairing frames are the exception -
  // they are how the link gets set up in the first place.
  if (linkEncrypted_ && (packet.flags & core::kFlagEncrypted) == 0) {
    switch (packet.type) {
      case core::PacketType::Data:
      case core::PacketType::Ack:
      case core::PacketType::Status:
      case core::PacketType::Cmd:
      case core::PacketType::TimeSync:
        ++stats_.rxPlaintextDropped;
        return;
      default:
        break;
    }
  }
  // Traffic is recorded for every packet: the page has to be able to say "a
  // packet arrived 3 s ago" without claiming the two devices are coupled. That
  // distinction is the whole point - peerOnline stays reserved for the peer we
  // are paired with, while the age of the last packet says whether anything is
  // on the air at all.
  stats_.lastPacketSeenMs = nowMs;
  stats_.lastPacketSrc = packet.src;

  // Only a peer we are actually paired with can count as "online". While
  // peerId_ is zero this accepted ANY packet on the channel, a third device's
  // broadcast included - so a device that had never been paired showed
  // "verbunden" for as long as anything moved on the air, and the 90 s timeout
  // could never fire because every stranger's packet re-armed it.
  const bool fromPeer = peerId_ != 0 && packet.src == peerId_;
  if (fromPeer) {
    // Every frame carries the sender's uptime in its header: when it goes
    // backwards, the peer has restarted.
    if (peerUptimeKnown_ && packet.uptimeS + 2 < peerUptimeS_) {
      peerRestarted_ = true;
      logEvent("peer %04X restarted", packet.src);
    }
    peerUptimeS_ = packet.uptimeS;
    peerUptimeKnown_ = true;
    stats_.peerOnline = true;
    stats_.lastPeerSeenMs = nowMs;
    stats_.peerRssi = stats_.lastRssi;
    stats_.peerSnr = stats_.lastSnr;
  }

  switch (packet.type) {
    case core::PacketType::Ack:
      // The payload carries the sequence number being acknowledged.
      if (packet.payloadLen >= 2) {
        const uint16_t acked = static_cast<uint16_t>(core::getLe(packet.payload, 2));
        if (awaitingAck_ && acked == awaitingAckSeq_) {
          awaitingAck_ = false;
          ++stats_.acksReceived;
        }
      }
      break;

    case core::PacketType::PairRequest: {
      if (packet.payloadLen < core::kX25519Size) break;
      // Answered when this side is free, or when the request comes from the
      // device it is already paired with (that one is re-pairing). A neighbour's
      // broadcast cannot take over an established link.
      const bool fromCurrentPeer = peerId_ != 0 && packet.src == peerId_;
      if (paired() && !fromCurrentPeer) break;
      // Both buttons pressed: both sides send requests, and if both answered,
      // each would adopt the other's net id and they would end up on different
      // keys. The lower id answers, the higher id stays the one that asks.
      if (initiating_ && srcId_ > packet.src) break;
      if (initiating_) {
        initiating_ = false;
        logEvent("pairing: %04X is pairing too - answering it", packet.src);
      }
      // One key pair per request: the requester repeats its request every few
      // seconds with the same public key, and every answer to it must lead to
      // the same secret, whichever answer it ends up hearing.
      const bool sameRequest = pendingKeyValid_ && packet.src == pendingConfirmFrom_ &&
                               memcmp(initiatorPub_, packet.payload, core::kX25519Size) == 0;
      if (!sameRequest) {
        forgetPairKeys();
        memcpy(initiatorPub_, packet.payload, core::kX25519Size);
        newPairKeys();
        uint8_t shared[core::kX25519Size];
        core::x25519(shared, pairPriv_, initiatorPub_);
        const bool ok = core::derivePairingSecret(shared, initiatorPub_, pairPub_, pendingSecret_);
        memset(shared, 0, sizeof(shared));
        if (!ok) {
          forgetPairKeys();
          logEvent("pairing: unusable key from %04X", packet.src);
          break;
        }
        // The requester adopts this side's net id from the answer's header, so
        // the key is derived for it.
        core::deriveLinkKey(pendingSecret_, netId_, pendingKey_);
        pendingKeyValid_ = true;
      }
      send(core::PacketType::PairAck, pairPub_, sizeof(pairPub_), packet.src, false);
      pendingConfirmFrom_ = packet.src;
      break;
    }

    case core::PacketType::PairAck: {
      // Only the side that asked takes an answer. Before, any unpaired device
      // adopted the net id from any answer it overheard.
      if (!initiating_ || packet.payloadLen < core::kX25519Size || packet.dst != srcId_) break;
      uint8_t shared[core::kX25519Size];
      uint8_t secret[core::kLinkSecretSize];
      core::x25519(shared, pairPriv_, packet.payload);
      const bool ok = core::derivePairingSecret(shared, pairPub_, packet.payload, secret);
      memset(shared, 0, sizeof(shared));
      if (!ok || packet.netId == 0) {
        logEvent("pairing: unusable answer from %04X", packet.src);
        break;
      }
      netId_ = packet.netId;
      configStore.setLinkSecret(secret);
      memset(secret, 0, sizeof(secret));
      markPaired(packet.src, nowMs);
      // Close the handshake: the answering side still has no idea who we are,
      // and holds the secret only as pending. The confirmation is sealed with
      // the new key, which is what makes the other side accept it. Repeated
      // from loop(), so a single lost frame does not leave the ends disagreeing.
      confirmsLeft_ = 3;
      nextConfirmMs_ = nowMs;
      logEvent("pairing confirmed to %04X", packet.src);
      break;
    }

    case core::PacketType::PairConfirm: {
      if (packet.payloadLen < 4 || packet.dst != srcId_) break;
      // Accepted only from the device this side answered, and only sealed with
      // the key the exchange produced - proof that both computed the same one.
      // The repeats that follow decode under the committed key and fall
      // through here harmlessly.
      const bool expected = pendingKeyValid_ && pendingConfirmFrom_ != 0 &&
                            packet.src == pendingConfirmFrom_ &&
                            (packet.flags & core::kFlagEncrypted) != 0;
      if (!expected) break;
      configStore.setLinkSecret(pendingSecret_);
      markPaired(packet.src, nowMs);
      break;
    }

    default:
      // A reading is acknowledged at the link layer, so the sender learns its
      // packet arrived without the application having to care.
      if (packet.type == core::PacketType::Data && packet.dst != 0) {
        uint8_t ack[2];
        core::putLe(ack, packet.seq, 2);
        send(core::PacketType::Ack, ack, sizeof(ack), packet.src, false);
      }
      inbound_ = packet;
      inboundReady_ = true;
      break;
  }
}

bool RadioLink::popReceived(core::Packet& out) {
  if (!inboundReady_) return false;
  out = inbound_;
  inboundReady_ = false;
  return true;
}

void RadioLink::refreshPeer(uint32_t nowMs) {
  (void)nowMs;
  if (!stats_.peerOnline) return;
  // Compare against the same clock the stamp was taken from. handlePacket()
  // stamps with millis(), while the `now` handed down from main.cpp was
  // captured before the WiFi and metering work of this iteration - subtracting
  // the two can go negative and, unsigned, look like a 71-minute silence.
  const uint32_t now = millis();
  if (stats_.lastPeerSeenMs == 0) {
    stats_.peerOnline = false;
    return;
  }
  if (olderThan(now, stats_.lastPeerSeenMs, PEER_TIMEOUT_MS)) {
    stats_.peerOnline = false;
    logEvent("peer lost (silent for %lus)",
             static_cast<unsigned long>(ageSeconds(now, stats_.lastPeerSeenMs)));
  }
}
