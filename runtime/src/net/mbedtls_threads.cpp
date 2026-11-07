// saturn-recomp runtime — Mbed TLS's mutexes (mbedtls/threading_alt.h) as std::mutex, on every platform.
#include "mbedtls_threads.h"
#include <mbedtls/threading.h>
#include <mutex>
#include <new>

namespace {

void init(mbedtls_threading_mutex_t* m) { m->mutex = new (std::nothrow) std::mutex; }

void free_mutex(mbedtls_threading_mutex_t* m) {
    delete static_cast<std::mutex*>(m->mutex);
    m->mutex = nullptr;
}

int lock(mbedtls_threading_mutex_t* m) {
    if (!m->mutex) return MBEDTLS_ERR_THREADING_MUTEX_ERROR;
    static_cast<std::mutex*>(m->mutex)->lock();
    return 0;
}

int unlock(mbedtls_threading_mutex_t* m) {
    if (!m->mutex) return MBEDTLS_ERR_THREADING_MUTEX_ERROR;
    static_cast<std::mutex*>(m->mutex)->unlock();
    return 0;
}

}  // namespace

void mbedtls_threads_start() {
    static std::once_flag once;
    std::call_once(once, [] { mbedtls_threading_set_alt(init, free_mutex, lock, unlock); });
}
