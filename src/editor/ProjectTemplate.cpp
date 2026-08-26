#include "ProjectTemplate.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Engine::Editor {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// ProjectTemplateRegistry
// ---------------------------------------------------------------------------

bool ProjectTemplateRegistry::register_template(ProjectTemplate tmpl) {
    if (tmpl.id.empty()) return false;
    if (by_id_.find(tmpl.id) != by_id_.end()) return false;
    order_.push_back(tmpl.id);
    by_id_.emplace(tmpl.id, std::move(tmpl));
    return true;
}

bool ProjectTemplateRegistry::contains(const std::string& id) const noexcept {
    return by_id_.find(id) != by_id_.end();
}

const ProjectTemplate* ProjectTemplateRegistry::find(const std::string& id) const noexcept {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? nullptr : &it->second;
}

std::size_t ProjectTemplateRegistry::size() const noexcept {
    return by_id_.size();
}

std::vector<ProjectTemplate> ProjectTemplateRegistry::templates() const {
    std::vector<ProjectTemplate> result;
    result.reserve(order_.size());
    for (const std::string& id : order_) {
        result.push_back(by_id_.at(id));
    }
    return result;
}

std::vector<std::string> ProjectTemplateRegistry::template_ids() const {
    return order_;
}

void ProjectTemplateRegistry::clear() noexcept {
    by_id_.clear();
    order_.clear();
}

// ---------------------------------------------------------------------------
// create_project_from_template
// ---------------------------------------------------------------------------

namespace {

std::string sanitize_slug(const std::string& name) {
    std::string slug = name;
    for (char& c : slug) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
            c = '_';
        }
    }
    // A slug made entirely of punctuation collapses to underscores; still
    // non-empty, which is all create_project_from_template requires.
    return slug;
}

// True when `relative` is safe to write under a project root: not absolute,
// not empty, and no ".." path segment (a template must never escape the
// project folder).
bool safe_relative_path(const std::string& relative) {
    if (relative.empty()) return false;
    const fs::path p(relative);
    if (p.is_absolute()) return false;
    for (const auto& part : p) {
        if (part == "..") return false;
    }
    return true;
}

} // namespace

std::string create_project_from_template(const std::string& root_dir,
                                         const std::string& name,
                                         const ProjectTemplate& tmpl,
                                         std::string& errorOut) {
    if (name.empty()) {
        errorOut = "project name is empty";
        return {};
    }
    if (tmpl.id.empty()) {
        errorOut = "template id is empty";
        return {};
    }
    // Refuse any path that would escape the project root before touching disk.
    for (const TemplateFile& f : tmpl.files) {
        if (!safe_relative_path(f.relative_path)) {
            errorOut = "template file has an unsafe relative path: " + f.relative_path;
            return {};
        }
    }

    const std::string slug = sanitize_slug(name);
    const fs::path root = fs::path(root_dir) / slug;

    std::error_code ec;
    if (fs::exists(root, ec)) {
        errorOut = "project folder already exists: " + root.string();
        return {};
    }

    // Materialize. Track that we created the root so a failure can roll back.
    std::vector<fs::path> written;
    const auto fail = [&](const std::string& msg) -> std::string {
        errorOut = msg;
        std::error_code ignored;
        for (auto it = written.rbegin(); it != written.rend(); ++it) {
            fs::remove(*it, ignored);
        }
        // Remove the root dir (and now-empty parents created for us) best-effort.
        fs::remove_all(root, ignored);
        return {};
    };

    for (const TemplateFile& f : tmpl.files) {
        const fs::path target = root / fs::path(f.relative_path);
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            return fail("could not create directory for: " + f.relative_path);
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail("could not write file: " + f.relative_path);
        }
        out << f.content;
        out.flush();
        if (!out) {
            return fail("write failed (flush) for: " + f.relative_path);
        }
        out.close();
        written.push_back(target);
    }

    // A template with zero files still materializes the (empty) project root.
    fs::create_directories(root, ec);
    if (ec) {
        return fail("could not create project folder");
    }

    return root.string();
}

// ---------------------------------------------------------------------------
// Built-in templates
// ---------------------------------------------------------------------------

void register_builtin_templates(ProjectTemplateRegistry& reg) {
    {
        ProjectTemplate t;
        t.id = "empty";
        t.name = "Empty";
        t.description = "Bare project: only a README and a project.json marker.";
        t.files = {
            {"README.md",
             "# Empty Project\n\nScaffolded by the VulkanCraft editor (template: empty).\n"},
            {"project.json",
             "{\n  \"name\": \"Empty\",\n  \"template\": \"empty\",\n  \"engine\": \"vulkancraft\"\n}\n"},
        };
        reg.register_template(std::move(t));
    }
    {
        ProjectTemplate t;
        t.id = "voxel_sandbox";
        t.name = "Voxel Sandbox";
        t.description = "Voxel world starter with a flat grass plane seed note.";
        t.files = {
            {"README.md",
             "# Voxel Sandbox\n\nStarter world for voxel gameplay experiments.\n"},
            {"project.json",
             "{\n  \"name\": \"Voxel Sandbox\",\n  \"template\": \"voxel_sandbox\",\n  \"engine\": \"vulkancraft\"\n}\n"},
            {"assets/scenes/notes.txt",
             "Flat world starter. Generate the scene via the editor's scene tool.\n"},
        };
        reg.register_template(std::move(t));
    }
    {
        ProjectTemplate t;
        t.id = "vehicle_demo";
        t.name = "Vehicle Demo";
        t.description = "Rigid-body vehicle starter (Jolt provider).";
        t.files = {
            {"README.md",
             "# Vehicle Demo\n\nDrivable rigid-body vehicle starter.\n"},
            {"project.json",
             "{\n  \"name\": \"Vehicle Demo\",\n  \"template\": \"vehicle_demo\",\n  \"engine\": \"vulkancraft\"\n}\n"},
        };
        reg.register_template(std::move(t));
    }
}

} // namespace Engine::Editor
