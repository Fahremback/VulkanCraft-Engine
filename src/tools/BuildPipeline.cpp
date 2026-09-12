#include "BuildPipeline.hpp"

#include <fstream>
#include <sstream>

namespace Engine::Tools {

BuildPipeline::BuildPipeline(const ProjectConfig& config) : config_(config) {
    hooks_.assign(static_cast<size_t>(Stage::GenerateDistributable) + 1, nullptr);
}

double BuildPipeline::now_ms() const noexcept {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started_)
        .count();
}

void BuildPipeline::set_stage_hook(Stage stage, StageHook hook) {
    hooks_[static_cast<size_t>(stage)] = std::move(hook);
}

void BuildPipeline::clear_stage_hook(Stage stage) {
    hooks_[static_cast<size_t>(stage)] = nullptr;
}

bool BuildPipeline::has_hook(Stage stage) const noexcept {
    return hooks_[static_cast<size_t>(stage)] != nullptr;
}

bool BuildPipeline::run_stage(Stage stage, std::string& message) {
    const auto stageStart = std::chrono::steady_clock::now();
    bool ok = false;
    if (hooks_[static_cast<size_t>(stage)]) {
        ok = hooks_[static_cast<size_t>(stage)](stage, config_, message);
    } else {
        switch (stage) {
            case Stage::Validate: ok = stage_validate(message); break;
            case Stage::ResolveDependencies: ok = stage_resolve_dependencies(message); break;
            case Stage::ImportAssets: ok = stage_import_assets(message); break;
            case Stage::CompileShaders: ok = stage_compile_shaders(message); break;
            case Stage::CookAssets: ok = stage_cook_assets(message); break;
            case Stage::PackageContent: ok = stage_package_content(message); break;
            case Stage::BuildExecutable: ok = stage_build_executable(message); break;
            case Stage::CopyDependencies: ok = stage_copy_dependencies(message); break;
            case Stage::GenerateDistributable: ok = stage_generate_distributable(message); break;
        }
    }
    const double elapsed = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - stageStart)
                               .count();
    lastStageMs_ = elapsed;
    return ok;
}

BuildPipeline::BuildReport BuildPipeline::run() {
    BuildReport report;
    report.success = true;
    started_ = std::chrono::steady_clock::now();
    const Stage order[] = {
        Stage::Validate, Stage::ResolveDependencies, Stage::ImportAssets,
        Stage::CompileShaders, Stage::CookAssets, Stage::PackageContent,
        Stage::BuildExecutable, Stage::CopyDependencies, Stage::GenerateDistributable};
    for (const Stage stage : order) {
        std::string message;
        const double stageStart = now_ms();
        const bool ok = run_stage(stage, message);
        const double elapsed = now_ms() - stageStart;
        StageResult result;
        result.stage = stage;
        result.success = ok;
        result.elapsedMilliseconds = elapsed;
        result.message = std::move(message);
        report.stages.push_back(std::move(result));
        if (!ok) {
            report.success = false;
            break;
        }
    }
    report.totalMilliseconds = now_ms();
    return report;
}

// ─── Default stage implementations ───
namespace {
bool ensure_directory(const std::filesystem::path& path, std::string& message) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        message = "Cannot create directory: " + path.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

bool copy_directory_tree(const std::filesystem::path& source,
                         const std::filesystem::path& destination,
                         std::string& message) {
    if (!std::filesystem::is_directory(source)) {
        message = "Source directory missing: " + source.string();
        return false;
    }
    if (!ensure_directory(destination, message)) return false;

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(source, ec), end;
         !ec && it != end; it.increment(ec)) {
        const auto relative = std::filesystem::relative(it->path(), source, ec);
        if (ec) break;
        const auto target = destination / relative;
        if (it->is_directory()) {
            std::filesystem::create_directories(target, ec);
        } else if (it->is_regular_file()) {
            std::filesystem::create_directories(target.parent_path(), ec);
            if (!ec) {
                std::filesystem::copy_file(it->path(), target,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        if (ec) break;
    }
    if (ec) {
        message = "Cannot copy directory tree from " + source.string() +
                  " to " + destination.string() + " (" + ec.message() + ")";
        return false;
    }
    return true;
}
} // namespace

bool BuildPipeline::stage_validate(std::string& message) {
    const ProjectConfig::ValidationReport report = config_.validate();
    if (!report.valid()) {
        std::ostringstream out;
        out << "Validation failed (" << report.error_count() << " errors):";
        for (const std::string& error : report.errors) out << "\n  - " << error;
        message = out.str();
        return false;
    }
    std::ostringstream out;
    out << "Project '" << config_.name << "' validated (" << report.warning_count() << " warnings)";
    message = out.str();
    return true;
}

bool BuildPipeline::stage_resolve_dependencies(std::string& message) {
    // Walk asset references (files under Assets) and record the closure.
    if (!std::filesystem::is_directory(config_.assets_path())) {
        message = "Assets directory missing: " + config_.assets_path().string();
        return false;
    }
    std::error_code ec;
    size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.assets_path(), ec)) {
        if (entry.is_regular_file()) ++count;
    }
    message = "Resolved dependencies: " + std::to_string(count) + " source files";
    return true;
}

