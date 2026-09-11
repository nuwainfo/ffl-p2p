/**
SPDX-License-Identifier: Apache-2.0

FastFileLink CLI - Fast, no-fuss file sharing
Copyright (C) 2025-2026 FastFileLink contributors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "quic/Credentials.h"

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include <array>
#include <ctime>
#include <mutex>
#include <stdexcept>
#include <string>

namespace ffl::quic {

namespace {
constexpr char kServerName[] = "ffl-p2p";
std::once_flag gGnuTLSInitOnce;

void checkGnuTLS(int result, const char *operation) {
    if (result < 0)
        throw std::runtime_error(std::string(operation) + ": " + gnutls_strerror(result));
}

std::string exportCertificatePEM(gnutls_x509_crt_t certificate) {
    gnutls_datum_t output{};
    checkGnuTLS(gnutls_x509_crt_export2(certificate, GNUTLS_X509_FMT_PEM, &output),
                "gnutls_x509_crt_export2");
    std::string result(reinterpret_cast<char *>(output.data), output.size);
    
    gnutls_free(output.data);
    return result;
}

std::string exportPrivateKeyPEM(gnutls_x509_privkey_t key) {
    gnutls_datum_t output{};
    checkGnuTLS(gnutls_x509_privkey_export2(key, GNUTLS_X509_FMT_PEM, &output),
                "gnutls_x509_privkey_export2");
    std::string result(reinterpret_cast<char *>(output.data), output.size);
    
    gnutls_free(output.data);
    return result;
}

} // namespace

void ensureGnuTLS() {
    std::call_once(gGnuTLSInitOnce, []() {
        const int result = gnutls_global_init();
        if (result < 0)
            throw std::runtime_error(std::string("gnutls_global_init: ") + gnutls_strerror(result));
    });
}

std::unique_ptr<Credentials> generateCredentials() {
    ensureGnuTLS();

    gnutls_x509_privkey_t key{};
    gnutls_x509_crt_t certificate{};
    checkGnuTLS(gnutls_x509_privkey_init(&key), "gnutls_x509_privkey_init");
    try {
        checkGnuTLS(gnutls_x509_privkey_generate(key, GNUTLS_PK_RSA, 2048, 0),
                    "gnutls_x509_privkey_generate");
        checkGnuTLS(gnutls_x509_crt_init(&certificate), "gnutls_x509_crt_init");
        checkGnuTLS(gnutls_x509_crt_set_version(certificate, 3), "gnutls_x509_crt_set_version");

        std::array<unsigned char, 16> serial{};
        checkGnuTLS(gnutls_rnd(GNUTLS_RND_RANDOM, serial.data(), serial.size()),
                    "gnutls_rnd(serial)");
        checkGnuTLS(gnutls_x509_crt_set_serial(certificate, serial.data(), serial.size()),
                    "gnutls_x509_crt_set_serial");

        const time_t current = std::time(nullptr);
        checkGnuTLS(gnutls_x509_crt_set_activation_time(certificate, current - 60),
                    "gnutls_x509_crt_set_activation_time");
        checkGnuTLS(gnutls_x509_crt_set_expiration_time(certificate, current + 24 * 60 * 60),
                    "gnutls_x509_crt_set_expiration_time");
        checkGnuTLS(gnutls_x509_crt_set_dn_by_oid(
                        certificate, GNUTLS_OID_X520_COMMON_NAME, 0,
                        kServerName, static_cast<unsigned int>(sizeof(kServerName) - 1)),
                    "gnutls_x509_crt_set_dn_by_oid");
        checkGnuTLS(gnutls_x509_crt_set_subject_alt_name(
                        certificate, GNUTLS_SAN_DNSNAME, kServerName,
                        static_cast<unsigned int>(sizeof(kServerName) - 1), 0),
                    "gnutls_x509_crt_set_subject_alt_name");
        checkGnuTLS(gnutls_x509_crt_set_key(certificate, key), "gnutls_x509_crt_set_key");
        checkGnuTLS(gnutls_x509_crt_set_basic_constraints(certificate, 0, -1),
                    "gnutls_x509_crt_set_basic_constraints");
        checkGnuTLS(gnutls_x509_crt_set_key_usage(certificate, GNUTLS_KEY_DIGITAL_SIGNATURE),
                    "gnutls_x509_crt_set_key_usage");
        checkGnuTLS(gnutls_x509_crt_set_key_purpose_oid(
                        certificate, GNUTLS_KP_TLS_WWW_SERVER, 0),
                    "gnutls_x509_crt_set_key_purpose_oid");
        checkGnuTLS(gnutls_x509_crt_sign2(
                        certificate, certificate, key, GNUTLS_DIG_SHA256, 0),
                    "gnutls_x509_crt_sign2");

        auto result = std::make_unique<Credentials>();
        
        result->certificatePEM = exportCertificatePEM(certificate);
        result->privateKeyPEM = exportPrivateKeyPEM(key);
        
        gnutls_x509_crt_deinit(certificate);
        gnutls_x509_privkey_deinit(key);
        
        return result;
    } catch (...) {
        if (certificate)
            gnutls_x509_crt_deinit(certificate);
        
        if (key)
            gnutls_x509_privkey_deinit(key);
        
        throw;
    }
}

} // namespace ffl::quic
