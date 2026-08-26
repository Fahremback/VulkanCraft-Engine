// Ktx2Transcoder.cpp — Agente 1 (task_plan C): the ONLY TU that includes the
// vendored Basis transcoder headers (external/solutions/ktx-software/
// external/basis_universal/transcoder, Apache-2.0, commit 31145d1b… via the
// ktx-software clone). Maps the CPU KTX2/Basis decode to the public contract
// engine/rendering/IKtx2Transcoder.hpp. Pure CPU, deterministic, all-or-nothing.
//
// BUG-010 lesson: basisu_transcoder.cpp is compiled INTO vc_sdk_public (see
// CMakeLists.txt) so the decode symbols travel with this adapter's objects.
// Zstd supercompression is disabled (BASISD_SUPPORT_KTX2_ZSTD=0) — the
// supported surface keeps the ETC1S/UASTC arithmetic payloads, which is the
// common GPU-friendly path; Zstd-compressed UASTC stays HUMAN-VISUAL-PENDING.

#define BASISD_SUPPORT_KTX2_ZSTD 0

#include "engine/rendering/IKtx2Transcoder.hpp"

#include "basisu_transcoder.h"

#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {
namespace {

basist::transcoder_texture_format toBasis(Ktx2Format format) {
    using F = basist::transcoder_texture_format;
    switch (format) {
        case Ktx2Format::Rgba32: return F::cTFRGBA32;
        case Ktx2Format::Bc1Rgb: return F::cTFBC1;
        case Ktx2Format::Bc3Rgba: return F::cTFBC3;
        case Ktx2Format::Bc4R: return F::cTFBC4;
        case Ktx2Format::Bc5Rg: return F::cTFBC5;
        case Ktx2Format::Bc7Rgba: return F::cTFBC7_RGBA;
        case Ktx2Format::Etc2Rgb: return F::cTFETC1_RGB;  // ETC1/ETC2-RGB block
        case Ktx2Format::Etc2Rgba: return F::cTFETC2_RGBA;
        case Ktx2Format::Astc4x4: return F::cTFASTC_4x4;
        case Ktx2Format::Pvrtc1Rgba: return F::cTFPVRTC1_4_RGBA;
        default: return F::cTFRGBA32;  // unreachable
    }
}

class Ktx2Transcoder final : public IKtx2Transcoder {
public:
    Ktx2Transcoder() { basist::basisu_transcoder_init(); }

    bool open(const std::uint8_t* data, std::size_t size,
              Ktx2Info& outInfo, std::string& errorOut) override {
        if (data == nullptr || size < 64) {
            errorOut = "ktx2: data too small or null";
            return false;
        }
        if (!transcoder_.init(data, static_cast<std::uint32_t>(size))) {
            errorOut = "ktx2: invalid KTX2 header";
            return false;
        }
        outInfo.width = transcoder_.get_width();
        outInfo.height = transcoder_.get_height();
        outInfo.levels = transcoder_.get_levels();
        outInfo.faces = transcoder_.get_faces();
        outInfo.layers = 1;
        // UASTC vs ETC1S: the KTX2 DFD color model distinguishes them
        // (KTX2_KDF_DF_MODEL_UASTC_* = 166/167/168/169, ETC1S = 163).
        const std::uint32_t model = transcoder_.get_dfd_color_model();
        outInfo.uastc =
            model == basist::KTX2_KDF_DF_MODEL_UASTC_LDR_4X4 ||
            model == basist::KTX2_KDF_DF_MODEL_UASTC_HDR_4X4 ||
            model == basist::KTX2_KDF_DF_MODEL_UASTC_HDR_6X6_INTERMEDIATE ||
            model == basist::KTX2_KDF_DF_MODEL_XUASTC_LDR_INTERMEDIATE;
        hasData_ = true;
        return true;
    }

    bool transcodeLevel(std::uint32_t level, std::uint32_t face,
                        Ktx2Format format, std::vector<std::uint8_t>& out,
                        std::string& errorOut) override {
        if (!hasData_) {
            errorOut = "ktx2: open() must succeed before transcoding";
            return false;
        }
        if (level >= transcoder_.get_levels() || face >= transcoder_.get_faces()) {
            errorOut = "ktx2: level/face out of range";
            return false;
        }
        if (!transcoder_.start_transcoding()) {
            errorOut = "ktx2: start_transcoding failed (corrupt payload)";
            return false;
        }
        const std::uint32_t w = transcoder_.get_width() >> level;
        const std::uint32_t h = transcoder_.get_height() >> level;
        if (w == 0 || h == 0) {
            errorOut = "ktx2: degenerate level size";
            return false;
        }
        // transcode_image_level's size parameter counts PIXELS for RGBA32 and
        // BLOCKS for block formats (per the vendored header docs).
        const bool rgba = (format == Ktx2Format::Rgba32);
        const std::size_t blocksX = w / 4 + (w % 4 ? 1u : 0u);
        const std::size_t blocksY = h / 4 + (h % 4 ? 1u : 0u);
        const std::uint32_t count = static_cast<std::uint32_t>(
            rgba ? (static_cast<std::size_t>(w) * h)
                 : (blocksX * blocksY));
        const std::size_t bytes =
            rgba ? (static_cast<std::size_t>(w) * h * 4)
                 : (blocksX * blocksY * 16);
        out.assign(bytes, 0);
        const bool ok = transcoder_.transcode_image_level(
            level, 0, face, out.data(), count, toBasis(format));
        if (!ok) {
            out.clear();
            errorOut = "ktx2: transcode_image_level failed";
            return false;
        }
        return true;
    }

private:
    basist::ktx2_transcoder transcoder_;
    bool hasData_{ false };
};

}  // namespace

std::unique_ptr<IKtx2Transcoder> create_ktx2_transcoder() {
    return std::make_unique<Ktx2Transcoder>();
}

}  // namespace Engine::Rendering
