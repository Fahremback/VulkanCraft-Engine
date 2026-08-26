// libsodium-probe.cpp — probe de utilização do libsodium vendido (§7,
// finding #298). Prova que o external é utilizável SEM wiring de CMake:
// compila e roda contra a lib estática buildada pelo MSVC (bin/x64/Release/
// v143/static/libsodium.lib). O probe usa a API pública para o caso de uso
// REAL do contrato IPackageManager (§6 item 5): assinatura e verificação
// ed25519 (crypto_sign) — o backend criptográfico plugável do
// ISignatureVerifier. Exit 0 = utilizável.
//
// Compilação (exemplo, após buildar a lib):
//   cl /EHsc /I <sodium-include> libsodium-probe.cpp <libsodium.lib> \
//      -o libsodium-probe

#include <sodium.h>

#include <cstdio>
#include <cstring>

int main(void) {
    int failures = 0;

    if (sodium_init() < 0) {
        std::printf("FAIL: sodium_init\n");
        return 1;
    }

    // 1. Gera um par de chaves ed25519.
    unsigned char pk[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk, sk);
    std::printf("keypair OK: pk[0]=%02x sk[0]=%02x\n", pk[0], sk[0]);

    // 2. Assina uma mensagem (content_hash do manifesto, opaco).
    const char* msg = "pkg-core-v1.0-hash-abcdef";
    const std::size_t msg_len = std::strlen(msg);
    unsigned char signed_msg[crypto_sign_BYTES + 1024];
    unsigned long long signed_len = 0;
    crypto_sign(signed_msg, &signed_len, reinterpret_cast<const unsigned char*>(msg),
                msg_len, sk);
    std::printf("sign OK: %llu bytes (detached size %d)\n", signed_len,
                crypto_sign_BYTES);

    // 3. Extrai a assinatura destacada (detached) — o formato que o
    //    ISignatureVerifier::verify recebe.
    unsigned char detached[crypto_sign_BYTES];
    std::memcpy(detached, signed_msg, crypto_sign_BYTES);
    const unsigned char* orig =
        signed_msg + crypto_sign_BYTES;  // mensagem original após a sig

    // 4. Verifica com a assinatura destacada.
    const int v = crypto_sign_verify_detached(detached, orig, msg_len, pk);
    if (v != 0) {
        std::printf("FAIL: verify detached\n");
        ++failures;
    } else {
        std::printf("verify detached OK\n");
    }

    // 5. Tamper: uma assinatura corrompida NÃO verifica (gate real).
    unsigned char bad[crypto_sign_BYTES];
    std::memcpy(bad, detached, crypto_sign_BYTES);
    bad[0] ^= 0x01;
    const int bad_v = crypto_sign_verify_detached(bad, orig, msg_len, pk);
    if (bad_v == 0) {
        std::printf("FAIL: tampered signature accepted\n");
        ++failures;
    } else {
        std::printf("tamper rejected OK\n");
    }

    // 6. Chave errada não verifica.
    unsigned char pk2[crypto_sign_PUBLICKEYBYTES];
    unsigned char sk2[crypto_sign_SECRETKEYBYTES];
    crypto_sign_keypair(pk2, sk2);
    const int wrong_v = crypto_sign_verify_detached(detached, orig, msg_len, pk2);
    if (wrong_v == 0) {
        std::printf("FAIL: wrong key accepted\n");
        ++failures;
    } else {
        std::printf("wrong key rejected OK\n");
    }

    if (failures == 0) {
        std::printf("libsodium-probe: ALL PASSED (ed25519 sign/verify for ISignatureVerifier)\n");
        return 0;
    }
    std::printf("libsodium-probe: %d FAILURE(S)\n", failures);
    return 1;
}
