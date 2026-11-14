#pragma once
#include <stddef.h>
#define MBEDTLS_MD_SHA256 1
const void *mbedtls_md_info_from_type(int);
int mbedtls_md(const void *, const unsigned char *, size_t, unsigned char *);
