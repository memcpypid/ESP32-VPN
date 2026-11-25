#include "ESP32VPN.h"
#include "MSChapV2.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <vector>
#include <esp_system.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/sockets.h>
#include <fcntl.h>

namespace esp32vpn {
namespace {

static uint16_t be16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }
static uint32_t be32(const uint8_t *p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
static void put16(std::vector<uint8_t> &v, uint16_t n) {
  v.push_back(n >> 8); v.push_back(n & 0xff);
}
static void put32(std::vector<uint8_t> &v, uint32_t n) {
  v.push_back(n >> 24); v.push_back(n >> 16); v.push_back(n >> 8); v.push_back(n);
}

enum : uint16_t { PPP_IP = 0x0021, PPP_LCP = 0xc021, PPP_PAP = 0xc023, PPP_CHAP = 0xc223, PPP_IPCP = 0x8021 };

class PPP {
 public:
  using Sender = bool (*)(const uint8_t *, size_t, void *);
  PPP() = default;

  void configure(const char *user, const char *pass, Sender sender, void *ctx,
                 IPv4Handler rx, void *rxCtx, LogHandler logger, void *logCtx) {
    user_ = user ? user : ""; pass_ = pass ? pass : "";
    send_ = sender; sendCtx_ = ctx; rx_ = rx; rxCtx_ = rxCtx;
    logger_ = logger; logCtx_ = logCtx;
    nextId_=1;lcpRequestId_=0;lcpMagic_=0;requestedMru_=1400;requestMru_=requestMagic_=true;lastLcpOptions_.clear();
    lcpUp_ = peerLcpUp_ = authStarted_ = authUp_ = ipcpUp_ = false;
    auth_ = Auth::None; memset(authChallenge_,0,sizeof(authChallenge_));
    memset(peerChallenge_,0,sizeof(peerChallenge_));memset(ntResponse_,0,sizeof(ntResponse_));
    local_ = dns_ = IPAddress();
  }
  void setReceiver(IPv4Handler rx, void *ctx) { rx_ = rx; rxCtx_ = ctx; }
  void start() { log("PPP: mulai negosiasi LCP"); sendLcpRequest(); }
  void loop(){if(!lcpUp_&&lcpRequestId_&&millis()-lastLcpRequest_>=3000){log("PPP TX: mengulang LCP Configure-Request");sendLcpRequest(true);}}
  bool up() const { return ipcpUp_; }
  IPAddress localIP() const { return local_; }
  IPAddress dnsIP() const { return dns_; }

  bool sendIPv4(const uint8_t *data, size_t len) {
    return ipcpUp_ && sendProtocol(PPP_IP, data, len);
  }

  void input(const uint8_t *data, size_t len) {
    // Address/control bytes are optional with protocol field compression.
    if (len >= 2 && data[0] == 0xff && data[1] == 0x03) { data += 2; len -= 2; }
    if (len < 1) return;
    uint16_t proto;
    if (data[0] & 1) { proto = data[0]; ++data; --len; }
    else { if (len < 2) return; proto = be16(data); data += 2; len -= 2; }
    if (proto == PPP_IP) { if (rx_) rx_(data, len, rxCtx_); return; }
    if (len < 4 || be16(data + 2) > len) return;
    const uint8_t code = data[0], id = data[1];
    const uint16_t plen = be16(data + 2);
    const uint8_t *opts = data + 4; const size_t optsLen = plen - 4;
    if (proto == PPP_LCP) handleLcp(code, id, opts, optsLen);
    else if (proto == PPP_PAP) handlePap(code, opts, optsLen);
    else if (proto == PPP_CHAP) handleChap(code, id, opts, optsLen);
    else if (proto == PPP_IPCP) handleIpcp(code, id, opts, optsLen);
    else if(lcpUp_){
      std::vector<uint8_t> rejected{uint8_t(proto>>8),uint8_t(proto)};
      rejected.insert(rejected.end(),data,data+std::min<size_t>(len,128));
      char message[80];snprintf(message,sizeof(message),"PPP: protocol 0x%04X belum didukung, mengirim Protocol-Reject",unsigned(proto));log(message);
      sendControl(PPP_LCP,8,nextId_++,rejected);
    }
  }

 private:
  void sendLcpRequest(bool retransmit=false) {
    // Authentication-Protocol is requested by the authenticator (server),
    // not by this client. Including it here would ask the server to login.
    // ICCN advertises synchronous framing. ACCM (option 2) is only valid for
    // asynchronous octet framing and caused an inconsistent link proposal.
    std::vector<uint8_t> o;
    if(requestMru_){o.push_back(1);o.push_back(4);put16(o,requestedMru_);}
    if(requestMagic_){o.push_back(5);o.push_back(6);}
    log("PPP TX: LCP Configure-Request (synchronous framing)");
    if(!retransmit){lcpRequestId_=nextId_++;lcpMagic_=uint32_t(esp_random());}
    if(requestMagic_)put32(o,lcpMagic_);
    lastLcpOptions_=o;
    lastLcpRequest_=millis();sendControl(PPP_LCP,1,lcpRequestId_,o);
  }
  void sendPap() {
    log("PPP TX: PAP Authenticate-Request (password disembunyikan)");
    std::vector<uint8_t> p;
    p.push_back(uint8_t(std::min<size_t>(user_.length(), 255)));
    p.insert(p.end(), user_.c_str(), user_.c_str() + user_.length());
    p.push_back(uint8_t(std::min<size_t>(pass_.length(), 255)));
    p.insert(p.end(), pass_.c_str(), pass_.c_str() + pass_.length());
    sendControl(PPP_PAP, 1, nextId_++, p);
  }
  void sendIpcpRequest() {
    log("PPP TX: IPCP Configure-Request (meminta IPv4 dan DNS)");
    std::vector<uint8_t> o{3,6,0,0,0,0, 129,6,0,0,0,0};
    sendControl(PPP_IPCP, 1, nextId_++, o);
  }
  void handleLcp(uint8_t code, uint8_t id, const uint8_t *o, size_t n) {
    logCode("PPP RX: LCP", code, id);
    if (code == 1) {
      // Ask for PAP if the peer proposes CHAP/EAP. This small implementation
      // deliberately never acknowledges an authentication method it cannot do.
      for (size_t i=0; i+2<=n && o[i+1]>=2 && i+o[i+1]<=n; i+=o[i+1]) {
        if (o[i] == 3 && o[i+1] == 5 && be16(o+i+2) == PPP_CHAP && o[i+4] == 0x81) {
          auth_=Auth::MSChapV2;log("PPP: server memilih MS-CHAPv2");
        } else if (o[i] == 3 && o[i+1] == 4 && be16(o+i+2) == PPP_PAP) {
          auth_=Auth::PAP;log("PPP: server memilih PAP");
        } else if (o[i] == 3) {
          log("PPP: server meminta CHAP/MS-CHAP/EAP; mencoba meminta PAP");
          std::vector<uint8_t> pap{3,4,0xc0,0x23};
          sendControl(PPP_LCP, 3, id, pap); return;
        }
      }
      std::vector<uint8_t> opts(o, o+n); sendControl(PPP_LCP, 2, id, opts);peerLcpUp_=true;maybeStartAuth();
    } else if (code == 2 && !lcpUp_) {
      if(id!=lcpRequestId_||n!=lastLcpOptions_.size()||(n&&memcmp(o,lastLcpOptions_.data(),n)!=0)){log("PPP: LCP Configure-Ack tidak cocok dengan request terakhir, diabaikan");return;}
      lcpUp_ = true;log("PPP: LCP Configure-Ack valid");maybeStartAuth();
    } else if(code==3){
      for(size_t i=0;i+2<=n&&o[i+1]>=2&&i+o[i+1]<=n;i+=o[i+1])if(o[i]==1&&o[i+1]==4)requestedMru_=be16(o+i+2);
      log("PPP: LCP Configure-Nak diterima, mengirim proposal baru");sendLcpRequest(false);
    } else if(code==4){
      for(size_t i=0;i+2<=n&&o[i+1]>=2&&i+o[i+1]<=n;i+=o[i+1]){if(o[i]==1)requestMru_=false;if(o[i]==5)requestMagic_=false;}
      log("PPP: LCP Configure-Reject diterima, menghapus opsi yang ditolak");sendLcpRequest(false);
    } else if (code == 9) { // Echo-Request
      std::vector<uint8_t> body(o, o+n); sendControl(PPP_LCP, 10, id, body);
    } else if (code == 5) { std::vector<uint8_t> empty; sendControl(PPP_LCP, 6, id, empty); }
  }
  void maybeStartAuth(){if(!lcpUp_||!peerLcpUp_||authStarted_)return;if(auth_==Auth::PAP){authStarted_=true;log("PPP: LCP tersambung");sendPap();}else if(auth_==Auth::None){authStarted_=true;authUp_=true;log("PPP: server tidak meminta autentikasi");sendIpcpRequest();}else log("PPP: LCP tersambung, menunggu MS-CHAPv2 Challenge");}
  void handlePap(uint8_t code, const uint8_t *body, size_t len) {
    if (code == 2 && !authUp_) {
      authUp_ = true; log("PPP RX: PAP Authenticate-Ack, login berhasil"); sendIpcpRequest();
    } else if (code == 3) {
      char message[96] = "PPP RX: PAP Authenticate-Nak, username/password ditolak";
      if (len && body[0] && size_t(body[0])+1 <= len) {
        snprintf(message, sizeof(message), "PPP RX: PAP ditolak: %.*s", int(body[0]), (const char*)body+1);
      }
      log(message);
    }
  }
  void handleChap(uint8_t code,uint8_t id,const uint8_t *body,size_t len){
    logCode("PPP RX: CHAP",code,id);
    if(code==1){
      if(auth_!=Auth::MSChapV2||len<17||body[0]!=16){log("PPP: CHAP Challenge tidak didukung/invalid");return;}
      memcpy(authChallenge_,body+1,16);
      if(!MSChapV2::generate(user_.c_str(),pass_.c_str(),authChallenge_,peerChallenge_,ntResponse_)){log("PPP: kalkulasi MS-CHAPv2 gagal");return;}
      std::vector<uint8_t> response;response.push_back(49);response.insert(response.end(),peerChallenge_,peerChallenge_+16);
      response.insert(response.end(),8,0);response.insert(response.end(),ntResponse_,ntResponse_+24);response.push_back(0);
      response.insert(response.end(),user_.c_str(),user_.c_str()+user_.length());
      authStarted_=true;log("PPP TX: MS-CHAPv2 Response (NT-Response dihitung)");sendControl(PPP_CHAP,2,id,response);
    }else if(code==3){
      if(!MSChapV2::verifyAuthenticator(user_.c_str(),pass_.c_str(),authChallenge_,peerChallenge_,ntResponse_,body,len)){log("PPP: MS-CHAPv2 Success memiliki authenticator response tidak valid");return;}
      if(!authUp_){authUp_=true;log("PPP: MS-CHAPv2 berhasil dan server terverifikasi");sendIpcpRequest();}
    }else if(code==4){char message[128];snprintf(message,sizeof(message),"PPP: MS-CHAPv2 ditolak: %.*s",int(std::min<size_t>(len,90)),(const char*)body);log(message);}
  }
  void handleIpcp(uint8_t code, uint8_t id, const uint8_t *o, size_t n) {
    logCode("PPP RX: IPCP", code, id);
    if (code == 1) { std::vector<uint8_t> opts(o,o+n); sendControl(PPP_IPCP,2,id,opts); return; }
    if (code == 3) { // Configure-Nak contains the address/DNS suggested by peer.
      for (size_t i=0; i+2<=n && o[i+1]>=2 && i+o[i+1]<=n; i+=o[i+1]) {
        if (o[i+1] == 6 && o[i] == 3) local_ = IPAddress(o[i+2],o[i+3],o[i+4],o[i+5]);
        if (o[i+1] == 6 && (o[i] == 129 || o[i] == 131)) dns_ = IPAddress(o[i+2],o[i+3],o[i+4],o[i+5]);
      }
      std::vector<uint8_t> opts(o,o+n); sendControl(PPP_IPCP,1,nextId_++,opts);
    } else if (code == 2) { ipcpUp_ = true; log("PPP: IPCP selesai, tunnel IPv4 aktif"); }
  }
  void sendControl(uint16_t proto, uint8_t code, uint8_t id, const std::vector<uint8_t> &body) {
    std::vector<uint8_t> p{code,id}; put16(p, uint16_t(body.size()+4));
    p.insert(p.end(), body.begin(), body.end()); sendProtocol(proto,p.data(),p.size());
  }
  bool sendProtocol(uint16_t proto, const uint8_t *data, size_t len) {
    if (!send_) return false;
    // Mirror this peer's PPP representation (it sends FF 03 on every frame).
    std::vector<uint8_t> p{0xff,0x03,uint8_t(proto>>8),uint8_t(proto)};
    p.insert(p.end(),data,data+len);
    if (proto != PPP_IP && len >= 2) logCode("PPP TX",data[0],data[1]);
    const bool ok=send_(p.data(),p.size(),sendCtx_);if(!ok)log("PPP TX: transport gagal mengirim frame");return ok;
  }
  void log(const char *message) { if (logger_) logger_(message, logCtx_); }
  void logCode(const char *prefix, uint8_t code, uint8_t id) {
    char message[80]; snprintf(message,sizeof(message),"%s code=%u id=%u",prefix,unsigned(code),unsigned(id)); log(message);
  }
  enum class Auth:uint8_t{None,PAP,MSChapV2};
  String user_, pass_; Sender send_ = nullptr; void *sendCtx_ = nullptr;
  IPv4Handler rx_ = nullptr; void *rxCtx_ = nullptr; uint8_t nextId_=1;
  uint8_t lcpRequestId_=0;uint32_t lastLcpRequest_=0,lcpMagic_=0;
  uint16_t requestedMru_=1400;bool requestMru_=true,requestMagic_=true;std::vector<uint8_t>lastLcpOptions_;
  LogHandler logger_ = nullptr; void *logCtx_ = nullptr;
  bool lcpUp_=false,peerLcpUp_=false,authStarted_=false,authUp_=false,ipcpUp_=false;
  Auth auth_=Auth::None;uint8_t authChallenge_[16],peerChallenge_[16],ntResponse_[24];IPAddress local_,dns_;
};

} // namespace

class ESP32VPN::Impl {
 public:
  bool openUdp(uint16_t localPort) {
    udpSocket=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if(udpSocket<0){log("L2TP UDP: socket() gagal");return false;}
    // Pin the outer L2TP transport to Wi-Fi before the VPN netif becomes the
    // default route. Otherwise an L2TP packet could be routed into itself.
    struct netif *n=nullptr;const IPAddress wifi=WiFi.localIP();
    for(n=netif_list;n;n=n->next){
      const ip4_addr_t *a=netif_ip4_addr(n);
      if(a&&ip4_addr1(a)==wifi[0]&&ip4_addr2(a)==wifi[1]&&
         ip4_addr3(a)==wifi[2]&&ip4_addr4(a)==wifi[3])break;
    }
    if(!n){logf("L2TP UDP: netif dengan IP Wi-Fi %s tidak ditemukan",wifi.toString().c_str());closeUdp();return false;}
    char ifname[NETIF_NAMESIZE]={n->name[0],n->name[1],0};
    snprintf(ifname+2,sizeof(ifname)-2,"%u",unsigned(n->num));
    struct ifreq iface{};
    strncpy(iface.ifr_name,ifname,sizeof(iface.ifr_name)-1);
    if(setsockopt(udpSocket,SOL_SOCKET,SO_BINDTODEVICE,&iface,sizeof(iface))<0){
      logf("L2TP UDP: SO_BINDTODEVICE ke %s gagal",ifname);
      closeUdp();return false;
    }
    sockaddr_in local{};local.sin_family=AF_INET;local.sin_port=htons(localPort);
    local.sin_addr.s_addr=INADDR_ANY;
    if(bind(udpSocket,reinterpret_cast<sockaddr*>(&local),sizeof(local))<0){logf("L2TP UDP: bind port lokal %u gagal",unsigned(localPort));closeUdp();return false;}
    const int flags=fcntl(udpSocket,F_GETFL,0);
    if(flags<0||fcntl(udpSocket,F_SETFL,flags|O_NONBLOCK)<0){log("L2TP UDP: pengaturan nonblocking gagal");closeUdp();return false;}
    logf("L2TP: transport dikunci ke interface %s",ifname);
    return true;
  }
  void closeUdp(){if(udpSocket>=0){close(udpSocket);udpSocket=-1;}}
  void hex(const char *prefix,const uint8_t *data,size_t length,size_t limit=48){
    char line[192];size_t used=snprintf(line,sizeof(line),"%s [%u]:",prefix,unsigned(length));
    const size_t count=std::min(length,limit);for(size_t i=0;i<count&&used+4<sizeof(line);i++)used+=snprintf(line+used,sizeof(line)-used," %02X",data[i]);
    if(count<length&&used+5<sizeof(line))snprintf(line+used,sizeof(line)-used," ...");
    log(line);
  }
  bool begin(const Config &c) {
    stop(); cfg=c; error="";
    if (!c.server || !c.username || !c.password) return fail("server/username/password wajib diisi");
    started=lastProgress=lastPeerActivity=millis();lastHelloSent=0;
    setState(State::Connecting,"memulai koneksi");
    localTunnel=uint16_t(esp_random()|1);localSession=uint16_t(esp_random()|1);
    ppp.configure(c.username,c.password,&Impl::pppSendThunk,this,rx,rxCtx,
                  c.debug?logger:nullptr,logCtx);
    const uint16_t port=c.port?c.port:(c.protocol==Protocol::L2TP?1701:443);
    logf("Target: protocol=%s server=%s port=%u user=%s timeout=%lu ms",
         c.protocol==Protocol::L2TP?"L2TP":"SSTP",c.server,unsigned(port),c.username,static_cast<unsigned long>(c.timeoutMs));
    IPAddress resolved;
    log("DNS: mencari alamat server...");
    if (!WiFi.hostByName(c.server,resolved)) return fail("DNS server VPN gagal ditemukan");
    serverIP=resolved;
    serverPort=port;
    logf("DNS: %s -> %s",c.server,resolved.toString().c_str());
    if (c.protocol == Protocol::L2TP) {
      if (!openUdp(c.localPort)) return fail("gagal membuka/mengunci socket UDP L2TP ke Wi-Fi");
      logf("L2TP: socket UDP lokal berhasil dibuka pada port %u",unsigned(c.localPort));
      sendSccrq();
    } else {
      if (c.insecureTls) tls.setInsecure();
      else if (c.caCertificate) tls.setCACert(c.caCertificate);
      else return fail("SSTP memerlukan caCertificate atau insecureTls");
      const uint16_t port = c.port ? c.port : 443;
      logf("SSTP: membuka TLS ke %s:%u (%s)",c.server,unsigned(port),c.insecureTls?"tanpa verifikasi sertifikat":"verifikasi CA aktif");
      if (!tls.connect(c.server,port)) return fail("koneksi TLS gagal");
      log("SSTP: TLS tersambung");
      sendSstpHttp();
    }
    return true;
  }
  void loop() {
    if (state==State::Idle) return;
    if (state==State::Error) {
      if(cfg.autoReconnect && int32_t(millis()-reconnectAt)>=0){
        Config retry=cfg;log("Reconnect: mencoba membangun ulang tunnel VPN");begin(retry);
      }
      return;
    }
    if(pppStarted)ppp.loop();
    if (state!=State::Connected && millis()-lastProgress > cfg.timeoutMs) {
      logf("Timeout: tidak ada kemajuan selama %lu ms, state=%s",static_cast<unsigned long>(millis()-lastProgress),stateName(state));
      fail("negosiasi VPN timeout"); return;
    }
    // Drain a short burst: TCP/HTTP commonly produces several consecutive
    // L2TP datagrams, and one packet per Arduino loop adds visible latency.
    if (cfg.protocol==Protocol::L2TP) {
      for(uint8_t i=0;i<8;i++) if(!pollL2tp()) break;
    } else pollSstp();
    if(state==State::Connected && cfg.protocol==Protocol::L2TP){
      const uint32_t now=millis();
      if(cfg.keepaliveTimeoutMs && now-lastPeerActivity>=cfg.keepaliveTimeoutMs){
        fail("peer L2TP tidak merespons keepalive");return;
      }
      if(cfg.keepaliveIntervalMs && now-lastPeerActivity>=cfg.keepaliveIntervalMs &&
         now-lastHelloSent>=cfg.keepaliveIntervalMs){
        lastHelloSent=now;log("L2TP TX: HELLO keepalive");sendControl(6);
      }
    }
    if (ppp.up() && state!=State::Connected) {
      setState(State::Connected,"VPN siap digunakan");
      logf("Alamat VPN: IP=%s DNS=%s",ppp.localIP().toString().c_str(),ppp.dnsIP().toString().c_str());
    }
  }
  void stop() { closeUdp();tls.stop();state=State::Idle;pppStarted=false;httpDone=false;dataSequencing=false;dataNs=dataNr=0;peerTunnel=peerSession=0;ns=nr=0;serverPort=0;sstpBuf.clear(); }
  bool fail(const char *s) { error=s; logf("ERROR: %s",s); state=State::Error;reconnectAt=millis()+cfg.reconnectDelayMs; return false; }
  void log(const char *s) { if (cfg.debug && logger) logger(s,logCtx); }
  void logf(const char *format,...) { char text[192];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);log(text); }
  static const char *stateName(State s) { const char*n[]={"Idle","Connecting","Negotiating","Authenticating","Configuring","Connected","Error"};return n[unsigned(s)]; }
  void setState(State next,const char *reason) { if(state!=next){state=next;lastProgress=millis();logf("STATE -> %s: %s",stateName(next),reason);} }
  static bool pppSendThunk(const uint8_t *p,size_t n,void *c) { return static_cast<Impl*>(c)->sendPpp(p,n); }
  bool sendUdp(const uint8_t*data,size_t length,const char*kind){
    if(udpSocket<0)return false;sockaddr_in remote{};remote.sin_family=AF_INET;remote.sin_port=htons(serverPort);
    const uint8_t address[4]={serverIP[0],serverIP[1],serverIP[2],serverIP[3]};
    memcpy(&remote.sin_addr.s_addr,address,4);
    const int sent=sendto(udpSocket,data,length,0,reinterpret_cast<sockaddr*>(&remote),sizeof(remote));
    if(sent!=int(length)){logf("%s: sendto ke %s:%u gagal",kind,serverIP.toString().c_str(),unsigned(serverPort));return false;}return true;
  }
  bool sendPpp(const uint8_t *p,size_t n) {
    if (cfg.protocol==Protocol::L2TP) {
      if (!peerSession) { log("PPP TX gagal: session L2TP belum tersedia"); return false; }
      // Mirror the LNS data header: no Length field unless sequencing is used.
      std::vector<uint8_t>h{uint8_t(dataSequencing?0x08:0x00),0x02,uint8_t(peerTunnel>>8),uint8_t(peerTunnel),uint8_t(peerSession>>8),uint8_t(peerSession)};
      if(dataSequencing){put16(h,dataNs++);put16(h,dataNr);}
      if(cfg.packetDump)logf("L2TP DATA TX: tunnel=%u session=%u seq=%s Ns=%u Nr=%u PPP=%u byte",unsigned(peerTunnel),unsigned(peerSession),dataSequencing?"on":"off",unsigned(dataNs-(dataSequencing?1:0)),unsigned(dataNr),unsigned(n));
      std::vector<uint8_t>datagram(h);datagram.insert(datagram.end(),p,p+n);if(cfg.packetDump)hex("L2TP DATA TX HEX",datagram.data(),datagram.size());
      return sendUdp(datagram.data(),datagram.size(),"L2TP DATA TX");
    }
    if (!tls.connected()) { log("PPP TX gagal: TLS SSTP terputus"); return false; }
    std::vector<uint8_t> f{0x10,0x00}; put16(f,uint16_t(n+4)); f.insert(f.end(),p,p+n);
    return tls.write(f.data(),f.size())==f.size();
  }

