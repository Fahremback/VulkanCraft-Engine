#pragma once

// ProjectTemplate — data-driven project scaffolding (ezEngine "project
// templates" pillar). Model-only: no ImGui, no GPU, fully headless-testable.
//
// A template is a stable id + display metadata + an ordered list of files
// (relative paths + content) written into a new project folder. The editor's
// project wizard lists templates from the registry and creates a project by
// materializing the scaffold; the existing `EditorApplication::create_project`
// (hardcoded single scene) becomes one built-in template's scene generator.
//
// Creation is all-or-nothing: `create_project_from_template` validates the
// name/template, refuses an existing folder, and rolls back (removes the
// created folder) if any file write fails — a half-written project is never
// left behind.

#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::Editor {

/// One file to materialize into the project root (relative path + content).
struct TemplateFile {
    std::string relative_path; ///< e.g. "README.md", "src/main.cpp", "project.json"
    std::string content;
};

/// A project template: metadata + scaffold files.
struct ProjectTemplate {
    std::string id;         ///< stable, unique id (e.g. "empty", "voxel_sandbox")
    std::string name;       ///< display name (localization is the wizard's job)
    std::string description;
    std::vector<TemplateFile> files; ///< scaffold, materialized in order
};

/// Owns the templates the project wizard offers. Insertion order is stable so
/// the wizard lists them deterministically. Registration is all-or-nothing
/// (empty/duplicate id refused without mutating the registry).
class ProjectTemplateRegistry {
public:
    bool register_template(ProjectTemplate tmpl);

    [[nodiscard]] bool contains(const std::string& id) const noexcept;
    [[nodiscard]] const ProjectTemplate* find(const std::string& id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

    /// All templates in insertion order.
    [[nodiscard]] std::vector<ProjectTemplate> templates() const;
    /// Just the ids, in insertion order.
    [[nodiscard]] std::vector<std::string> template_ids() const;

    void clear() noexcept;

private:
    std::unordered_map<std::string, ProjectTemplate> by_id_;
    std::vector<std::string> order_;
};

/// Materialize a project folder from a template. On success returns the root
/// path (non-empty) and leaves `errorOut` untouched; on failure returns an
/// empty string and writes a human-readable diagnostic to `errorOut`, with the
/// filesystem left unchanged (partial output removed).
std::string create_project_from_template(const std::string& root_dir,
                                         const std::string& name,
                                         const ProjectTemplate& tmpl,
                                         std::string& errorOut);

/// Built-in templates the editor registers at startup.
void register_builtin_templates(ProjectTemplateRegistry& reg);

} // namespace Engine::Editor
