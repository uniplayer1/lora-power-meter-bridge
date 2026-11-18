#pragma once
// HTTP interface: dashboard, first-run setup, REST API for Home Assistant,
// captive portal and OTA upload.

#include <Arduino.h>
#include <WebServer.h>

#include "app_config.h"
#include "meter_reader.h"
#include "radio_link.h"

// The state document the web UI and the MQTT bridge both publish: one
// serializer, so the two can never drift apart. It used to be written twice and
// the MQTT copy quietly lost the device identity.
// `apClient` and `canConfigure` decide which values are allowed out: the setup
// code and the link secret only leave over the device's own access point, and
// the MQTT bridge is off-device, so it passes false, false.
String buildStateJson(const RadioLink& link, const MeterReader& meter, bool apClient,
                      bool canConfigure);

class WebUi {
 public:
  void begin(RadioLink* link, MeterReader* meter);
  void loop();

  bool restartPending() const { return restartAtMs_ != 0; }
  uint32_t restartAtMs() const { return restartAtMs_; }
  void requestRestart(uint32_t delayMs = 1500);

 private:
  // pages
  void handleRoot();
  void handleSetupPage();
  void handleLoginPage();
  void handleLoginPost();
  void handleLogout();
  void handleStyle();
  void handleScript();
  void servePage(const char* html);
  void handleCaptiveProbe();
  void handleNotFound();

  // api
  void handleState();
  void handleScan();
  void handleDump();
  void handleSetupSave();
  void handleSave();
  void handleAction();
  void handleOtaUpload();
  void handleOtaDone();
  void handleHaPackage();
  void handleHaCard();
  void handleMqttDump();

  // Access control. isApClient() is also read by the shared state document,
  // which keeps the setup code and the link secret inside the device's own
  // access point.
  bool authorized();
  bool isApClient();
  // Anything that can change a setting needs this: a client on the device's own
  // access point already knows the WLAN password, which by default is the same
  // code the login asks for. Treating the AP as read-only made every save
  // answer "not logged in" on a page that never offered a login.
  bool mayConfigure();
  void requireAuthOrRedirect();
  String buildHaPackage() const;
  String buildHaCard() const;
  String argOr(const char* name, const char* fallback);
  bool applySaveField(const String& key, const String& value);
  void sendJson(int code, const String& body);

  WebServer server_{WEB_PORT};
  RadioLink* link_ = nullptr;
  MeterReader* meter_ = nullptr;

  String sessionToken_;
  uint32_t sessionSinceMs_ = 0;
  uint32_t restartAtMs_ = 0;
  // A network change requested by the setup page, applied a moment after the
  // answer has gone out: switching WiFi first dropped the very connection the
  // answer had to travel over.
  uint32_t netChangeAtMs_ = 0;
  bool netChangeAp_ = false;  // the AP name or code changed as well
  uint8_t failedLogins_ = 0;
  bool otaRejected_ = false;  // the upload in progress is the wrong image
  uint32_t lastFailedLoginMs_ = 0;
};
