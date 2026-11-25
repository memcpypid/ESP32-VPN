#pragma once

#include <Arduino.h>

namespace esp32vpn {

class MSChapV2 {
 public:
  static bool generate(const char *username, const char *password,
                       const uint8_t authenticatorChallenge[16],
                       uint8_t peerChallenge[16], uint8_t ntResponse[24]);
  static bool verifyAuthenticator(const char *username, const char *password,
                                  const uint8_t authenticatorChallenge[16],
                                  const uint8_t peerChallenge[16],
                                  const uint8_t ntResponse[24],
                                  const uint8_t *message, size_t length);
};

}  // namespace esp32vpn

