#include "app_config.h"

#include <esp_partition.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <stdarg.h>

#include "firmware_version.h"
#include "wall_clock.h"

ConfigStore configStore;
core::RingLog<String, LOG_CAPACITY> eventLog;

namespace {

// 4: meter UART pins moved to the end of the struct. The version matters: a
// blob from a build with a different field order must not be interpreted, and
// bumping this makes the loader keep only the identity and start the rest from
// the defaults.
constexpr uint16_t kSchema = 4;

// Unambiguous alphabet for the setup code: no I/O/0/1 to misread on an OLED.
const char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

uint32_t hash32(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 16777619u;
  }
  return h;
}

}  // namespace

void logEvent(const char* fmt, ...) {
  char buffer[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);

  // Stamped with the time of day once the clock is set; before that the log
  // has only its order to go by.
  char stamp[12];
  clockFormat(stamp, sizeof(stamp), "%H:%M:%S ");
  eventLog.push(String(stamp) + buffer);
  Serial.printf("[EVT] %s%s\n", stamp, buffer);
}

namespace {

// NVS is where the whole setup lives, so make it work before trusting it.
// A board that ships with a factory image can carry an NVS partition whose
// layout the running IDF cannot mount; nvs_flash_init() then fails, every read
// reports "key not found" and every write is silently dropped - the device
// looks like it forgets its settings on every reboot. Erasing and
// re-initialising is the documented recovery, and it is done here rather than
// left to whoever holds the board.
void ensureNvsReady() {
  const esp_partition_t* partition =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, nullptr);
  if (partition == nullptr) {
    Serial.println(F("[CFG] FATAL: the partition table has no nvs partition"));
    return;
  }
  Serial.printf("[CFG] nvs partition: offset 0x%06lX size %lu bytes\n",
                static_cast<unsigned long>(partition->address),
                static_cast<unsigned long>(partition->size));

  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK) {
    Serial.printf("[CFG] nvs_flash_init failed: %s - erasing and retrying\n", esp_err_to_name(err));
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  Serial.printf("[CFG] nvs ready: %s\n", esp_err_to_name(err));
}

}  // namespace

