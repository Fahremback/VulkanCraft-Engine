#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::entity {

enum class FieldKind : std::uint8_t {
    Int, Float, Bool, String, Vec3, Quat, Enum, Json,
    Variant, Array, Map, Optional, Uuid, Handle, Range
};

const char* field_kind_name(FieldKind kind);

struct FieldInfo {
    std::string name;
    FieldKind kind{FieldKind::Float};
    std::string value_type;
    std::string default_value;
    std::string alias;
    std::string since;
    std::string deprecated_since;
    double minimum{0.0};
    double maximum{0.0};
    bool has_range{false};
    bool optional{false};
};

struct TypeInfo {
    std::string name;
    std::string stable_id;
    std::string alias;
    std::string version{"1.0.0"};
    std::vector<FieldInfo> fields;
};

struct ReflectionMigration {
    std::string type_name;
    std::string from_version;
    std::string to_version;
    std::string description;
};

class IReflection {
public:
    virtual ~IReflection() = default;
    virtual bool register_type(const TypeInfo&, std::string& errorOut) = 0;
    virtual bool register_migration(const ReflectionMigration&, std::string& errorOut) = 0;
    virtual bool load_from_json(const std::string&, std::string& errorOut) = 0;
    virtual std::string to_json() const = 0;
    virtual const TypeInfo* type(const std::string& name) const = 0;
    virtual std::vector<std::string> type_names() const = 0;
    virtual std::vector<std::string> field_names(const std::string& typeName) const = 0;
    virtual bool has_field(const std::string& typeName, const std::string& field) const = 0;
    virtual std::size_t count() const = 0;
    virtual std::vector<ReflectionMigration> migrations() const = 0;
    virtual void clear() = 0;
};

std::unique_ptr<IReflection> create_reflection();

} // namespace engine::entity
