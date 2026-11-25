#include "ESP32VPNNetif.h"

#include <cstring>
#include <new>
#include <vector>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

namespace esp32vpn {

namespace {
constexpr UBaseType_t kOutboundQueueLength = 32;
constexpr uint8_t kMaxPacketsPerLoop = 16;
}  // namespace

class ESP32VPNNetif::Impl {
 public:
  struct PendingPacket {
    std::vector<uint8_t> bytes;
  };

  static uint32_t addChecksum(uint32_t sum, const uint8_t *data, size_t length) {
    while (length >= 2) {
      sum += (uint16_t(data[0]) << 8) | data[1];
      data += 2;
      length -= 2;
    }
    if (length) sum += uint16_t(data[0]) << 8;
    return sum;
  }

  static uint16_t finishChecksum(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return uint16_t(~sum);
  }

  // lwIP normally generates these in software, but a custom raw-IP netif must
  // not depend on Ethernet hardware-offload settings inherited from Wi-Fi.
  static bool prepareIPv4(uint8_t *packet, size_t length) {
    if (!packet || length < 20 || (packet[0] >> 4) != 4) return false;
    const size_t ihl = size_t(packet[0] & 0x0f) * 4;
    const size_t total = (size_t(packet[2]) << 8) | packet[3];
    if (ihl < 20 || total < ihl || total > length) return false;
    packet[10] = packet[11] = 0;
    const uint16_t ipChecksum = finishChecksum(addChecksum(0, packet, ihl));
    packet[10] = uint8_t(ipChecksum >> 8);
    packet[11] = uint8_t(ipChecksum);

    // Only first/non-fragmented packets contain a complete transport header.
    const uint16_t fragment = (uint16_t(packet[6]) << 8) | packet[7];
    if ((fragment & 0x3fffU) != 0) return true;
    const uint8_t protocol = packet[9];
    if (protocol != 6 && protocol != 17) return true;
    const size_t transportLength = total - ihl;
    const size_t checksumOffset = protocol == 6 ? 16 : 6;
    if (transportLength < checksumOffset + 2) return false;
    uint8_t *transport = packet + ihl;
    transport[checksumOffset] = transport[checksumOffset + 1] = 0;
    uint32_t sum = addChecksum(0, packet + 12, 8);
    sum += protocol;
    sum += transportLength;
    uint16_t checksum = finishChecksum(addChecksum(sum, transport, transportLength));
    if (protocol == 17 && checksum == 0) checksum = 0xffff;
    transport[checksumOffset] = uint8_t(checksum >> 8);
    transport[checksumOffset + 1] = uint8_t(checksum);
    return true;
  }

  static err_t initNetif(struct netif *n) {
    n->name[0] = 'v';
    n->name[1] = 'p';
    n->mtu = 1400;
    n->output = &Impl::output;
    n->flags = NETIF_FLAG_UP | NETIF_FLAG_LINK_UP;
    return ERR_OK;
  }

  static err_t output(struct netif *n, struct pbuf *p,
                      const ip4_addr_t *) {
    Impl *self = static_cast<Impl *>(n->state);
    if (!self || !self->vpn || !self->vpn->connected() || !p || !p->tot_len)
      return ERR_IF;
    if (p->tot_len > 1500) return ERR_MEM;
    std::vector<uint8_t> packet(p->tot_len);
    if (pbuf_copy_partial(p, packet.data(), p->tot_len, 0) != p->tot_len)
      return ERR_IF;
    if (!prepareIPv4(packet.data(), packet.size())) return ERR_IF;
    const size_t ihl = size_t(packet[0] & 0x0f) * 4;
    if (packet[9] == 6 && packet.size() >= ihl + 20) {
      const uint8_t flags = packet[ihl + 13];
      if (self->debug && (flags & 0x02)) {
        const uint16_t sourcePort = (uint16_t(packet[ihl]) << 8) | packet[ihl + 1];
        const uint16_t destinationPort = (uint16_t(packet[ihl + 2]) << 8) | packet[ihl + 3];
        Serial.printf("[VPN-NETIF] TCP TX %u -> %u flags=%s checksum=dihitung ulang\n",
                      unsigned(sourcePort), unsigned(destinationPort),
                      (flags & 0x10) ? "SYN-ACK" : "SYN");
      }
    }
    // output() runs inside lwIP's tcpip thread. Calling sendto() here would
    // synchronously post back to the same thread and deadlock the whole stack.
    PendingPacket *pending = new (std::nothrow) PendingPacket;
    if (!pending) return ERR_MEM;
    pending->bytes.swap(packet);
    if (!self->outbound ||
        xQueueSend(self->outbound, &pending, 0) != pdTRUE) {
      delete pending;
      return ERR_MEM;
    }
    return ERR_OK;
  }

  static void input(const uint8_t *packet, size_t length, void *context) {
    Impl *self = static_cast<Impl *>(context);
    if (!self || !self->running || !packet || length < 20 || length > 65535)
      return;
    struct pbuf *p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(length), PBUF_POOL);
    if (!p) return;
    if (pbuf_take(p, packet, length) != ERR_OK ||
        tcpip_input(p, &self->netif) != ERR_OK) {
      pbuf_free(p);
    }
  }

