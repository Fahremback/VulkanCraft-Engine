#include "editor/BlockTextureAtlas.hpp"

#include <cstdlib>
#include <iostream>

namespace {

Engine::Editor::BlockFacePixels solid(std::uint32_t width, std::uint32_t height,
                                      std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    Engine::Editor::BlockFacePixels pixels;
    pixels.width = width;
    pixels.height = height;
    pixels.rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    for (std::size_t i = 0; i < pixels.rgba.size(); i += 4u) {
        pixels.rgba[i + 0] = r;
        pixels.rgba[i + 1] = g;
        pixels.rgba[i + 2] = b;
        pixels.rgba[i + 3] = 255;
    }
    return pixels;
}

bool region_is(const Engine::Editor::BlockFacePixels& atlas, std::uint32_t region,
               std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    const std::uint32_t faceWidth = atlas.width / 3u;
    for (std::uint32_t y = 0; y < atlas.height; ++y) {
        for (std::uint32_t x = region * faceWidth; x < (region + 1u) * faceWidth; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * atlas.width + x) * 4u;
            if (atlas.rgba[offset] != r || atlas.rgba[offset + 1] != g ||
                atlas.rgba[offset + 2] != b || atlas.rgba[offset + 3] != 255) return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const auto top = solid(2, 3, 255, 0, 0);
    const auto side = solid(2, 3, 0, 255, 0);
    const auto bottom = solid(2, 3, 0, 0, 255);
    Engine::Editor::BlockFacePixels atlas;
    std::string error;
    if (!Engine::Editor::compose_horizontal_block_atlas(top, side, bottom, atlas, &error)) {
        std::cerr << error << '\n';
        return EXIT_FAILURE;
    }
    if (atlas.width != 6 || atlas.height != 3 || atlas.rgba.size() != 72 ||
        !region_is(atlas, 0, 255, 0, 0) || !region_is(atlas, 1, 0, 255, 0) ||
        !region_is(atlas, 2, 0, 0, 255)) {
        std::cerr << "atlas regions or row stride are corrupt\n";
        return EXIT_FAILURE;
    }

    auto wrong = bottom;
    wrong.width = 1;
    Engine::Editor::BlockFacePixels untouched = solid(1, 1, 7, 8, 9);
    if (Engine::Editor::compose_horizontal_block_atlas(top, side, wrong, untouched, &error) ||
        untouched.width != 1 || untouched.height != 1 || untouched.rgba[0] != 7) {
        std::cerr << "invalid dimensions must fail without changing output\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
