#pragma once

#include <Arduino.h>

#include "ESP32VPN.h"
#include "ESP32VPNNetif.h"

namespace esp32vpn {

struct WorkerConfig {
  uint32_t stackSize = 8192;
  UBaseType_t priority = 2;
  BaseType_t core = 0;
  TickType_t idleTicks = 1;
  uint32_t adapterRetryMs = 1000;
};

// Owns all periodic VPN and netif processing in one FreeRTOS task. Once this
// worker is started, the application must not call vpn.loop() or adapter.loop().
class ESP32VPNWorker {
 public:
  ESP32VPNWorker();
  ~ESP32VPNWorker();

  bool begin(ESP32VPN &vpn, ESP32VPNNetif &adapter,
             bool makeDefault = true,
             const WorkerConfig &config = WorkerConfig());
  void stop();

  bool running() const;
  bool ready() const;
  const char *lastError() const;

 private:
  class Impl;
  Impl *_impl;
};

}  // namespace esp32vpn
