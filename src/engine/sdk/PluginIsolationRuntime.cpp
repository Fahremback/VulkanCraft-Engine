#include "engine/plugins/IPluginIsolationRuntime.hpp"
#include <map>
namespace engine::plugins { namespace {
struct Record { std::uint64_t timeout{}, limit{}, usage{}; bool active{}, cancelled{}, healthy{true}; };
class Runtime final : public IPluginIsolationRuntime {
    std::map<std::string, Record> records_;
public:
    bool register_plugin(const std::string& n, std::uint64_t timeout, std::uint64_t limit, std::string& e) override { if(n.empty()){e="plugin_name_required";return false;} if(records_.count(n)){e="duplicate_plugin";return false;} records_[n]={timeout,limit,0,false,false,true}; return true; }
    bool begin_call(const std::string& n, std::string& e) override { auto i=records_.find(n);if(i==records_.end()){e="plugin_not_found";return false;}auto&r=i->second;if(!r.healthy||r.cancelled){e="plugin_unavailable";return false;}if(r.active){e="plugin_busy";return false;}r.active=true;return true; }
    bool end_call(const std::string& n, std::uint64_t elapsed, std::uint64_t usage, std::string& e) override { auto i=records_.find(n);if(i==records_.end()){e="plugin_not_found";return false;}auto&r=i->second;if(!r.active){e="call_not_active";return false;}r.active=false;r.usage=usage;if((r.timeout&&elapsed>r.timeout)||(r.limit&&usage>r.limit)){r.healthy=false;e=(r.timeout&&elapsed>r.timeout)?"plugin_timeout":"plugin_memory_limit";return false;}return true; }
    bool cancel(const std::string& n, std::string& e) override { auto i=records_.find(n);if(i==records_.end()){e="plugin_not_found";return false;}i->second.cancelled=true;i->second.active=false;return true; }
    bool healthy(const std::string& n) const override { auto i=records_.find(n);return i!=records_.end()&&i->second.healthy&&!i->second.cancelled; }
    bool unload(const std::string& n, std::string& e) override { auto i=records_.find(n);if(i==records_.end()){e="plugin_not_found";return false;}records_.erase(i);return true; }
}; }
std::unique_ptr<IPluginIsolationRuntime> create_plugin_isolation_runtime(){return std::make_unique<Runtime>();}
}
