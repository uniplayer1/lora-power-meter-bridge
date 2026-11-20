#include "web_server.h"

#include <Update.h>
#include <WiFi.h>
#include <esp_system.h>

#include "config.h"
#include "display_ui.h"
#include "firmware_version.h"
#include "ha_mqtt.h"
#include "json_util.h"
#include "net.h"
#include "wall_clock.h"
#include "time_util.h"
#include "web_assets.h"

namespace {

const char* unitName(uint8_t unit) {
  switch (unit) {
    case core::kUnitWh: return "Wh";
    case core::kUnitVarh: return "varh";
    case core::kUnitVAh: return "VAh";
    case core::kUnitW: return "W";
    case core::kUnitA: return "A";
    case core::kUnitV: return "V";
    case core::kUnitHz: return "Hz";
    default: return "";
  }
}

String obisName(uint32_t obis) {
  char buf[16];
  switch (obis) {
    case core::kObisImportTotal: return "1.8.0";
    case core::kObisExportTotal: return "2.8.0";
    case core::kObisImportT1: return "1.8.1";
    case core::kObisImportT2: return "1.8.2";
    case core::kObisExportT1: return "2.8.1";
    case core::kObisExportT2: return "2.8.2";
    case core::kObisPowerTotal: return "1.7.0";
    case core::kObisPowerL1: return "36.7.0";
    case core::kObisPowerL2: return "56.7.0";
    case core::kObisPowerL3: return "76.7.0";
    case core::kObisVoltageL1: return "32.7.0";
    case core::kObisVoltageL2: return "52.7.0";
    case core::kObisVoltageL3: return "72.7.0";
    case core::kObisCurrentL1: return "31.7.0";
    case core::kObisCurrentL2: return "51.7.0";
    case core::kObisCurrentL3: return "71.7.0";
    default:
      snprintf(buf, sizeof(buf), "%u.%u.%u", (obis >> 16) & 0xFF, (obis >> 8) & 0xFF, obis & 0xFF);
      return String(buf);
  }
}

// A value with no reading yet must be JSON null, not 0: Home Assistant would
// otherwise chart a drop to zero every time the meter link hiccups.
String jsonOrNull(bool present, double value, int decimals) {
  if (!present) return "null";
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", decimals, value);
  return String(buf);
}

String zeroPad4(uint16_t v) {
  char buf[6];
  snprintf(buf, sizeof(buf), "%04u", static_cast<unsigned>(v));
  return String(buf);
}

}  // namespace

void WebUi::begin(RadioLink* link, MeterReader* meter) {
  link_ = link;
  meter_ = meter;
  // Reserved now, while the heap is still in one piece.
  stateJsonBuffer();

  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/setup", HTTP_GET, [this]() { handleSetupPage(); });
  server_.on("/login", HTTP_GET, [this]() { handleLoginPage(); });
  server_.on("/login", HTTP_POST, [this]() { handleLoginPost(); });
  server_.on("/logout", HTTP_POST, [this]() { handleLogout(); });
  server_.on("/style.css", HTTP_GET, [this]() { handleStyle(); });
  server_.on("/app.js", HTTP_GET, [this]() { handleScript(); });

  // The state endpoint is the integration surface, and it is served under both
  // names: /api/v1/state is what the docs, the REST package and the Home
  // Assistant integration use, /api/state is what the web UI calls. Only one of
  // them existed for a while, so the advertised URL was a 404.
  server_.on("/api/v1/state", HTTP_GET, [this]() { handleState(); });
  server_.on("/api/state", HTTP_GET, [this]() { handleState(); });
  server_.on("/api/scan", HTTP_GET, [this]() { handleScan(); });
  server_.on("/api/dump", HTTP_GET, [this]() { handleDump(); });
  server_.on("/api/setup", HTTP_POST, [this]() { handleSetupSave(); });
  server_.on("/api/save", HTTP_POST, [this]() { handleSave(); });
  server_.on("/api/action", HTTP_POST, [this]() { handleAction(); });
  server_.on("/ha/power_meter.yaml", HTTP_GET, [this]() { handleHaPackage(); });
  server_.on("/ha/card.yaml", HTTP_GET, [this]() { handleHaCard(); });
  server_.on("/api/mqtt", HTTP_GET, [this]() { handleMqttDump(); });

  server_.on("/ota", HTTP_POST, [this]() { handleOtaDone(); }, [this]() { handleOtaUpload(); });

  // Captive-portal probes: answer them with a redirect so the phone pops the
  // setup page instead of silently having no internet.
  for (const char* path : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                           "/ncsi.txt", "/connecttest.txt", "/redirect", "/success.txt", "/canonical.html"}) {
    server_.on(path, HTTP_GET, [this]() { handleCaptiveProbe(); });
  }

  // WebServer only stores request headers it was told to collect. Without this
  // registration header("Cookie") is always an empty string, so every
  // cookie-based session was silently rejected: the login succeeded, the
  // browser stored and sent the cookie, and the server ignored it - the user
  // was bounced back to the login form with no error at all.
  static const char* kCollectedHeaders[] = {"Cookie"};
  server_.collectHeaders(kCollectedHeaders, 1);

  server_.onNotFound([this]() { handleNotFound(); });
  server_.begin();
  logEvent("web UI on port %d", WEB_PORT);
}

void WebUi::loop() {
  server_.handleClient();
  if (netChangeAtMs_ != 0 && static_cast<int32_t>(millis() - netChangeAtMs_) >= 0) {
    netChangeAtMs_ = 0;
    netReconfigure();
    if (netChangeAp_) netApplyApSettings();
    netChangeAp_ = false;
  }
  if (sessionToken_.length() > 0 && millis() - sessionSinceMs_ > SESSION_TIMEOUT_MS) {
    sessionToken_ = "";
  }
}

void WebUi::requestRestart(uint32_t delayMs) {
  restartAtMs_ = millis() + delayMs;
  if (restartAtMs_ == 0) restartAtMs_ = 1;  // 0 means "none pending"
}

bool WebUi::isApClient() {
  if (!netApUp()) return false;
  const IPAddress remote = server_.client().remoteIP();
  const IPAddress ap = WiFi.softAPIP();
  return remote[0] == ap[0] && remote[1] == ap[1] && remote[2] == ap[2];
}

bool WebUi::authorized() {
  if (sessionToken_.length() == 0) return false;

  // Cookie first, and a ?t=<token> fallback: a browser that refuses to keep the
  // cookie would otherwise bounce every login straight back to the login page
  // with no explanation at all.
  const String cookie = server_.header("Cookie");
  if (cookie.indexOf("sid=" + sessionToken_) >= 0) {
    sessionSinceMs_ = millis();
    return true;
  }
  if (server_.hasArg("t") && server_.arg("t") == sessionToken_) {
    sessionSinceMs_ = millis();
    return true;
  }
  return false;
}

bool WebUi::mayConfigure() { return authorized() || isApClient(); }

void WebUi::requireAuthOrRedirect() {
  server_.sendHeader("Location", "/login", true);
  server_.send(302, "text/plain", "");
}

void WebUi::sendJson(int code, const String& body) {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(code, "application/json", body);
}

String WebUi::argOr(const char* name, const char* fallback) {
  return server_.hasArg(name) ? server_.arg(name) : String(fallback);
}

// ── pages ────────────────────────────────────────────────────

void WebUi::handleRoot() {
  if (!configStore.provisioned()) {
    server_.sendHeader("Location", "/setup", true);
    server_.send(302, "text/plain", "");
    return;
  }
  if (!authorized() && !isApClient()) {
    requireAuthOrRedirect();
    return;
  }
  servePage(kAppHtml);
}

void WebUi::handleSetupPage() {
  if (configStore.provisioned() && !authorized() && !isApClient()) {
    requireAuthOrRedirect();
    return;
  }
  // A configured device opens the setup form only when asked for it ("Rolle
  // ändern" links to /setup?edit=1). A reload of /setup, or the phone's
  // captive-portal check after reconnecting, lands on the dashboard instead of
  // a form that looks as if setup had not worked.
  if (configStore.provisioned() && !server_.hasArg("edit")) {
    server_.sendHeader("Location", "/", true);
    server_.send(302, "text/plain", "");
    return;
  }
  servePage(kSetupHtml);
}

