// ZstdCompression — Zstandard adapter behind ICompressionProvider.
//
// The ONLY translation unit that includes the promoted solution's headers for
// compression. Keeps zstd out of the public API (DEPENDENCY_POLICY).
#include "engine/compression/ICompressionProvider.hpp"

#include <zstd.h>

#include <cstddef>
#include <memory>
#include <string>

namespace engine {
namespace compression {
namespace {

class ZstdCompression final : public ICompressionProvider {
public:
    std::string compress(const std::string& data) const override {
        // Zstd compresses size-0 input into a valid (non-empty) frame, so
        // empty == failure stays unambiguous and empty input round-trips.
        const std::size_t bound = ZSTD_compressBound(data.size());
        std::string out(bound, '\0');
        const std::size_t written =
            ZSTD_compress(out.data(), out.size(), data.data(), data.size(), 3);
        if (ZSTD_isError(written)) return {};
        out.resize(written);
        return out;
    }

    std::string decompress(const std::string& data) const override {
        if (data.empty()) return {};
        const unsigned long long contentSize =
            ZSTD_getFrameContentSize(data.data(), data.size());
        if (contentSize == ZSTD_CONTENTSIZE_ERROR ||
            contentSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            return {};
        }
        std::string out(static_cast<std::size_t>(contentSize), '\0');
        const std::size_t written = ZSTD_decompress(
            out.data(), out.size(), data.data(), data.size());
        if (ZSTD_isError(written)) return {};
        out.resize(written);
        return out;
    }

    bool is_compressed(const std::string& data) const override {
        if (data.size() < 4) return false;
        // Zstd frame magic: 0x28 0xB5 0x2F 0xFD.
        const unsigned char* p =
            reinterpret_cast<const unsigned char*>(data.data());
        return p[0] == 0x28u && p[1] == 0xB5u && p[2] == 0x2Fu && p[3] == 0xFDu;
    }
};

}  // namespace

std::shared_ptr<ICompressionProvider> create_zstd_compression_provider() {
    return std::make_shared<ZstdCompression>();
}

}  // namespace compression
}  // namespace engine
