#include "MSChapV2.h"

#include <cstring>
#include <vector>
#include <esp_system.h>
#include <mbedtls/sha1.h>

namespace esp32vpn {
namespace {

static uint32_t rol(uint32_t x,uint8_t n){return (x<<n)|(x>>(32-n));}
static uint32_t le32(const uint8_t*p){return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
static void putle(uint8_t*p,uint32_t x){p[0]=x;p[1]=x>>8;p[2]=x>>16;p[3]=x>>24;}

// Compact RFC 1320 MD4, used for the NT password hash.
static void md4(const uint8_t *data,size_t length,uint8_t out[16]){
  std::vector<uint8_t> m(data,data+length);m.push_back(0x80);
  while((m.size()%64)!=56)m.push_back(0);
  uint64_t bits=uint64_t(length)*8;
  for(int i=0;i<8;i++)m.push_back(bits>>(8*i));
  uint32_t a0=0x67452301,b0=0xefcdab89,c0=0x98badcfe,d0=0x10325476;
  const uint8_t s1[]={3,7,11,19},s2[]={3,5,9,13},s3[]={3,9,11,15};
  for(size_t off=0;off<m.size();off+=64){uint32_t x[16];for(int i=0;i<16;i++)x[i]=le32(&m[off+i*4]);uint32_t a=a0,b=b0,c=c0,d=d0;
    for(int i=0;i<16;i++){uint32_t f=(b&c)|(~b&d);uint32_t t=rol(a+f+x[i],s1[i%4]);a=d;d=c;c=b;b=t;}
    for(int i=0;i<16;i++){int k=(i%4)*4+i/4;uint32_t f=(b&c)|(b&d)|(c&d);uint32_t t=rol(a+f+x[k]+0x5a827999,s2[i%4]);a=d;d=c;c=b;b=t;}
    const uint8_t order[]={0,8,4,12,2,10,6,14,1,9,5,13,3,11,7,15};
    for(int i=0;i<16;i++){uint32_t f=b^c^d;uint32_t t=rol(a+f+x[order[i]]+0x6ed9eba1,s3[i%4]);a=d;d=c;c=b;b=t;}
    a0+=a;b0+=b;c0+=c;d0+=d;
  }putle(out,a0);putle(out+4,b0);putle(out+8,c0);putle(out+12,d0);
}

static const uint8_t IP[]={58,50,42,34,26,18,10,2,60,52,44,36,28,20,12,4,62,54,46,38,30,22,14,6,64,56,48,40,32,24,16,8,57,49,41,33,25,17,9,1,59,51,43,35,27,19,11,3,61,53,45,37,29,21,13,5,63,55,47,39,31,23,15,7};
static const uint8_t FP[]={40,8,48,16,56,24,64,32,39,7,47,15,55,23,63,31,38,6,46,14,54,22,62,30,37,5,45,13,53,21,61,29,36,4,44,12,52,20,60,28,35,3,43,11,51,19,59,27,34,2,42,10,50,18,58,26,33,1,41,9,49,17,57,25};
static const uint8_t E[]={32,1,2,3,4,5,4,5,6,7,8,9,8,9,10,11,12,13,12,13,14,15,16,17,16,17,18,19,20,21,20,21,22,23,24,25,24,25,26,27,28,29,28,29,30,31,32,1};
static const uint8_t P[]={16,7,20,21,29,12,28,17,1,15,23,26,5,18,31,10,2,8,24,14,32,27,3,9,19,13,30,6,22,11,4,25};
static const uint8_t PC1[]={57,49,41,33,25,17,9,1,58,50,42,34,26,18,10,2,59,51,43,35,27,19,11,3,60,52,44,36,63,55,47,39,31,23,15,7,62,54,46,38,30,22,14,6,61,53,45,37,29,21,13,5,28,20,12,4};
static const uint8_t PC2[]={14,17,11,24,1,5,3,28,15,6,21,10,23,19,12,4,26,8,16,7,27,20,13,2,41,52,31,37,47,55,30,40,51,45,33,48,44,49,39,56,34,53,46,42,50,36,29,32};
static const uint8_t SH[]={1,1,2,2,2,2,2,2,1,2,2,2,2,2,2,1};
static const uint8_t S[8][64]={
{14,4,13,1,2,15,11,8,3,10,6,12,5,9,0,7,0,15,7,4,14,2,13,1,10,6,12,11,9,5,3,8,4,1,14,8,13,6,2,11,15,12,9,7,3,10,5,0,15,12,8,2,4,9,1,7,5,11,3,14,10,0,6,13},
{15,1,8,14,6,11,3,4,9,7,2,13,12,0,5,10,3,13,4,7,15,2,8,14,12,0,1,10,6,9,11,5,0,14,7,11,10,4,13,1,5,8,12,6,9,3,2,15,13,8,10,1,3,15,4,2,11,6,7,12,0,5,14,9},
{10,0,9,14,6,3,15,5,1,13,12,7,11,4,2,8,13,7,0,9,3,4,6,10,2,8,5,14,12,11,15,1,13,6,4,9,8,15,3,0,11,1,2,12,5,10,14,7,1,10,13,0,6,9,8,7,4,15,14,3,11,5,2,12},
{7,13,14,3,0,6,9,10,1,2,8,5,11,12,4,15,13,8,11,5,6,15,0,3,4,7,2,12,1,10,14,9,10,6,9,0,12,11,7,13,15,1,3,14,5,2,8,4,3,15,0,6,10,1,13,8,9,4,5,11,12,7,2,14},
{2,12,4,1,7,10,11,6,8,5,3,15,13,0,14,9,14,11,2,12,4,7,13,1,5,0,15,10,3,9,8,6,4,2,1,11,10,13,7,8,15,9,12,5,6,3,0,14,11,8,12,7,1,14,2,13,6,15,0,9,10,4,5,3},
{12,1,10,15,9,2,6,8,0,13,3,4,14,7,5,11,10,15,4,2,7,12,9,5,6,1,13,14,0,11,3,8,9,14,15,5,2,8,12,3,7,0,4,10,1,13,11,6,4,3,2,12,9,5,15,10,11,14,1,7,6,0,8,13},
{4,11,2,14,15,0,8,13,3,12,9,7,5,10,6,1,13,0,11,7,4,9,1,10,14,3,5,12,2,15,8,6,1,4,11,13,12,3,7,14,10,15,6,8,0,5,9,2,6,11,13,8,1,4,10,7,9,5,0,15,14,2,3,12},
{13,2,8,4,6,15,11,1,10,9,3,14,5,0,12,7,1,15,13,8,10,3,7,4,12,5,6,11,0,14,9,2,7,11,4,1,9,12,14,2,0,6,10,13,15,3,5,8,2,1,14,7,4,10,8,13,15,12,9,0,3,5,6,11}};

static uint64_t perm(uint64_t x,const uint8_t*t,int n,int inbits){uint64_t y=0;for(int i=0;i<n;i++)y=(y<<1)|((x>>(inbits-t[i]))&1);return y;}
static uint64_t load64(const uint8_t*p){uint64_t x=0;for(int i=0;i<8;i++)x=(x<<8)|p[i];return x;}
static void store64(uint8_t*p,uint64_t x){for(int i=7;i>=0;i--){p[i]=x;x>>=8;}}
static uint32_t feistel(uint32_t r,uint64_t key){uint64_t x=perm(r,E,48,32)^key;uint32_t y=0;for(int i=0;i<8;i++){uint8_t v=(x>>(42-6*i))&0x3f;uint8_t row=((v&0x20)>>4)|(v&1),col=(v>>1)&15;y=(y<<4)|S[i][row*16+col];}return uint32_t(perm(y,P,32,32));}
static void des(const uint8_t clear[8],const uint8_t raw[7],uint8_t out[8]){
  uint8_t k[8];k[0]=raw[0]&0xfe;k[1]=uint8_t((raw[0]<<7)|(raw[1]>>1))&0xfe;k[2]=uint8_t((raw[1]<<6)|(raw[2]>>2))&0xfe;k[3]=uint8_t((raw[2]<<5)|(raw[3]>>3))&0xfe;k[4]=uint8_t((raw[3]<<4)|(raw[4]>>4))&0xfe;k[5]=uint8_t((raw[4]<<3)|(raw[5]>>5))&0xfe;k[6]=uint8_t((raw[5]<<2)|(raw[6]>>6))&0xfe;k[7]=uint8_t(raw[6]<<1)&0xfe;
  uint64_t cd=perm(load64(k),PC1,56,64),sub[16];uint32_t c=(cd>>28)&0xfffffff,d=cd&0xfffffff;
  for(int i=0;i<16;i++){c=((c<<SH[i])|(c>>(28-SH[i])))&0xfffffff;d=((d<<SH[i])|(d>>(28-SH[i])))&0xfffffff;sub[i]=perm((uint64_t(c)<<28)|d,PC2,48,56);}
  uint64_t b=perm(load64(clear),IP,64,64);uint32_t l=b>>32,r=b;
  for(int i=0;i<16;i++){uint32_t t=r;r=l^feistel(r,sub[i]);l=t;}store64(out,perm((uint64_t(r)<<32)|l,FP,64,64));
}

static const char *bareUser(const char*u){const char*p=strrchr(u,'\\');return p?p+1:u;}
static void passwordHash(const char*p,uint8_t out[16]){std::vector<uint8_t>u;while(*p){uint8_t c=*p++;u.push_back(c);u.push_back(0);}md4(u.data(),u.size(),out);}
static void challengeHash(const uint8_t peer[16],const uint8_t auth[16],const char*user,uint8_t out[8]){std::vector<uint8_t>b(peer,peer+16);b.insert(b.end(),auth,auth+16);const char*u=bareUser(user);b.insert(b.end(),u,u+strlen(u));uint8_t h[20];mbedtls_sha1_ret(b.data(),b.size(),h);memcpy(out,h,8);}
static void expectedAuth(const char*user,const char*password,const uint8_t auth[16],const uint8_t peer[16],const uint8_t nt[24],uint8_t out[20]){static const uint8_t m1[]="Magic server to client signing constant";static const uint8_t m2[]="Pad to make it do more than one iteration";uint8_t ph[16],phh[16],ch[8];passwordHash(password,ph);md4(ph,16,phh);std::vector<uint8_t>b(phh,phh+16);b.insert(b.end(),nt,nt+24);b.insert(b.end(),m1,m1+sizeof(m1)-1);uint8_t digest[20];mbedtls_sha1_ret(b.data(),b.size(),digest);challengeHash(peer,auth,user,ch);b.assign(digest,digest+20);b.insert(b.end(),ch,ch+8);b.insert(b.end(),m2,m2+sizeof(m2)-1);mbedtls_sha1_ret(b.data(),b.size(),out);}
static uint8_t hexval(uint8_t c){if(c>='0'&&c<='9')return c-'0';if(c>='A'&&c<='F')return c-'A'+10;if(c>='a'&&c<='f')return c-'a'+10;return 255;}
}

bool MSChapV2::generate(const char*user,const char*password,const uint8_t auth[16],uint8_t peer[16],uint8_t nt[24]){for(int i=0;i<16;i+=4){uint32_t r=esp_random();memcpy(peer+i,&r,4);}uint8_t ch[8],ph[16],z[21]={0};challengeHash(peer,auth,user,ch);passwordHash(password,ph);memcpy(z,ph,16);for(int i=0;i<3;i++)des(ch,z+i*7,nt+i*8);return true;}
bool MSChapV2::verifyAuthenticator(const char*user,const char*password,const uint8_t auth[16],const uint8_t peer[16],const uint8_t nt[24],const uint8_t*msg,size_t len){if(len<42||msg[0]!='S'||msg[1]!='=')return false;uint8_t expected[20];expectedAuth(user,password,auth,peer,nt,expected);uint8_t diff=0;for(int i=0;i<20;i++){uint8_t hi=hexval(msg[2+i*2]),lo=hexval(msg[3+i*2]);if(hi>15||lo>15)return false;diff|=expected[i]^uint8_t((hi<<4)|lo);}return diff==0;}

} // namespace esp32vpn
