#include "net.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"
#include "firmware_version.h"

namespace {

DNSServer dnsServer;
bool dnsRunning = false;
bool apUp = false;
bool portal = false;
bool staAttempted = false;
uint32_t staStartedAtMs = 0;
uint32_t lastStaAttemptMs = 0;
uint32_t staUpSinceMs = 0;  // when the current station connection started, 0 while down
uint32_t lastDnsTickMs = 0;

const byte kDnsPort = 53;

void startAccessPoint() {
  if (apUp) return;
  WiFi.softAP(configStore.apSsid(), configStore.setupCode(), AP_CHANNEL, 0, AP_MAX_CLIENTS);
  apUp = true;
  logEvent("AP '%s' up on %s", configStore.apSsid(), WiFi.softAPIP().toString().c_str());
}

void startPortal() {
  if (dnsRunning) return;
  dnsServer.start(kDnsPort, "*", WiFi.softAPIP());
  dnsRunning = true;
  portal = true;
}

void stopPortal() {
  if (!dnsRunning) return;
  dnsServer.stop();
  dnsRunning = false;
  portal = false;
}

void startMdns() {
  const String host = netHostName();
  if (!MDNS.begin(host.c_str())) return;

  MDNS.addService("http", "tcp", WEB_PORT);

  // Advertise a service of our own so Home Assistant can find the device
  // instead of being told its address. Home Assistant matches advertised
  // services against its integrations, so this only becomes a "discovered"
  // card once the matching integration is installed - but the announcement
  // itself costs nothing and is the part that belongs on the device.
  const DeviceConfig& cfg = configStore.get();
  MDNS.setInstanceName(cfg.deviceName[0] ? cfg.deviceName : "Power Meter");
  MDNS.addService("lilygopower", "tcp", WEB_PORT);
  MDNS.addServiceTxt("lilygopower", "tcp", "id", String(configStore.shortId(), HEX).c_str());
  MDNS.addServiceTxt("lilygopower", "tcp", "mac", configStore.macString());
  MDNS.addServiceTxt("lilygopower", "tcp", "fw", FW_ID);
  MDNS.addServiceTxt("lilygopower", "tcp", "role",
                     cfg.role == DeviceRole::Meter ? "meter"
                                                   : (cfg.role == DeviceRole::Gateway ? "gateway" : "unset"));
  MDNS.addServiceTxt("lilygopower", "tcp", "path", "/api/v1/state");
  MDNS.addServiceTxt("lilygopower", "tcp", "model", "T3-S3 power meter link");

  logEvent("mDNS: http://%s.local and _lilygopower._tcp announced", host.c_str());
}

}  // namespace

// Defined at the bottom of this file; the captive-portal redirect runs from the
// main loop.
void handleDns();

String netHostName() {
  char host[32];
  snprintf(host, sizeof(host), "lilygo-power-%04X", configStore.shortId());
  return String(host);
}