void WebUi::handleLoginPage() {
  String page(kLoginHtml);
  page.replace("/style.css", String("/style.css?v=") + FW_ID);
  // The error slot is filled only when a login attempt actually failed; a
  // silent bounce back to the same form is indistinguishable from a broken page.
  if (server_.hasArg("e")) {
    // Point at the display only while the code is actually readable there: telling
      // somebody to read a screen that has it hidden is worse than saying nothing.
      const String hint = configStore.get().hideCodeOnDisplay
          ? String("Code stimmt nicht. Das Display zeigt ihn nicht an – er ist nur das "
                 "Passwort dieses WLANs und steht in deinen Notizen aus dem Setup.")
        : String("Code stimmt nicht. Der Code steht auf dem OLED (Seite „Zugang\u201c) und "
                 "lautet auch das WLAN-Passwort.");
    page.replace("<!--ERR-->", "<p class=\"bad\">" + hint + "</p>");
  } else {
    page.replace("<!--ERR-->", "");
  }
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html", page);
}

void WebUi::handleLoginPost() {
  String entered = argOr("pw", "");
  entered.trim();

  const DeviceConfig& cfg = configStore.get();
  // Codes are displayed in capitals and phone keyboards love to add a trailing
  // space or capitalise, so compare case-insensitively - an access code is not
  // a secret worth failing over letter case.
  const String pw = entered;
  auto matches = [&](const char* expected) {
    if (expected == nullptr || expected[0] == '\0') return false;
    return pw.equalsIgnoreCase(expected);
  };

  // A wrong guess costs a real pause once a few have failed in a row. The delay
  // is deliberately short enough that an owner mistyping twice does not notice,
  // and long enough that guessing an 8-character code is hopeless.
  if (failedLogins_ >= LOGIN_ATTEMPT_LIMIT &&
      millis() - lastFailedLoginMs_ < LOGIN_LOCKOUT_MS) {
    logEvent("web login throttled from %s", server_.client().remoteIP().toString().c_str());
    server_.sendHeader("Retry-After", String(LOGIN_LOCKOUT_MS / 1000UL));
    server_.send(429, "text/plain; charset=utf-8", "Zu viele Versuche. Bitte kurz warten.");
    return;
  }

  // Only the dashboard password is checked. By default it *is* the setup code
  // from the display; as soon as the owner sets an individual password, the
  // displayed code stops being a way in - otherwise "I changed the password"
  // would mean nothing.
  if (pw.length() > 0 && matches(cfg.webPassword)) {
    char token[24];
    snprintf(token, sizeof(token), "%08X%08X", esp_random(), esp_random());
    sessionToken_ = token;
    sessionSinceMs_ = millis();
    failedLogins_ = 0;
    server_.sendHeader("Set-Cookie", "sid=" + sessionToken_ + "; Path=/; Max-Age=86400; SameSite=Lax");
    // Also hand the token to the page explicitly, so access does not depend on
    // the browser accepting our cookie.
    server_.sendHeader("Location", "/?t=" + sessionToken_, true);
    server_.send(302, "text/plain", "");
    logEvent("web login ok from %s", server_.client().remoteIP().toString().c_str());
    return;
  }

  // Say what happened, on the page and in the log. The length is logged too:
  // it is what tells a typing mistake apart from a browser autofilling an old
  // password into the field.
  logEvent("web login FAILED from %s (entered %u chars)",
           server_.client().remoteIP().toString().c_str(), static_cast<unsigned>(pw.length()));
  // Throttle before answering: the code is short enough to walk through by
  // hand, and every guess used to cost nothing but a round trip.
    failedLogins_ = failedLogins_ >= 0xFF ? 0xFF : static_cast<uint8_t>(failedLogins_ + 1);
  lastFailedLoginMs_ = millis();
  server_.sendHeader("Location", "/login?e=1", true);
  server_.send(302, "text/plain", "");
}

void WebUi::handleLogout() {
  sessionToken_ = "";
  server_.sendHeader("Set-Cookie", "sid=; Path=/; Max-Age=0");
  server_.sendHeader("Location", "/login", true);
  server_.send(302, "text/plain", "");
}

// The stylesheet and the script are stamped with the firmware id, because a
// browser holding on to the previous build's CSS renders a layout that looks
// like a firmware bug - which is exactly how a stale stylesheet was diagnosed
// once already.
void WebUi::servePage(const char* html) {
  String page(html);
  page.replace("/style.css", String("/style.css?v=") + FW_ID);
  page.replace("/app.js", String("/app.js?v=") + FW_ID);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/html", page);
}

void WebUi::handleStyle() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "text/css", kStyleCss);
}

void WebUi::handleScript() {
  server_.sendHeader("Cache-Control", "no-store");
  server_.send_P(200, "application/javascript", kAppJs);
}

void WebUi::handleCaptiveProbe() {
  String target = "http://" + netIp() + "/setup";
  server_.sendHeader("Location", target, true);
  server_.send(302, "text/html", "<html><body>Weiter zur Einrichtung: <a href=\"" + target + "\">" + target + "</a></body></html>");
}

void WebUi::handleNotFound() {
  // Only while the device is genuinely unconfigured or has no network. A
  // provisioned device that is online must answer 404 like any other host, even
  // if it still offers its own access point.
  if (netInSetupMode() || !configStore.provisioned()) {
    handleCaptiveProbe();
    return;
  }
  server_.send(404, "text/plain", "not found");
}

// ── api ──────────────────────────────────────────────────────

String& stateJsonBuffer() {
  // Sized for the full document with a full event log (48 lines of up to 128
  // characters, escaped) plus the rest, with room to spare.
  static String buffer;
  static bool reserved = false;
  if (!reserved) {
    reserved = buffer.reserve(STATE_JSON_RESERVE);
    if (!reserved) logEvent("state buffer: could not reserve %u bytes", STATE_JSON_RESERVE);
  }
  return buffer;
}

