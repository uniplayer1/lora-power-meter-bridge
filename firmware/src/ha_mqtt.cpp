#include "ha_mqtt.h"

#include <PubSubClient.h>
#include <WiFi.h>

#include "firmware_version.h"
#include "json_util.h"
#include "time_util.h"
#include "web_server.h"

namespace {

WiFiClient networkClient;
PubSubClient client(networkClient);

// One retained state topic feeds every entity, so a discovery config only has
// to point at a JSON key. Short keys keep the payloads small - they are
// published on every meter update.
struct Entity {
  const char* component;    // sensor | binary_sensor
  const char* object;       // object id inside the discovery topic
  const char* name;         // what Home Assistant shows
  const char* valueTemplate;
  const char* unit;
  const char* deviceClass;
  const char* stateClass;
};

const Entity kEntities[] = {
    {"sensor", "import", "Stromzaehler Bezug", "{{ value_json.meter.import_kwh }}", "kWh", "energy",
     "total_increasing"},
    {"sensor", "export", "Stromzaehler Einspeisung", "{{ value_json.meter.export_kwh }}", "kWh",
     "energy", "total_increasing"},
    {"sensor", "power", "Stromzaehler Leistung", "{{ value_json.meter.power_w }}", "W", "power",
     "measurement"},
    {"sensor", "power_l1", "Stromzaehler Leistung L1", "{{ value_json.meter.l1_w }}", "W", "power",
     "measurement"},
    {"sensor", "power_l2", "Stromzaehler Leistung L2", "{{ value_json.meter.l2_w }}", "W", "power",
     "measurement"},
    {"sensor", "power_l3", "Stromzaehler Leistung L3", "{{ value_json.meter.l3_w }}", "W", "power",
     "measurement"},
    {"sensor", "meter_age", "Stromzaehler Alter der Messung", "{{ value_json.meter.age }}", "s",
     "duration", "measurement"},
    {"sensor", "telegrams", "Stromzaehler Telegramme", "{{ value_json.meter.telegrams }}", "", "",
     "total_increasing"},
    {"sensor", "crc_errors", "Stromzaehler Pruefsummenfehler", "{{ value_json.meter.crc_errors }}",
     "", "", "total_increasing"},
    {"sensor", "rssi", "Stromzaehler Link RSSI", "{{ value_json.link.rssi }}", "dBm",
     "signal_strength", "measurement"},
    {"sensor", "snr", "Stromzaehler Link SNR", "{{ value_json.link.snr }}", "dB", "", "measurement"},
    {"sensor", "link_age", "Stromzaehler Link Alter", "{{ value_json.link.age }}", "s", "duration",
     "measurement"},
    {"sensor", "duty", "Stromzaehler Sendezeit", "{{ value_json.link.duty_pct }}", "%", "",
     "measurement"},
    {"sensor", "uptime", "Stromzaehler Laufzeit", "{{ value_json.device.uptime }}", "s", "duration",
     "total_increasing"},
    {"sensor", "heap", "Stromzaehler freier Speicher", "{{ value_json.device.heap }}", "B", "",
     "measurement"},
    {"binary_sensor", "peer", "Stromzaehler Keller-Link", "{{ value_json.link.peer }}", "", "connectivity",
     ""},
    {"binary_sensor", "meter_valid", "Stromzaehler Messung gueltig", "{{ value_json.meter.valid }}",
     "", "connectivity", ""},
};

String nodeId() {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "lilygo_power_%04X", configStore.shortId());
  return String(buffer);
}

String topicFor(const DeviceConfig& cfg, const String& component, const String& object) {
  return String(cfg.mqttPrefix) + "/" + component + "/" + nodeId() + "/" + object + "/config";
}

String discoveryPayload(const DeviceConfig& cfg, const Entity& entity) {
  const String base = cfg.mqttBase;
  String payload = "{\"name\":\"";
  payload += entity.name;
  payload += "\",\"uniq_id\":\"" + nodeId() + "_" + entity.object + "\"";
  payload += ",\"stat_t\":\"" + base + "/state\"";
  payload += ",\"avty_t\":\"" + base + "/status\"";
  payload += ",\"val_tpl\":\"" + String(entity.valueTemplate) + "\"";
  if (entity.unit[0]) payload += ",\"unit_of_meas\":\"" + String(entity.unit) + "\"";
  if (entity.deviceClass[0]) payload += ",\"dev_cla\":\"" + String(entity.deviceClass) + "\"";
  if (entity.stateClass[0]) payload += ",\"stat_cla\":\"" + String(entity.stateClass) + "\"";
  if (strcmp(entity.component, "binary_sensor") == 0) payload += ",\"pl_on\":\"True\",\"pl_off\":\"False\"";
  payload += ",\"dev\":{";
  payload += "\"ids\":[\"" + nodeId() + "\"],";
  payload += "\"name\":\"" + escapeJson(cfg.deviceName[0] ? String(cfg.deviceName) : String("Power Meter")) + "\",";
  payload += "\"mf\":\"LilyGo\",\"mdl\":\"T3-S3 power meter link\",\"sw\":\"" FW_ID "\",";
  payload += "\"cu\":\"http://" + WiFi.localIP().toString() + "/\"";
  payload += "}}";
  return payload;
}

}  // namespace