void netBegin() {
  const DeviceConfig& cfg = configStore.get();
  WiFi.persistent(false);
  WiFi.setHostname(netHostName().c_str());

  const bool wantSta = cfg.staConfigured && cfg.staSsid[0] != '\0';
  // The node is a fixed installation: its AP is the on-site status display.
  const bool alwaysAp = cfg.role == DeviceRole::Meter || cfg.keepApOn || !wantSta;

  if (wantSta) {
    WiFi.mode(alwaysAp ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(cfg.staSsid, cfg.staPass);
    staAttempted = true;
    staStartedAtMs = millis();
    lastStaAttemptMs = staStartedAtMs;
    logEvent("joining '%s' ...", cfg.staSsid);
  } else {
    WiFi.mode(WIFI_AP);
  }

  if (alwaysAp) {
    startAccessPoint();
    if (!configStore.provisioned() || !wantSta) startPortal();
  }
  startMdns();
}

int32_t netApOffInS() {
  const DeviceConfig& cfg = configStore.get();
  const bool wantSta = cfg.staConfigured && cfg.staSsid[0] != '\0';
  if (!apUp || !wantSta || cfg.keepApOn || cfg.role != DeviceRole::Gateway) return -1;
  if (WiFi.status() != WL_CONNECTED || staUpSinceMs == 0) return -1;
  const uint32_t up = millis() - staUpSinceMs;
  return up >= STA_STABLE_MS ? 0 : static_cast<int32_t>((STA_STABLE_MS - up) / 1000UL);
}

void netStopAccessPoint() {
  stopPortal();
  if (apUp) {
    WiFi.softAPdisconnect(true);
    apUp = false;
    logEvent("AP down");
  }
  if (WiFi.getMode() == WIFI_AP_STA) WiFi.mode(WIFI_STA);
}

void netLoop() {
  handleDns();

  const DeviceConfig& cfg = configStore.get();
  const bool wantSta = cfg.staConfigured && cfg.staSsid[0] != '\0';
  const bool staNow = WiFi.status() == WL_CONNECTED;
  if (staNow && staUpSinceMs == 0) staUpSinceMs = millis();
  // While connected, the "could not join" timer is held at zero, so it measures
  // how long a connection has been LOST. It used to count from the first join
  // attempt: on a board up for more than 20 s, any momentary drop - including
  // the one when the access point is switched off - brought the access point
  // straight back, and the two-minute stable timer started over, forever.
  if (staNow) staStartedAtMs = millis();

  if (wantSta && !staNow) {
    const uint32_t now = millis();
    const bool timedOut = staAttempted && (now - staStartedAtMs > STA_CONNECT_TIMEOUT_MS);
    staUpSinceMs = 0;

    if (timedOut && !apUp) {
      // Could not join: bring the access point back rather than being unreachable.
      logEvent("WiFi join failed - falling back to the access point");
      // Keep the station side alive so the retry below can still succeed.
      WiFi.mode(WIFI_AP_STA);
      startAccessPoint();
      // The captive portal belongs to a device that is not configured yet. A
      // device that IS configured and merely cannot reach its network must serve
      // its dashboard on the access point it just brought up, not the setup page
      // - redirecting there on every dropped join is what made a working device
      // look like it kept falling back into configuration mode.
      if (!configStore.provisioned()) startPortal();
    }
    if (now - lastStaAttemptMs > STA_FAIL_BACKOFF_MS) {
      lastStaAttemptMs = now;
      staStartedAtMs = now;
      staAttempted = true;
      logEvent("retrying WiFi join ...");
      WiFi.begin(cfg.staSsid, cfg.staPass);
    }
    return;
  }

  if (wantSta && staNow && apUp && !cfg.keepApOn && cfg.role == DeviceRole::Gateway &&
      millis() - staUpSinceMs > STA_STABLE_MS) {
    // Provisioned and reliably online: the flat's page lives on the LAN now. The
    // wait matters - on a link that comes and goes, giving the AP up eagerly
    // dropped the very connection the owner was using, once per join.
    netStopAccessPoint();
  }

  // Provisioned and online: the captive portal has done its job. The access
  // point may well stay up (meter role, or the owner asked for it) - but a
  // client on it must land on the dashboard, not on the setup page. Leaving the
  // portal flag set is what made a connected device look like it was still
  // being configured: every unknown URL was redirected to /setup.
  if (wantSta && staNow && configStore.provisioned() && netPortalActive()) {
    stopPortal();
    logEvent("setup portal closed - the device is online");
  }
}

void netReconfigure() {
  const DeviceConfig& cfg = configStore.get();
  const bool wantSta = cfg.staConfigured && cfg.staSsid[0] != '\0';
  const bool apWanted = cfg.role == DeviceRole::Meter || cfg.keepApOn || !wantSta;
  // An access point that is up stays up while the station joins. The owner is
  // usually connected to it right now (first setup), and the setup page polls
  // it to learn the address the device gets in the home network. Tearing it
  // down here cut that connection mid-request, so the page never heard back
  // and stayed on the setup form. netLoop() takes a gateway's AP down once the
  // station connection has been stable for a while.
  const bool keepAp = apUp;

  WiFi.disconnect(false, false);  // the station side only
  staUpSinceMs = 0;

  if (wantSta) {
    WiFi.mode(apWanted || keepAp ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(cfg.staSsid, cfg.staPass);
    staAttempted = true;
    staStartedAtMs = millis();
    lastStaAttemptMs = staStartedAtMs;
  } else {
    WiFi.mode(WIFI_AP);
  }

  if (apWanted || keepAp) {
    startAccessPoint();  // no-op while it is already up
    if (!configStore.provisioned() || !wantSta) {
      startPortal();
    } else {
      stopPortal();
    }
  } else {
    stopPortal();
    apUp = false;
  }
}

void netApplyApSettings() {
  const DeviceConfig& cfg = configStore.get();
  const bool wantSta = cfg.staConfigured && cfg.staSsid[0] != '\0';
  const bool alwaysAp = cfg.role == DeviceRole::Meter || cfg.keepApOn || !wantSta;

  stopPortal();
  if (apUp) {
    WiFi.softAPdisconnect(true);
    apUp = false;
    delay(150);
  }
  if (!alwaysAp) return;

  WiFi.mode(wantSta ? WIFI_AP_STA : WIFI_AP);
  startAccessPoint();
  if (!configStore.provisioned() || !wantSta) startPortal();
  logEvent("AP settings applied: '%s'", configStore.apSsid());
}

bool netStaUp() { return WiFi.status() == WL_CONNECTED; }
bool netApUp() { return apUp; }
bool netInSetupMode() { return apUp && (!configStore.provisioned() || !netStaUp()); }
bool netPortalActive() { return portal; }

String netIp() {
  if (netStaUp()) return WiFi.localIP().toString();
  if (apUp) return WiFi.softAPIP().toString();
  return String("-");
}

String netApIp() { return apUp ? WiFi.softAPIP().toString() : String("-"); }

int netStaRssi() { return netStaUp() ? WiFi.RSSI() : 0; }

uint32_t netApClients() { return apUp ? WiFi.softAPgetStationNum() : 0; }

const char* netModeName() {
  switch (WiFi.getMode()) {
    case WIFI_AP:
      return "AP";
    case WIFI_STA:
      return "STA";
    case WIFI_AP_STA:
      return "AP+STA";
    default:
      return "off";
  }
}

const char* netStateName() {
  if (netStaUp()) return "Verbunden";
  // Two different situations, and conflating them cost hours: a device that is
  // not configured yet IS in configuration mode, while a configured device whose
  // network is unreachable is simply offline with its own access point up.
  if (apUp) return configStore.provisioned() ? "Getrennt (AP aktiv)" : "Konfigurations-WLAN";
  return "Getrennt";
}

void handleDns() {
  if (!dnsRunning) return;
  const uint32_t now = millis();
  if (now - lastDnsTickMs < 10) return;
  lastDnsTickMs = now;
  dnsServer.processNextRequest();
}
