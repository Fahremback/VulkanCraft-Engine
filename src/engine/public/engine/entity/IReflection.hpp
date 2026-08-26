// IReflection — registry público de TIPOS e CAMPOS (reflection mínima).
// Componente CORE do §1 item 50 ("publicar assets, componentes, reflection,
// scripting e painéis de depuração"): componentes/assets registram seus
// tipos (nome + campos com kind); editor, scripting e MCP consultam o schema
// (quais campos existem, de que tipo) sem conhecer o runtime. Puro e
// testável headless — a serialização real continua no dono de cada tipo.
//
// JSON versionado all-or-nothing (nome vazio/duplicado, campo vazio/
// duplicado → rejeita o doc inteiro).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace entity {

enum class FieldKind : std::uint8_t {
    Int, Float, Bool, String, Vec3, Quat, Enum, Json
};

const char* field_kind_name(FieldKind kind);

struct FieldInfo {
    std::string name;
    FieldKind kind{ FieldKind::Float };
};

struct TypeInfo {
    std::string name;
    std::vector<FieldInfo> fields;
};

class IReflection {
public:
    virtual ~IReflection() = default;

    // All-or-nothing: nome vazio/duplicado, campo com nome vazio/duplicado
    // → rejeita (nunca registra parcial).
    virtual bool register_type(const TypeInfo& type, std::string& errorOut) = 0;

    // Carrega um doc JSON versionado ({"version":1,"types":[...]}).
    virtual bool load_from_json(const std::string& jsonText,
                                std::string& errorOut) = 0;
    virtual std::string to_json() const = 0;

    virtual const TypeInfo* type(const std::string& name) const = 0;
    virtual std::vector<std::string> type_names() const = 0;  // ordem crescente
    virtual std::vector<std::string> field_names(const std::string& typeName) const = 0;
    virtual bool has_field(const std::string& typeName, const std::string& field) const = 0;
    virtual std::size_t count() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IReflection> create_reflection();

}  // namespace entity
}  // namespace engine
