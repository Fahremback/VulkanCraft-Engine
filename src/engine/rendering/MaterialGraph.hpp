#pragma once

#include <glm/glm.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace Engine::Rendering {

using MaterialNodeId = uint32_t;
inline constexpr MaterialNodeId InvalidMaterialNode = 0;

enum class MaterialValueType : uint8_t { Bool, Float, Vec2, Vec3, Vec4, Texture2D };
using MaterialValue = std::variant<bool, float, glm::vec2, glm::vec3, glm::vec4, std::string>;

[[nodiscard]] MaterialValueType material_value_type(const MaterialValue& value);
[[nodiscard]] std::string_view material_type_name(MaterialValueType type) noexcept;

struct MaterialParameter {
    std::string name;
    MaterialValueType type{MaterialValueType::Float};
    MaterialValue defaultValue{0.0f};
    bool exposed{true};
};

enum class MaterialNodeKind : uint8_t {
    Constant,
    Parameter,
    Add,
    Multiply,
    Lerp,
    TextureSample,
    Output
};

struct MaterialInput {
    std::string name;
    MaterialValueType type{MaterialValueType::Float};
    MaterialNodeId source{InvalidMaterialNode};
    bool required{true};
};

struct MaterialNode {
    MaterialNodeId id{InvalidMaterialNode};
    MaterialNodeKind kind{MaterialNodeKind::Constant};
    std::string label;
    MaterialValueType outputType{MaterialValueType::Float};
    std::vector<MaterialInput> inputs;
    MaterialValue value{0.0f};
    std::string parameter;
};

struct MaterialGraphError {
    MaterialNodeId node{InvalidMaterialNode};
    std::string message;
};

enum class MaterialIROp : uint8_t { Constant, LoadParameter, Add, Multiply, Lerp, TextureSample, StoreOutput };

struct MaterialIRInstruction {
    uint32_t result{0};
    MaterialIROp op{MaterialIROp::Constant};
    MaterialValueType type{MaterialValueType::Float};
    std::vector<uint32_t> operands;
    MaterialValue literal{0.0f};
    std::string symbol;
    // Originating node; TextureSample emission uses it to select the sampler.
    MaterialNodeId nodeId{InvalidMaterialNode};
};

struct MaterialIR {
    std::vector<MaterialIRInstruction> instructions;
    std::unordered_map<std::string, uint32_t> outputs;
};

struct MaterialCompileResult {
    MaterialIR ir;
    std::vector<MaterialGraphError> errors;
    [[nodiscard]] explicit operator bool() const noexcept { return errors.empty(); }
};

class MaterialGraph final {
public:
    [[nodiscard]] MaterialNodeId add_constant(std::string label, MaterialValue value);
    [[nodiscard]] MaterialNodeId add_parameter(std::string name);
    [[nodiscard]] MaterialNodeId add_operation(MaterialNodeKind kind, MaterialValueType type,
                                               std::string label = {});
    [[nodiscard]] MaterialNodeId add_texture_sample(std::string label = "Texture Sample");
    [[nodiscard]] MaterialNodeId add_output(std::string semantic, MaterialValueType type);

    [[nodiscard]] bool remove_node(MaterialNodeId id);
    [[nodiscard]] bool connect(MaterialNodeId source, MaterialNodeId destination, size_t inputIndex,
                               std::string* error = nullptr);
    [[nodiscard]] bool disconnect(MaterialNodeId destination, size_t inputIndex);
    [[nodiscard]] const MaterialNode* find_node(MaterialNodeId id) const noexcept;
    [[nodiscard]] MaterialNode* find_node(MaterialNodeId id) noexcept;
    [[nodiscard]] const std::vector<MaterialNode>& nodes() const noexcept { return nodes_; }

    [[nodiscard]] bool define_parameter(MaterialParameter parameter, std::string* error = nullptr);
    [[nodiscard]] bool remove_parameter(std::string_view name);
    [[nodiscard]] const MaterialParameter* find_parameter(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<MaterialParameter> parameters() const;
    [[nodiscard]] MaterialCompileResult compile() const;

private:
    MaterialNodeId nextId_{1};
    std::vector<MaterialNode> nodes_;
    std::unordered_map<std::string, MaterialParameter> parameters_;
};

class MaterialDefinition final {
public:
    explicit MaterialDefinition(std::string name = "Material");
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] MaterialGraph& graph() noexcept { return graph_; }
    [[nodiscard]] const MaterialGraph& graph() const noexcept { return graph_; }
    [[nodiscard]] MaterialCompileResult compile() const { return graph_.compile(); }

private:
    std::string name_;
    MaterialGraph graph_;
};

class MaterialInstance final {
public:
    explicit MaterialInstance(std::shared_ptr<const MaterialDefinition> parent = {});
    void set_parent(std::shared_ptr<const MaterialDefinition> parent);
    [[nodiscard]] const std::shared_ptr<const MaterialDefinition>& parent() const noexcept { return parent_; }
    [[nodiscard]] bool set_parameter(std::string name, MaterialValue value, std::string* error = nullptr);
    [[nodiscard]] bool clear_parameter(std::string_view name);
    [[nodiscard]] std::optional<MaterialValue> parameter(std::string_view name) const;
    [[nodiscard]] bool has_override(std::string_view name) const;
    [[nodiscard]] const std::unordered_map<std::string, MaterialValue>& overrides() const noexcept { return overrides_; }

private:
    std::shared_ptr<const MaterialDefinition> parent_;
    std::unordered_map<std::string, MaterialValue> overrides_;
};

struct ShaderDefine {
    std::string name;
    std::string value;
    auto operator<=>(const ShaderDefine&) const = default;
};

class ShaderPermutation final {
public:
    [[nodiscard]] bool set(std::string name, std::string value = "1");
    [[nodiscard]] bool erase(std::string_view name);
    [[nodiscard]] std::optional<std::string> value(std::string_view name) const;
    [[nodiscard]] std::vector<ShaderDefine> defines() const;
    [[nodiscard]] uint64_t hash() const;
    [[nodiscard]] std::string canonical_key() const;

private:
    std::unordered_map<std::string, std::string> defines_;
};

struct ShaderCacheEntry {
    uint64_t permutationHash{};
    uint64_t sourceHash{};
    uint64_t binaryHash{};
    uint32_t compilerVersion{};
    std::string target;
    std::string binaryPath;
    std::vector<std::string> dependencies;
};

class ShaderCacheMetadata final {
public:
    void upsert(ShaderCacheEntry entry);
    [[nodiscard]] const ShaderCacheEntry* find(uint64_t permutationHash, std::string_view target) const noexcept;
    [[nodiscard]] bool is_stale(uint64_t permutationHash, std::string_view target,
                                uint64_t sourceHash, uint32_t compilerVersion,
                                const std::vector<std::string>& dependencies) const;
    [[nodiscard]] bool erase(uint64_t permutationHash, std::string_view target);
    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

private:
    std::vector<ShaderCacheEntry> entries_;
};

} // namespace Engine::Rendering