bool BuildPipeline::stage_import_assets(std::string& message) {
    if (!ensure_directory(config_.intermediate_path(), message)) return false;
    // For each source asset, produce an entry in the Intermediate folder.
    std::error_code ec;
    size_t imported = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.assets_path(), ec)) {
        if (!entry.is_regular_file()) continue;
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), config_.assets_path(), ec);
        const std::filesystem::path target = config_.intermediate_path() / (relative.string() + ".imported");
        std::ofstream out(target, std::ios::trunc);
        out << "imported " << entry.path().filename().string() << "\n";
        ++imported;
    }
    message = "Imported " + std::to_string(imported) + " assets into " + config_.intermediate_path().string();
    return true;
}

bool BuildPipeline::stage_compile_shaders(std::string& message) {
    const std::filesystem::path shaderDir = config_.intermediate_path() / "shaders";
    if (!ensure_directory(shaderDir, message)) return false;

    // The canonical shared build owns shader compilation. Packaging stages the
    // exact SPIR-V bytes produced there; it must never fabricate a manifest that
    // claims shaders were compiled when no .spv exists.
    const std::filesystem::path canonicalShaderDir =
        (config_.enginePath / "out" / "dev-shared" / "shaders").lexically_normal();
    if (!std::filesystem::is_directory(canonicalShaderDir)) {
        message = "Canonical compiled shader directory is missing: " +
                  canonicalShaderDir.string();
        return false;
    }

    std::ofstream manifest(shaderDir / "shader_manifest.txt", std::ios::trunc);
    if (!manifest) {
        message = "Cannot create shader manifest in " + shaderDir.string();
        return false;
    }
    manifest << "# staged SPIR-V for " << profile_name(config_.activeProfile) << "\n";

    std::error_code ec;
    std::size_t copied = 0;
    for (std::filesystem::recursive_directory_iterator it(canonicalShaderDir, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file() || it->path().extension() != ".spv") continue;
        const auto relative = std::filesystem::relative(it->path(), canonicalShaderDir, ec);
        if (ec) break;
        const auto target = shaderDir / relative;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) break;
        std::filesystem::copy_file(it->path(), target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) break;
        manifest << relative.generic_string() << "\n";
        ++copied;
    }
    if (ec) {
        message = "Failed staging canonical shaders (" + ec.message() + ")";
        return false;
    }
    if (copied == 0) {
        message = "Canonical shader directory contains no SPIR-V files: " +
                  canonicalShaderDir.string();
        return false;
    }
    if (!manifest.good()) {
        message = "Failed writing shader manifest in " + shaderDir.string();
        return false;
    }
    message = "Staged " + std::to_string(copied) +
              " canonical SPIR-V shaders from " + canonicalShaderDir.string();
    return true;
}

bool BuildPipeline::stage_cook_assets(std::string& message) {
    const std::filesystem::path cookedDir = config_.build_path() / "Content";
    if (!ensure_directory(cookedDir, message)) return false;
    // Copy intermediate assets to the cooked Content folder.
    std::error_code ec;
    size_t cooked = 0;
    if (std::filesystem::is_directory(config_.intermediate_path())) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(config_.intermediate_path(), ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".imported") continue;
            const std::filesystem::path relative = std::filesystem::relative(entry.path(), config_.intermediate_path(), ec);
            const std::filesystem::path target = cookedDir / relative;
            if (!ensure_directory(target.parent_path(), message)) return false;
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) ++cooked;
        }
    }
    message = "Cooked " + std::to_string(cooked) + " assets to " + cookedDir.string();
    return true;
}

