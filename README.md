# ESP32VPN L2TP

Library L2TP untuk ESP32 Arduino yang membangun tunnel L2TP/UDP, kemudian
menjalankan PPP di dalam tunnel. Implementasi saat ini
mendukung LCP, PAP, MS-CHAPv2, IPCP, alamat IPv4, DNS, pengiriman paket IPv4, dan callback
paket IPv4 masuk. Semua pemrosesan dibuat non-blocking setelah koneksi awal.

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
3. Salin `secrets.h.example` menjadi `secrets.h`, lalu isi Wi-Fi dan akun VPN.

Untuk sketch utama, salin `secrets.h.example` menjadi `secrets.h`, lalu isi
credential lokal. `secrets.h` sudah masuk `.gitignore` dan tidak akan ikut
terunggah ke GitHub.

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
- `L2TPWebServer`: adaptor lwIP dan HTTP server port `8291` melalui VPN.

## API singkat

```cpp
esp32vpn::Config c;
c.server = "vpn.example.com";
c.username = "user";
c.password = "secret";
c.localPort = 1701; // source port UDP L2TP; gunakan 0 jika 1701 sudah dipakai
c.debug = true;

esp32vpn::ESP32VPN vpn;
esp32vpn::ESP32VPNNetif adapter;
vpn.onIPv4(onPacket);
vpn.begin(c);

void loop() {
  vpn.loop();
  adapter.loop();
}
```

`sendIPv4()` menerima satu paket IPv4 lengkap. `onIPv4()` menerima paket dari
tunnel. Untuk memakai socket Arduino biasa, aktifkan adaptor lwIP setelah VPN
tersambung:

```cpp
#include <ESP32VPNNetif.h>
#include <ESP32VPNWorker.h>

if (vpn.connected()) adapter.begin(vpn, true); // VPN menjadi default route
```

Untuk aplikasi sinkron seperti `WebServer`, gunakan worker bawaan agar respons
HTTP tidak menghentikan L2TP/PPP:

```cpp
esp32vpn::ESP32VPNWorker worker;

void setup() {
  // Hubungkan Wi-Fi, pasang config, lalu vpn.begin(c) terlebih dahulu.
  worker.begin(vpn, adapter, true);
}

void loop() {
  // Tidak perlu lagi memanggil vpn.loop() atau adapter.loop().
  if (worker.ready()) server.handleClient();
}
```

Worker menjadi satu-satunya pemilik proses periodik VPN, menunggu tunnel
tersambung, mengaktifkan adaptor, dan mencoba ulang adaptor bila diperlukan.
Core, stack, prioritas, interval task, dan retry dapat diatur melalui
`WorkerConfig`. Contoh lengkap ada di `examples/L2TPWebServer`.

Adaptor menyediakan antrean burst 32 paket dan memproses maksimal 16 paket per
putaran. Ini mengurangi packet drop saat TCP mengirim burst tanpa membuat task
VPN melakukan busy-loop atau menghambat task Wi-Fi/lwIP.

Sketch contoh menjalankan HTTP server pada port `8291`. Agar bisa diakses dari
internet melalui `rmtsg3.perwiramedia.com:8228`, router/server VPN harus memiliki
DNAT/port-forward TCP menuju alamat IPCP ESP32, misalnya
`10.18.0.228:8291`. Port publik tidak dapat dibuka oleh library ESP32 sendiri.

## Batasan penting

- Autentikasi PPP mendukung PAP dan MS-CHAPv2. Implementasi MS-CHAPv2
  memvalidasi authenticator response server sesuai RFC 2759.
- L2TP ini adalah L2TP polos, bukan L2TP/IPsec. L2TP tidak mengenkripsi data.
- `ESP32VPNNetif` saat ini ditargetkan dan diuji-kompilasi untuk Arduino-ESP32
  2.0.x. Adaptor memasang VPN sebagai default route; socket UDP L2TP dikunci ke
  antarmuka Wi-Fi untuk mencegah loop routing.
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

