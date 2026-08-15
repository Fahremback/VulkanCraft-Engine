#include "MaterialGraph.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Engine::Rendering {
namespace {

bool valid_parameter_name(std::string_view name) {
    if (name.empty()) return false;
    const auto validFirst = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; };
    const auto validRest = [&](char c) { return validFirst(c) || (c >= '0' && c <= '9'); };
    return validFirst(name.front()) && std::all_of(name.begin() + 1, name.end(), validRest);
}

std::vector<MaterialInput> operation_inputs(MaterialNodeKind kind, MaterialValueType type) {
    switch (kind) {
    case MaterialNodeKind::Add:
    case MaterialNodeKind::Multiply:
        return {{"A", type}, {"B", type}};
    case MaterialNodeKind::Lerp:
        return {{"A", type}, {"B", type}, {"Alpha", MaterialValueType::Float}};
    default:
        return {};
    }
}

MaterialIROp ir_op(MaterialNodeKind kind) {
    switch (kind) {
    case MaterialNodeKind::Constant: return MaterialIROp::Constant;
    case MaterialNodeKind::Parameter: return MaterialIROp::LoadParameter;
    case MaterialNodeKind::Add: return MaterialIROp::Add;
    case MaterialNodeKind::Multiply: return MaterialIROp::Multiply;
    case MaterialNodeKind::Lerp: return MaterialIROp::Lerp;
    case MaterialNodeKind::TextureSample: return MaterialIROp::TextureSample;
    case MaterialNodeKind::Output: return MaterialIROp::StoreOutput;
    }
    return MaterialIROp::Constant;
}

uint64_t fnv1a(std::string_view value, uint64_t hash = 14695981039346656037ull) noexcept {
    for (const unsigned char c : value) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

MaterialValueType material_value_type(const MaterialValue& value) {
    return std::visit([](const auto& item) -> MaterialValueType {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) return MaterialValueType::Bool;
        else if constexpr (std::is_same_v<T, float>) return MaterialValueType::Float;
        else if constexpr (std::is_same_v<T, glm::vec2>) return MaterialValueType::Vec2;
        else if constexpr (std::is_same_v<T, glm::vec3>) return MaterialValueType::Vec3;
        else if constexpr (std::is_same_v<T, glm::vec4>) return MaterialValueType::Vec4;
        else return MaterialValueType::Texture2D;
    }, value);
}

std::string_view material_type_name(MaterialValueType type) noexcept {
    switch (type) {
    case MaterialValueType::Bool: return "bool";
    case MaterialValueType::Float: return "float";
    case MaterialValueType::Vec2: return "vec2";
    case MaterialValueType::Vec3: return "vec3";
    case MaterialValueType::Vec4: return "vec4";
    case MaterialValueType::Texture2D: return "texture2D";
    }
    return "unknown";
}

MaterialNodeId MaterialGraph::add_constant(std::string label, MaterialValue value) {
    const MaterialNodeId id = nextId_++;
    nodes_.push_back({id, MaterialNodeKind::Constant, std::move(label), material_value_type(value), {}, std::move(value), {}});
    return id;
}

MaterialNodeId MaterialGraph::add_parameter(std::string name) {
    const MaterialNodeId id = nextId_++;
    const auto it = parameters_.find(name);
    const MaterialValueType type = it == parameters_.end() ? MaterialValueType::Float : it->second.type;
    nodes_.push_back({id, MaterialNodeKind::Parameter, name, type, {}, 0.0f, std::move(name)});
    return id;
}

MaterialNodeId MaterialGraph::add_operation(MaterialNodeKind kind, MaterialValueType type, std::string label) {
    if (kind != MaterialNodeKind::Add && kind != MaterialNodeKind::Multiply && kind != MaterialNodeKind::Lerp)
        return InvalidMaterialNode;
    if (type == MaterialValueType::Bool || type == MaterialValueType::Texture2D)
        return InvalidMaterialNode;
    const MaterialNodeId id = nextId_++;
    if (label.empty()) {
        label = kind == MaterialNodeKind::Add ? "Add" : kind == MaterialNodeKind::Multiply ? "Multiply" : "Lerp";
    }
    nodes_.push_back({id, kind, std::move(label), type, operation_inputs(kind, type), 0.0f, {}});
    return id;
}

