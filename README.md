# ESP32VPN L2TP

Library L2TP untuk ESP32 Arduino yang membangun tunnel L2TP/UDP, kemudian
menjalankan PPP di dalam tunnel. Implementasi saat ini
mendukung LCP, PAP, MS-CHAPv2, IPCP, alamat IPv4, DNS, pengiriman paket IPv4, dan callback
paket IPv4 masuk. `ESP32VPNWorker` dapat menjalankan pemrosesan VPN dan adaptor
di task FreeRTOS terpisah agar aplikasi sinkron tidak menghentikan tunnel.

## Kompatibilitas board

Library ini dikembangkan dan diuji dengan:

- Board platform: Espressif ESP32
- Arduino core: `esp32 by Espressif Systems` versi `2.0.17`
- Target pengujian: ESP32 klasik (`esp32` / Xtensa)

Versi core lain mungkin dapat digunakan, tetapi API internal lwIP/netif dapat
berbeda. Versi `2.0.17` adalah versi referensi yang saat ini tervalidasi.

Detail wire format, state machine, dan referensi RFC tersedia di
[`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Instalasi

1. Pindahkan folder ini ke `Arduino/libraries/ESP32VPN`, atau buka sketch utama
   langsung dari folder proyek.
2. Pilih board ESP32 pada Arduino IDE.
3. Buka salah satu sketch di folder `examples`, lalu isi konfigurasi Wi-Fi dan
   akun VPN milik perangkat Anda.

## Mengaktifkan dan menonaktifkan debug

Debug library default-nya nonaktif. Aktifkan melalui konfigurasi dan callback:

```cpp
config.debug = true;
vpn.onLog(vpnLog);
adapter.setDebug(true); // bila memakai ESP32VPNNetif
```

Untuk menonaktifkannya tanpa menghapus callback:

```cpp
config.debug = false;
adapter.setDebug(false);
```

Hexdump per paket dipisahkan dari debug biasa karena output Serial 115200 dapat
menahan loop saat trafik ramai:

```cpp
config.debug = true;       // event dan state penting
config.packetDump = false; // set true hanya saat analisis paket
```

Pada sketch dan contoh yang disertakan, cukup ubah satu konstanta:

```cpp
const bool VPN_DEBUG = false;
```

## Keepalive dan reconnect

Library mengirim L2TP HELLO saat tunnel tidak menerima paket selama 20 detik.
Jika peer tetap diam selama 90 detik, tunnel dibangun ulang otomatis. Nilainya
dapat diubah atau dinonaktifkan:

```cpp
config.keepaliveIntervalMs = 20000; // 0 = HELLO nonaktif
config.keepaliveTimeoutMs = 90000;  // 0 = watchdog nonaktif
config.autoReconnect = true;
config.reconnectDelayMs = 3000;
```

## Examples

- `L2TPBasic`: koneksi L2TP/PPP dan menampilkan IP hasil IPCP.
- `L2TPWebServer`: worker FreeRTOS, adaptor lwIP, dan HTTP server melalui VPN.

## API singkat

```cpp
esp32vpn::Config c;
c.server = "vpn.example.com";
c.username = "user";
c.password = "secret";
c.localPort = 1701; // source port UDP L2TP; gunakan 0 jika 1701 sudah dipakai
c.debug = true;

#include <ESP32VPNNetif.h>
#include <ESP32VPNWorker.h>

esp32vpn::ESP32VPN vpn;
esp32vpn::ESP32VPNNetif adapter;
esp32vpn::ESP32VPNWorker worker;

void setup() {
  // Hubungkan Wi-Fi terlebih dahulu.
  vpn.begin(c);
  if (!worker.begin(vpn, adapter, true)) {
    Serial.println(worker.lastError());
  }
}

void loop() {
  // Tidak perlu lagi memanggil vpn.loop() atau adapter.loop().
  if (worker.ready()) server.handleClient();
}
```

`worker.begin()` membuat task FreeRTOS, menjalankan `vpn.loop()`, menunggu PPP
Connected, mengaktifkan adaptor lwIP, menguras antrean paket, dan mencoba ulang
adaptor bila diperlukan. Jangan memanggil `vpn.loop()` atau `adapter.loop()`
sendiri selama worker aktif. Gunakan mode loop manual hanya jika aplikasi tidak
memakai worker.

Konfigurasi task bersifat opsional:

```cpp
esp32vpn::WorkerConfig wc;
wc.stackSize = 8192;
wc.priority = 2;
wc.core = 0;
wc.idleTicks = 1;
wc.adapterRetryMs = 1000;
worker.begin(vpn, adapter, true, wc);
```

`sendIPv4()` menerima satu paket IPv4 lengkap. `onIPv4()` menerima paket dari
tunnel. `ESP32VPNNetif` memasukkan paket tersebut ke stack lwIP sehingga socket
TCP/UDP Arduino dapat menggunakan alamat hasil IPCP.

## Batasan penting

- Autentikasi PPP mendukung PAP dan MS-CHAPv2. Implementasi MS-CHAPv2
  memvalidasi authenticator response server sesuai RFC 2759.
- L2TP ini adalah L2TP polos, bukan L2TP/IPsec. L2TP tidak mengenkripsi data.
- `ESP32VPNNetif` saat ini ditargetkan dan diuji-kompilasi untuk Arduino-ESP32
  2.0.x. Adaptor memasang VPN sebagai default route; socket UDP L2TP dikunci ke
  antarmuka Wi-Fi untuk mencegah loop routing.
- `ESP32VPNWorker` memiliki satu task sebagai pemilik pemrosesan periodik VPN
  dan netif. Jangan menjalankan loop manual secara bersamaan dengan worker.
- IPv6, MPPE, CHAP-MD5, EAP, fragment reassembly, dan persistensi
  sesi belum diimplementasikan.

Untuk penggunaan internet penuh yang aman, target lanjutan yang disarankan
adalah MPPE, retransmisi kontrol L2TP, dan pengujian
interoperabilitas terhadap server yang digunakan.

## Membaca debug Serial

Pasang callback `onLog()` sebelum `begin()` dan buka Serial Monitor pada 115200
baud. Library menampilkan DNS, pembukaan UDP, semua pesan kontrol L2TP,
LCP, PAP, MS-CHAPv2, IPCP, perubahan state, dan tahap yang mengalami timeout. Password tidak
pernah dicetak.

Beberapa petunjuk diagnosis:

- Berhenti setelah `L2TP TX: SCCRQ`: server/port UDP 1701 tidak menjawab, UDP
  diblokir NAT/firewall, atau server sebenarnya mewajibkan L2TP/IPsec.
- Menerima `SCCRP` tetapi tidak `ICRP`: server menolak pembuatan data session.
- Pesan `server memilih MS-CHAPv2` akan diikuti Challenge dan Response. Pesan
  `CHAP Challenge tidak didukung` berarti server memilih varian CHAP selain MS-CHAPv2.
- `PAP Authenticate-Nak` atau `MS-CHAPv2 ditolak`: username, password, domain, atau kebijakan autentikasi
  server menolak login.
- IPCP selesai berarti sesi VPN dan alamat IPv4 tunnel sudah berhasil dibentuk.
