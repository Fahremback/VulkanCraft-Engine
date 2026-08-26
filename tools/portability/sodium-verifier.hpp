// sodium-verifier.hpp — implementação CONCRETA do contrato ISignatureVerifier
// (engine::packaging, §6 item 5) com libsodium real (ed25519). Prova o
// wiring do contrato plugável com criptografia de verdade — o backend que o
// finding #298 mostrou ser utilizável. Assinatura no formato opaco do
// contrato: "hex(pk)|hex(sig)" — o verifier extrai pk e sig, e verifica.
// Self-contained (std + sodium.h). Consumido pelo gate
// package-manager-sodium-gate.cpp.
#ifndef SODIUM_VERIFIER_HPP
#define SODIUM_VERIFIER_HPP

#include <sodium.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "engine/packaging/IPackageManager.hpp"

namespace engine::packaging {

// Verifier plugável baseado em libsodium (ed25519).
// Formato da assinatura: "<hex(pk)>|<hex(sig)>" — 64+1+128 chars.
struct SodiumSignatureVerifier final : ISignatureVerifier {
    bool verify(const std::string& packageName, const std::string& contentHash,
                const std::string& signature) override {
        (void)packageName;
        const std::size_t bar = signature.find('|');
        if (bar == std::string::npos) return false;
        const std::string pk_hex = signature.substr(0, bar);
        const std::string sig_hex = signature.substr(bar + 1);
        if (pk_hex.size() != crypto_sign_PUBLICKEYBYTES * 2 ||
            sig_hex.size() != crypto_sign_BYTES * 2) {
            return false;
        }
        unsigned char pk[crypto_sign_PUBLICKEYBYTES];
        unsigned char sig[crypto_sign_BYTES];
        if (!hex_decode(pk_hex, pk, sizeof(pk))) return false;
        if (!hex_decode(sig_hex, sig, sizeof(sig))) return false;
        const unsigned char* msg =
            reinterpret_cast<const unsigned char*>(contentHash.data());
        const std::size_t msg_len = contentHash.size();
        return crypto_sign_verify_detached(sig, msg, msg_len, pk) == 0;
    }

private:
    static int hex_val(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    static bool hex_decode(const std::string& hex, unsigned char* out, std::size_t n) {
        if (hex.size() != n * 2) return false;
        for (std::size_t i = 0; i < n; ++i) {
            const int hi = hex_val(hex[i * 2]);
            const int lo = hex_val(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) return false;
            out[i] = static_cast<unsigned char>((hi << 4) | lo);
        }
        return true;
    }
};

}  // namespace engine::packaging

#endif  // SODIUM_VERIFIER_HPP
