#pragma once

// IKtx2Transcoder — Agente 1 (task_plan C): the PUBLIC contract for CPU
// KTX2/Basis transcoding (promoted from external/solutions/ktx-software,
// Apache-2.0). Decodes supercompressed KTX2 textures (ETC1S/UASTC Basis) to
// GPU-friendly block formats (BC7, ETC2, BC1/3/4/5, ASTC, PVRTC) or plain
// RGBA32, so the material/sky pipeline (A.15) can consume KTX2 assets on any
// hardware — no Vulkan involved in the decode.
//
// Self-contained std; no external headers. Deterministic. All-or-nothing:
// invalid/truncated data or a refused format yields false + a diagnostic and
// leaves `out` untouched.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Engine::Rendering {

// Output formats (subset of the Basis transcoder's; every one is CPU-decoded).
enum class Ktx2Format : std::uint8_t {
    Rgba32,   // 32bpp RGBA raster (R first, A last) — for verification/CPU use
    Bc1Rgb,   // DXT1
    Bc3Rgba,  // DXT5
    Bc4R,     // ATI1
    Bc5Rg,    // ATI2
    Bc7Rgba,  // highest quality
    Etc2Rgb,
    Etc2Rgba,
    Astc4x4,
    Pvrtc1Rgba,
    Unknown,
};

// Texture description after a successful init.
struct Ktx2Info {
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::uint32_t levels{ 0 };
    std::uint32_t faces{ 0 };  // 1 = 2D, 6 = cubemap
    std::uint32_t layers{ 0 };  // 1 for plain textures
    bool uastc{ false };        // UASTC (vs ETC1S) supercompressed payload
};

class IKtx2Transcoder {
public:
    virtual ~IKtx2Transcoder() = default;

    // Parses and validates the KTX2 container; reports the texture description.
    // The caller keeps `data` alive for the object's lifetime. Returns false
    // (with a diagnostic) if the bytes are not a valid KTX2 file.
    virtual bool open(const std::uint8_t* data, std::size_t size,
                      Ktx2Info& outInfo, std::string& errorOut) = 0;

    // Decodes level `level` of face `face` (layer 0) to `format` into `out`
    // (resized by the adapter to the exact byte count). Returns false and
    // leaves `out` untouched on any refusal (bad state, bad level/face,
    // unsupported format, invalid data).
    virtual bool transcodeLevel(std::uint32_t level, std::uint32_t face,
                                Ktx2Format format, std::vector<std::uint8_t>& out,
                                std::string& errorOut) = 0;
};

// Creates the transcoder (lazily initializes the Basis lookup tables).
std::unique_ptr<IKtx2Transcoder> create_ktx2_transcoder();

}  // namespace Engine::Rendering