  void avp(std::vector<uint8_t>&v,uint16_t type,const uint8_t *d,size_t n,bool mandatory=true) {
    put16(v,uint16_t((mandatory?0x8000:0)|uint16_t(n+6))); put16(v,0); put16(v,type); v.insert(v.end(),d,d+n);
  }
  void avp16(std::vector<uint8_t>&v,uint16_t type,uint16_t x) { uint8_t d[2]={uint8_t(x>>8),uint8_t(x)}; avp(v,type,d,2); }
  void sendControl(uint16_t msg,const std::vector<uint8_t>&extra={},uint16_t session=0) {
    std::vector<uint8_t> b; uint8_t m[2]={uint8_t(msg>>8),uint8_t(msg)}; avp(b,0,m,2);
    b.insert(b.end(),extra.begin(),extra.end()); std::vector<uint8_t> p{0xc8,0x02,0,0};
    put16(p,peerTunnel);put16(p,session);put16(p,ns++);put16(p,nr);p.insert(p.end(),b.begin(),b.end());
    p[2]=p.size()>>8;p[3]=p.size(); logf("L2TP TX: %s(%u) tunnel=%u session=%u Ns=%u Nr=%u",l2tpName(msg),unsigned(msg),unsigned(peerTunnel),unsigned(session),unsigned(ns-1),unsigned(nr));
    sendUdp(p.data(),p.size(),"L2TP CONTROL TX");
  }
  void sendZlb(){
    std::vector<uint8_t>p{0xc8,0x02,0,12};put16(p,peerTunnel);put16(p,0);put16(p,ns);put16(p,nr);
    logf("L2TP TX: ZLB-ACK tunnel=%u Ns=%u Nr=%u",unsigned(peerTunnel),unsigned(ns),unsigned(nr));
    sendUdp(p.data(),p.size(),"L2TP ZLB TX");
  }
  static const char *l2tpName(uint16_t m){switch(m){case 1:return"SCCRQ";case 2:return"SCCRP";case 3:return"SCCCN";case 6:return"HELLO";case 10:return"ICRQ";case 11:return"ICRP";case 12:return"ICCN";case 14:return"CDN";case 4:return"StopCCN";default:return"Control";}}
  void sendSccrq() {
    std::vector<uint8_t>x; uint8_t ver[2]={1,0};avp(x,2,ver,2); uint8_t caps[4]={0,0,0,3};avp(x,3,caps,4);
    avp16(x,9,localTunnel); const char *host="esp32";avp(x,7,(const uint8_t*)host,6);sendControl(1,x); setState(State::Negotiating,"menunggu jawaban L2TP SCCRP");
  }
  bool pollL2tp() {
    uint8_t packet[1600];sockaddr_in remote{};socklen_t remoteLen=sizeof(remote);
    int n=recvfrom(udpSocket,packet,sizeof(packet),0,reinterpret_cast<sockaddr*>(&remote),&remoteLen);
    if(n<=0)return false;std::vector<uint8_t>b(packet,packet+n);IPAddress remoteIP(reinterpret_cast<uint8_t*>(&remote.sin_addr.s_addr));const uint16_t remotePort=ntohs(remote.sin_port);
    if(remoteIP!=serverIP){logf("L2TP RX: paket dari peer asing %s diabaikan",remoteIP.toString().c_str());return true;}
    lastPeerActivity=millis();
    const bool packetIsControl=(packet[0]&0x80)!=0;
    if(packetIsControl||cfg.packetDump)logf("L2TP RX: %d byte dari %s:%u",n,remoteIP.toString().c_str(),unsigned(remotePort));
    if(remoteIP==serverIP && serverPort!=remotePort){
      serverPort=remotePort;logf("L2TP: port peer dikunci ke %u",unsigned(serverPort));
    }
    if(n<6){log("L2TP RX: paket terlalu pendek, diabaikan");return true;} bool ctrl=b[0]&0x80; size_t off=(b[0]&0x40)?4:2;
    if (ctrl) {
      if(n<12)return true;
      const uint16_t rxNs=be16(b.data()+8),rxNr=be16(b.data()+10);
      const uint16_t declared=(b[0]&0x40)?be16(b.data()+2):uint16_t(n);
      if(declared==12){logf("L2TP RX: ZLB-ACK Ns=%u Nr=%u",unsigned(rxNs),unsigned(rxNr));lastProgress=millis();return true;}
      if(rxNs!=nr){logf("L2TP RX: control duplicate/out-of-order Ns=%u, expected=%u",unsigned(rxNs),unsigned(nr));sendZlb();return true;}
      nr=uint16_t(nr+1);off=12;uint16_t msg=0,assignedTunnel=0,assignedSession=0;
      while(off+6<=b.size()) {uint16_t l=be16(b.data()+off)&0x3ff; if(l<6||off+l>b.size())break;uint16_t t=be16(b.data()+off+4);if(t==0&&l>=8)msg=be16(b.data()+off+6);if(t==9&&l>=8)assignedTunnel=be16(b.data()+off+6);if(t==14&&l>=8)assignedSession=be16(b.data()+off+6);if(t==1&&l>=10)logf("L2TP: Result-Code=%u Error-Code=%u",unsigned(be16(b.data()+off+6)),unsigned(be16(b.data()+off+8)));off+=l;}
      logf("L2TP RX: %s(%u), assignedTunnel=%u assignedSession=%u",l2tpName(msg),unsigned(msg),unsigned(assignedTunnel),unsigned(assignedSession));
      lastProgress=millis();
      if(msg==2){peerTunnel=assignedTunnel;logf("L2TP: control tunnel terbentuk, peerTunnel=%u",unsigned(peerTunnel));sendControl(3);std::vector<uint8_t>x;avp16(x,14,localSession);uint8_t serial[4]={0,0,0,1};avp(x,15,serial,4);sendControl(10,x);}
      else if(msg==11){peerSession=assignedSession;logf("L2TP: data session diterima, peerSession=%u",unsigned(peerSession));std::vector<uint8_t>x;uint8_t speed[4]={0x00,0x98,0x96,0x80};uint8_t synchronous[4]={0,0,0,1};avp(x,24,speed,4);avp(x,19,synchronous,4);sendControl(12,x,peerSession);pppStarted=true;setState(State::Authenticating,"memulai autentikasi PPP");ppp.start();}
      else if(msg==6)sendZlb();
    } else {
      lastProgress=millis();
      if(cfg.packetDump)hex("L2TP DATA RX HEX",b.data(),b.size());
      off=2; if(b[0]&0x40)off+=2; off+=4; // optional length, tunnel, session
      dataSequencing=(b[0]&0x08)!=0;
      if(dataSequencing){if(off+4>b.size())return true;const uint16_t rxNs=be16(b.data()+off);dataNr=rxNs+1;if(cfg.packetDump)logf("L2TP DATA RX: sequencing aktif Ns=%u Nr=%u; balasan akan memakai sequence",unsigned(rxNs),unsigned(be16(b.data()+off+2)));off+=4;}
      else if(cfg.packetDump)log("L2TP DATA RX: sequencing nonaktif");
      if(b[0]&0x02){if(off>=b.size())return true;const uint8_t pad=b[off];off+=1+pad;} // optional offset pad
      if(off<b.size())ppp.input(b.data()+off,b.size()-off);
    }
    return true;
  }
  void sendSstpHttp() {
    String h=String("SSTP_DUPLEX_POST /sra_{BA195980-CD49-458b-9E23-C84EE0ADCD75}/ HTTP/1.1\r\nHost: ")+cfg.server+"\r\nContent-Length: 18446744073709551615\r\nSSTPCORRELATIONID: {00000000-0000-0000-0000-000000000000}\r\n\r\n";
    tls.write((const uint8_t*)h.c_str(),h.length()); log("SSTP TX: HTTP SSTP_DUPLEX_POST");setState(State::Negotiating,"menunggu HTTP 200 dari server SSTP");
  }
  void pollSstp() {
    while(tls.available())sstpBuf.push_back(tls.read());
    if(!httpDone){auto it=std::search(sstpBuf.begin(),sstpBuf.end(),(const uint8_t*)"\r\n\r\n",(const uint8_t*)"\r\n\r\n"+4);if(it==sstpBuf.end())return;String status((const char*)sstpBuf.data(),std::min<size_t>(sstpBuf.size(),32));logf("SSTP RX: %s",status.c_str());if(status.indexOf("200")<0){fail("server SSTP menolak HTTP");return;}sstpBuf.erase(sstpBuf.begin(),it+4);httpDone=true;lastProgress=millis();sendSstpConnect();}
    while(sstpBuf.size()>=4){uint16_t n=be16(sstpBuf.data()+2);if(n<4||n>8192){fail("frame SSTP tidak valid");return;}if(sstpBuf.size()<n)return;bool control=sstpBuf[1]&1;lastProgress=millis();if(control&&n>=8){uint16_t msg=be16(sstpBuf.data()+4);logf("SSTP RX: control message=%u length=%u",unsigned(msg),unsigned(n));if(msg==2){pppStarted=true;setState(State::Authenticating,"SSTP Call-Connect-Ack diterima");ppp.start();}}else if(!control)ppp.input(sstpBuf.data()+4,n-4);sstpBuf.erase(sstpBuf.begin(),sstpBuf.begin()+n);}
  }
  void sendSstpConnect(){uint8_t p[]={0x10,0x01,0x00,0x0e,0x00,0x01,0x00,0x01,0x00,0x06,0x00,0x01,0x00,0x01};tls.write(p,sizeof(p));log("SSTP TX: Call-Connect-Request");}

