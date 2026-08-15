#pragma once
#include "ProjectConfig.hpp"
#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace Engine::Tools {

// Full build pipeline (README §39): Validate → Resolve dependencies → Import
// assets → Compile shaders → Cook → Package content → Build executable →
// Copy dependencies → Generate distributable.
//
// Each stage is implemented with real filesystem work where possible and is
// instrumented with timing so builds are reproducible and auditable.
class BuildPipeline final {
public:
    enum class Stage : uint8_t {
        Validate,
        ResolveDependencies,
        ImportAssets,
        CompileShaders,
        CookAssets,
        PackageContent,
        BuildExecutable,
        CopyDependencies,
        GenerateDistributable
    };

    static constexpr const char* stage_name(Stage stage) noexcept {
        switch (stage) {
            case Stage::Validate: return "Validate";
            case Stage::ResolveDependencies: return "ResolveDependencies";
            case Stage::ImportAssets: return "ImportAssets";
            case Stage::CompileShaders: return "CompileShaders";
            case Stage::CookAssets: return "CookAssets";
            case Stage::PackageContent: return "PackageContent";
            case Stage::BuildExecutable: return "BuildExecutable";
            case Stage::CopyDependencies: return "CopyDependencies";
            case Stage::GenerateDistributable: return "GenerateDistributable";
        }
        return "Unknown";
    }

    struct StageResult {
        Stage stage{Stage::Validate};
        bool success{false};
        double elapsedMilliseconds{0};
        std::string message;
    };

    struct BuildReport {
        std::vector<StageResult> stages;
        bool success{false};
        double totalMilliseconds{0};
        [[nodiscard]] const StageResult* last_stage() const {
            return stages.empty() ? nullptr : &stages.back();
        }
    };

    // Stage hooks let the host (editor CLI or tool) run the actual work;
    // default implementations perform meaningful filesystem operations.
    using StageHook = std::function<bool(Stage, const ProjectConfig&, std::string&)>;

    explicit BuildPipeline(const ProjectConfig& config);

    void set_stage_hook(Stage stage, StageHook hook);
    void clear_stage_hook(Stage stage);
    [[nodiscard]] bool has_hook(Stage stage) const noexcept;

    // Runs the pipeline end-to-end; aborts at the first failed stage.
    [[nodiscard]] BuildReport run();

    // Individual stages (usable by tests without a full build).
    [[nodiscard]] bool run_stage(Stage stage, std::string& message);

private:
    ProjectConfig config_;
    std::vector<StageHook> hooks_;
    std::chrono::steady_clock::time_point started_;
    double lastStageMs_{0};
    double now_ms() const noexcept;

    // Default implementations.
    bool stage_validate(std::string& message);
    bool stage_resolve_dependencies(std::string& message);
    bool stage_import_assets(std::string& message);
    bool stage_compile_shaders(std::string& message);
    bool stage_cook_assets(std::string& message);
    bool stage_package_content(std::string& message);
    bool stage_build_executable(std::string& message);
    bool stage_copy_dependencies(std::string& message);
    bool stage_generate_distributable(std::string& message);
};

} // namespace Engine::Tools
