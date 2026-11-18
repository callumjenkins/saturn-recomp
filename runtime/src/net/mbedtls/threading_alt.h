// The mutex Mbed TLS locks with MBEDTLS_THREADING_ALT: a std::mutex, made and freed in mbedtls_threads.cpp.
#ifndef SATURN_THREADING_ALT_H
#define SATURN_THREADING_ALT_H
typedef struct mbedtls_threading_mutex_t {
    void* mutex;
} mbedtls_threading_mutex_t;
#endif