  Config cfg; State state=State::Idle; String error; uint32_t started=0,lastProgress=0;
  uint32_t lastPeerActivity=0,lastHelloSent=0,reconnectAt=0;
  int udpSocket=-1; WiFiClientSecure tls; PPP ppp;IPAddress serverIP;uint16_t serverPort=0;bool pppStarted=false,httpDone=false;
  uint16_t localTunnel=0x1001,localSession=0x2001,peerTunnel=0,peerSession=0,ns=0,nr=0;
  bool dataSequencing=false;uint16_t dataNs=0,dataNr=0;
  std::vector<uint8_t> sstpBuf; IPv4Handler rx=nullptr;void*rxCtx=nullptr;LogHandler logger=nullptr;void*logCtx=nullptr;
};

ESP32VPN::ESP32VPN():_impl(new Impl){}
ESP32VPN::~ESP32VPN(){delete _impl;}
bool ESP32VPN::begin(const Config&c){return _impl->begin(c);} void ESP32VPN::loop(){_impl->loop();}
void ESP32VPN::stop(){_impl->stop();} State ESP32VPN::state()const{return _impl->state;}
bool ESP32VPN::connected()const{return _impl->state==State::Connected;} const char* ESP32VPN::lastError()const{return _impl->error.c_str();}
IPAddress ESP32VPN::localIP()const{return _impl->ppp.localIP();} IPAddress ESP32VPN::dnsIP()const{return _impl->ppp.dnsIP();}
bool ESP32VPN::sendIPv4(const uint8_t*p,size_t n){return _impl->ppp.sendIPv4(p,n);}
void ESP32VPN::onIPv4(IPv4Handler h,void*c){_impl->rx=h;_impl->rxCtx=c;_impl->ppp.setReceiver(h,c);}
void ESP32VPN::onLog(LogHandler h,void*c){_impl->logger=h;_impl->logCtx=c;}

} // namespace esp32vpn
