#pragma once

#include <Arduino.h>

namespace esp32vpn {

enum class State : uint8_t {
  Idle, Connecting, Negotiating, Authenticating, Configuring, Connected, Error
};

struct Config {
  const char *server = nullptr;
  uint16_t port = 0;                 // 0 = UDP 1701
  uint16_t localPort = 1701;         // UDP source port for L2TP; 0 = ephemeral
  const char *username = nullptr;
  const char *password = nullptr;
  bool debug = false;                // enable protocol/debug log callbacks
  bool packetDump = false;           // verbose per-packet hex; can be slow
  uint32_t timeoutMs = 15000;
  uint32_t keepaliveIntervalMs = 20000; // 0 = disable active L2TP HELLO
  uint32_t keepaliveTimeoutMs = 90000;  // reconnect if peer stays silent
  bool autoReconnect = true;
  uint32_t reconnectDelayMs = 3000;
};

using IPv4Handler = void (*)(const uint8_t *packet, size_t length, void *context);
using LogHandler = void (*)(const char *message, void *context);

class ESP32VPN {
 public:
  ESP32VPN();
  ~ESP32VPN();

  bool begin(const Config &config);
  void loop();
  void stop();

  State state() const;
  bool connected() const;
  const char *lastError() const;
  IPAddress localIP() const;
  IPAddress dnsIP() const;

  // Sends one complete IPv4 packet through PPP. Applications can connect this
  // to a lwIP netif adapter; received packets are delivered to IPv4Handler.
  bool sendIPv4(const uint8_t *packet, size_t length);
  void onIPv4(IPv4Handler handler, void *context = nullptr);
  void onLog(LogHandler handler, void *context = nullptr);

 private:
  class Impl;
  Impl *_impl;
};

}  // namespace esp32vpn

