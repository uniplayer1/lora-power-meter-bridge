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

  // The pairing handshake itself stays in the clear: it is how the two ends find
  // each other, and it carries no readings. Everything else is sealed once a key
  // exists, with the tag in place of the CRC.
  const bool sealThis = linkEncrypted_ && payloadLen > 0 && type != core::PacketType::PairRequest &&
                        type != core::PacketType::PairAck && type != core::PacketType::PairConfirm;

  uint8_t wire[core::kHeaderSize + core::kMaxPayload + core::kLinkTagSize];
  const size_t len = core::encodePacket(packet, wire, sizeof(wire), sealThis ? linkKey_ : nullptr);
  if (len == 0) return false;

  const int state = radio_.transmit(wire, len);
  radio_.startReceive();

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

void RadioLink::startPairing(uint32_t nowMs) {
  stats_.pairingActive = true;
  stats_.pairState = PairState::Running;
  stats_.pairingStartedMs = nowMs;
  stats_.pairingAttempts = 0;
  stats_.pairingResult[0] = '\0';
  lastPairTxMs_ = nowMs - PAIRING_RETRY_MS;  // fire immediately
  logEvent("pairing started (timeout %lus)", static_cast<unsigned long>(PAIRING_TIMEOUT_MS / 1000));
}

void RadioLink::stopPairing() {
  stats_.pairingActive = false;
  if (stats_.pairState == PairState::Running) stats_.pairState = PairState::Unpaired;
  logEvent("pairing stopped");
}

void RadioLink::clearPairing() {
  peerId_ = 0;
  stats_.pairState = PairState::Unpaired;
  stats_.pairingActive = false;
  stats_.pairingResult[0] = '\0';
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

void RadioLink::markPaired(uint16_t peerId) {
  peerId_ = peerId;
  // Pairing can adopt the other end's net id, and the net id is part of the key
  // derivation: without this the two ends would derive different keys and every
  // frame would be refused.
  updateLinkKey();
  stats_.peerOnline = false;
  // A stamp from before the pairing would show as an age for a peer that has
  // not been heard from yet.
  stats_.lastPeerSeenMs = 0;
  stats_.pairState = PairState::Done;
  stats_.pairingActive = false;
  stats_.pairingFinishedMs = millis();
  snprintf(stats_.pairingResult, sizeof(stats_.pairingResult), "Verbunden mit %04X", peerId);
  configStore.mutableGet().peerShortId = peerId;
  configStore.mutableGet().netId = netId_;
  configStore.mutableGet().pairKey = pairKey_;
  configStore.save();
  logEvent("paired with %04X on net %04X", peerId, netId_);
}

void RadioLink::loop(uint32_t nowMs) {
  if (rxFlag_) {
    rxFlag_ = false;
    onReceive();
  }

  refreshPeer(nowMs);

  if (awaitingAck_ && olderThan(millis(), awaitingAckSinceMs_, 8000)) {
    awaitingAck_ = false;
    ++stats_.acksMissed;
  }

  if (stats_.pairingActive) {
    if (nowMs - stats_.pairingStartedMs > PAIRING_TIMEOUT_MS) {
      stats_.pairingActive = false;
      stats_.pairState = PairState::TimedOut;
      stats_.pairingFinishedMs = nowMs;
      snprintf(stats_.pairingResult, sizeof(stats_.pairingResult),
               "Keine Gegenstelle geantwortet (%lu Versuche)",
               static_cast<unsigned long>(stats_.pairingAttempts));
      logEvent("pairing timed out after %lu attempts",
               static_cast<unsigned long>(stats_.pairingAttempts));
    } else if (nowMs - lastPairTxMs_ > PAIRING_RETRY_MS) {
      lastPairTxMs_ = nowMs;
      ++stats_.pairingAttempts;
      uint8_t payload[4];
      core::putLe(payload, pairKey_, 4);
      if (!send(core::PacketType::PairRequest, payload, sizeof(payload), 0, false)) {
        // Duty cycle or the radio refused it; say so instead of looking idle.
        logEvent("pairing: send refused (Zeitbudget oder Funkfehler)");
      }
      if (stats_.pairingAttempts == 1) {
        snprintf(stats_.pairingResult, sizeof(stats_.pairingResult), "Suche laeuft ...");
      }
    }
  }

  // Periodic presence so both ends can tell "link down" from "meter quiet".
  if (stats_.radioOk && nowMs - lastStatusTxMs_ > HEARTBEAT_MS) {
    lastStatusTxMs_ = nowMs;
    uint8_t payload[2];
    core::putLe(payload, static_cast<uint64_t>(stats_.txPackets & 0xFFFF), 2);
    send(core::PacketType::Status, payload, sizeof(payload), peerId_ ? peerId_ : 0, false);
  }
}

void RadioLink::onReceive() {
  const size_t available = radio_.getPacketLength();
  if (available == 0 || available > core::kHeaderSize + core::kMaxPayload + core::kCrcSize) {
    radio_.startReceive();
    return;
  }

  uint8_t buffer[core::kHeaderSize + core::kMaxPayload + core::kCrcSize];
  const int state = radio_.readData(buffer, available);
  const int rssi = radio_.getRSSI();
  const float snr = radio_.getSNR();
  radio_.startReceive();

  if (state != RADIOLIB_ERR_NONE) {
    ++stats_.rxCorrupt;
    return;
  }

  core::Packet packet;
  if (!core::decodePacket(buffer, available, packet, linkEncrypted_ ? linkKey_ : nullptr)) {
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
      // Answer only when unpaired, so an established link cannot be stolen by a
      // neighbour's pairing broadcast.
      if (paired() || packet.payloadLen < 4) break;
      const uint32_t key = static_cast<uint32_t>(core::getLe(packet.payload, 4));
      uint8_t reply[8];
      core::putLe(reply, pairKey_, 4);
      core::putLe(reply + 4, netId_, 4);
      send(core::PacketType::PairAck, reply, sizeof(reply), packet.src, false);
      (void)key;
      break;
    }

    case core::PacketType::PairAck: {
      if (paired() || packet.payloadLen < 8) break;
      const uint32_t nodeKey = static_cast<uint32_t>(core::getLe(packet.payload, 4));
      const uint32_t nodeNet = static_cast<uint32_t>(core::getLe(packet.payload + 4, 4));
      netId_ = static_cast<uint16_t>(nodeNet & 0xFFFF);
      pairKey_ = nodeKey;
      markPaired(packet.src);
      // Close the handshake: the answering side still has no idea who we are,
      // and would keep reporting itself as unpaired.
      uint8_t confirm[4];
      core::putLe(confirm, srcId_, 4);
      send(core::PacketType::PairConfirm, confirm, sizeof(confirm), packet.src, false);
      logEvent("pairing confirmed to %04X", packet.src);
      break;
    }

    case core::PacketType::PairConfirm: {
      if (packet.payloadLen < 4) break;
      const uint16_t initiator = static_cast<uint16_t>(core::getLe(packet.payload, 4));
      markPaired(initiator != 0 ? initiator : static_cast<uint16_t>(packet.src));
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