bool buildStateJson(String& out, const RadioLink& linkRef, const MeterReader& meterRef, bool apClient,
                    bool canConfigure, bool includeLog) {
  const DeviceConfig& cfg = configStore.get();
  const RadioLink* link = &linkRef;
  const MeterReader* meter = &meterRef;
  const LinkStats& ls = link->stats();
  const MeterStats& ms = meter->stats();
  const core::MeterValues& values = meter->values();
  const uint32_t now = millis();

  String& j = out;
  j.remove(0);  // keeps the reserved capacity
  j += "{";

  j += "\"device\":{";
  j += "\"name\":\"" + escapeJson(cfg.deviceName[0] ? String(cfg.deviceName) : String("Power Meter")) + "\",";
  j += "\"role\":\"";
  j += cfg.role == DeviceRole::Meter ? "meter" : (cfg.role == DeviceRole::Gateway ? "gateway" : "unset");
  j += "\",\"fw\":\"" FW_ID "\",\"mac\":\"" + String(configStore.macString()) + "\",";
  j += "\"uptime\":" + String(now / 1000) + ",\"heap\":" + String(ESP.getFreeHeap());
  // The lowest free heap since boot and the largest block that can still be
  // allocated: a leak shows in the first, fragmentation in the second.
  j += ",\"heap_min\":" + String(ESP.getMinFreeHeap()) + ",\"heap_block\":" + String(ESP.getMaxAllocHeap());
  j += ",\"host\":\"" + netHostName() + ".local\"";
  // Wall clock: seconds since 1970 (null while unset), where it came from, and
  // how long ago it was last synchronised.
  {
    const uint64_t epochMs = clockEpochMs();
    j += ",\"time\":" + (epochMs ? String(static_cast<unsigned long>(epochMs / 1000ULL)) : String("null"));
    j += ",\"time_source\":\"" + String(clockSourceName()) + "\"";
    const uint32_t synced = clockSyncedAtMs();
    j += ",\"time_synced_s\":" + (epochMs && synced ? String(ageSeconds(now, synced)) : String("null"));
  }
  // Whether the code from the display is hidden there, and whether the owner set
  // a dashboard password of their own. Neither password itself is ever sent.
  j += ",\"hide_code\":" + String(cfg.hideCodeOnDisplay ? "true" : "false");
  j += ",\"web_password_custom\":" +
       String(strcmp(cfg.webPassword, cfg.apPassword) == 0 ? "false" : "true");
  j += ",\"can_configure\":" + String(canConfigure ? "true" : "false");
  j += "},";

  // The radio settings, so the form shows what is actually in use. Without them
  // the fields came up blank, which reads as "nothing set" and invites a save
  // that quietly overwrites a deliberate choice with the default.
  j += "\"radio\":{";
  j += "\"freq_mhz\":" + String(cfg.loraFreqMhz, 4) + ",";
  j += "\"freq_min\":" + String(LORA_FREQ_MIN_MHZ, 4) + ",";
  j += "\"freq_max\":" + String(LORA_FREQ_MAX_MHZ, 4) + ",";
  j += "\"sf\":" + String(cfg.loraSf) + ",";
  j += "\"cr\":" + String(cfg.loraCr) + ",";
  j += "\"power_dbm\":" + String(cfg.loraPowerDbm) + ",";
  j += "\"power_max\":" + String(LORA_DEFAULT_POWER_DBM) + ",";
  j += "\"interval_ms\":" + String(cfg.sendIntervalMs) + ",";
  // The scheduler uses the longer of the configured interval and this airtime
  // floor for the current spreading factor.
  j += "\"min_interval_ms\":" + String(link->minSendIntervalMs()) + ",";
  // What the modem is tuned to right now. Equal to the stored values unless a
  // re-tune failed - which is exactly the case the page has to be able to show.
  j += "\"active_freq_mhz\":" + String(link->frequencyMhz(), 4) + ",";
  j += "\"active_sf\":" + String(link->spreadingFactor()) + ",";
  j += "\"active_power_dbm\":" + String(link->outputPowerDbm());
  j += "},";

  j += "\"wifi\":{";
  j += "\"mode\":\"" + String(netModeName()) + "\",";
  j += "\"state\":\"" + String(netStateName()) + "\",";
  // The mode string cannot answer "am I online": a device that is connected and
  // still offers its own access point reports AP+STA, which the page read as "no
  // WLAN" and warned about while the connection was fine.
  j += "\"connected\":" + String(netStaUp() ? "true" : "false") + ",";
  j += "\"sta_ssid\":\"" + escapeJson(String(cfg.staSsid)) + "\",";
  j += "\"ip\":\"" + netIp() + "\",";
  j += "\"ap_ssid\":\"" + escapeJson(String(configStore.apSsid())) + "\",";
  j += "\"ap_up\":" + String(netApUp() ? "true" : "false") + ",";
  j += "\"ap_off_in_s\":" + String(netApOffInS()) + ",";
  j += "\"setup_mode\":" + String(netInSetupMode() ? "true" : "false") + ",";
  j += "\"portal\":" + String(netPortalActive() ? "true" : "false") + ",";
  j += "\"keep_ap\":" + String(cfg.keepApOn ? "true" : "false") + ",";
  // Only over the access point: there the code is not a secret, it is the
  // password of the network the client just joined.
  j += "\"setup_code\":\"" + escapeJson(String(apClient ? configStore.setupCode() : "")) + "\",";
  j += "\"rssi\":" + String(netStaRssi()) + ",";
  j += "\"clients\":" + String(netApClients()) + ",";
  j += "\"configured\":" + String(cfg.staConfigured ? "true" : "false");
  j += "},";

  j += "\"link\":{";
  j += "\"radio\":" + String(ls.radioOk ? "true" : "false") + ",";
  j += "\"peer\":" + String(ls.peerOnline ? "true" : "false") + ",";
  j += "\"paired\":" + String(link->paired() ? "true" : "false") + ",";
  j += "\"net\":\"" + String(cfg.netId, HEX) + "\",";
  j += "\"peer_id\":\"" + String(cfg.peerShortId, HEX) + "\",";
  j += "\"rssi\":" + String(ls.peerOnline ? ls.peerRssi : 0) + ",";
  j += "\"snr\":" + String(ls.peerSnr, 1) + ",";
  // null rather than the uptime when the peer has never been heard from: with a
  // plain number the page showed "last packet 3 minutes ago" on a device that
  // had never exchanged a packet with anyone.
  j += "\"age\":" + (ls.lastPeerSeenMs ? String(ageSeconds(now, ls.lastPeerSeenMs)) : String("null")) + ",";
  // Traffic from any device on the link, coupled or not.
  j += "\"traffic_age\":" +
       (ls.lastPacketSeenMs ? String(ageSeconds(now, ls.lastPacketSeenMs)) : String("null")) + ",";
  j += "\"traffic_src\":\"" + String(ls.lastPacketSrc, HEX) + "\",";
  // How long the peer may stay silent before the link counts as dropped. Shown
  // on the page so "still connected" has a visible expiry instead of being a
  // guess about how the radio feels.
  j += "\"peer_timeout_s\":" + String(PEER_TIMEOUT_MS / 1000UL) + ",";
  // Whether readings travel sealed, whether a secret exists at all, and what the
  // link has refused: two counters, because "noisy air" and "not written by our
  // peer" are different faults and look identical without them.
  j += "\"encrypted\":" + String(link->encryptionEnabled() ? "true" : "false") + ",";
  j += "\"secret_set\":" + String(configStore.linkSecretSet() ? "true" : "false") + ",";
  // Four digits from the agreed key, equal on both devices when the pairing
  // worked. The key itself never leaves the device.
  j += "\"check_code\":\"" + (ls.checkCodeValid ? zeroPad4(ls.checkCode) : String("")) + "\",";
  j += "\"auth_failures\":" + String(ls.rxAuthFailures) + ",";
  j += "\"plaintext_dropped\":" + String(ls.rxPlaintextDropped) + ",";
  j += "\"tx\":" + String(ls.txPackets) + ",\"rx\":" + String(ls.rxPackets) + ",";
  j += "\"tx_failures\":" + String(ls.txFailures) + ",\"corrupt\":" + String(ls.rxCorrupt) + ",";
  j += "\"acks\":" + String(ls.acksReceived) + ",\"missed\":" + String(ls.acksMissed) + ",";
  j += "\"blocked\":" + String(ls.txBlocked) + ",";
  j += "\"airtime_ms\":" + String(ls.airtimeMs) + ",";
  j += "\"budget_ms\":" + String(link->budget().budgetMs()) + ",";
  j += "\"duty_pct\":" + String(link->budget().usedPercent(now)) + ",";
  j += "\"interval_ms\":" + String(cfg.sendIntervalMs) + ",";
  // Pairing progress, so the button has something to say while it runs and can
  // report a timeout instead of leaving the owner guessing.
  j += "\"pair_state\":\"" + String(link->pairStateName()) + "\",";
  j += "\"pair_attempts\":" + String(ls.pairingAttempts) + ",";
  j += "\"pair_elapsed_s\":" +
       String(ls.pairState == PairState::Running ? ageSeconds(now, ls.pairingStartedMs) : 0) + ",";
  j += "\"pair_timeout_s\":" + String(link->pairingTimeoutMs() / 1000) + ",";
  j += "\"pair_result\":\"" + escapeJson(String(ls.pairingResult)) + "\"";
  j += "},";

  const core::Reading* import = values.find(core::kObisImportTotal);
  const core::Reading* exportR = values.find(core::kObisExportTotal);
  const core::Reading* power = values.find(core::kObisPowerTotal);
  const core::Reading* l1 = values.find(core::kObisPowerL1);
  const core::Reading* l2 = values.find(core::kObisPowerL2);
  const core::Reading* l3 = values.find(core::kObisPowerL3);

  j += "\"meter\":{";
  j += "\"protocol\":\"" + String(meter->activeProtocolName()) + "\",";
  j += "\"polarity\":\"";
  j += ms.invertedNow ? "invertiert" : "normal";
  j += "\",\"valid\":" + String(values.valid ? "true" : "false") + ",";
  j += "\"age\":" +
       (ms.lastTelegramAtMs ? String(ageSeconds(now, ms.lastTelegramAtMs)) : String("null")) + ",";
  j += "\"import_kwh\":" + jsonOrNull(import != nullptr, import ? import->wh() / 1000.0 : 0, 4) + ",";
  j += "\"export_kwh\":" + jsonOrNull(exportR != nullptr, exportR ? exportR->wh() / 1000.0 : 0, 4) + ",";
  j += "\"power_w\":" + jsonOrNull(power != nullptr, power ? power->watts() : 0, 1) + ",";
  j += "\"l1_w\":" + jsonOrNull(l1 != nullptr, l1 ? l1->watts() : 0, 1) + ",";
  j += "\"l2_w\":" + jsonOrNull(l2 != nullptr, l2 ? l2->watts() : 0, 1) + ",";
  j += "\"l3_w\":" + jsonOrNull(l3 != nullptr, l3 ? l3->watts() : 0, 1) + ",";
  j += "\"telegrams\":" + String(ms.telegrams) + ",";
  j += "\"crc_errors\":" + String(ms.crcErrors) + ",";
  j += "\"parse_errors\":" + String(ms.parseErrors) + ",";
  j += "\"overflow_errors\":" + String(ms.overflowErrors) + ",";
  j += "\"unframed_errors\":" + String(ms.unframedErrors) + ",";
  j += "\"d0_sessions\":" + String(ms.d0Sessions) + ",";
  // Raw byte count on the meter wire: zero means nothing is driving the pin at
  // all, which is a different fault from "bytes arrive but do not decode".
  j += "\"bytes\":" + String(ms.bytesSeen) + ",";
  j += "\"polarity_flips\":" + String(ms.inversionsTried) + ",";
  j += "\"simulating\":" + String(ms.simulating ? "true" : "false") + ",";
  j += "\"loopback\":" + String(ms.loopback ? "true" : "false") + ",";
  j += "\"config_protocol\":" + String(static_cast<int>(cfg.meterProtocol)) + ",";
  j += "\"config_polarity\":" + String(static_cast<int>(cfg.meterInvert)) + ",";
  j += "\"rx_pin\":" + String(meter->rxPin()) + ",";
  j += "\"tx_pin\":" + String(meter->txPin()) + ",";
  j += "\"values\":[";
  for (uint8_t i = 0; i < values.count; ++i) {
    if (i > 0) j += ",";
    char value[32];
    snprintf(value, sizeof(value), "%.4f", values.slots[i].value());
    j += "{\"obis\":\"" + obisName(values.slots[i].obis) + "\",\"value\":" + String(value) +
         ",\"unit\":\"" + String(unitName(values.slots[i].unit)) + "\"}";
  }
  j += "],\"dump\":\"";
  String dump;
  meter->dumpTelegram(dump);
  j += escapeJson(dump);
  j += "\"},";

  j += "\"ha\":{\"enabled\":" + String(cfg.haEnabled ? "true" : "false") + ",";
  j += "\"url\":\"http://" + netIp() + "/api/v1/state\"},";

  const MqttStats& mqtt = haMqtt.stats();
  j += "\"mqtt\":{";
  j += "\"enabled\":" + String(mqtt.enabled ? "true" : "false") + ",";
  j += "\"connected\":" + String(mqtt.connected ? "true" : "false") + ",";
  j += "\"announced\":" + String(mqtt.announced ? "true" : "false") + ",";
  j += "\"host\":\"" + escapeJson(String(cfg.mqttHost)) + "\",";
  j += "\"port\":" + String(cfg.mqttPort) + ",";
  j += "\"user\":\"" + escapeJson(String(cfg.mqttUser)) + "\",";
  j += "\"base\":\"" + escapeJson(String(cfg.mqttBase)) + "\",";
  j += "\"prefix\":\"" + escapeJson(String(cfg.mqttPrefix)) + "\",";
  j += "\"connects\":" + String(mqtt.connects) + ",";
  j += "\"failures\":" + String(mqtt.failures) + ",";
  j += "\"last_error\":" + String(mqtt.lastError) + "},";

  // The OLED page counter, so the web UI can drive it and report the raw BOOT
  // button level - that is what tells a stuck button apart from dead firmware.
  // The meter node's own status, as last reported over the link (gateway only;
  // null until a report arrived). Its airtime, memory and reception are not
  // the gateway's - Home Assistant shows them on a device of their own.
  if (link->hasPeerReport()) {
    const core::NodeStatus& r = link->peerReport();
    const uint32_t age = ageSeconds(now, link->peerReportAtMs());
    const float budgetDs = LORA_DUTY_BUDGET_MS / 100.0f;
    j += "\"peer_status\":{";
    j += "\"age\":" + String(age);
    j += ",\"duty_pct\":" + String(r.airtimeUsedDs * 100.0f / budgetDs, 1);
    j += ",\"airtime_s\":" + String(r.airtimeUsedDs / 10.0f, 1);
    j += ",\"blocked\":" + String(r.txBlocked);
    j += ",\"uptime\":" + String(link->peerReportUptimeS() + age);
    j += ",\"heap\":" + String(static_cast<uint32_t>(r.heapFreeKb) * 1024u);
    j += ",\"heap_min\":" + String(static_cast<uint32_t>(r.heapMinKb) * 1024u);
    j += ",\"rssi\":" + String(r.rssi);
    j += ",\"snr\":" + String(r.snrQuarterDb / 4.0f, 2);
    j += ",\"telegrams\":" + String(r.telegrams);
    j += ",\"telegram_errors\":" + String(r.telegramErrors);
    j += ",\"telegram_age\":" +
         (r.telegramAgeS == core::NodeStatus::kAgeNever ? String("null") : String(r.telegramAgeS + age));
    j += ",\"meter_protocol\":\"";
    j += (r.meterFlags & core::NodeStatus::kMeterSml) ? "sml" : ((r.meterFlags & core::NodeStatus::kMeterD0) ? "d0" : "");
    j += "\",\"meter_resolved\":" + String((r.meterFlags & core::NodeStatus::kMeterResolved) ? "true" : "false");
    j += ",\"inverted\":" + String((r.meterFlags & core::NodeStatus::kMeterInverted) ? "true" : "false");
    j += ",\"simulating\":" + String((r.meterFlags & core::NodeStatus::kMeterSimulating) ? "true" : "false");
    j += ",\"rx_pin\":" + String(r.rxPin);
    j += ",\"bytes\":" + String(r.bytesSeen);
    j += "},";
  } else {
    j += "\"peer_status\":null,";
  }

  j += "\"oled\":{\"page\":" + String(displayUi.page() + 1) + ",\"pages\":" +
       String(DisplayUi::pageCount()) + ",\"button\":" +
       String(displayUi.buttonDown() ? "true" : "false") + "},";

  j += "\"log\":[";
  if (includeLog) {
    for (size_t i = 0; i < eventLog.size(); ++i) {
      LogLine line;
      if (eventLog.at(i, line)) {
        if (i > 0) j += ',';
        j += '"';
        appendJsonEscaped(j, line.text);
        j += '"';
      }
    }
  }
  j += "]}";
  // A failed append leaves the String as it was, so a document that ran out of
  // memory half way does not end where a complete one does.
  return j.endsWith("]}");
}

