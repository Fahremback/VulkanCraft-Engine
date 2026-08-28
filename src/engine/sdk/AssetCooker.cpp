#include "engine/assets/IAssetCooker.hpp"
#include <map>
#include <utility>
namespace engine::assets { namespace {
class Cooker final : public IAssetCooker {
 std::map<std::uint64_t,CookArtifact> cache_;
 static std::uint64_t hash(const FormatDocument& d,const CookOptions& o){std::uint64_t h=1469598103934665603ull;auto add=[&](std::uint8_t c){h^=c;h*=1099511628211ull;};for(char c:d.source_name)add(static_cast<std::uint8_t>(c));add(static_cast<std::uint8_t>(d.format));for(auto b:d.bytes)add(b);for(char c:o.platform)add(static_cast<std::uint8_t>(c));for(unsigned i=0;i<4;++i)add(static_cast<std::uint8_t>(o.version>>(i*8)));add(o.compress?1:0);return h;}
public:
 bool cook(const FormatDocument& d,const CookOptions& o,CookArtifact& out,std::string& e) override {if(d.source_name.empty()||d.bytes.empty()){e="invalid_document";return false;}const auto key=hash(d,o);auto i=cache_.find(key);if(i!=cache_.end()){out=i->second;out.cache_hit=true;return true;}out={d.source_name,d.format,d.bytes,key,false};cache_[key]=out;return true;}
 bool validate(const CookArtifact& a,std::string& e)const override {if(a.source_name.empty()||a.bytes.empty()||a.content_hash==0){e="invalid_artifact";return false;}return true;}
 void clear_cache() override {cache_.clear();}
};}
std::unique_ptr<IAssetCooker> create_asset_cooker(){return std::make_unique<Cooker>();}
}
