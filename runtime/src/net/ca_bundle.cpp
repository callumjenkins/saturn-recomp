// saturn-recomp runtime — the compiled-in CA certificates (ca_bundle.h).
#include "ca_bundle.h"
#include "cacert.h"

const std::string& ca_bundle_pem() {
    static const std::string pem(reinterpret_cast<const char*>(cacert_pem), sizeof cacert_pem);
    return pem;
}
