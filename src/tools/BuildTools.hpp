#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace Engine::Tools {
struct ShaderCompileOptions { std::string stage; std::string entry{"main"}; std::vector<std::filesystem::path> includeDirectories; std::vector<std::string> defines; bool debug{}; };
struct ToolResult { bool success{}; std::string message; std::vector<std::filesystem::path> outputs; explicit operator bool()const noexcept{return success;} };
class ShaderCompiler final { public: static ToolResult compile(const std::filesystem::path& source,const std::filesystem::path& output,const ShaderCompileOptions& options); };
struct ProjectOptions { std::string name; bool voxelPlugin{}; std::filesystem::path enginePath; };
class ProjectGenerator final { public: static ToolResult generate(const std::filesystem::path& directory,const ProjectOptions& options); };
struct PackageOptions { std::string platform{"windows-x64"}; std::string configuration{"Shipping"}; bool includeSymbols{}; };
class PackageBuilder final { public: static ToolResult build(const std::filesystem::path& executable,const std::filesystem::path& cookedContent,const std::filesystem::path& output,const PackageOptions& options); };
} // namespace Engine::Tools
