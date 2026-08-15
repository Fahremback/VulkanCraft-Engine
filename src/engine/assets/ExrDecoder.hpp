#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Engine {

// EXR decodificado (via tinyexr — biblioteca externa, header-only) no mesmo
// shape que o decoder HDR da engine: RGBA half-float (8 bytes por pixel,
// bitDepth 32 / channels 4), pronto para o payload VCTEX v3.
struct DecodedExr {
    uint32_t width{ 0 };
    uint32_t height{ 0 };
    std::vector<uint8_t> rgba16f;
};

// Decodes an OpenEXR file (uncompressed, ZIP/ZIPS, PIZ, RLE, ZFP) into RGBA16F.
// Returns false and fills *error on failure.
bool decode_exr(std::span<const uint8_t> bytes, DecodedExr& out, std::string* error = nullptr);

// IEEE 754 half-precision converter (round-to-nearest-even) — exposta para os
// testes e para o cook de EXR.
uint16_t float_to_half_rne(float value);

} // namespace Engine
