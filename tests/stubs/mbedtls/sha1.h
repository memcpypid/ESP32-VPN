#pragma once
#include <cstddef>
int mbedtls_sha1_ret(const unsigned char *input, size_t length,
                     unsigned char output[20]);