void HaMqtt::begin(const DeviceConfig& config) {
  stats_.enabled = mqttConfigured(config);
  if (!stats_.enabled) return;
  // A meter update with the per-phase values is around 500 bytes; the default
  // 256-byte buffer would silently truncate the publish.
  client.setBufferSize(2048);
  client.setServer(config.mqttHost, config.mqttPort);
  stats_.lastConnectAttemptMs = 0;
  logEvent("mqtt: broker %s:%u, base %s", config.mqttHost, config.mqttPort, config.mqttBase);
}

void HaMqtt::applyConfig(const DeviceConfig& config) {
  stats_.enabled = mqttConfigured(config);
  stats_.announced = false;
  if (!stats_.enabled) {
    if (client.connected()) client.disconnect();
    logEvent("mqtt: disabled");
    return;
  }
  client.setBufferSize(2048);
  client.setServer(config.mqttHost, config.mqttPort);
  if (client.connected()) client.disconnect();
  stats_.lastConnectAttemptMs = 0;
  logEvent("mqtt: now %s:%u", config.mqttHost, config.mqttPort);
}

void HaMqtt::connectIfNeeded(uint32_t nowMs) {
  const DeviceConfig& cfg = configStore.get();
  if (!stats_.enabled || client.connected()) return;
  if (stats_.lastConnectAttemptMs != 0 && nowMs - stats_.lastConnectAttemptMs < MQTT_RETRY_MS) return;
  stats_.lastConnectAttemptMs = nowMs;
  ++stats_.connects;

  const String availability = String(cfg.mqttBase) + "/status";
  const String clientId = nodeId() + "_" + String(configStore.shortId(), HEX);

  const bool ok = (cfg.mqttUser[0] != '\0')
                      ? client.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPass, availability.c_str(),
                                       0, true, "offline")
                      : client.connect(clientId.c_str(), nullptr, nullptr, availability.c_str(), 0, true,
                                       "offline");

  stats_.lastError = client.state();
  if (!ok) {
    ++stats_.failures;
    return;
  }

  ++stats_.publishes;  // the birth message is a publish too
  client.publish(availability.c_str(), "online", true);
  stats_.connected = true;
  logEvent("mqtt: connected to %s", cfg.mqttHost);
  announce(nowMs);
}

bool HaMqtt::announce(uint32_t nowMs) {
  (void)nowMs;
  const DeviceConfig& cfg = configStore.get();
  if (!stats_.enabled || !client.connected()) return false;

  for (const Entity& entity : kEntities) {
    const String topic = topicFor(cfg, entity.component, entity.object);
    const String payload = discoveryPayload(cfg, entity);
    if (client.publish(topic.c_str(), payload.c_str(), true)) {
      ++stats_.publishes;
      snprintf(stats_.lastTopic, sizeof(stats_.lastTopic), "%s", topic.c_str());
    }
  }
  const String availability = String(cfg.mqttBase) + "/status";
  client.publish(availability.c_str(), "online", true);
  stats_.announced = true;
  logEvent("mqtt: %u entities announced to %s", static_cast<unsigned>(sizeof(kEntities) / sizeof(kEntities[0])),
           cfg.mqttPrefix);
  return true;
}

void HaMqtt::publishState(uint32_t nowMs, const MeterReader& meter, const RadioLink& link) {
  if (!stats_.enabled || !client.connected()) return;
  const DeviceConfig& cfg = configStore.get();
  const String topic = String(cfg.mqttBase) + "/state";
  const String payload = buildStateJson(link, meter, false, false);
  if (client.publish(topic.c_str(), payload.c_str(), true)) {
    ++stats_.publishes;
    stats_.lastPublishMs = nowMs;
  }
}

void HaMqtt::loop(uint32_t nowMs, const MeterReader& meter, const RadioLink& link) {
  if (!stats_.enabled) return;
  if (!client.connected()) {
    stats_.connected = false;
    stats_.announced = false;
    connectIfNeeded(nowMs);
    return;
  }
  client.loop();

  if (!stats_.announced) {
    announce(nowMs);
    return;
  }
  // Publish on change and at least every 30 s: the state topic is retained, so
  // a restart of Home Assistant still finds the latest reading.
  static uint32_t lastPublish = 0;
  if (nowMs - lastPublish >= 30000) {
    lastPublish = nowMs;
    publishState(nowMs, meter, link);
  }
}

String HaMqtt::describeDiscovery(const DeviceConfig& config, const MeterReader& meter,
                                 const RadioLink& link) const {
  String out;
  if (!mqttConfigured(config)) {
    out = "MQTT is not configured. Enter a broker host on this page and the device will\n"
          "announce itself to Home Assistant - no file to copy anywhere.\n";
    return out;
  }
  out = "broker: " + String(config.mqttHost) + ":" + String(config.mqttPort) + "\n";
  out += "state topic: " + String(config.mqttBase) + "/state (retained)\n";
  out += "availability: " + String(config.mqttBase) + "/status (LWT offline)\n";
  out += "discovery prefix: " + String(config.mqttPrefix) + "\n\n";
  for (const Entity& entity : kEntities) {
    out += topicFor(config, entity.component, entity.object) + "\n";
    out += "  " + discoveryPayload(config, entity) + "\n";
  }
  out += "\nstate payload (this is what the entities read):\n  ";
  out += buildStateJson(link, meter, false, false);
  out += "\n";
  return out;
}