MaterialNodeId MaterialGraph::add_texture_sample(std::string label) {
    const MaterialNodeId id = nextId_++;
    // Texture/UV inputs are optional: the editor binds the texture asset via the
    // node value (asset UUID string) and sampling uses the mesh UVs.
    nodes_.push_back({id, MaterialNodeKind::TextureSample, std::move(label), MaterialValueType::Vec4,
                      {{"Texture", MaterialValueType::Texture2D, InvalidMaterialNode, false},
                       {"UV", MaterialValueType::Vec2, InvalidMaterialNode, false}}, 0.0f, {}});
    return id;
}

MaterialNodeId MaterialGraph::add_output(std::string semantic, MaterialValueType type) {
    if (semantic.empty() || type == MaterialValueType::Texture2D) return InvalidMaterialNode;
    const MaterialNodeId id = nextId_++;
    nodes_.push_back({id, MaterialNodeKind::Output, semantic, type, {{"Value", type}}, 0.0f, std::move(semantic)});
    return id;
}

bool MaterialGraph::remove_node(MaterialNodeId id) {
    const auto oldSize = nodes_.size();
    std::erase_if(nodes_, [id](const MaterialNode& node) { return node.id == id; });
    if (nodes_.size() == oldSize) return false;
    for (auto& node : nodes_)
        for (auto& input : node.inputs)
            if (input.source == id) input.source = InvalidMaterialNode;
    return true;
}

bool MaterialGraph::connect(MaterialNodeId source, MaterialNodeId destination, size_t inputIndex, std::string* error) {
    MaterialNode* destinationNode = find_node(destination);
    const MaterialNode* sourceNode = find_node(source);
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (!sourceNode || !destinationNode) return fail("Source or destination node does not exist");
    if (source == destination) return fail("A node cannot connect to itself");
    if (inputIndex >= destinationNode->inputs.size()) return fail("Input index is out of range");
    if (sourceNode->kind == MaterialNodeKind::Output) return fail("Output nodes cannot be used as values");
    // Implicit RGBA→RGB swizzle at output sinks only (generator emits .rgb).
    const bool swizzleOk = destinationNode->kind == MaterialNodeKind::Output &&
                           destinationNode->inputs[inputIndex].type == MaterialValueType::Vec3 &&
                           sourceNode->outputType == MaterialValueType::Vec4;
    if (!swizzleOk && sourceNode->outputType != destinationNode->inputs[inputIndex].type) {
        return fail("Type mismatch: expected " + std::string(material_type_name(destinationNode->inputs[inputIndex].type)) +
                    ", got " + std::string(material_type_name(sourceNode->outputType)));
    }
    destinationNode->inputs[inputIndex].source = source;
    return true;
}

bool MaterialGraph::disconnect(MaterialNodeId destination, size_t inputIndex) {
    MaterialNode* node = find_node(destination);
    if (!node || inputIndex >= node->inputs.size() || node->inputs[inputIndex].source == InvalidMaterialNode) return false;
    node->inputs[inputIndex].source = InvalidMaterialNode;
    return true;
}

const MaterialNode* MaterialGraph::find_node(MaterialNodeId id) const noexcept {
    const auto it = std::find_if(nodes_.begin(), nodes_.end(), [id](const MaterialNode& node) { return node.id == id; });
    return it == nodes_.end() ? nullptr : &*it;
}

MaterialNode* MaterialGraph::find_node(MaterialNodeId id) noexcept {
    return const_cast<MaterialNode*>(std::as_const(*this).find_node(id));
}

bool MaterialGraph::define_parameter(MaterialParameter parameter, std::string* error) {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (!valid_parameter_name(parameter.name)) return fail("Parameter name must be a valid identifier");
    if (material_value_type(parameter.defaultValue) != parameter.type) return fail("Parameter default value has the wrong type");
    const auto [it, inserted] = parameters_.insert_or_assign(parameter.name, std::move(parameter));
    for (auto& node : nodes_)
        if (node.kind == MaterialNodeKind::Parameter && node.parameter == it->first) node.outputType = it->second.type;
    return inserted || it != parameters_.end();
}

bool MaterialGraph::remove_parameter(std::string_view name) {
    return parameters_.erase(std::string(name)) != 0;
}

const MaterialParameter* MaterialGraph::find_parameter(std::string_view name) const noexcept {
    const auto it = parameters_.find(std::string(name));
    return it == parameters_.end() ? nullptr : &it->second;
}

std::vector<MaterialParameter> MaterialGraph::parameters() const {
    std::vector<MaterialParameter> result;
    result.reserve(parameters_.size());
    for (const auto& [_, parameter] : parameters_) result.push_back(parameter);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return result;
}