  bool begin(ESP32VPN &v, bool makeDefault) {
    stop();
    error = "";
    if (!v.connected()) {
      error = "VPN belum Connected";
      return false;
    }
    const IPAddress address = v.localIP();
    if (address == IPAddress()) {
      error = "alamat IPv4 VPN belum tersedia";
      return false;
    }

    vpn = &v;
    // TCP commonly emits ACK/data bursts. Eight slots were too easy to fill
    // while an application was streaming a response through the VPN.
    outbound = xQueueCreate(kOutboundQueueLength, sizeof(PendingPacket *));
    if (!outbound) {
      vpn = nullptr;
      error = "gagal membuat antrean paket VPN";
      return false;
    }
    ip4_addr_t ip, mask, gateway;
    IP4_ADDR(&ip, address[0], address[1], address[2], address[3]);
    IP4_ADDR(&mask, 255, 255, 255, 255);
    ip4_addr_set_zero(&gateway);
    memset(&netif, 0, sizeof(netif));
    LOCK_TCPIP_CORE();
    struct netif *added = netif_add(&netif, &ip, &mask, &gateway, this,
                                    &Impl::initNetif, tcpip_input);
    UNLOCK_TCPIP_CORE();
    if (!added) {
      vQueueDelete(outbound);
      outbound = nullptr;
      vpn = nullptr;
      error = "lwIP gagal menambahkan netif VPN";
      return false;
    }
    previousDefault = netif_default;
    LOCK_TCPIP_CORE();
    netif_set_up(&netif);
    netif_set_link_up(&netif);
    UNLOCK_TCPIP_CORE();
    running = true;
    v.onIPv4(&Impl::input, this);
    if (makeDefault) {
      LOCK_TCPIP_CORE();
      netif_set_default(&netif);
      UNLOCK_TCPIP_CORE();
    }
    isDefault = makeDefault;
    return true;
  }

  void loop() {
    if (!running || !vpn || !outbound) return;
    if (vpn->connected()) {
      const IPAddress address = vpn->localIP();
      const ip4_addr_t *current = netif_ip4_addr(&netif);
      if (address != IPAddress() &&
          (!current || ip4_addr1(current) != address[0] ||
           ip4_addr2(current) != address[1] ||
           ip4_addr3(current) != address[2] ||
           ip4_addr4(current) != address[3])) {
        ip4_addr_t ip, mask, gateway;
        IP4_ADDR(&ip, address[0], address[1], address[2], address[3]);
        IP4_ADDR(&mask, 255, 255, 255, 255);
        ip4_addr_set_zero(&gateway);
        LOCK_TCPIP_CORE();
        netif_set_addr(&netif, &ip, &mask, &gateway);
        UNLOCK_TCPIP_CORE();
        if (debug)
          Serial.printf("[VPN-NETIF] alamat diperbarui menjadi %s\n",
                        address.toString().c_str());
      }
    }
    PendingPacket *pending = nullptr;
    uint8_t processed = 0;
    while (processed < kMaxPacketsPerLoop &&
           xQueueReceive(outbound, &pending, 0) == pdTRUE) {
      if (pending) {
        if (!vpn->sendIPv4(pending->bytes.data(), pending->bytes.size()) && debug)
          Serial.println("[VPN-NETIF] gagal mengirim paket antrean ke PPP");
        delete pending;
      }
      ++processed;
    }
  }

  void stop() {
    if (!running) return;
    running = false;
    if (vpn) vpn->onIPv4(nullptr, nullptr);
    LOCK_TCPIP_CORE();
    if (isDefault && netif_default == &netif) netif_set_default(previousDefault);
    netif_set_down(&netif);
    netif_remove(&netif);
    UNLOCK_TCPIP_CORE();
    PendingPacket *pending = nullptr;
    while (outbound && xQueueReceive(outbound, &pending, 0) == pdTRUE)
      delete pending;
    if (outbound) vQueueDelete(outbound);
    outbound = nullptr;
    vpn = nullptr;
    previousDefault = nullptr;
    isDefault = false;
  }

  ESP32VPN *vpn = nullptr;
  struct netif netif;
  struct netif *previousDefault = nullptr;
  volatile bool running = false;
  bool isDefault = false;
  bool debug = false;
  QueueHandle_t outbound = nullptr;
  String error;
};

ESP32VPNNetif::ESP32VPNNetif() : _impl(new Impl) {}
ESP32VPNNetif::~ESP32VPNNetif() { _impl->stop(); delete _impl; }
bool ESP32VPNNetif::begin(ESP32VPN &vpn, bool makeDefault) {
  return _impl->begin(vpn, makeDefault);
}
void ESP32VPNNetif::stop() { _impl->stop(); }
void ESP32VPNNetif::loop() { _impl->loop(); }
void ESP32VPNNetif::setDebug(bool enabled) { _impl->debug = enabled; }
bool ESP32VPNNetif::active() const { return _impl->running; }
const char *ESP32VPNNetif::lastError() const { return _impl->error.c_str(); }

}  // namespace esp32vpn
