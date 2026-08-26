// ProjectTemplateTests — headless coverage for the editor's data-driven
// project template system (ProjectTemplate.hpp/cpp). Mirrors the ezEngine
// "project templates" pillar: templates register stable metadata + scaffold
// files; create_project_from_template materializes a project all-or-nothing.
// No UI, no GPU. Standalone main() with CHECK (pattern: EditorPluginTests).

#include "../src/editor/ProjectTemplate.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace Engine::Editor;
namespace fs = std::filesystem;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "ProjectTemplateTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

ProjectTemplate make_template(std::string id, std::vector<TemplateFile> files) {
    ProjectTemplate t;
    t.id = std::move(id);
    t.name = "Test " + t.id;
    t.description = "test template";
    t.files = std::move(files);
    return t;
}

bool run_all() {
    const fs::path tmp = fs::temp_directory_path() / "vc_pt_tests";
    std::error_code ec;
    fs::remove_all(tmp, ec);

    // ---- Registry ---------------------------------------------------------
    ProjectTemplateRegistry reg;
    CHECK(reg.size() == 0);
    CHECK(reg.register_template(make_template("empty", {})));
    CHECK(reg.register_template(make_template("sandbox", {})));
    CHECK(reg.register_template(make_template("demo", {})));
    CHECK(reg.size() == 3);
    CHECK(reg.contains("empty"));
    CHECK(!reg.contains("missing"));
    CHECK(reg.find("sandbox") != nullptr);
    CHECK(reg.find("sandbox")->name == "Test sandbox");
    CHECK(reg.find("nope") == nullptr);

    // Duplicate / empty id refused without mutation.
    CHECK(!reg.register_template(make_template("empty", {})));
    CHECK(reg.size() == 3);
    CHECK(!reg.register_template(make_template("", {})));
    CHECK(reg.size() == 3);

    // Stable insertion order (templates + ids).
    const std::vector<ProjectTemplate> all = reg.templates();
    CHECK(all.size() == 3);
    CHECK(all[0].id == "empty");
    CHECK(all[1].id == "sandbox");
    CHECK(all[2].id == "demo");
    const std::vector<std::string> ids = reg.template_ids();
    CHECK(ids.size() == 3);
    CHECK(ids[0] == "empty");

    reg.clear();
    CHECK(reg.size() == 0);

    // Built-in templates registered and non-empty.
    register_builtin_templates(reg);
    CHECK(reg.size() >= 3);
    CHECK(reg.contains("empty"));
    CHECK(reg.contains("voxel_sandbox"));
    CHECK(reg.contains("vehicle_demo"));
    CHECK(!reg.find("empty")->files.empty());
    CHECK(!reg.find("voxel_sandbox")->files.empty());
    CHECK(!reg.find("vehicle_demo")->files.empty());

    // ---- create_project_from_template ------------------------------------
    ProjectTemplate tpl = make_template("rich", {
        {"README.md", "hello\n"},
        {"assets/scenes/notes.txt", "notes\n"},
        {"project.json", "{}\n"},
    });
    std::string err;

    // Happy path: nested dirs + content round-trip.
    std::string root = create_project_from_template(tmp.string(), "My Game", tpl, err);
    CHECK(!root.empty());
    CHECK(err.empty());
    CHECK(fs::exists(fs::path(root) / "README.md"));
    CHECK(read_file(fs::path(root) / "README.md") == "hello\n");
    CHECK(fs::exists(fs::path(root) / "assets" / "scenes" / "notes.txt"));
    CHECK(read_file(fs::path(root) / "assets" / "scenes" / "notes.txt") == "notes\n");
    CHECK(fs::exists(fs::path(root) / "project.json"));

    // Name sanitization: "My Game" -> "My_Game".
    CHECK(fs::path(root).filename() == "My_Game");

    // Existing folder refused (all-or-nothing: no clobber).
    const std::string before = read_file(fs::path(root) / "README.md");
    std::string dup = create_project_from_template(tmp.string(), "My Game", tpl, err);
    CHECK(dup.empty());
    CHECK(!err.empty());
    CHECK(read_file(fs::path(root) / "README.md") == before); // untouched

    // Empty name refused.
    err.clear();
    CHECK(create_project_from_template(tmp.string(), "", tpl, err).empty());
    CHECK(!err.empty());

    // Unsafe relative path (..) refused without touching disk.
    ProjectTemplate evil = make_template("evil", {{"../escape.txt", "x\n"}});
    err.clear();
    const fs::path beforeEvil = tmp / "escape.txt";
    CHECK(create_project_from_template(tmp.string(), "Evil", evil, err).empty());
    CHECK(!err.empty());
    CHECK(!fs::exists(beforeEvil));

    // Absolute path refused.
    ProjectTemplate abs = make_template("abs", {{"C:/definitely/not/here.txt", "x\n"}});
    err.clear();
    CHECK(create_project_from_template(tmp.string(), "Abs", abs, err).empty());
    CHECK(!err.empty());

    // Empty relative path refused.
    ProjectTemplate emptyFile = make_template("emptyfile", {{"", "x\n"}});
    err.clear();
    CHECK(create_project_from_template(tmp.string(), "EmptyFile", emptyFile, err).empty());
    CHECK(!err.empty());

    // Zero-file template still materializes the folder.
    ProjectTemplate empty = make_template("no_files", {});
    err.clear();
    root = create_project_from_template(tmp.string(), "Bare", empty, err);
    CHECK(!root.empty());
    CHECK(fs::exists(fs::path(root)));
    CHECK(fs::is_directory(fs::path(root)));

    fs::remove_all(tmp, ec);
    std::cout << "ProjectTemplateTests: all checks passed\n";
    return true;
}

} // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
