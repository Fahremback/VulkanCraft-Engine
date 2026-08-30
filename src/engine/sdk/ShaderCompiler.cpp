// ShaderCompiler.cpp — adapter for IShaderCompiler.
// Uses glslc, spirv-val, spirv-opt from Vulkan SDK via std::system().
// Headless, deterministic, no Vulkan device required.

#include "engine/rendering/IShaderCompiler.hpp"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <array>
#include <cstdio>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace vc::rendering {

static std::string getTempDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH + 1] = {};
    if (GetTempPathW(MAX_PATH + 1, buf)) {
        char narrow[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, narrow, sizeof(narrow), 0, 0);
        std::string s(narrow);
        while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
        return s;
    }
#endif
    const char* e = getenv("TEMP"); if (!e) e = getenv("TMP");
    return e ? e : "/tmp";
}

static int s_counter = 0;

static std::string writeTemp(const void* data, size_t size, const std::string& ext) {
#ifdef _WIN32
    std::string p = getTempDir() + "\\vc_shader_" + std::to_string(s_counter++) + ext;
#else
    std::string p = getTempDir() + "/vc_shader_" + std::to_string(s_counter++) + ext;
#endif
    std::ofstream f(p, std::ios::binary);
    f.write(static_cast<const char*>(data), size);
    return p;
}

static int runCmd(const std::string& cmd) {
#ifdef _WIN32
    // <nul prevents tools from hanging waiting for stdin
    return std::system((cmd + " <nul >nul 2>&1").c_str());
#else
    return std::system((cmd + " </dev/null >/dev/null 2>&1").c_str());
#endif
}

static std::string findTool(const std::string& name) {
#ifdef _WIN32
    const char* sdk = getenv("VULKAN_SDK");
    if (sdk) {
        std::string p = std::string(sdk) + "\\Bin\\" + name + ".exe";
        if (fs::exists(p)) return p;
    }
    std::string fb = "C:\\VulkanSDK\\1.4.341.1\\Bin\\" + name + ".exe";
    if (fs::exists(fb)) return fb;
#else
    const char* sdk = getenv("VULKAN_SDK");
    if (sdk) {
        std::string p = std::string(sdk) + "/Bin/" + name;
        if (fs::exists(p)) return p;
    }
#endif
    return "";
}

static std::string stageArg(ShaderStage s) {
    if (s == ShaderStage::Vertex)   return "-fshader-stage=vert";
    if (s == ShaderStage::Fragment) return "-fshader-stage=frag";
    if (s == ShaderStage::Compute)  return "-fshader-stage=comp";
    if (s == ShaderStage::Task)     return "-fshader-stage=task";
    if (s == ShaderStage::Mesh)     return "-fshader-stage=mesh";
    return "";
}

// ─── Config ───────────────────────────────────────

bool ShaderCompilerConfig::validate() const {
    if (optLevel < 0 || optLevel > 2) return false;
    if (!targetEnv.empty()) {
        char* end = nullptr;
        float v = std::strtof(targetEnv.c_str(), &end);
        if (end == targetEnv.c_str() || v < 1.0f || v > 1.6f) return false;
    }
    return true;
}

static std::string je(const std::string& s) {
    std::string o;
    for (char c : s) { if(c=='"')o+="\\\""; else if(c=='\\')o+="\\\\"; else if(c=='\n')o+="\\n"; else o+=c; }
    return o;
}

std::string ShaderCompilerConfig::toJson() const {
    std::ostringstream o;
    o << "{\"glslcPath\":\"" << je(glslcPath) << "\",\"spirvValPath\":\"" << je(spirvValPath)
      << "\",\"targetEnv\":\"" << je(targetEnv) << "\",\"optLevel\":" << optLevel << ",\"defines\":[";
    for (size_t i = 0; i < defines.size(); ++i) { if(i) o<<","; o<<"\""<<je(defines[i])<<"\""; }
    o << "]}"; return o.str();
}

static std::string jf(const std::string& j, const std::string& k) {
    auto n = "\"" + k + "\""; auto p = j.find(n); if(p==std::string::npos) return "";
    p = j.find(':', p+n.size()); if(p==std::string::npos) return "";
    p++; while(p<j.size()&&j[p]==' ') p++;
    if(p>=j.size()) return "";
    if(j[p]=='"'){p++; auto e=j.find('"',p); return e!=std::string::npos?j.substr(p,e-p):"";}
    auto e=j.find_first_of(",}",p); return j.substr(p,(e==std::string::npos?j.size():e)-p);
}

static std::vector<std::string> ja(const std::string& j, const std::string& k) {
    std::vector<std::string> r; auto n="\""+k+"\""; auto p=j.find(n);
    if(p==std::string::npos) return r; p=j.find('[',p); if(p==std::string::npos) return r;
    auto e=j.find(']',p); if(e==std::string::npos) return r;
    std::string a=j.substr(p+1,e-p-1); size_t i=0;
    while(i<a.size()){auto q=a.find('"',i);if(q==std::string::npos)break;
        auto qe=a.find('"',q+1);if(qe==std::string::npos)break;
        r.push_back(a.substr(q+1,qe-q-1));i=qe+1;} return r;
}

ShaderCompilerConfig ShaderCompilerConfig::fromJson(const std::string& j, std::string& err) {
    ShaderCompilerConfig c; c.glslcPath=jf(j,"glslcPath"); c.spirvValPath=jf(j,"spirvValPath");
    c.targetEnv=jf(j,"targetEnv"); auto os=jf(j,"optLevel"); if(!os.empty()) c.optLevel=std::stoi(os);
    c.defines=ja(j,"defines"); if(!c.validate()){err="invalid config";return {};} return c;
}

