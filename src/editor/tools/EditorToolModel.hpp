#pragma once

#include "../../engine/core/uuid/UUID.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

namespace Engine::Editor {

enum class ValidationSeverity : uint8_t { Info, Warning, Error };
struct ValidationIssue { ValidationSeverity severity{}; std::string field; std::string message; };

class EditorDocumentModel {
public:
    virtual ~EditorDocumentModel() = default;
    [[nodiscard]] UUID asset() const noexcept { return asset_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    [[nodiscard]] uint64_t revision() const noexcept { return revision_; }
    virtual void open(UUID asset) { asset_ = asset; dirty_ = false; revision_ = 0; }
    void mark_saved() noexcept { dirty_ = false; }
    [[nodiscard]] virtual std::vector<ValidationIssue> validate() const = 0;
protected:
    void changed() noexcept { dirty_ = true; ++revision_; }
private:
    UUID asset_{0, 0}; bool dirty_{}; uint64_t revision_{};
};

template<class T> bool erase_index(std::vector<T>& values, size_t index) {
    if (index >= values.size()) return false; values.erase(values.begin() + static_cast<std::ptrdiff_t>(index)); return true;
}

} // namespace Engine::Editor
