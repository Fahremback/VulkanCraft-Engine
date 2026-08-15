#pragma once

#include <string>
#include <vector>

namespace Engine {

// Arguments: <registry.db> <root-uuid> [root-uuid ...] <output-directory>.
int run_asset_cooker(const std::vector<std::string>& arguments);

} // namespace Engine