void WebUi::handleState() {
  // Read-only and open on purpose: Home Assistant polls this, and neither its
  // REST sensors nor the custom integration have a way to log in. Nothing that
  // *changes* the device is reachable without a session, and the access code in
  // the payload is only filled in for clients on the device's own access point.
  String& j = stateJsonBuffer();
  if (!buildStateJson(j, *link_, *meter_, isApClient(), mayConfigure())) {
    static uint32_t lastWarnMs = 0;
    if (lastWarnMs == 0 || millis() - lastWarnMs > 60000) {
      lastWarnMs = millis();
      logEvent("state: out of memory (free %u, largest block %u)", ESP.getFreeHeap(),
               ESP.getMaxAllocHeap());
    }
    sendJson(503, "{\"ok\":false,\"message\":\"Speicher knapp - Gerät neu starten\"}");
    return;
  }
  sendJson(200, j);
}

void WebUi::handleScan() {
  const int found = WiFi.scanNetworks();
  String j = "[";
  int emitted = 0;
  for (int i = 0; i < found && emitted < WIFI_SCAN_MAX; ++i) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // hidden networks are useless to the owner
    if (emitted > 0) j += ",";
    j += "{\"ssid\":\"" + escapeJson(ssid) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    ++emitted;
  }
  j += "]";
  WiFi.scanDelete();
  sendJson(200, j);
}

