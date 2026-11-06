/* Small libc extensions used by the bundled LeanSDR code. */
#ifndef LINRAD_LEAN_WIN32_COMPAT_H
#define LINRAD_LEAN_WIN32_COMPAT_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
static inline double drand48(void) {
    static thread_local uint64_t state=UINT64_C(0x1234abcd330e);
    state=(state*UINT64_C(0x5deece66d)+11)&UINT64_C(0xffffffffffff);
    return (double)state/281474976710656.0;
}
static inline float exp10f(float x) { return powf(10.0f,x); }
static inline int inet_aton(const char *text,struct in_addr *address) { return inet_pton(AF_INET,text,address); }
static inline char *strndup(const char *text,size_t size) {
    size_t n=strnlen(text,size); char *copy=(char *)malloc(n+1);
    if (copy) { memcpy(copy,text,n); copy[n]=0; }
    return copy;
}
#endif
