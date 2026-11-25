#include <WiFi.h>
#include <ESP32VPN.h>

using namespace esp32vpn;

const char *WIFI_SSID = "NAMA_WIFI";
const char *WIFI_PASSWORD = "PASSWORD_WIFI";
const char *VPN_SERVER = "vpn.example.com";
const char *VPN_USERNAME = "username_vpn";
const char *VPN_PASSWORD = "password_vpn";
const bool VPN_DEBUG = true;  // false = debug VPN nonaktif

ESP32VPN vpn;

void vpnLog(const char *message, void *) {
  Serial.printf("[%10lu ms][VPN] %s\n", millis(), message);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(250);

  Config config;
  config.protocol = Protocol::L2TP;
  config.server = VPN_SERVER;
  config.username = VPN_USERNAME;
  config.password = VPN_PASSWORD;
  config.localPort = 1701;
  config.timeoutMs = 30000;
  config.debug = VPN_DEBUG;
  config.packetDump = false;

  vpn.onLog(vpnLog);
  if (!vpn.begin(config))
    Serial.printf("VPN gagal: %s\n", vpn.lastError());
}

void loop() {
  vpn.loop();
  static bool shown = false;
  if (vpn.connected() && !shown) {
    shown = true;
    Serial.printf("VPN connected: IP=%s DNS=%s\n",
                  vpn.localIP().toString().c_str(),
                  vpn.dnsIP().toString().c_str());
  }
  delay(0);
}