bool BuildPipeline::stage_package_content(std::string& message) {
    const std::filesystem::path packageDir = config_.build_path() / "Package";
    if (!ensure_directory(packageDir, message)) return false;
    // A real package manifest describing the content bundle.
    std::ofstream manifest(packageDir / "content.pkg", std::ios::trunc);
    manifest << "VCPACKAGE 1\n";
    manifest << "project " << config_.name << "\n";
    manifest << "profile " << profile_name(config_.activeProfile) << "\n";
    manifest << "platform " << platform_name(config_.targetPlatform) << "\n";
    manifest << "initialScene " << config_.initialScene << "\n";
    manifest << "plugins " << config_.enabledPlugins.size() << "\n";
    for (const std::string& plugin : config_.enabledPlugins) manifest << "  " << plugin << "\n";
    message = std::string("Packaged content for ") + profile_name(config_.activeProfile);
    return true;
}

bool BuildPipeline::stage_build_executable(std::string& message) {
    const std::filesystem::path binDir = config_.build_path() / "Binaries";
    if (!ensure_directory(binDir, message)) return false;

    // A project build may wrap the canonical engine build, but it must never
    // manufacture an executable marker. The shared development tree is the
    // single native build authority used by the editor/MCP/certification flow.
    const bool dedicated = config_.activeProfile == BuildProfile::Server ||
                           config_.targetPlatform == TargetPlatform::DedicatedServer;
#ifdef _WIN32
    const char* canonicalName = dedicated ? "VulkanEngineServer.exe" : "VulkanEngineGame.exe";
    const std::filesystem::path target = binDir / (config_.name + ".exe");
#else
    const char* canonicalName = dedicated ? "VulkanEngineServer" : "VulkanEngineGame";
    const std::filesystem::path target = binDir / config_.name;
#endif
    const std::filesystem::path canonical =
        (config_.enginePath / "out" / "dev-shared" / canonicalName).lexically_normal();
    if (!std::filesystem::is_regular_file(canonical)) {
        message = "Canonical native executable is missing: " + canonical.string() +
                  ". Build the shared engine tree before packaging the project.";
        return false;
    }

    std::error_code ec;
    std::filesystem::copy_file(canonical, target,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        message = "Cannot stage native executable from " + canonical.string() +
                  " to " + target.string() + " (" + ec.message() + ")";
        return false;
    }
    message = "Staged native executable " + target.filename().string() +
              " from " + canonical.string();
    return true;
}

bool BuildPipeline::stage_copy_dependencies(std::string& message) {
    const std::filesystem::path binDir = config_.build_path() / "Binaries";
    if (!ensure_directory(binDir, message)) return false;

    const std::filesystem::path canonicalBin =
        (config_.enginePath / "out" / "dev-shared").lexically_normal();
    if (!std::filesystem::is_directory(canonicalBin)) {
        message = "Canonical binary directory is missing: " + canonicalBin.string();
        return false;
    }

    std::error_code ec;
    std::size_t copied = 0;
    for (std::filesystem::directory_iterator it(canonicalBin, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".dll" && ext != ".so" && ext != ".dylib") continue;
        std::filesystem::copy_file(it->path(), binDir / it->path().filename(),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) break;
        ++copied;
    }
    if (ec) {
        message = "Failed staging runtime dependencies from " + canonicalBin.string() +
                  " (" + ec.message() + ")";
        return false;
    }
    message = "Staged " + std::to_string(copied) +
              " runtime dependencies beside the native executable";
    return true;
}

bool BuildPipeline::stage_generate_distributable(std::string& message) {
    const std::filesystem::path distDir = config_.build_path() / "Distributable";
    std::error_code ec;
    std::filesystem::remove_all(distDir, ec);
    if (ec) {
        message = "Cannot clear distributable directory: " + distDir.string() +
                  " (" + ec.message() + ")";
        return false;
    }
    if (!ensure_directory(distDir, message)) return false;

    // Final distributable: cooked content + package metadata + native binaries.
    const std::filesystem::path contentSrc = config_.build_path() / "Content";
    const std::filesystem::path packageSrc = config_.build_path() / "Package";
    const std::filesystem::path binariesSrc = config_.build_path() / "Binaries";
    if (!copy_directory_tree(contentSrc, distDir / "Content", message)) return false;
    if (!copy_directory_tree(packageSrc, distDir / "Package", message)) return false;
    if (!copy_directory_tree(binariesSrc, distDir / "Binaries", message)) return false;

    std::ofstream launch(distDir / "run_game.bat", std::ios::trunc);
    if (!launch) {
        message = "Cannot create distributable launcher: " +
                  (distDir / "run_game.bat").string();
        return false;
    }
    launch << "@echo off\n";
    launch << "cd /d %~dp0\n";
    launch << "Binaries\\" << config_.name << ".exe\n";
    if (!launch.good()) {
        message = "Failed writing distributable launcher";
        return false;
    }
    message = "Generated distributable at " + distDir.string();
    return true;
}

} // namespace Engine::Tools
