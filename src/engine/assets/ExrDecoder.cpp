#include "ExrDecoder.hpp"

#include "tinyexr.h"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace Engine {

uint16_t float_to_half_rne(float value) {
    const uint32_t bits = std::bit_cast<uint32_t>(value);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0) {
        // Subnormal or zero: shift the mantissa right until it fits 10 bits.
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        const uint32_t half = mantissa >> shift;
        const uint32_t round = (mantissa >> (shift - 1)) & 1u;
        return static_cast<uint16_t>(sign | (half + round));
    }
    if (exponent >= 31) {
        // Infinity / overflow.
        return static_cast<uint16_t>(sign | 0x7C00u);
    }
    const uint32_t half = (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    const uint32_t round = (mantissa >> 12) & 1u;
    return static_cast<uint16_t>(sign | (half + round));
}

bool decode_exr(std::span<const uint8_t> bytes, DecodedExr& out, std::string* error) {
    if (bytes.empty()) {
        if (error) *error = "empty EXR input";
        return false;
    }

    int width = 0;
    int height = 0;
    float* rgba = nullptr;
    const char* tinyexrError = nullptr;

    const int result = LoadEXRFromMemory(&rgba, &width, &height, bytes.data(), bytes.size(),
                                         &tinyexrError);
    if (result != TINYEXR_SUCCESS) {
        if (error) *error = tinyexrError ? tinyexrError : "LoadEXRFromMemory failed";
        if (tinyexrError) FreeEXRErrorMessage(tinyexrError);
        return false;
    }
    if (!rgba || width <= 0 || height <= 0) {
        if (error) *error = "EXR produced no pixels";
        if (rgba) std::free(rgba);
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    out.width = static_cast<uint32_t>(width);
    out.height = static_cast<uint32_t>(height);
    out.rgba16f.resize(pixelCount * 4 * 2);

    const float* source = rgba;
    uint8_t* destination = out.rgba16f.data();
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        for (int component = 0; component < 4; ++component) {
            const uint16_t half = float_to_half_rne(*source++);
            std::memcpy(destination, &half, sizeof(half));
            destination += sizeof(half);
        }
    }
    std::free(rgba);
    return true;
}

} // namespace Engine
