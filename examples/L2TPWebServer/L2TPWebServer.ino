#include <WiFi.h>
#include <WebServer.h>
#include <ESP32VPN.h>
#include <ESP32VPNNetif.h>

using namespace esp32vpn;

const char *WIFI_SSID = "NAMA_WIFI";
const char *WIFI_PASSWORD = "PASSWORD_WIFI";
const char *VPN_SERVER = "vpn.example.com";
const char *VPN_USERNAME = "username_vpn";
const char *VPN_PASSWORD = "password_vpn";
const bool VPN_DEBUG = true;  // false = semua debug library nonaktif

ESP32VPN vpn;
ESP32VPNNetif adapter;
WebServer server(8291); // Ganti Port sesuai dengan Port Forwarding VPN

void vpnLog(const char *message, void *) {
  Serial.printf("[%10lu ms][VPN] %s\n", millis(), message);
}

void setup() {
  Serial.begin(115200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(250);

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain", "ESP32 L2TP VPN web server aktif\n");
  });

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
  adapter.setDebug(VPN_DEBUG);
  if (!vpn.begin(config))
    Serial.printf("VPN gagal: %s\n", vpn.lastError());
}

void loop() {
  vpn.loop();
  adapter.loop();

  static bool started = false;
  if (vpn.connected() && !started) {
    if (!adapter.begin(vpn, true)) {
      Serial.printf("Adapter gagal: %s\n", adapter.lastError());
      return;
    }
    server.begin();
    started = true;
    Serial.printf("HTTP server: http://%s:8291/\n",
                  vpn.localIP().toString().c_str());
  }
  if (started) server.handleClient();
  delay(0);
}