MaterialCompileResult MaterialGraph::compile() const {
    MaterialCompileResult result;
    std::unordered_map<MaterialNodeId, const MaterialNode*> byId;
    for (const auto& node : nodes_) {
        if (node.id == InvalidMaterialNode || !byId.emplace(node.id, &node).second)
            result.errors.push_back({node.id, "Invalid or duplicate node id"});
    }

    std::vector<const MaterialNode*> outputs;
    for (const auto& node : nodes_) if (node.kind == MaterialNodeKind::Output) outputs.push_back(&node);
    if (outputs.empty()) result.errors.push_back({InvalidMaterialNode, "Material graph has no output"});
    std::sort(outputs.begin(), outputs.end(), [](const auto* a, const auto* b) {
        return a->parameter == b->parameter ? a->id < b->id : a->parameter < b->parameter;
    });

    std::unordered_map<MaterialNodeId, uint8_t> state;
    std::unordered_map<MaterialNodeId, uint32_t> registers;
    uint32_t nextRegister = 1;

    std::function<std::optional<uint32_t>(const MaterialNode&)> emit = [&](const MaterialNode& node) -> std::optional<uint32_t> {
        if (state[node.id] == 2) return registers[node.id];
        if (state[node.id] == 1) {
            result.errors.push_back({node.id, "Cycle detected in material graph"});
            return std::nullopt;
        }
        state[node.id] = 1;
        std::vector<uint32_t> operands;
        bool valid = true;
        for (const auto& input : node.inputs) {
            if (input.source == InvalidMaterialNode) {
                if (input.required) result.errors.push_back({node.id, "Required input '" + input.name + "' is not connected"});
                valid = false;
                continue;
            }
            const auto sourceIt = byId.find(input.source);
            if (sourceIt == byId.end()) {
                result.errors.push_back({node.id, "Input '" + input.name + "' references a missing node"});
                valid = false;
                continue;
            }
            // Implicit RGBA→RGB swizzle is allowed only at output sinks, where
            // the GLSL generator appends .rgb to the source register.
            const bool swizzleOk = node.kind == MaterialNodeKind::Output &&
                                   input.type == MaterialValueType::Vec3 &&
                                   sourceIt->second->outputType == MaterialValueType::Vec4;
            if (!swizzleOk && sourceIt->second->outputType != input.type) {
                result.errors.push_back({node.id, "Input '" + input.name + "' expects " + std::string(material_type_name(input.type))});
                valid = false;
                continue;
            }
            const auto operand = emit(*sourceIt->second);
            if (operand) operands.push_back(*operand); else valid = false;
        }

        if (node.kind == MaterialNodeKind::Parameter) {
            const MaterialParameter* parameter = find_parameter(node.parameter);
            if (!parameter) {
                result.errors.push_back({node.id, "Unknown material parameter '" + node.parameter + "'"});
                valid = false;
            } else if (parameter->type != node.outputType) {
                result.errors.push_back({node.id, "Material parameter node type is stale"});
                valid = false;
            }
        }
        if (node.kind == MaterialNodeKind::Constant && material_value_type(node.value) != node.outputType) {
            result.errors.push_back({node.id, "Constant value does not match its declared type"});
            valid = false;
        }

        state[node.id] = 2;
        if (!valid) return std::nullopt;
        if (node.kind == MaterialNodeKind::Output) {
            if (result.ir.outputs.contains(node.parameter)) {
                result.errors.push_back({node.id, "Duplicate material output '" + node.parameter + "'"});
                return std::nullopt;
            }
            result.ir.instructions.push_back({0, MaterialIROp::StoreOutput, node.outputType, operands, 0.0f,
                                              node.parameter, node.id});
            result.ir.outputs.emplace(node.parameter, operands.front());
            registers[node.id] = operands.front();
            return operands.front();
        }
        const uint32_t reg = nextRegister++;
        MaterialIRInstruction instruction{reg, ir_op(node.kind), node.outputType, std::move(operands),
                                          node.value, node.parameter, node.id};
        if (node.kind == MaterialNodeKind::Parameter) instruction.literal = find_parameter(node.parameter)->defaultValue;
        result.ir.instructions.push_back(std::move(instruction));
        registers[node.id] = reg;
        return reg;
    };

    for (const MaterialNode* output : outputs) emit(*output);
    if (!result.errors.empty()) {
        result.ir = {};
    }
    return result;
}

