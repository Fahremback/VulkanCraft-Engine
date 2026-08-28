#include "engine/entity/IReflection.hpp"
#include "RegistryJson.hpp"
#include <algorithm>
#include <map>
#include <sstream>

namespace engine::entity {
namespace {
const char* kind_name(FieldKind k) {
    switch (k) {
    case FieldKind::Int:return "int"; case FieldKind::Float:return "float";
    case FieldKind::Bool:return "bool"; case FieldKind::String:return "string";
    case FieldKind::Vec3:return "vec3"; case FieldKind::Quat:return "quat";
    case FieldKind::Enum:return "enum"; case FieldKind::Json:return "json";
    case FieldKind::Variant:return "variant"; case FieldKind::Array:return "array";
    case FieldKind::Map:return "map"; case FieldKind::Optional:return "optional";
    case FieldKind::Uuid:return "uuid"; case FieldKind::Handle:return "handle";
    case FieldKind::Range:return "range";
    }
    return "json";
}
bool parse_kind(const std::string& s, FieldKind& out) {
    for (int i=0;i<=static_cast<int>(FieldKind::Range);++i) {
        auto k=static_cast<FieldKind>(i); if (s==kind_name(k)){out=k;return true;}
    }
    return false;
}
std::string esc(const std::string& s) {
    std::string o; for(char c:s){ if(c=='"'||c=='\\')o+='\\'; if(c=='\n')o+="\\n"; else o+=c; } return o;
}
bool valid_field(const FieldInfo& f, const std::string& type, std::string& e) {
    if(f.name.empty()){e="reflection: empty field in '"+type+"'";return false;}
    if(f.has_range && f.minimum>f.maximum){e="reflection: invalid range for '"+f.name+"'";return false;}
    return true;
}
bool valid_type(const TypeInfo& t,std::string& e){
    if(t.name.empty()){e="reflection: empty type name";return false;}
    if(t.stable_id.empty()){e="reflection: stable_id required for '"+t.name+"'";return false;}
    for(size_t i=0;i<t.fields.size();++i){if(!valid_field(t.fields[i],t.name,e))return false;for(size_t j=0;j<i;++j)if(t.fields[i].name==t.fields[j].name){e="reflection: duplicate field '"+t.fields[i].name+"'";return false;}}
    return true;
}
class Reflection final: public IReflection {
 std::map<std::string,TypeInfo> types_; std::vector<ReflectionMigration> migrations_;
public:
 bool register_type(const TypeInfo&t,std::string&e)override{TypeInfo copy=t;if(copy.stable_id.empty())copy.stable_id=copy.name;if(!valid_type(copy,e))return false;if(types_.count(copy.name)||(!copy.alias.empty()&&std::any_of(types_.begin(),types_.end(),[&](auto&p){return p.second.alias==copy.alias;}))){e="reflection: duplicate type or alias";return false;}types_[copy.name]=std::move(copy);return true;}
 bool register_migration(const ReflectionMigration&m,std::string&e)override{if(m.type_name.empty()||m.from_version.empty()||m.to_version.empty()){e="reflection: invalid migration";return false;}if(!types_.count(m.type_name)){e="reflection: migration type not found";return false;}migrations_.push_back(m);return true;}
 bool load_from_json(const std::string&text,std::string&e)override{sdk::JsonValue root;if(!sdk::json_parse(text,root,e)||!root.is_object()){if(e.empty())e="reflection: root must be object";return false;}if(static_cast<int>(sdk::json_number(root,"version",1))!=1){e="reflection: unsupported version";return false;}auto* list=root.field("types");if(!list||list->kind!=sdk::JsonValue::Kind::Array){e="reflection: types must be array";return false;}std::map<std::string,TypeInfo> parsed;for(const auto&v:list->array){if(!v.is_object()){e="reflection: type must be object";return false;}TypeInfo t;t.name=sdk::json_string(v,"name","");t.stable_id=sdk::json_string(v,"stable_id",t.name);t.alias=sdk::json_string(v,"alias","");t.version=sdk::json_string(v,"version","1.0.0");auto*f=v.field("fields");if(f){if(f->kind!=sdk::JsonValue::Kind::Array){e="reflection: fields must be array";return false;}for(const auto&fv:f->array){FieldInfo x;x.name=sdk::json_string(fv,"name","");x.value_type=sdk::json_string(fv,"value_type","");x.default_value=sdk::json_string(fv,"default","");x.alias=sdk::json_string(fv,"alias","");x.deprecated_since=sdk::json_string(fv,"deprecated_since","");FieldKind k;if(!parse_kind(sdk::json_string(fv,"kind","float"),k)){e="reflection: unknown field kind";return false;}x.kind=k;x.optional=k==FieldKind::Optional; t.fields.push_back(x);}}if(!valid_type(t,e)||parsed.count(t.name)){if(e.empty())e="reflection: duplicate type";return false;}parsed.emplace(t.name,std::move(t));}types_=std::move(parsed);return true;}
 std::string to_json()const override{std::ostringstream o;o<<"{\"version\":1,\"types\":[";bool first=true;for(const auto&[n,t]:types_){if(!first)o<<',';first=false;o<<"{\"name\":\""<<esc(t.name)<<"\",\"stable_id\":\""<<esc(t.stable_id)<<"\",\"alias\":\""<<esc(t.alias)<<"\",\"version\":\""<<esc(t.version)<<"\",\"fields\":[";for(size_t i=0;i<t.fields.size();++i){if(i)o<<',';const auto&f=t.fields[i];o<<"{\"name\":\""<<esc(f.name)<<"\",\"kind\":\""<<kind_name(f.kind)<<"\"}";}o<<"]}";}o<<"]}";return o.str();}
 const TypeInfo* type(const std::string&n)const override{auto i=types_.find(n);if(i!=types_.end())return &i->second;for(const auto&[_,t]:types_)if(t.alias==n)return &t;return nullptr;}
 std::vector<std::string> type_names()const override{std::vector<std::string>o;for(const auto&[n,_]:types_)o.push_back(n);return o;}
 std::vector<std::string> field_names(const std::string&n)const override{std::vector<std::string>o;auto*t=type(n);if(t)for(const auto&f:t->fields)o.push_back(f.name);return o;}
 bool has_field(const std::string&n,const std::string&f)const override{auto*t=type(n);if(!t)return false;return std::any_of(t->fields.begin(),t->fields.end(),[&](const auto&x){return x.name==f||x.alias==f;});}
 std::size_t count()const override{return types_.size();} std::vector<ReflectionMigration> migrations()const override{return migrations_;} void clear()override{types_.clear();migrations_.clear();}
};}
const char* field_kind_name(FieldKind k){return kind_name(k);} std::unique_ptr<IReflection> create_reflection(){return std::make_unique<Reflection>();}
}
