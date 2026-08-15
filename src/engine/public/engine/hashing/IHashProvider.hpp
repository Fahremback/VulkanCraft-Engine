// IHashProvider — public content-hashing contract.
//
// Promoted solution (META section 32): BLAKE3 from external/solutions/blake3
// backs save integrity (replacing the v3 FNV-1a checksum) and will back
// content addressing / deduplication. Consumers only see this interface; the
// blake3 headers live behind the adapter in the SDK module. Self-contained.
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace engine {
namespace hashing {

// Digest of a byte string. The digest is 32 raw bytes (BLAKE3-256); callers
// wanting text should hex-encode it (see to_hex below).
class IHashProvider {
public:
    virtual ~IHashProvider() = default;

    // BLAKE3-256 digest of `data` as 32 raw bytes (never empty).
    virtual std::string hash(const std::string& data) const = 0;

    // Hex-encoded digest (lowercase, 2 chars per byte).
    virtual std::string hash_hex(const std::string& data) const = 0;

    virtual std::size_t digest_size() const = 0;
};

// Lowercase hex encoding of a raw digest/bytes string.
std::string to_hex(const std::string& bytes);

// Factory: builds the BLAKE3-backed provider. The implementation lives in the
// engine SDK module; headers of the promoted solution never appear here.
std::shared_ptr<IHashProvider> create_blake3_hash_provider();

}  // namespace hashing
}  // namespace engine
