// saturn-recomp runtime — the CA certificates TLS is checked against: Mozilla's, compiled in.
#pragma once
#include <string>

const std::string& ca_bundle_pem();
