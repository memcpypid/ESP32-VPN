# Catatan protokol ESP32VPN

Dokumen ini menjelaskan wire format dan keputusan interoperabilitas library.
Sumber utama adalah RFC 2661 (L2TPv2), RFC 1661/1662 (PPP dan framing), RFC
1334 (PAP), dan RFC 2759 (MS-CHAPv2).

## Urutan L2TP

Control connection menggunakan:

1. `SCCRQ` dari ESP32 dengan Protocol Version, Host Name, Framing
   Capabilities, dan Assigned Tunnel ID.
2. `SCCRP` dari LNS, termasuk Assigned Tunnel ID milik LNS.
3. `SCCCN` dari ESP32.
4. `ICRQ` dengan Assigned Session ID lokal dan Call Serial Number.
5. `ICRP` dengan Assigned Session ID milik LNS.
6. `ICCN` pada Session ID milik LNS, berisi Tx Connect Speed dan Framing Type.

Tunnel ID dan Session ID dalam paket keluar selalu nilai yang diberikan peer.
ID lokal hanya muncul dalam AVP penetapan dan paket yang dikirim peer kepada
ESP32.

Control header menggunakan Length dan sequence `Ns/Nr`. ZLB tidak mengonsumsi
nomor sequence. Paket control duplikat tidak diproses ulang dan tetap diberi
ZLB-ACK. Alamat IP dan pasangan port UDP dikunci selama umur tunnel.

## Kanal data dan PPP

Kanal data memakai header L2TPv2, Tunnel ID peer, Session ID peer, dan mengikuti
keputusan sequencing LNS. Server uji mengirim representasi PPP dengan Address
dan Control `FF 03`; library mencerminkan representasi tersebut untuk
interoperabilitas, tetapi parser juga menerima Protocol field langsung.

LCP diumumkan sebagai synchronous framing pada ICCN. Karena itu Configure-
Request lokal tidak memakai ACCM. Configure-Ack divalidasi terhadap Identifier
dan opsi request terakhir. Configure-Nak dan Configure-Reject menghasilkan
proposal baru; retransmisi memakai request yang sama.

Urutan PPP yang diharapkan:

1. Kedua arah bertukar LCP Configure-Request/Configure-Ack.
2. Server meminta PAP atau MS-CHAPv2.
3. Setelah autentikasi berhasil, kedua arah menegosiasikan IPCP.
4. `Connected` hanya tercapai setelah IPCP Configure-Ack.

## MS-CHAPv2

NT-Response menggunakan ChallengeHash SHA-1, NT password hash MD4, dan tiga
operasi DES sesuai RFC 2759. Success packet tidak langsung dipercaya: nilai
`S=` dibandingkan constant-time dengan AuthenticatorResponse yang dihitung
lokal. Implementasi diuji memakai contoh resmi RFC 2759 bagian 9.2.

## Keamanan dan batasan

L2TP polos tidak memberikan enkripsi. Library ini belum mengimplementasikan
IPsec atau MPPE. `ESP32VPNNetif` menyediakan adaptor default-route lwIP agar
socket TCP/UDP Arduino dapat memakai alamat IPv4 hasil IPCP.

Untuk diagnosis interoperabilitas, capture jaringan merupakan sumber kebenaran.
Log `TX HEX` menunjukkan buffer sebelum `sendto`, bukan bukti bahwa paket tiba
dan diterima LNS.
