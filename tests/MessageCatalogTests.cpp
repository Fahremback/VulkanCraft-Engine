// MessageCatalogTests — headless coverage for the public message-catalog
// contract (engine/editor/IMessageCatalog.hpp, adapter MessageCatalog.cpp):
// stable ids, severity, parameterized render ({0}..{n}), actionable hints,
// all-or-nothing load/add, deterministic ids(), and safe unknown/missing-param
// rendering. Standalone main() with CHECK.

#include "engine/editor/IMessageCatalog.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace engine::editor;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "MessageCatalogTests failure at line " << __LINE__ << ": " #condition "\n"; \
    return false; } } while (false)

namespace {

MessageCatalogDoc make_doc() {
    MessageCatalogDoc doc;
    {
        CatalogMessage m;
        m.id = "asset.import.failed";
        m.severity = MessageSeverity::Error;
        m.text = "Failed to import {0}: {1}";
        m.action = "asset:open_import_settings";
        doc.messages.push_back(m);
    }
    {
        CatalogMessage m;
        m.id = "save.ok";
        m.severity = MessageSeverity::Info;
        m.text = "Saved {0}";
        doc.messages.push_back(m);
    }
    {
        CatalogMessage m;
        m.id = "command.unknown";
        m.severity = MessageSeverity::Warning;
        m.text = "Unknown command {0}";
        doc.messages.push_back(m);
    }
    return doc;
}

bool run_all() {
    std::string err;

    // ---- Doc validate + JSON round-trip ----------------------------------
    {
        const MessageCatalogDoc doc = make_doc();
        CHECK(doc.validate(err));
        const std::string json = doc.to_json();
        MessageCatalogDoc back;
        CHECK(back.load_from_json(json, err));
        CHECK(back.messages.size() == 3);
        CHECK(back.messages[0].id == "asset.import.failed");
        CHECK(back.messages[0].severity == MessageSeverity::Error);
        CHECK(back.to_json() == json);  // bit-exact

        // Refusals do not mutate.
        MessageCatalogDoc untouched = make_doc();
        CHECK(!untouched.load_from_json("{bad", err));
        CHECK(untouched.messages.size() == 3);
        CHECK(!untouched.load_from_json("{\"version\":99}", err));
        CHECK(untouched.messages.size() == 3);
        CHECK(!untouched.load_from_json(
            "{\"version\":1,\"messages\":[{\"id\":\"x\",\"severity\":\"epic\","
            "\"text\":\"t\"}]}", err));
        CHECK(untouched.messages.size() == 3);
        // Duplicate ids refused.
        CHECK(!untouched.load_from_json(
            "{\"version\":1,\"messages\":[{\"id\":\"a\",\"text\":\"t\"},"
            "{\"id\":\"a\",\"text\":\"t\"}]}", err));
        CHECK(untouched.messages.size() == 3);
    }

    // ---- Lookup + render --------------------------------------------------
    {
        auto catalog = create_message_catalog(make_doc(), err);
        CHECK(catalog != nullptr);

        const CatalogMessage* found = catalog->find("save.ok");
        CHECK(found != nullptr);
        CHECK(found->severity == MessageSeverity::Info);
        CHECK(catalog->find("nope") == nullptr);

        std::string out = catalog->render("asset.import.failed",
                                          { "texture.png", "bad format" }, err);
        CHECK(out == "Failed to import texture.png: bad format");
        CHECK(err.empty());

        out = catalog->render("save.ok", { "scene.vc" }, err);
        CHECK(out == "Saved scene.vc");
        CHECK(err.empty());
    }

    // ---- Actionable hint --------------------------------------------------
    {
        auto catalog = create_message_catalog(make_doc(), err);
        CHECK(catalog != nullptr);
        CHECK(catalog->find("asset.import.failed")->action ==
              "asset:open_import_settings");
    }

    // ---- Safe unknown id / missing params ---------------------------------
    {
        auto catalog = create_message_catalog(make_doc(), err);
        CHECK(catalog != nullptr);

        const std::string unknown = catalog->render("nope", {}, err);
        CHECK(unknown == "[unknown:nope]");
        CHECK(!err.empty());

        err.clear();
        const std::string missing = catalog->render("save.ok", {}, err);
        CHECK(missing == "Saved {0}");  // placeholder stays visible
        CHECK(!err.empty());            // diagnostic reported
    }

    // ---- add: all-or-nothing ---------------------------------------------
    {
        auto catalog = create_message_catalog(make_doc(), err);
        CHECK(catalog != nullptr);
        const std::size_t before = catalog->size();

        CatalogMessage m;
        m.id = "new.msg";
        m.severity = MessageSeverity::Warning;
        m.text = "Hello {0}";
        CHECK(catalog->add(m, err));
        CHECK(catalog->size() == before + 1);
        CHECK(catalog->find("new.msg") != nullptr);
        CHECK(catalog->render("new.msg", { "world" }, err) == "Hello world");

        // Duplicate refused (no mutation).
        CatalogMessage dup = m;
        dup.id = "save.ok";
        CHECK(!catalog->add(dup, err));
        CHECK(catalog->size() == before + 1);

        CatalogMessage empty;
        CHECK(!catalog->add(empty, err));
        CHECK(catalog->size() == before + 1);
    }

    // ---- Deterministic ids() ----------------------------------------------
    {
        auto catalog = create_message_catalog(make_doc(), err);
        CHECK(catalog != nullptr);
        const std::vector<std::string> ids = catalog->ids();
        CHECK(ids.size() == 3);
        CHECK(ids[0] == "asset.import.failed");
        CHECK(ids[1] == "command.unknown");
        CHECK(ids[2] == "save.ok");  // sorted
    }

    // ---- Determinism cross-instance --------------------------------------
    {
        const auto doc = make_doc();
        auto a = create_message_catalog(doc, err);
        auto b = create_message_catalog(doc, err);
        CHECK(a != nullptr && b != nullptr);
        CHECK(a->ids() == b->ids());
        CHECK(a->render("asset.import.failed", { "x", "y" }, err) ==
              b->render("asset.import.failed", { "x", "y" }, err));
    }

    std::cout << "MessageCatalogTests: all checks passed\n";
    return true;
}

}  // namespace

int main() {
    if (!run_all()) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
