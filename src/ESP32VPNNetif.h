#pragma once

#include "ESP32VPN.h"

namespace esp32vpn {

// Bridges complete IPv4 packets between ESP32VPN and lwIP. Once started, TCP,
// UDP and servers created with the normal Arduino networking APIs can use the
// address negotiated by IPCP.
class ESP32VPNNetif {
 public:
  ESP32VPNNetif();
  ~ESP32VPNNetif();

  // makeDefault is needed for replies from sockets bound to the VPN address.
  bool begin(ESP32VPN &vpn, bool makeDefault = true);
  void setDebug(bool enabled);
  // Drain outbound packets outside lwIP's tcpip thread. Call frequently from
  // the Arduino loop, before or after ESP32VPN::loop().
  void loop();
  void stop();
  bool active() const;
  const char *lastError() const;

 private:
  class Impl;
  Impl *_impl;
};

}  // namespace esp32vpn