void WebUi::handleDump() {
  String dump;
  if (meter_) meter_->dumpTelegram(dump);
  server_.send(200, "text/plain", dump);
}

void WebUi::handleSetupSave() {
  // The setup page is reachable while the device is unprovisioned, but a
  // provisioned device must not be re-rolled by anyone who can reach the port:
  // this writes the role, the WLAN credentials and the meter settings in one
  // go, which is exactly what the logged-in path protects.
  if (configStore.provisioned() && !mayConfigure()) {
    sendJson(401, "{\"ok\":false,\"message\":\"Nicht angemeldet\"}");
    return;
  }

  DeviceConfig& cfg = configStore.mutableGet();

  const int role = argOr("role", "0").toInt();
  if (role != 1 && role != 2) {
    sendJson(400, "{\"ok\":false,\"message\":\"Rolle fehlt\"}");
    return;
  }
  const DeviceRole roleBefore = cfg.role;
  cfg.role = static_cast<DeviceRole>(role);

  const String apBefore = String(configStore.apSsid()) + "\n" + configStore.setupCode();
  if (server_.hasArg("apname")) configStore.setApSsid(server_.arg("apname"));
  if (server_.hasArg("apcode")) configStore.setSetupCode(server_.arg("apcode"));
  const bool apChanged = apBefore != String(configStore.apSsid()) + "\n" + configStore.setupCode();
  const String staBefore = String(cfg.staSsid) + "\n" + cfg.staPass;

  const String name = argOr("name", "");
  if (name.length() > 0) {
    strncpy(cfg.deviceName, name.c_str(), sizeof(cfg.deviceName) - 1);
  } else {
    snprintf(cfg.deviceName, sizeof(cfg.deviceName), "%s",
             role == 1 ? "Keller" : "Wohnung");
  }

  const String ssid = argOr("ssid", "");
  const String pass = argOr("pass", "");
  if (ssid.length() > 0) {
    strncpy(cfg.staSsid, ssid.c_str(), sizeof(cfg.staSsid) - 1);
    strncpy(cfg.staPass, pass.c_str(), sizeof(cfg.staPass) - 1);
    cfg.staConfigured = true;
  }

  // Range-checked like every other enum that comes off the wire: an
  // out-of-range value here used to be cast and persisted, so the meter reader
  // came up in a state no code path expects.
  const int protocol = argOr("protocol", "0").toInt();
  if (protocol >= static_cast<int>(MeterProtocol::Auto) &&
      protocol <= static_cast<int>(MeterProtocol::D0)) {
    cfg.meterProtocol = static_cast<MeterProtocol>(protocol);
  }
  const int polarity = argOr("polarity", "0").toInt();
  if (polarity >= static_cast<int>(MeterInvert::Auto) &&
      polarity <= static_cast<int>(MeterInvert::Inverted)) {
    cfg.meterInvert = static_cast<MeterInvert>(polarity);
  }
  const uint32_t interval = argOr("interval", "10").toInt() * 1000UL;
  if (interval >= SEND_INTERVAL_MIN_MS && interval <= SEND_INTERVAL_MAX_MS) {
    cfg.sendIntervalMs = interval;
  }

  configStore.save();
  if (meter_) {
    meter_->setProtocol(cfg.meterProtocol);
    meter_->setPolarity(cfg.meterInvert);
    // The read-head pins were posted by the setup page and dropped here, so a
    // board wired to other pins came up listening to the wrong one.
    if (role == 1 && server_.hasArg("meterRx") && server_.hasArg("meterTx") &&
        server_.arg("meterRx").length() && server_.arg("meterTx").length()) {
      meter_->setPins(static_cast<uint8_t>(server_.arg("meterRx").toInt()),
                      static_cast<uint8_t>(server_.arg("meterTx").toInt()));
    }
  }
  // WiFi is switched only when something about it changed, and only after the
  // answer below is out. It tells the page where the device will be found next.
  const bool staChanged = staBefore != String(cfg.staSsid) + "\n" + cfg.staPass;
  const bool wantSta = cfg.staConfigured && cfg.staSsid[0] != '\0';
  const bool netChange = staChanged || apChanged || roleBefore != cfg.role || (wantSta && !netStaUp());
  if (netChange) {
    netChangeAtMs_ = millis() + 1500;
    if (netChangeAtMs_ == 0) netChangeAtMs_ = 1;
    netChangeAp_ = apChanged;
  }

  logEvent("provisioned as %s (net %04X)", role == 1 ? "meter node" : "gateway", cfg.netId);
  String j = "{\"ok\":true,\"message\":\"Gespeichert\",\"role\":" + String(role);
  j += ",\"net_change\":" + String(netChange ? "true" : "false");
  j += ",\"ap_changed\":" + String(apChanged ? "true" : "false");
  j += ",\"ap_ssid\":\"" + escapeJson(String(configStore.apSsid())) + "\"";
  j += ",\"ap_ip\":\"" + WiFi.softAPIP().toString() + "\"";
  j += ",\"sta_ssid\":\"" + escapeJson(String(cfg.staSsid)) + "\"";
  j += ",\"host\":\"" + netHostName() + ".local\"";
  j += "}";
  sendJson(200, j);
}

bool WebUi::applySaveField(const String& key, const String& value) {
  DeviceConfig& cfg = configStore.mutableGet();
  const bool on = (value == "1" || value == "on" || value == "true");

  // Returns false for a value that is refused, so the page can say so. Dropping
  // it silently is how "868.0" looked saved while the old frequency stayed.
  if (key == "freq") {
    const float freq = value.toFloat();
    if (freq < LORA_FREQ_MIN_MHZ || freq > LORA_FREQ_MAX_MHZ) return false;
    cfg.loraFreqMhz = freq;
  } else if (key == "sf") {
    const int sf = value.toInt();
    if (sf < 7 || sf > 12) return false;
    cfg.loraSf = static_cast<uint8_t>(sf);
  } else if (key == "power") {
    const int power = value.toInt();
    if (power < 2 || power > LORA_DEFAULT_POWER_DBM) return false;
    cfg.loraPowerDbm = static_cast<int8_t>(power);
  } else if (key == "ssid") {
    if (value.length() > 0) {
      strncpy(cfg.staSsid, value.c_str(), sizeof(cfg.staSsid) - 1);
      cfg.staConfigured = true;
    }
  } else if (key == "pass") {
    if (value.length() > 0) strncpy(cfg.staPass, value.c_str(), sizeof(cfg.staPass) - 1);
  } else if (key == "keepap") {
    cfg.keepApOn = on;
  } else if (key == "hidecode") {
    cfg.hideCodeOnDisplay = on;
  } else if (key == "newpass") {
    // Empty means "leave it"; anything shorter than 8 is refused, not dropped.
    if (value.length() == 0) return true;
    if (value.length() < 8) return false;
    snprintf(cfg.webPassword, sizeof(cfg.webPassword), "%s", value.c_str());
  } else if (key == "apcode") {
    if (value.length() == 0) return true;
    return configStore.setSetupCode(value);
  } else if (key == "apname") {
    if (value.length() == 0) return true;
    return configStore.setApSsid(value);
  } else if (key == "mqttHost") {
    if (value.length() > 0) {
      strncpy(cfg.mqttHost, value.c_str(), sizeof(cfg.mqttHost) - 1);
      cfg.mqttEnabled = true;
    }
  } else if (key == "mqttPort") {
    const int port = value.toInt();
    if (port > 0 && port < 65536) cfg.mqttPort = static_cast<uint16_t>(port);
  } else if (key == "mqttUser") {
    strncpy(cfg.mqttUser, value.c_str(), sizeof(cfg.mqttUser) - 1);
  } else if (key == "mqttPass") {
    if (value.length() > 0) strncpy(cfg.mqttPass, value.c_str(), sizeof(cfg.mqttPass) - 1);
  } else if (key == "mqttPrefix") {
    if (value.length() > 0) strncpy(cfg.mqttPrefix, value.c_str(), sizeof(cfg.mqttPrefix) - 1);
  } else if (key == "mqttOff") {
    cfg.mqttEnabled = false;
    cfg.mqttHost[0] = '\0';
  } else if (key == "devname") {
    if (value.length() > 0) strncpy(cfg.deviceName, value.c_str(), sizeof(cfg.deviceName) - 1);
  } else if (key == "interval") {
    const uint32_t interval = value.toInt() * 1000UL;
    if (interval < SEND_INTERVAL_MIN_MS || interval > SEND_INTERVAL_MAX_MS) return false;
    cfg.sendIntervalMs = interval;
  } else if (key == "protocol") {
    // Range-checked like the setup path: an unknown value cast to the enum puts
    // the reader in a state no code path expects.
    const int protocol = value.toInt();
    if (protocol < static_cast<int>(MeterProtocol::Auto) ||
        protocol > static_cast<int>(MeterProtocol::D0)) {
      return false;
    }
    cfg.meterProtocol = static_cast<MeterProtocol>(protocol);
    if (meter_) meter_->setProtocol(cfg.meterProtocol);
  } else if (key == "polarity") {
    const int polarity = value.toInt();
    if (polarity < static_cast<int>(MeterInvert::Auto) ||
        polarity > static_cast<int>(MeterInvert::Inverted)) {
      return false;
    }
    cfg.meterInvert = static_cast<MeterInvert>(polarity);
    if (meter_) meter_->setPolarity(cfg.meterInvert);
  }
  return true;
}