bool ConfigStore::begin() {
  deriveIdentity();
  applyDefaults();
  ensureNvsReady();

  // ONE Preferences session for the whole load.
  //
  // Preferences::begin() refuses to open while a session is already open - it
  // returns false, silently. Leaving the session open here is what made the
  // firmware forget everything: the second begin() below was skipped, and every
  // later begin() inside save() returned false, so putBytes/putString were
  // never reached and every write was dropped without an error message. The
  // device looked like it simply refused to remember its setup.
  if (!prefs_.begin(CFG_NAMESPACE, false)) {
    Serial.println("[CFG] NVS unavailable - running with defaults (nothing persists)");
    return true;
  }

  DeviceConfig stored;
  bool replacedStoredBlob = false;
  bool replacedSetupCode = false;
  const size_t read = prefs_.getBytesLength("cfg");
  if (read == 0) {
    Serial.println(F("[CFG] no stored settings yet - starting from defaults"));
  } else if (read > sizeof(DeviceConfig)) {
    // Written by a newer firmware: refuse rather than misread the layout.
    Serial.printf("[CFG] stored settings are %u bytes, this build knows %u - using defaults\n",
                  static_cast<unsigned>(read), static_cast<unsigned>(sizeof(DeviceConfig)));
  } else {
    // A shorter blob is a valid older layout: the fields only ever get appended,
    // so the prefix still means what it meant. Overlay it on the defaults.
    memset(&stored, 0, sizeof(stored));
    prefs_.getBytes("cfg", &stored, read);
    if (read < sizeof(DeviceConfig)) {
      Serial.printf("[CFG] stored settings are %u bytes, newer build has %u - keeping the known fields\n",
                    static_cast<unsigned>(read), static_cast<unsigned>(sizeof(DeviceConfig)));
    }
    if (stored.schema == kSchema) {
      DeviceConfig merged = cfg_;  // defaults, including the appended fields
      memcpy(&merged, &stored, read);
      cfg_ = merged;
      Serial.printf("[CFG] loaded stored settings: role=%d name='%s'\n", static_cast<int>(cfg_.role),
                    cfg_.deviceName);
    } else {
      Serial.printf("[CFG] schema %u -> %u, keeping identity and resetting the rest\n", stored.schema,
                    kSchema);
      // Keep the things that identify this board, drop the layout that changed.
      cfg_.netId = stored.netId;
      cfg_.pairKey = stored.pairKey;
      cfg_.role = stored.role;
      // Replace the unreadable blob at once, so this happens exactly once.
      // Written through the session that is already open: save() opens its own,
      // and Preferences refuses to open a second one - the write would be
      // dropped and the migration would run again on every boot.
      replacedStoredBlob = true;
    }
  }

  // The AP name and code live under their own keys, so the owner can rename the
  // access point without the settings blob changing size.
  const String storedSsid = prefs_.getString("apssid", "");
  if (storedSsid.length() >= 4) {
    snprintf(apSsid_, sizeof(apSsid_), "%s", storedSsid.c_str());
  }
  // The per-board randomness behind the setup code. Created here on first boot
  // and written back with the rest, so the code stays stable from then on.
  networkSeed_ = prefs_.getUInt("netseed", 0);
  if (networkSeed_ == 0) {
    networkSeed_ = esp_random();
    if (networkSeed_ == 0) networkSeed_ = 0x5EED0001u;
    prefs_.putUInt("netseed", networkSeed_);
    // Any code derived before this had no seed behind it: derive it again.
    replacedSetupCode = true;
  }

  const String storedCode = prefs_.getString("apcode", "");
  if (storedCode.length() >= 8 && !replacedSetupCode) {
    snprintf(cfg_.apPassword, sizeof(cfg_.apPassword), "%s", storedCode.c_str());
  }

  // The link secret is under its own key for the same reason: it is agreed during
  // pairing and its size may change, and the settings blob must not change size
  // because of it. A secret of another size (the 8-byte typed-in key of 0.1.0)
  // is ignored, so those boards simply pair again.
  if (prefs_.getBytesLength("lsecret") == sizeof(linkSecret_)) {
    prefs_.getBytes("lsecret", linkSecret_, sizeof(linkSecret_));
  }

  // Persist a first-boot identity and a migrated blob while the session is still
  // open. It is closed right after: every later write needs a session of its
  // own, and an open one silently swallows them.
  if (cfg_.netId == 0 || cfg_.pairKey == 0) {
    regenerateNetwork();
    replacedStoredBlob = true;
  }
  if (replacedSetupCode) generateSetupCode();
  if (replacedStoredBlob || replacedSetupCode) {
    cfg_.schema = kSchema;
    if (prefs_.putBytes("cfg", &cfg_, sizeof(cfg_)) != sizeof(cfg_)) {
      Serial.println("[CFG] WARNING: could not persist the repaired settings");
    }
    prefs_.putString("apcode", String(cfg_.apPassword));
  }

  prefs_.end();

  // A blob from before the pins were settable carries zeroes there.
  if (!meterPinAllowed(cfg_.meterRxPin)) cfg_.meterRxPin = METER_UART_RX_PIN;
  if (!meterPinAllowed(cfg_.meterTxPin) || cfg_.meterTxPin == cfg_.meterRxPin) {
    cfg_.meterTxPin = METER_UART_TX_PIN;
  }

  // Repair anything implausible rather than trusting it. A misread blob has to
  // be harmless: an out-of-band frequency is a legal problem, not just a broken
  // link, so the radio settings are always checked against their limits here.
  if (cfg_.loraFreqMhz < LORA_FREQ_MIN_MHZ || cfg_.loraFreqMhz > LORA_FREQ_MAX_MHZ) {
    cfg_.loraFreqMhz = LORA_DEFAULT_FREQ_MHZ;
  }
  if (cfg_.loraSf < 7 || cfg_.loraSf > 12) cfg_.loraSf = LORA_DEFAULT_SF;
  if (cfg_.loraCr < 5 || cfg_.loraCr > 8) cfg_.loraCr = LORA_DEFAULT_CR;
  if (cfg_.loraPowerDbm < 2 || cfg_.loraPowerDbm > LORA_DEFAULT_POWER_DBM) {
    cfg_.loraPowerDbm = LORA_DEFAULT_POWER_DBM;
  }
  if (cfg_.sendIntervalMs < SEND_INTERVAL_MIN_MS || cfg_.sendIntervalMs > SEND_INTERVAL_MAX_MS) {
    cfg_.sendIntervalMs = SEND_INTERVAL_DEFAULT_MS;
  }
  if (cfg_.meterBaud < 300 || cfg_.meterBaud > 115200) cfg_.meterBaud = METER_SML_BAUD;
  if (cfg_.mqttPort == 0) cfg_.mqttPort = 1883;
  if (cfg_.haPort == 0) cfg_.haPort = WEB_PORT;

  // A garbled SSID would only fail the join; make it a clean "not configured".
  bool ssidSane = cfg_.staSsid[0] != '\0';
  for (size_t i = 0; ssidSane && i < sizeof(cfg_.staSsid) && cfg_.staSsid[i] != '\0'; ++i) {
    const char c = cfg_.staSsid[i];
    if (c < 0x20 || c > 0x7E) ssidSane = false;
  }
  if (!ssidSane) {
    cfg_.staSsid[0] = '\0';
    cfg_.staPass[0] = '\0';
    cfg_.staConfigured = false;
  }

  // A blob from before MQTT existed has no base topic; derive it.
  if (cfg_.mqttBase[0] == '\0') {
    snprintf(cfg_.mqttBase, sizeof(cfg_.mqttBase), "lilygo/%04X", shortId_);
  }
  if (cfg_.mqttPrefix[0] == '\0') {
    snprintf(cfg_.mqttPrefix, sizeof(cfg_.mqttPrefix), "homeassistant");
  }

  Serial.printf("[CFG] %s role=%d net=%04X code=%s\n", mac_, static_cast<int>(cfg_.role),
                cfg_.netId, cfg_.apPassword);
  return true;
}

