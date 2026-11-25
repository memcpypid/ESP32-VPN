#include "ESP32VPNWorker.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esp32vpn {

class ESP32VPNWorker::Impl {
 public:
  ~Impl() {
    stop();
    if (stopped) vSemaphoreDelete(stopped);
  }

  bool begin(ESP32VPN &vpnRef, ESP32VPNNetif &adapterRef,
             bool useAsDefault, const WorkerConfig &workerConfig) {
    stop();
    error = "";
    vpn = &vpnRef;
    adapter = &adapterRef;
    makeDefault = useAsDefault;
    config = workerConfig;
    stopRequested = false;
    adapterReady = adapter->active();
    if (!stopped) stopped = xSemaphoreCreateBinary();
    if (!stopped) {
      vpn = nullptr;
      adapter = nullptr;
      error = "gagal membuat semaphore worker VPN";
      return false;
    }

    const BaseType_t created = xTaskCreatePinnedToCore(
        &Impl::taskEntry, "l2tp-worker", config.stackSize, this,
        config.priority, &task, config.core);
    if (created != pdPASS) {
      task = nullptr;
      vpn = nullptr;
      adapter = nullptr;
      error = "gagal membuat task FreeRTOS VPN";
      return false;
    }
    return true;
  }

  void stop() {
    if (!task) return;
    stopRequested = true;
    if (!stopped ||
        xSemaphoreTake(stopped, pdMS_TO_TICKS(1000)) != pdTRUE) {
      vTaskDelete(task);
    }
    task = nullptr;
    vpn = nullptr;
    adapter = nullptr;
    adapterReady = false;
  }

  static void taskEntry(void *context) {
    static_cast<Impl *>(context)->run();
  }

  void run() {
    uint32_t nextAdapterAttempt = 0;
    while (!stopRequested) {
      vpn->loop();
      if (vpn->connected() && !adapter->active() &&
          static_cast<int32_t>(millis() - nextAdapterAttempt) >= 0) {
        if (adapter->begin(*vpn, makeDefault)) {
          adapterReady = true;
          error = "";
        } else {
          error = adapter->lastError();
          nextAdapterAttempt = millis() + config.adapterRetryMs;
        }
      }
      adapter->loop();
      vTaskDelay(config.idleTicks ? config.idleTicks : 1);
    }
    xSemaphoreGive(stopped);
    vTaskDelete(nullptr);
  }

  ESP32VPN *vpn = nullptr;
  ESP32VPNNetif *adapter = nullptr;
  WorkerConfig config;
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t stopped = nullptr;
  volatile bool stopRequested = false;
  volatile bool adapterReady = false;
  bool makeDefault = true;
  String error;
};

ESP32VPNWorker::ESP32VPNWorker() : _impl(new Impl) {}
ESP32VPNWorker::~ESP32VPNWorker() {
  delete _impl;
}
bool ESP32VPNWorker::begin(ESP32VPN &vpn, ESP32VPNNetif &adapter,
                           bool makeDefault, const WorkerConfig &config) {
  return _impl->begin(vpn, adapter, makeDefault, config);
}
void ESP32VPNWorker::stop() { _impl->stop(); }
bool ESP32VPNWorker::running() const { return _impl->task != nullptr; }
bool ESP32VPNWorker::ready() const { return _impl->adapterReady; }
const char *ESP32VPNWorker::lastError() const { return _impl->error.c_str(); }

}  // namespace esp32vpn
