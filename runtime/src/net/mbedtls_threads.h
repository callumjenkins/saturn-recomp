// saturn-recomp runtime — Mbed TLS's locking, which must be in place before libdatachannel first uses it.
#pragma once

// Gives Mbed TLS its mutexes. Only the first call does anything.
void mbedtls_threads_start();