// 8 characters, stable per board and short enough to type on a phone. The MAC
// alone will not do as the seed: it is printed on the label and advertised over
// mDNS, and this code is also the access point password. The per-board random
// networkSeed_ is what keeps the code from being a lookup.
void ConfigStore::generateSetupCode() {
  const uint32_t boardHash = hash32(reinterpret_cast<const uint8_t*>(mac_), strlen(mac_));
  const uint32_t mixed = boardHash ^ networkSeed_;
  uint32_t state = mixed ? mixed : 0x1234567u;
  for (size_t i = 0; i < 8; ++i) {
    state = state * 1664525u + 1013904223u;
    cfg_.apPassword[i] = kCodeAlphabet[(state >> 16) % (sizeof(kCodeAlphabet) - 1)];
  }
  cfg_.apPassword[8] = '\0';
  strncpy(cfg_.webPassword, cfg_.apPassword, sizeof(cfg_.webPassword) - 1);
}

void ConfigStore::deriveIdentity() {
  const uint64_t chip = ESP.getEfuseMac();
  uint8_t mac[6];
  for (int i = 0; i < 6; ++i) mac[i] = static_cast<uint8_t>((chip >> (8 * i)) & 0xFF);

  snprintf(mac_, sizeof(mac_), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
           mac[4], mac[5]);
  shortId_ = static_cast<uint16_t>((static_cast<uint16_t>(mac[4]) << 8) | mac[5]);

  snprintf(apSsid_, sizeof(apSsid_), "%s-%02X%02X", AP_SSID_PREFIX, mac[4], mac[5]);
}

void ConfigStore::applyDefaults() {
  memset(&cfg_, 0, sizeof(cfg_));
  cfg_.schema = kSchema;
  cfg_.role = DeviceRole::Unset;
  cfg_.deviceName[0] = '\0';
  cfg_.netId = 0;
  cfg_.pairKey = 0;
  cfg_.peerShortId = 0;

  cfg_.meterProtocol = MeterProtocol::Auto;
  cfg_.meterInvert = MeterInvert::Auto;
  cfg_.meterBaud = METER_SML_BAUD;
  cfg_.sendIntervalMs = SEND_INTERVAL_DEFAULT_MS;
  cfg_.meterRxPin = METER_UART_RX_PIN;
  cfg_.meterTxPin = METER_UART_TX_PIN;

  cfg_.loraFreqMhz = LORA_DEFAULT_FREQ_MHZ;
  cfg_.loraSf = LORA_DEFAULT_SF;
  cfg_.loraCr = LORA_DEFAULT_CR;
  cfg_.loraPowerDbm = LORA_DEFAULT_POWER_DBM;

  generateSetupCode();
  cfg_.keepApOn = false;
  cfg_.hideCodeOnDisplay = false;

  cfg_.staConfigured = false;
  cfg_.haEnabled = true;
  cfg_.haPort = WEB_PORT;

  // Home Assistant over MQTT: off until a broker is entered, because guessing
  // one would mean connection attempts against a stranger's host.
  cfg_.mqttEnabled = false;
  cfg_.mqttHost[0] = '\0';
  cfg_.mqttPort = 1883;
  cfg_.mqttUser[0] = '\0';
  cfg_.mqttPass[0] = '\0';
  snprintf(cfg_.mqttPrefix, sizeof(cfg_.mqttPrefix), "homeassistant");
  snprintf(cfg_.mqttBase, sizeof(cfg_.mqttBase), "lilygo/%04X", shortId_);
}

