#pragma once
// Persistent device configuration and identity.
//
// Everything the owner can change lives in one NVS blob, so a factory reset is
// a single namespace erase and an OTA update never loses settings. Identity is
// derived from the chip MAC: one image, two boards, no build-time flags.

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include "crypto.h"
#include "ring_log.h"

enum class DeviceRole : uint8_t {
  Unset = 0,    // freshly flashed: the setup page asks which board this is
  Meter = 1,    // basement node: reads the meter, sends over LoRa, keeps its AP
  Gateway = 2,  // flat: receives over LoRa, serves REST for Home Assistant
};

enum class MeterProtocol : uint8_t {
  Auto = 0,  // listen for SML, fall back to an active D0 wake-up
  Sml = 1,   // passive: modern German meters push SML every 1-2 s
  D0 = 2,    // active: IEC 62056-21 mode C for older meters
};

enum class MeterInvert : uint8_t {
  Auto = 0,  // flip polarity until a telegram checksums
  Normal = 1,
  Inverted = 2,
};

struct DeviceConfig {
  uint16_t schema;  // bumped when the layout changes; an old blob is discarded

  DeviceRole role;
  char deviceName[28];

  // Link identity. Both boards must agree, which the pairing flow arranges.
  uint16_t netId;
  uint32_t pairKey;
  uint16_t peerShortId;

  // Meter side
  MeterProtocol meterProtocol;
  MeterInvert meterInvert;
  uint32_t meterBaud;
  uint32_t sendIntervalMs;

  // Radio
  float loraFreqMhz;
  uint8_t loraSf;
  uint8_t loraCr;
  int8_t loraPowerDbm;

  // Access
  char apPassword[16];  // setup code: AP password and initial web password
  char webPassword[28];
  bool keepApOn;

  // Station WiFi (gateway; the node may also join it for OTA)
  bool staConfigured;
  char staSsid[33];
  char staPass[65];

  // Home Assistant. REST needs a package file, MQTT discovery needs a broker
  // but nothing else - both are offered, because only MQTT can make the device
  // announce itself without anyone touching a config file.
  bool haEnabled;
  uint16_t haPort;

  // ── fields appended after the initial release ──────────────
  // Loading tolerates a shorter stored blob and overlays the prefix, so adding
  // settings never wipes an installation.
  bool mqttEnabled;
  char mqttHost[64];
  uint16_t mqttPort;
  char mqttUser[32];
  char mqttPass[64];
  char mqttBase[48];   // state topic prefix, defaults to lilygo/<short id>
  char mqttPrefix[24]; // discovery prefix, defaults to homeassistant

  // APPEND ONLY from here on. The loader treats a shorter stored blob as an
  // older layout and overlays its prefix, which only holds if new fields are
  // added at the end - inserting one in the middle silently shifts every field
  // after it (it put the radio on 568 MHz once).
  //
  // Where the read head is wired: defaults are the QWIIC "Uart1" pins, which
  // are also on the expansion header; any pin meterPinAllowed() accepts works.
  uint8_t meterRxPin;
  uint8_t meterTxPin;

  // Keep the access code off the OLED. The code is the access point's password
  // AND the first web login, and a display in a shared hallway or next to the
  // meter box shows it to anyone walking past. The CLI (webpass show on) is the
  // way back in if the code is ever forgotten and not written down.
  bool hideCodeOnDisplay;
};

inline bool mqttConfigured(const DeviceConfig& cfg) {
  return cfg.mqttEnabled && cfg.mqttHost[0] != '\0';
}

class ConfigStore {
 public:
  bool begin();  // loads from NVS, fills defaults on first boot
  const DeviceConfig& get() const { return cfg_; }
  DeviceConfig& mutableGet() { return cfg_; }

  bool save();
  bool reset();  // factory reset: erase NVS, re-derive identity, keep nothing

  bool provisioned() const { return cfg_.role != DeviceRole::Unset; }
  bool roleIs(DeviceRole role) const { return cfg_.role == role; }

  // Identity derived from the chip MAC: stable across reboots, different
  // between two boards running the same image.
  const char* macString() const { return mac_; }
  uint16_t shortId() const { return shortId_; }
  const char* setupCode() const { return cfg_.apPassword; }
  const char* apSsid() const { return apSsid_; }

  // The access point's own name, stored under its own NVS key so it can be
  // changed without invalidating the settings blob. Falls back to the name
  // derived from the MAC.
  bool setApSsid(const String& ssid);
  bool setSetupCode(const String& code);

  // Fresh random net id + pairing key, generated on first boot and on reset.
  void regenerateNetwork();

  // The link secret: 8 random bytes the two ends derive their radio keys from.
  // The sensor node generates it once and shows it on its display; the gateway
  // is given the same value once, typed in. Nothing about it is ever transmitted,
  // which is what makes the link private. Stored under its own NVS key so that
  // growing it cannot invalidate the settings blob.
  bool linkSecretSet() const;
  bool linkSecret(uint8_t out[core::kLinkSecretSize]) const;
  bool setLinkSecret(const uint8_t secret[core::kLinkSecretSize]);
  void clearLinkSecret();   // back to a plaintext link

 private:
  void applyDefaults();
  void deriveIdentity();
  void generateSetupCode();
  void storeApIdentity();
  void storeLinkSecret();

  DeviceConfig cfg_;
  Preferences prefs_;
  char mac_[18] = {0};
  char apSsid_[40] = {0};
  uint16_t shortId_ = 0;
  uint8_t linkSecret_[core::kLinkSecretSize] = {0};
  // Per-board randomness generated on first boot and stored under its own NVS
  // key. It is mixed into the setup code, so the code cannot be recomputed from
  // the MAC alone - the MAC is printed on the label and advertised over mDNS.
  uint32_t networkSeed_ = 0;
};

// Process-wide instance: there is exactly one device configuration.
extern ConfigStore configStore;

// On-device event log, surfaced in the web UI.
extern core::RingLog<String, LOG_CAPACITY> eventLog;
void logEvent(const char* fmt, ...);
