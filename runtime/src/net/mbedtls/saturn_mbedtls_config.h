// Mbed TLS as saturn-recomp builds it, on top of its default configuration: locking through the runtime's own
// mutexes (mbedtls_threads.cpp), since libdatachannel's threads share PSA's global state, and the DTLS-SRTP
// extension, which libdatachannel's DTLS sets up even without media.
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
#define MBEDTLS_SSL_DTLS_SRTP