void ConfigStore::regenerateNetwork() {
  cfg_.netId = static_cast<uint16_t>(esp_random() & 0xFFFF);
  if (cfg_.netId == 0) cfg_.netId = 0x1A2B;
  cfg_.pairKey = esp_random();
  if (cfg_.pairKey == 0) cfg_.pairKey = 0x5A5A1234u;
}

bool ConfigStore::setApSsid(const String& ssid) {
  // WPA2 SSIDs are 1-32 bytes; keep to characters that survive a phone's WiFi
  // list without escaping surprises.
  String clean;
  for (size_t i = 0; i < ssid.length() && clean.length() < 32; ++i) {
    const char c = ssid[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == ' ' || c == '.') {
      clean += c;
    }
  }
  clean.trim();
  if (clean.length() < 4) return false;

  snprintf(apSsid_, sizeof(apSsid_), "%s", clean.c_str());
  storeApIdentity();
  return true;
}

bool ConfigStore::setSetupCode(const String& code) {
  String clean;
  for (size_t i = 0; i < code.length() && clean.length() < 15; ++i) {
    const char c = code[i];
    if (isalnum(static_cast<unsigned char>(c))) clean += c;
  }
  // WPA2 needs at least 8 characters; anything shorter would make the AP open.
  if (clean.length() < 8) return false;

  // While the dashboard password still *is* the code, it follows along - one
  // code for everything, as long as the owner has not split them apart.
  const bool sharedWithDashboard = strcmp(cfg_.webPassword, cfg_.apPassword) == 0;
  snprintf(cfg_.apPassword, sizeof(cfg_.apPassword), "%s", clean.c_str());
  if (sharedWithDashboard) {
    snprintf(cfg_.webPassword, sizeof(cfg_.webPassword), "%s", clean.c_str());
  }
  storeApIdentity();
  return true;
}

void ConfigStore::storeApIdentity() {
  if (!prefs_.begin(CFG_NAMESPACE, false)) return;
  prefs_.putString("apssid", String(apSsid_));
  prefs_.putString("apcode", String(cfg_.apPassword));
  prefs_.end();
}

void ConfigStore::storeLinkSecret() {
  if (!prefs_.begin(CFG_NAMESPACE, false)) return;
  prefs_.putBytes("lsecret", linkSecret_, sizeof(linkSecret_));
  prefs_.end();
}

bool ConfigStore::linkSecretSet() const {
  for (uint8_t byte : linkSecret_) {
    if (byte != 0) return true;
  }
  return false;
}

bool ConfigStore::linkSecret(uint8_t out[core::kLinkSecretSize]) const {
  memcpy(out, linkSecret_, sizeof(linkSecret_));
  return linkSecretSet();
}

bool ConfigStore::setLinkSecret(const uint8_t secret[core::kLinkSecretSize]) {
  if (secret == nullptr) return false;
  bool any = false;
  for (size_t i = 0; i < core::kLinkSecretSize; ++i) {
    linkSecret_[i] = secret[i];
    if (secret[i] != 0) any = true;
  }
  // All zero means "no secret", not "a key of zeroes": the rest of the firmware
  // decides whether the link is encrypted by asking linkSecretSet().
  if (!any) return false;
  storeLinkSecret();
  return true;
}

void ConfigStore::clearLinkSecret() {
  memset(linkSecret_, 0, sizeof(linkSecret_));
  storeLinkSecret();
}

bool ConfigStore::save() {
  if (!prefs_.begin(CFG_NAMESPACE, false)) return false;
  cfg_.schema = kSchema;
  const size_t written = prefs_.putBytes("cfg", &cfg_, sizeof(cfg_));
  prefs_.end();
  return written == sizeof(cfg_);
}

bool ConfigStore::reset() {
  if (prefs_.begin(CFG_NAMESPACE, false)) {
    prefs_.clear();
    prefs_.end();
  }
  // A factory reset must not keep the old seed, or the setup code - which is
  // also the access point password - comes back identical.
  networkSeed_ = esp_random();
  if (networkSeed_ == 0) networkSeed_ = 0x5EED0001u;
  applyDefaults();
  regenerateNetwork();
  generateSetupCode();

  const bool saved = save();
  if (prefs_.begin(CFG_NAMESPACE, false)) {
    prefs_.putUInt("netseed", networkSeed_);
    prefs_.putString("apcode", String(cfg_.apPassword));
    prefs_.end();
  }
  return saved;
}