MaterialDefinition::MaterialDefinition(std::string name) : name_(std::move(name)) {
    if (name_.empty()) name_ = "Material";
}

MaterialInstance::MaterialInstance(std::shared_ptr<const MaterialDefinition> parent) : parent_(std::move(parent)) {}

void MaterialInstance::set_parent(std::shared_ptr<const MaterialDefinition> parent) {
    parent_ = std::move(parent);
    if (!parent_) { overrides_.clear(); return; }
    std::erase_if(overrides_, [&](const auto& item) {
        const auto* parameter = parent_->graph().find_parameter(item.first);
        return !parameter || parameter->type != material_value_type(item.second);
    });
}

bool MaterialInstance::set_parameter(std::string name, MaterialValue value, std::string* error) {
    auto fail = [&](std::string message) { if (error) *error = std::move(message); return false; };
    if (!parent_) return fail("Material instance has no parent");
    const auto* definition = parent_->graph().find_parameter(name);
    if (!definition) return fail("Parent material has no parameter named '" + name + "'");
    if (!definition->exposed) return fail("Material parameter is not exposed");
    if (definition->type != material_value_type(value)) return fail("Material parameter type mismatch");
    overrides_.insert_or_assign(std::move(name), std::move(value));
    return true;
}

bool MaterialInstance::clear_parameter(std::string_view name) { return overrides_.erase(std::string(name)) != 0; }

std::optional<MaterialValue> MaterialInstance::parameter(std::string_view name) const {
    const auto overrideIt = overrides_.find(std::string(name));
    if (overrideIt != overrides_.end()) return overrideIt->second;
    if (!parent_) return std::nullopt;
    const auto* definition = parent_->graph().find_parameter(name);
    return definition ? std::optional<MaterialValue>(definition->defaultValue) : std::nullopt;
}

bool MaterialInstance::has_override(std::string_view name) const { return overrides_.contains(std::string(name)); }

bool ShaderPermutation::set(std::string name, std::string value) {
    if (!valid_parameter_name(name) || value.empty()) return false;
    defines_.insert_or_assign(std::move(name), std::move(value));
    return true;
}

bool ShaderPermutation::erase(std::string_view name) { return defines_.erase(std::string(name)) != 0; }

std::optional<std::string> ShaderPermutation::value(std::string_view name) const {
    const auto it = defines_.find(std::string(name));
    return it == defines_.end() ? std::nullopt : std::optional<std::string>(it->second);
}

std::vector<ShaderDefine> ShaderPermutation::defines() const {
    std::vector<ShaderDefine> result;
    result.reserve(defines_.size());
    for (const auto& [name, value] : defines_) result.push_back({name, value});
    std::sort(result.begin(), result.end());
    return result;
}

std::string ShaderPermutation::canonical_key() const {
    std::ostringstream stream;
    for (const auto& define : defines()) stream << define.name << '=' << define.value << ';';
    return stream.str();
}

uint64_t ShaderPermutation::hash() const { return fnv1a(canonical_key()); }

void ShaderCacheMetadata::upsert(ShaderCacheEntry entry) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& current) {
        return current.permutationHash == entry.permutationHash && current.target == entry.target;
    });
    std::sort(entry.dependencies.begin(), entry.dependencies.end());
    entry.dependencies.erase(std::unique(entry.dependencies.begin(), entry.dependencies.end()), entry.dependencies.end());
    if (it == entries_.end()) entries_.push_back(std::move(entry)); else *it = std::move(entry);
}

const ShaderCacheEntry* ShaderCacheMetadata::find(uint64_t permutationHash, std::string_view target) const noexcept {
    const auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) {
        return entry.permutationHash == permutationHash && entry.target == target;
    });
    return it == entries_.end() ? nullptr : &*it;
}

bool ShaderCacheMetadata::is_stale(uint64_t permutationHash, std::string_view target, uint64_t sourceHash,
                                   uint32_t compilerVersion, const std::vector<std::string>& dependencies) const {
    const ShaderCacheEntry* entry = find(permutationHash, target);
    if (!entry || entry->sourceHash != sourceHash || entry->compilerVersion != compilerVersion || entry->binaryPath.empty()) return true;
    auto normalized = dependencies;
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    return entry->dependencies != normalized;
}

bool ShaderCacheMetadata::erase(uint64_t permutationHash, std::string_view target) {
    return std::erase_if(entries_, [&](const auto& entry) {
        return entry.permutationHash == permutationHash && entry.target == target;
    }) != 0;
}

} // namespace Engine::Rendering