void WebUi::handleSave() {
  if (!mayConfigure()) {
    sendJson(401, "{\"ok\":false,\"message\":\"Nicht angemeldet\"}");
    return;
  }
  // Changes are detected by comparing before and after, not by which fields
  // were posted: the forms always send every field they show, and restarting
  // the WiFi because an unchanged SSID came along dropped the owner's phone off
  // the access point for nothing.
  const DeviceConfig before = configStore.get();
  const String apSsidBefore = configStore.apSsid();

  bool mqttChanged = false;
  String refused;
  for (int i = 0; i < server_.args(); ++i) {
    const String key = server_.argName(i);
    if (key == "t") continue;  // the session token rides along as an argument
    if (!applySaveField(key, server_.arg(i))) {
      if (refused.length()) refused += ", ";
      refused += key;
    }
    if (key.startsWith("mqtt")) mqttChanged = true;
  }

  const DeviceConfig& after = configStore.get();
  const bool radioChanged = before.loraFreqMhz != after.loraFreqMhz ||
                            before.loraSf != after.loraSf ||
                            before.loraPowerDbm != after.loraPowerDbm;
  const bool netChanged = strcmp(before.staSsid, after.staSsid) != 0 ||
                          strcmp(before.staPass, after.staPass) != 0 ||
                          before.keepApOn != after.keepApOn ||
                          before.staConfigured != after.staConfigured;
  const bool apChanged = apSsidBefore != configStore.apSsid() ||
                         strcmp(before.apPassword, after.apPassword) != 0;
  // The two UART pins belong together: setting them one at a time would hit the
  // "rx == tx" guard on the way through.
  // Only when they really change: moving the UART restarts protocol detection.
  if (server_.hasArg("meterRx") && server_.hasArg("meterTx") && meter_ &&
      (server_.arg("meterRx").toInt() != meter_->rxPin() ||
       server_.arg("meterTx").toInt() != meter_->txPin())) {
    if (!meter_->setPins(static_cast<uint8_t>(server_.arg("meterRx").toInt()),
                         static_cast<uint8_t>(server_.arg("meterTx").toInt()))) {
      configStore.save();  // the other fields of this form were applied and stay
      sendJson(400, "{\"ok\":false,\"message\":\"Pin nicht erlaubt oder RX = TX - siehe Hinweis\"}");
      return;
    }
  }

  configStore.save();
  if (mqttChanged) haMqtt.applyConfig(configStore.get());
  // Applied to the running modem at once. It used to be logged as "restarting
  // the link" and then only stored, so the page showed the new values as "in
  // operation" while the radio kept using the old ones until the next reboot.
  if (radioChanged && link_) link_->applySettings(after);
  // The access point cannot be renamed while it runs, so it is raised again -
  // the owner's phone will briefly drop off and then see the new name.
  if (netChanged) {
    netReconfigure();
  } else if (apChanged) {
    netApplyApSettings();
  }
  if (refused.length()) {
    // ok:false so the page shows it as an error; `refused` names the fields so
    // the page can say which ones in words instead of internal keys.
    sendJson(200, "{\"ok\":false,\"refused\":\"" + escapeJson(refused) +
                      "\",\"message\":\"Nicht uebernommen: " + escapeJson(refused) + "\"}");
    return;
  }
  sendJson(200, "{\"ok\":true,\"message\":\"Gespeichert\"}");
}

void WebUi::handleAction() {
  if (!mayConfigure()) {
    sendJson(401, "{\"ok\":false,\"message\":\"Nicht angemeldet\"}");
    return;
  }
  const String action = argOr("action", "");
  DeviceConfig& cfg = configStore.mutableGet();

  if (action == "pair") {
    if (link_) link_->startPairing(millis());
    sendJson(200, "{\"ok\":true,\"message\":\"Kopplung gestartet\"}");
  } else if (action == "unpair") {
    if (link_) link_->clearPairing();
    cfg.peerShortId = 0;
    configStore.save();
    sendJson(200, "{\"ok\":true,\"message\":\"Kopplung gelöst - zum neu Koppeln auf einem der beiden Geräte „Koppeln“ drücken\"}");
  } else if (action == "sim-on" || action == "sim-off") {
    if (meter_) meter_->setSimulation(action == "sim-on");
    const String msg = String("Simulator ") + (action == "sim-on" ? "an" : "aus");
    sendJson(200, "{\"ok\":true,\"message\":\"" + msg + "\"}");
  } else if (action == "loopback-on" || action == "loopback-off") {
    if (meter_) meter_->setLoopback(action == "loopback-on");
    const String msg = String("Loopback ") + (action == "loopback-on" ? "an" : "aus");
    sendJson(200, "{\"ok\":true,\"message\":\"" + msg + "\"}");
  } else if (action == "meter-scope") {
    if (!meter_) {
      sendJson(400, "{\"ok\":false,\"message\":\"kein Zaehler-Modul\"}");
    } else {
      // Three samples, because the test that matters is a human holding a TV
      // remote against the head: a single short window is easy to miss.
      String report;
      for (int i = 1; i <= 3; ++i) {
        report += "Messung ";
        report += String(i);
        report += "/3\n";
        report += meter_->runScope(1000);
        report += "\n";
      }
      sendJson(200, "{\"ok\":true,\"message\":\"" + escapeJson(report) + "\"}");
    }
  } else if (action == "mqtt-announce") {
    const bool ok = haMqtt.announce(millis());
    sendJson(200, ok ? "{\"ok\":true,\"message\":\"Discovery gesendet\"}"
                     : "{\"ok\":false,\"message\":\"Kein Broker verbunden - Host eintragen und speichern\"}");
  } else if (action == "page-next") {
    displayUi.nextPage();
    sendJson(200, "{\"ok\":true,\"message\":\"Display weiter\"}");
  } else if (action == "reboot") {
    sendJson(200, "{\"ok\":true,\"message\":\"Neustart\"}");
    requestRestart(800);
  } else if (action == "factory") {
    configStore.reset();
    if (meter_) {
      meter_->setSimulation(false);
      meter_->setLoopback(false);
    }
    sendJson(200, "{\"ok\":true,\"message\":\"Werkszustand – Neustart\"}");
    requestRestart(800);
  } else {
    sendJson(400, "{\"ok\":false,\"message\":\"Unbekannte Aktion\"}");
  }
}

