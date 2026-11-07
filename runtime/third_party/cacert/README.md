# Mozilla's CA certificates

`cacert.pem` is curl's extract of the root certificates in Mozilla's NSS, as of 2026-11-05
(<https://curl.se/ca/cacert.pem>, SHA-256 `427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285`), unchanged.
A session checks the playtest Worker's certificate against it, the same way on every platform, since Mbed TLS reads no
system store. It is under the Mozilla Public License 2.0, as its header says.
