// Blake3Hash — BLAKE3 adapter behind IHashProvider.
//
// The ONLY translation unit that includes the promoted solution's headers for
// hashing. Keeps blake3 out of the public API (DEPENDENCY_POLICY).
#include "engine/hashing/IHashProvider.hpp"

#include <blake3.h>

#include <cstddef>
#include <memory>
#include <string>

namespace engine {
namespace hashing {
namespace {

class Blake3Hash final : public IHashProvider {
public:
    std::string hash(const std::string& data) const override {
        blake3_hasher hasher;
        blake3_hasher_init(&hasher);
        blake3_hasher_update(&hasher, data.data(), data.size());
        std::string out(BLAKE3_OUT_LEN, '\0');
        blake3_hasher_finalize(
            &hasher, reinterpret_cast<uint8_t*>(out.data()), BLAKE3_OUT_LEN);
        return out;
    }

    std::string hash_hex(const std::string& data) const override {
        return to_hex(hash(data));
    }

    std::size_t digest_size() const override { return BLAKE3_OUT_LEN; }
};

}  // namespace

std::string to_hex(const std::string& bytes) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char c : bytes) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0Fu]);
    }
    return out;
}

std::shared_ptr<IHashProvider> create_blake3_hash_provider() {
    return std::make_shared<Blake3Hash>();
}

}  // namespace hashing
}  // namespace engine