void WebUi::handleOtaUpload() {
  HTTPUpload& upload = server_.upload();
  // The upload callback runs for every chunk before the request handler does,
  // so the guard has to be here as well: without it an unauthenticated body was
  // streamed straight into the inactive app slot.
  if (!mayConfigure()) {
    if (upload.status == UPLOAD_FILE_START) {
      logEvent("OTA refused from %s (not authorised)",
               server_.client().remoteIP().toString().c_str());
    }
    return;
  }
  if (upload.status == UPLOAD_FILE_START) {
    logEvent("OTA: %s", upload.filename.c_str());
    otaRejected_ = false;
    otaTooLarge_ = false;
    // HTTPUpload carries no content length in this core, so the request header
    // is the only place to learn the size before the bytes arrive. It is the
    // multipart body, a few hundred bytes above the image - close enough to
    // refuse an image that cannot fit, and never too small to catch one that can.
    otaExpectedBytes_ = static_cast<size_t>(server_.header("Content-Length").toInt());
    // A release ships two images: "-update.bin" (the app, for this page) and
    // "-factory.bin" (bootloader + partition table + app, for a USB flash at
    // 0x0). The factory image is the easy one to pick by mistake, and written
    // into an app slot it can never boot.
    String lower = upload.filename;
    lower.toLowerCase();
    if (lower.indexOf("factory") >= 0) {
      otaRejected_ = true;
      logEvent("OTA refused: '%s' is the USB factory image, use the -update.bin",
               upload.filename.c_str());
      return;
    }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      logEvent("OTA: begin failed (%s)", Update.errorString());
    }
  } else if (otaRejected_) {
    return;  // the rest of a refused upload is read and dropped
  } else if (otaTooLarge_) {
    return;  // the rest of an image that does not fit is read and dropped
    } else if (upload.status == UPLOAD_FILE_WRITE && otaExpectedBytes_ > 0 &&
             upload.totalSize + upload.currentSize > ESP.getFreeSketchSpace()) {
    // Caught while it streams: refusing at the end would already have filled the
    // inactive slot with bytes that can never boot.
    otaTooLarge_ = true;
    Update.abort();
    logEvent("OTA refused: upload exceeds the %u-byte slot",
             static_cast<unsigned>(ESP.getFreeSketchSpace()));
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      logEvent("OTA: write failed (%s)", Update.errorString());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    // An image that does not fit the inactive slot must never be booted. Update
    // checks the size too, but the failure has to reach the owner in words: a
    // botched update used to look like a settings reset, because what came up
    // afterwards was the empty configuration the half-written image had left.
    const size_t slot = ESP.getFreeSketchSpace();
    if (upload.totalSize > slot) {
      otaTooLarge_ = true;
      Update.abort();
      logEvent("OTA refused: %u bytes do not fit the %u-byte slot",
               static_cast<unsigned>(upload.totalSize), static_cast<unsigned>(slot));
      return;
    }
    if (Update.end(true)) {
      logEvent("OTA: %u bytes written, restarting", upload.totalSize);
    } else {
      logEvent("OTA: finish failed (%s)", Update.errorString());
    }
  }
}

void WebUi::handleOtaDone() {
  if (!mayConfigure()) {
    sendJson(401, "{\"ok\":false,\"message\":\"Nicht angemeldet\"}");
    return;
  }
  if (otaTooLarge_) {
    otaTooLarge_ = false;
    server_.send(400, "text/html",
                 "<meta charset='utf-8'><meta http-equiv='refresh' content='15;url=/#system'>"
                 "<body style='background:#070b18;color:#f87171;font-family:sans-serif;padding:40px'>"
                 "<h2>Zu gross</h2><p>Die Datei passt nicht in den freien Speicher f&uuml;r das Update. "
                 "Es wurde nichts geschrieben &ndash; die laufende Firmware und alle Einstellungen "
                 "bleiben unver&auml;ndert.</p></body>");
    return;
  }
  if (otaRejected_) {
    otaRejected_ = false;
    server_.send(400, "text/html",
                 "<meta charset='utf-8'><meta http-equiv='refresh' content='15;url=/#system'>"
                 "<body style='background:#070b18;color:#f87171;font-family:sans-serif;padding:40px'>"
                 "<h2>Falsche Datei</h2><p>Das ist das Werks-Image f&uuml;r USB. F&uuml;r das Update "
                 "hier bitte die Datei <b>&hellip;-update.bin</b> verwenden.</p></body>");
    return;
  }
  const bool ok = !Update.hasError() && Update.isFinished();
  server_.send(200, "text/html",
               ok ? "<meta http-equiv='refresh' content='12;url=/'>"
                    "<body style='background:#070b18;color:#e9edfb;font-family:sans-serif;padding:40px'>"
                    "<h2>Update erfolgreich</h2><p>Das Ger&auml;t startet neu &hellip;</p></body>"
                  : "<meta http-equiv='refresh' content='12;url=/'>"
                    "<body style='background:#070b18;color:#f87171;font-family:sans-serif;padding:40px'>"
                    "<h2>Update fehlgeschlagen</h2><p>Alte Firmware l&auml;uft weiter.</p></body>");
  if (ok) requestRestart(1200);
}

// The device hands the owner the exact YAML for its own endpoint and entity
// ids, so nothing has to be typed by hand.
void WebUi::handleHaPackage() {
  server_.sendHeader("Content-Disposition", "attachment; filename=\"power_meter.yaml\"");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/yaml", buildHaPackage());
}

// Prints the topics and payloads the device would publish. Without a broker
// this is the only way to see what Home Assistant would be told.
void WebUi::handleMqttDump() {
  String dump = haMqtt.describeDiscovery(configStore.get(), *meter_, *link_);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/plain", dump);
}

void WebUi::handleHaCard() {
  server_.sendHeader("Content-Disposition", "attachment; filename=\"power_meter_card.yaml\"");
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(200, "text/yaml", buildHaCard());
}

// Availability of the REST sensors: readings while the meter reading is valid,
// the Keller's report values once a report has arrived, the Wohnung's own
// values always. File scope, because a lambda's default argument below cannot
// name a local.
namespace {
const char* const kWhenMeterValid = "{{ value_json.meter.valid }}";
const char* const kWhenReported = "{{ value_json.peer_status is not none }}";
const char* const kAlways = "";
}  // namespace