// ─── Adapter ──────────────────────────────────────

class ShaderCompilerImpl : public IShaderCompiler {
public:
    std::vector<uint32_t> compile(const char* src, ShaderStage stage,
                                  const ShaderCompilerConfig& cfg, std::string& err) override {
        if(!src||!cfg.validate()){err="invalid input";return{};}
        std::string glslc = cfg.glslcPath.empty() ? findTool("glslc") : cfg.glslcPath;
        if(glslc.empty()){err="glslc not found";return{};}

        std::string srcPath = writeTemp(src, std::strlen(src), ".glsl");
        std::string spvPath = srcPath + ".spv";

        std::string cmd = glslc + " " + stageArg(stage);
        if(!cfg.targetEnv.empty()) cmd += " --target-env=vulkan" + cfg.targetEnv;
        for(auto& d : cfg.defines) cmd += " -D" + d;
        cmd += " -o " + spvPath + " " + srcPath;

        int ec = runCmd(cmd);
        std::remove(srcPath.c_str());
        if(ec!=0){err="glslc failed (exit "+std::to_string(ec)+")";std::remove(spvPath.c_str());return{};}

        std::ifstream f(spvPath, std::ios::binary|std::ios::ate);
        if(!f.is_open()){err="cannot read "+spvPath;return{};}
        auto sz=f.tellg();f.seekg(0);
        std::vector<uint32_t> spirv(sz/4);
        f.read(reinterpret_cast<char*>(spirv.data()),sz);
        f.close();std::remove(spvPath.c_str());
        if(spirv.empty()||spirv[0]!=0x07230203){err="invalid SPIR-V";return{};}
        return spirv;
    }

    ShaderCompileResult compileAndValidate(const char* src, ShaderStage stage,
                                           const ShaderCompilerConfig& cfg) override {
        ShaderCompileResult r; r.sourceSize=src?std::strlen(src):0;
        std::string err; r.spirv=compile(src,stage,cfg,err);
        if(r.spirv.empty()){r.errorLog=err;return r;}
        r.spirvSize=r.spirv.size()*4;
        if(!validate(r.spirv.data(),r.spirv.size(),cfg,err)){
            r.errorLog="val: "+err;r.spirv.clear();return r;}
        r.stageMask=detectStages(r.spirv);r.success=true;return r;
    }

    bool validate(const uint32_t* spirv, size_t wc, const ShaderCompilerConfig& cfg, std::string& err) override {
        if(!spirv||wc<5||spirv[0]!=0x07230203){err="bad SPIR-V";return false;}
        std::string sv = cfg.spirvValPath.empty() ? findTool("spirv-val") : cfg.spirvValPath;
        if(sv.empty()) return true;
        std::string tmp = writeTemp(spirv, wc*4, ".spv");
        int ec = runCmd(sv + " " + tmp);
        std::remove(tmp.c_str());
        if(ec!=0){err="spirv-val failed";return false;}
        return true;
    }

    std::vector<uint32_t> optimize(const uint32_t* spirv, size_t wc, int ol, std::string& err) override {
        if(!spirv||wc<5){err="bad SPIR-V";return{};}
        std::string so = findTool("spirv-opt");
        if(so.empty()){return std::vector<uint32_t>(spirv, spirv+wc);}
        std::string ti = writeTemp(spirv, wc*4, ".spv");
        std::string to = ti + ".opt";
        std::string flag = (ol==1)?"-Os":(ol==2)?"-O3":"-O";
        std::string cmd = so + " --target-env spv1.5 " + flag + " " + ti + " -o " + to;
        int ec = runCmd(cmd);
        std::remove(ti.c_str());
        if(ec!=0){std::remove(to.c_str());return std::vector<uint32_t>(spirv, spirv+wc);}
        std::ifstream f(to, std::ios::binary|std::ios::ate);
        if(!f.is_open()){err="cannot read optimized";return{};}
        auto sz=f.tellg();f.seekg(0);
        std::vector<uint32_t> r(sz/4);f.read(reinterpret_cast<char*>(r.data()),sz);
        f.close();std::remove(to.c_str());
        if(r.empty()||r[0]!=0x07230203){err="invalid optimized SPIR-V";return{};}
        return r;
    }

    std::vector<std::string> availablePasses() const override {
        return {"strip-decorations","eliminate-dead-constant","fold-spec-const-op-and-composite",
                "set-spec-const-default-value","compact-ids","merge-blocks","private-to-local",
                "eliminate-dead-functions","eliminate-dead-variables","flatten-decorations"};
    }

private:
    static uint32_t detectStages(const std::vector<uint32_t>& sp) {
        uint32_t m=0;
        for(size_t i=5;i+1<sp.size();){
            uint32_t op=sp[i]&0xFF,cnt=(sp[i]>>16)&0xFFFF;
            if(op==15&&cnt>=2){uint32_t md=sp[i+1];
                if(md==0)m|=1;else if(md==4)m|=4;else if(md==5)m|=0x20;}
            i+=(cnt==0)?1:cnt;} return m;
    }
};

std::unique_ptr<IShaderCompiler> create_shader_compiler(std::string& err) {
    if(findTool("glslc").empty()){err="glslc not found";return nullptr;}
    return std::make_unique<ShaderCompilerImpl>();
}

} // namespace vc::rendering
