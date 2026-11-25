#include <cstdio>
#include <cstring>
#include <openssl/sha.h>
#include "../src/MSChapV2.cpp"

static const uint32_t randomWords[]={0x24234021,0x2a265e25,0x2b5f2928,0x7e7c333a};
static int randomIndex=0;
uint32_t esp_random(){return randomWords[randomIndex++];}
int mbedtls_sha1_ret(const unsigned char *input,size_t length,unsigned char output[20]){SHA1(input,length,output);return 0;}

int main(){
  const uint8_t auth[]={0x5b,0x5d,0x7c,0x7d,0x7b,0x3f,0x2f,0x3e,0x3c,0x2c,0x60,0x21,0x32,0x26,0x26,0x28};
  const uint8_t expectedNt[]={0x82,0x30,0x9e,0xcd,0x8d,0x70,0x8b,0x5e,0xa0,0x8f,0xaa,0x39,0x81,0xcd,0x83,0x54,0x42,0x33,0x11,0x4a,0x3d,0x85,0xd6,0xdf};
  const char success[]="S=407A5589115FD0D6209F510FE9C04566932CDA56";
  uint8_t peer[16],nt[24];
  esp32vpn::MSChapV2::generate("User","clientPass",auth,peer,nt);
  bool ntOk=memcmp(nt,expectedNt,24)==0;
  if(!ntOk){printf("Actual NT: ");for(uint8_t b:nt)printf("%02X",b);printf("\n");}
  bool authOk=esp32vpn::MSChapV2::verifyAuthenticator("User","clientPass",auth,peer,nt,(const uint8_t*)success,strlen(success));
  printf("RFC2759 NT-Response: %s\nRFC2759 AuthenticatorResponse: %s\n",ntOk?"PASS":"FAIL",authOk?"PASS":"FAIL");
  return ntOk&&authOk?0:1;
}