String WebUi::buildHaPackage() const {
  const DeviceConfig& cfg = configStore.get();
  const String base = "http://" + netIp();
  const String id = String(configStore.shortId(), HEX);
  const char* name = cfg.deviceName[0] ? cfg.deviceName : "Power Meter";

  String y = String(
      "# Stromzaehler -> Home Assistant (REST)\n"
      "#\n"
      "# 1) Diese Datei nach <ha-config>/packages/power_meter.yaml kopieren\n"
      "# 2) In configuration.yaml ergaenzen:\n"
      "#      homeassistant:\n"
      "#        packages: !include_dir_named packages\n"
      "# 3) Home Assistant neu starten - die Entitaeten erscheinen automatisch\n"
      "#\n"
      "# Erzeugt vom Geraet selbst, Endpunkt und IDs sind bereits eingesetzt.\n\n"
      "rest:\n"
      "  - resource: ") + base + "/api/v1/state\n" +
      "    scan_interval: 10\n"
      "    timeout: 5\n"
      "    sensor:\n";

  auto addSensor = [&](const char* object, const char* friendly, const char* template_,
                       const char* unit, const char* deviceClass, const char* stateClass,
                       const char* availability = kWhenMeterValid) {
    y += "      - name: \"";
    y += friendly;
    y += "\"\n        unique_id: lilygo_";
    y += id;
    y += "_";
    y += object;
    y += "\n        value_template: \"";
    y += template_;
    y += "\"\n";
    if (unit && *unit) {
      y += "        unit_of_measurement: \"";
      y += unit;
      y += "\"\n";
    }
    if (deviceClass && *deviceClass) {
      y += "        device_class: ";
      y += deviceClass;
      y += "\n";
    }
    if (stateClass && *stateClass) {
      y += "        state_class: ";
      y += stateClass;
      y += "\n";
    }
    if (availability && *availability) {
      y += "        availability: \"";
      y += availability;
      y += "\"\n";
    }
  };

  addSensor("import", "Stromzähler Bezug", "{{ value_json.meter.import_kwh }}", "kWh", "energy",
            "total_increasing");
  addSensor("export", "Stromzähler Einspeisung", "{{ value_json.meter.export_kwh }}", "kWh",
            "energy", "total_increasing");
  addSensor("power", "Stromzähler Leistung", "{{ value_json.meter.power_w }}", "W", "power",
            "measurement");
  addSensor("power_l1", "Stromzähler Leistung L1", "{{ value_json.meter.l1_w }}", "W", "power",
            "measurement");
  addSensor("power_l2", "Stromzähler Leistung L2", "{{ value_json.meter.l2_w }}", "W", "power",
            "measurement");
  addSensor("power_l3", "Stromzähler Leistung L3", "{{ value_json.meter.l3_w }}", "W", "power",
            "measurement");
  addSensor("age", "Stromzähler Alter der Messung", "{{ value_json.meter.age }}", "s", "duration",
            "measurement");
  // Framing trouble has its own counter: a telegram that never completes is a
  // different fault from one that arrives and does not decode.
  addSensor("overflow_errors", "Stromzähler Telegramme zu lang", "{{ value_json.meter.overflow_errors }}",
            "", "", "total_increasing");
  addSensor("unframed_errors", "Stromzähler Rahmenfehler", "{{ value_json.meter.unframed_errors }}",
            "", "", "total_increasing");
  addSensor("telegrams", "Stromzähler Telegramme", "{{ value_json.meter.telegrams }}", "", "",
            "total_increasing");
  addSensor("crc", "Stromzähler Prüfsummenfehler", "{{ value_json.meter.crc_errors }}", "", "",
            "total_increasing");

  // The Keller board, from its own status report every few minutes. Named by
  // board: the airtime, memory and reception of the two boards are separate
  // values, and one "Stromzähler Sendezeit" was the gateway's while it read
  // like the meter's.
  addSensor("node_duty", "Keller Sendezeit-Budget genutzt", "{{ value_json.peer_status.duty_pct }}", "%", "",
            "measurement", kWhenReported);
  addSensor("node_blocked", "Keller zurückgehaltene Sendungen", "{{ value_json.peer_status.blocked }}", "", "",
            "measurement", kWhenReported);
  addSensor("node_rssi", "Keller Empfang vom Gateway RSSI", "{{ value_json.peer_status.rssi }}", "dBm",
            "signal_strength", "measurement", kWhenReported);
  addSensor("node_snr", "Keller Empfang vom Gateway SNR", "{{ value_json.peer_status.snr }}", "dB", "",
            "measurement", kWhenReported);
  addSensor("node_heap", "Keller freier Speicher", "{{ value_json.peer_status.heap }}", "B", "data_size",
            "measurement", kWhenReported);
  addSensor("node_heap_min", "Keller Speicher-Tiefstwert", "{{ value_json.peer_status.heap_min }}", "B",
            "data_size", "measurement", kWhenReported);
  addSensor("node_uptime", "Keller Laufzeit", "{{ value_json.peer_status.uptime }}", "s", "duration",
            "total_increasing", kWhenReported);
  addSensor("node_bytes", "Keller Bytes vom Lesekopf", "{{ value_json.peer_status.bytes }}", "", "",
            "total_increasing", kWhenReported);
  addSensor("node_report_age", "Keller letzter Statusbericht", "{{ value_json.peer_status.age }}", "s",
            "duration", "measurement", kWhenReported);

  // The Wohnung board (this gateway).
  addSensor("duty", "Wohnung Sendezeit-Budget genutzt", "{{ value_json.link.duty_pct }}", "%", "",
            "measurement", kAlways);
  addSensor("rssi", "Wohnung Empfang vom Keller RSSI", "{{ value_json.link.rssi }}", "dBm", "signal_strength",
            "measurement", kAlways);
  addSensor("snr", "Wohnung Empfang vom Keller SNR", "{{ value_json.link.snr }}", "dB", "", "measurement",
            kAlways);
  addSensor("heap", "Wohnung freier Speicher", "{{ value_json.device.heap }}", "B", "data_size", "measurement",
            kAlways);
  addSensor("heap_block", "Wohnung größter freier Block", "{{ value_json.device.heap_block }}", "B",
            "data_size", "measurement", kAlways);
  addSensor("uptime", "Wohnung Laufzeit", "{{ value_json.device.uptime }}", "s", "duration",
            "total_increasing", kAlways);

  y += "\n    binary_sensor:\n";
  y += "      - name: \"Wohnung Funkverbindung zum Keller\"\n";
  y += "        unique_id: lilygo_" + id + "_peer\n";
  y += "        value_template: \"{{ value_json.link.peer }}\"\n";
  y += "        device_class: connectivity\n";
  y += "      - name: \"Stromzähler Messung gültig\"\n";
  y += "        unique_id: lilygo_" + id + "_valid\n";
  y += "        value_template: \"{{ value_json.meter.valid }}\"\n";
  y += "        device_class: connectivity\n";
  y += "\n# Geraetename laut Geraet: " + String(name) + "\n";
  return y;
}

String WebUi::buildHaCard() const {
  // Entity ids as Home Assistant derives them from the names in the YAML
  // package (umlauts become plain letters: "Stromzähler" -> "stromzahler").
  // Entities created before a rename keep their old ids - on an older
  // installation they may read stromzaehler_..., and the card needs adjusting.
  return String(
      "# Dashboard-Karte fuer Home Assistant (Lovelace, YAML-Modus)\n"
      "# Einfach als neue Karte im Raw-Konfigurationseditor einfuegen.\n"
      "# Wurden die Entitaeten vor einer Umbenennung angelegt, behalten sie ihre\n"
      "# alten IDs (z. B. sensor.stromzaehler_...): dann die IDs hier anpassen.\n"
      "type: vertical-stack\n"
      "cards:\n"
      "  - type: gauge\n"
      "    entity: sensor.stromzahler_leistung\n"
      "    name: Leistung\n"
      "    min: 0\n"
      "    max: 4000\n"
      "    needle: true\n"
      "  - type: glance\n"
      "    entities:\n"
      "      - entity: sensor.stromzahler_bezug\n"
      "        name: Bezug\n"
      "      - entity: sensor.stromzahler_einspeisung\n"
      "        name: Einspeisung\n"
      "      - entity: binary_sensor.wohnung_funkverbindung_zum_keller\n"
      "        name: Funk\n"
      "  - type: entities\n"
      "    title: Keller (Zaehler)\n"
      "    entities:\n"
      "      - entity: sensor.stromzahler_alter_der_messung\n"
      "        name: Alter der Messung\n"
      "      - entity: sensor.stromzahler_prufsummenfehler\n"
      "        name: Pruefsummenfehler\n"
      "      - entity: sensor.keller_sendezeit_budget_genutzt\n"
      "        name: Sendezeit-Budget genutzt\n"
      "      - entity: sensor.keller_empfang_vom_gateway_rssi\n"
      "        name: Empfang vom Gateway\n"
      "      - entity: sensor.keller_freier_speicher\n"
      "        name: Freier Speicher\n"
      "  - type: entities\n"
      "    title: Wohnung (Gateway)\n"
      "    entities:\n"
      "      - entity: sensor.wohnung_sendezeit_budget_genutzt\n"
      "        name: Sendezeit-Budget genutzt\n"
      "      - entity: sensor.wohnung_empfang_vom_keller_rssi\n"
      "        name: Empfang vom Keller\n"
      "      - entity: sensor.wohnung_freier_speicher\n"
      "        name: Freier Speicher\n");
}
