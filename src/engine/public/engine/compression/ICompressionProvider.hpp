// ICompressionProvider — public compression contract.
//
// Promoted solution (META section 32): Zstandard from
// external/solutions/zstd backs the persistent world save. Consumers only see
// this interface; the zstd headers live behind the adapter in the SDK module.
// Self-contained: no external includes, safe for any project TU.
#pragma once

#include <memory>
#include <string>

namespace engine {
namespace compression {

// Compress/decompress byte strings. Empty input is valid and round-trips to
// empty. Failure is reported by returning an empty result from an operation
// that should have produced output (or by is_compressed() being false).
class ICompressionProvider {
public:
    virtual ~ICompressionProvider() = default;

    // Returns the compressed bytes, or an empty string on failure (the empty
    // input compresses to a non-empty zstd frame, so empty == failure).
    virtual std::string compress(const std::string& data) const = 0;

    // Returns the decompressed bytes, or an empty string when the input is not
    // a valid compressed frame for this provider.
    virtual std::string decompress(const std::string& data) const = 0;

    // True when the leading bytes are a frame produced by this provider.
    virtual bool is_compressed(const std::string& data) const = 0;
};

// Factory: builds the Zstandard-backed provider. The implementation lives in
// the engine SDK module; headers of the promoted solution never appear here.
std::shared_ptr<ICompressionProvider> create_zstd_compression_provider();

}  // namespace compression
}  // namespace engine
