#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Engine::Editor {

struct BlockFacePixels {
    std::uint32_t width{ 0 };
    std::uint32_t height{ 0 };
    std::vector<std::uint8_t> rgba;

    [[nodiscard]] bool valid() const noexcept {
        return width > 0 && height > 0 &&
               rgba.size() == static_cast<std::size_t>(width) * height * 4u;
    }
};

// Produces one tightly packed horizontal image: [top | side | bottom].
// Output is transactional: invalid input leaves it unchanged.
bool compose_horizontal_block_atlas(const BlockFacePixels& top,
                                    const BlockFacePixels& side,
                                    const BlockFacePixels& bottom,
                                    BlockFacePixels& output,
                                    std::string* error = nullptr);

} // namespace Engine::Editor
