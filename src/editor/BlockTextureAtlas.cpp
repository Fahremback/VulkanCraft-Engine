#include "BlockTextureAtlas.hpp"

#include <cstring>

namespace Engine::Editor {

bool compose_horizontal_block_atlas(const BlockFacePixels& top,
                                    const BlockFacePixels& side,
                                    const BlockFacePixels& bottom,
                                    BlockFacePixels& output,
                                    std::string* error) {
    if (!top.valid() || !side.valid() || !bottom.valid()) {
        if (error) *error = "block face has invalid RGBA storage";
        return false;
    }
    if (top.width != side.width || top.width != bottom.width ||
        top.height != side.height || top.height != bottom.height) {
        if (error) *error = "block faces must have identical dimensions";
        return false;
    }

    BlockFacePixels composed;
    composed.width = top.width * 3u;
    composed.height = top.height;
    composed.rgba.resize(static_cast<std::size_t>(composed.width) * composed.height * 4u);
    const BlockFacePixels* faces[3] = { &top, &side, &bottom };
    const std::size_t faceRowBytes = static_cast<std::size_t>(top.width) * 4u;
    const std::size_t atlasRowBytes = static_cast<std::size_t>(composed.width) * 4u;
    for (std::uint32_t y = 0; y < top.height; ++y) {
        for (std::uint32_t region = 0; region < 3u; ++region) {
            const std::uint8_t* src = faces[region]->rgba.data() +
                                      static_cast<std::size_t>(y) * faceRowBytes;
            std::uint8_t* dst = composed.rgba.data() +
                                static_cast<std::size_t>(y) * atlasRowBytes +
                                static_cast<std::size_t>(region) * faceRowBytes;
            std::memcpy(dst, src, faceRowBytes);
        }
    }
    output = std::move(composed);
    if (error) error->clear();
    return true;
}

} // namespace Engine::Editor
