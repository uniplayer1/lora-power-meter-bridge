#pragma once
// Home Assistant over MQTT, with discovery.
//
// This is the only way the device can *announce* itself: Home Assistant has no
// REST discovery, so the REST path always needs a package file copied in. With
// a broker configured, the gateway publishes its own entity descriptions as
// retained discovery configs and every sensor appears without anyone editing a
// file. Both paths stay available and describe the same values.

#include <Arduino.h>

#include "app_config.h"
#include "meter_reader.h"
#include "radio_link.h"

struct MqttStats {
  bool enabled = false;
  bool connected = false;
  uint32_t connects = 0;
  uint32_t failures = 0;
  uint32_t publishes = 0;
  uint32_t lastPublishMs = 0;
  uint32_t lastConnectAttemptMs = 0;
  int lastError = 0;
  char lastTopic[64] = {0};
  bool announced = false;
};

class HaMqtt {
 public:
  void begin(const DeviceConfig& config);
  void loop(uint32_t nowMs, const MeterReader& meter, const RadioLink& link);

  // Publishes the discovery configs once per boot (retained), so entities
  // reappear even if someone deletes them in Home Assistant.
  bool announce(uint32_t nowMs);
  void publishState(uint32_t nowMs, const MeterReader& meter, const RadioLink& link);

  const MqttStats& stats() const { return stats_; }
  void applyConfig(const DeviceConfig& config);

  // Everything that would be published, for `mqtt dump` and the web UI: it is
  // the only way to check the discovery payloads without a broker.
  String describeDiscovery(const DeviceConfig& config, const MeterReader& meter,
                           const RadioLink& link) const;

 private:
  void connectIfNeeded(uint32_t nowMs);

  MqttStats stats_;
  bool started_ = false;
};

// Single instance, defined in main.cpp: the web UI reports its state and can
// trigger a discovery announcement on demand.
extern HaMqtt haMqtt;
