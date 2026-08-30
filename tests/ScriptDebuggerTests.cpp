// ---------------------------------------------------------------------------
// ScriptDebuggerTests.cpp
//
// Covers (README sections 25 + 37 + 36):
//   • ScriptDebugger: breakpoints pause execution, step executes one
//     instruction, step over/out, call stack grows/shrinks, variable
//     inspection, watch expressions, states + event callbacks
//   • Real-VM bridge: attach(ScriptVM&), from_script_program() conversion
//   • ScriptHotReload: mtime+size detection, recompile + program swap,
//     global-state preservation, reload listeners
//   • PlayModeManager: separate Play World (Scene::clone_for_play), scene
//     copy/restore, N-tick simulation, editor world integrity (the legacy
//     EditorRuntimeBridge adapter was removed — test-only, 0 product users)
//
// Sources required when wiring this into CMake:
//   tests/ScriptDebuggerTests.cpp
//   src/engine/scripting/ScriptDebugger.cpp
//   src/engine/scripting/ScriptHotReload.cpp
//   src/engine/editor/play_mode/PlayMode.hpp
//   src/engine/scripting/ScriptRuntime.cpp
//   src/engine/core/uuid/UUID.cpp
//   src/engine/scene/Scene.cpp
//   src/engine/scene/ComponentRegistry.cpp
//   src/engine/core/serialization/Serializer.cpp
// link: vc_build_options glm
// ---------------------------------------------------------------------------

#include "../src/engine/scripting/ScriptDebugger.hpp"
#include "../src/engine/scripting/ScriptHotReload.hpp"
#include "../src/engine/editor/play_mode/PlayMode.hpp"
#include "../src/engine/scene/Scene.hpp"
#include "../src/engine/core/uuid/UUID.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace Engine;
using namespace Engine::Scripting;
using namespace Engine::Editor;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Script debugger check failed at line " << __LINE__ << ": " #condition "\n"; \
    return EXIT_FAILURE; \
} } while (false)

void section(const char* name) {
    std::cout << "[section] " << name << "\n";
}

bool near(float a, float b) {
    return std::abs(a - b) < 0.0001f;
}

// main {0..4}: Push 1, Call helper, Store "ten", Return
// helper {4..6}: Push 10, Return
DebugProgram make_call_program() {
    DebugProgram p;
    auto inst = [](DebugOpCode op, int line) {
        DebugInstruction i;
        i.opcode = op;
        i.line = line;
        return i;
    };
    p.instructions.resize(6);
    p.instructions[0] = inst(DebugOpCode::Push, 1);
    p.instructions[0].operand = ScriptValue{ 1.0 };
    p.instructions[1] = inst(DebugOpCode::Call, 2);
    p.instructions[1].target = 4;
    p.instructions[2] = inst(DebugOpCode::StoreVariable, 3);
    p.instructions[2].text = "ten";
    p.instructions[3] = inst(DebugOpCode::Return, 4);
    p.instructions[4] = inst(DebugOpCode::Push, 5);
    p.instructions[4].operand = ScriptValue{ 10.0 };
    p.instructions[5] = inst(DebugOpCode::Return, 6);
    p.entries["main"] = 0;
    p.functions.push_back({ "main", 0, 4 });
    p.functions.push_back({ "helper", 4, 6 });
    return p;
}

ScriptProgram make_real_vm_program() {
    ScriptProgram p;
    Instruction push1;  push1.opcode = OpCode::PushInteger;  push1.operand = ScriptValue{ int64_t(1) };
    Instruction push2;  push2.opcode = OpCode::PushInteger;  push2.operand = ScriptValue{ int64_t(2) };
    Instruction add;    add.opcode = OpCode::AddFloat;
    Instruction store;  store.opcode = OpCode::StoreVariable; store.text = "sum";
    Instruction ret;    ret.opcode = OpCode::Return;
    p.instructions = { push1, push2, add, store, ret };
    p.eventEntries["init"] = 0;
    return p;
}

} // namespace

int main() {
    // =====================================================================
    // A) ScriptDebugger — self-hosted interpreter
    // =====================================================================
    section("debugger: breakpoints pause execution");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        dbg.add_breakpoint(2); // StoreVariable "ten"
        CHECK(dbg.start_event("main"));
        CHECK(dbg.state() == ScriptDebugState::Running);
        CHECK(dbg.continue_run(1000) == ScriptDebugState::Paused);
        CHECK(dbg.instruction_pointer() == 2);
        CHECK(dbg.frame_count() == 1);
        CHECK(!dbg.variable("ten")); // paused BEFORE the instruction ran
    }

    section("debugger: step executes exactly one instruction");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        dbg.add_breakpoint(2);
        dbg.start_event("main");
        dbg.continue_run(1000);
        CHECK(dbg.state() == ScriptDebugState::Paused);

        CHECK(dbg.step_into() == ScriptDebugState::Running); // StoreVariable "ten" (ip 2 → 3)
        CHECK(dbg.instruction_pointer() == 3);
        CHECK(dbg.variable("ten") && *dbg.variable("ten") == "10");

        CHECK(dbg.step_into() == ScriptDebugState::Stopped); // Return ends main
    }

    section("debugger: call stack grows and shrinks");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        CHECK(dbg.start_event("main"));
        CHECK(dbg.frame_count() == 1);
        CHECK(dbg.call_stack().front().name == "main");

        CHECK(dbg.step_into() == ScriptDebugState::Running); // Push 1
        CHECK(dbg.frame_count() == 1);

        CHECK(dbg.step_into() == ScriptDebugState::Running); // Call helper
        CHECK(dbg.frame_count() == 2);
        CHECK(dbg.call_stack().back().name == "helper");

        CHECK(dbg.step_into() == ScriptDebugState::Running); // Push 10
        CHECK(dbg.frame_count() == 2);

        CHECK(dbg.step_into() == ScriptDebugState::Running); // Return → caller
        CHECK(dbg.frame_count() == 1);
        CHECK(dbg.instruction_pointer() == 2);

        CHECK(dbg.step_into() == ScriptDebugState::Running); // Store ten
        CHECK(dbg.step_into() == ScriptDebugState::Stopped); // Return → done
        CHECK(dbg.frame_count() == 0);
    }

    section("debugger: step over skips a whole call");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        dbg.start_event("main");
        dbg.step_into(); // Push 1 → ip 1 (standing on Call)
        CHECK(dbg.step_over() == ScriptDebugState::Running);
        CHECK(dbg.instruction_pointer() == 2); // right after the call returned
        CHECK(dbg.frame_count() == 1);
        CHECK(!dbg.variable("ten")); // StoreVariable not reached yet
        dbg.step_into();
        CHECK(dbg.variable("ten") && *dbg.variable("ten") == "10");
    }

    section("debugger: step out returns to the caller");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        dbg.start_event("main");
        dbg.step_into(); // Push 1
        dbg.step_into(); // Call → inside helper (ip 4, 2 frames)
        CHECK(dbg.frame_count() == 2);
        CHECK(dbg.step_out() == ScriptDebugState::Running);
        CHECK(dbg.instruction_pointer() == 2);
        CHECK(dbg.frame_count() == 1);
    }

    section("debugger: line breakpoints map to instructions");
    {
        ScriptDebugger dbg;
        DebugProgram p = make_call_program();
        p.lineToInstruction[42] = 2;
        dbg.load(std::move(p));
        dbg.add_line_breakpoint(42);
        CHECK(dbg.has_breakpoint(2));
        CHECK(dbg.breakpoint_at_line(42) && *dbg.breakpoint_at_line(42) == 2);
        dbg.start_event("main");
        CHECK(dbg.continue_run(1000) == ScriptDebugState::Paused);
        CHECK(dbg.instruction_pointer() == 2);
        CHECK(dbg.current_line() == 42);
    }

    section("debugger: variables are inspectable and watch expressions evaluate");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        dbg.start_event("main");
        CHECK(dbg.continue_run(1000) == ScriptDebugState::Stopped);
        CHECK(dbg.variable("ten") && *dbg.variable("ten") == "10");
        CHECK(dbg.variables().count("ten") == 1);

        dbg.set_variable("x", ScriptValue{ int64_t(42) });
        CHECK(dbg.variable("x") && *dbg.variable("x") == "42");

        dbg.add_watch("ten + 1");
        dbg.add_watch("x * 2 == 84");
        dbg.add_watch("(ten == 10) && (x >= 42)");
        dbg.add_watch("missing");
        dbg.add_watch("1 / 0");
        dbg.evaluate_watches();
        CHECK(dbg.watches().size() == 5);
        CHECK(dbg.watches()[0].result == "11");
        CHECK(dbg.watches()[1].result == "true");
        CHECK(dbg.watches()[2].result == "true");
        CHECK(dbg.watches()[3].result == "undefined");
        CHECK(dbg.watches()[4].result == "error: division by zero");

        const size_t watchId = dbg.watches()[0].id;
        CHECK(dbg.remove_watch(watchId));
        CHECK(dbg.watches().size() == 4);
    }

    section("debugger: state and breakpoint event callbacks fire");
    {
        ScriptDebugger dbg;
        dbg.load(make_call_program());
        dbg.add_breakpoint(2);
        std::vector<ScriptDebugState> states;
        size_t breakpointHits = 0;
        dbg.set_state_callback([&](ScriptDebugState s) { states.push_back(s); });
        dbg.set_breakpoint_callback([&](size_t, int) { ++breakpointHits; });
        dbg.start_event("main");
        dbg.continue_run(1000);
        CHECK(dbg.state() == ScriptDebugState::Paused);
        CHECK(breakpointHits == 1);
        bool sawPaused = false;
        for (auto s : states) if (s == ScriptDebugState::Paused) sawPaused = true;
        CHECK(sawPaused);
    }

    // =====================================================================
    // B) ScriptDebugger — real ScriptVM integration
    // =====================================================================
    section("debugger: from_script_program bridges real bytecode");
    {
        ScriptProgram prog = make_real_vm_program();
        DebugProgram converted = ScriptDebugger::from_script_program(prog);
        CHECK(converted.instructions.size() == 5);
        CHECK(converted.entries.count("init") == 1 && converted.entries.at("init") == 0);
        CHECK(converted.functions.size() == 1 && converted.functions[0].name == "main");

        ScriptDebugger dbg;
        dbg.load(std::move(converted));
        CHECK(dbg.start_event("init"));
        CHECK(dbg.continue_run(1000) == ScriptDebugState::Stopped);
        CHECK(dbg.variable("sum") && *dbg.variable("sum") == "3");
    }

    section("debugger: attach() drives a live ScriptVM");
    {
        ScriptVM vm;
        vm.load(make_real_vm_program());
        ScriptDebugger dbg;
        dbg.attach(vm);
        CHECK(dbg.attached());
        CHECK(dbg.start_event("init"));
        dbg.add_breakpoint(3); // StoreVariable "sum"
        CHECK(dbg.continue_run(1000) == ScriptDebugState::Paused);
        CHECK(vm.status() == VMStatus::Paused);
        CHECK(dbg.variable("sum") && *dbg.variable("sum") == "3");

        dbg.add_watch("sum * 2");
        dbg.evaluate_watches();
        CHECK(dbg.watches().size() == 1 && dbg.watches()[0].result == "6");

        CHECK(dbg.step_into() == ScriptDebugState::Stopped); // Return
        CHECK(vm.status() == VMStatus::Completed);
    }

    // =====================================================================
    // C) ScriptHotReload
    // =====================================================================
    section("hot reload: mtime+size detection, recompile, state preservation");
    {
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path() / ("script_hot_reload_" + UUID().to_string());
        std::filesystem::create_directories(tmp);
        const std::filesystem::path script = tmp / "test.script";

        std::string contentA = "payload=alpha";
        std::string contentB = "payload=beta-longer";
        std::string contentBad = "BAD payload";
        {
            std::ofstream out(script, std::ios::trunc);
            out << contentA;
        }

        size_t lastCompiledSize = 0;
        ScriptHotReload hr;
        hr.set_compiler([&](const std::filesystem::path& p) -> ScriptCompileResult {
            std::ifstream in(p);
            std::string content((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
            lastCompiledSize = content.size();
            ScriptCompileResult result;
            if (content.find("BAD") != std::string::npos) {
                result.success = false;
                result.diagnostics.push_back({ UUID(), "syntax error: BAD marker" });
                return result;
            }
            result.success = true;
            Instruction push;
            push.opcode = OpCode::PushInteger;
            push.operand = ScriptValue{ int64_t(content.size()) };
            Instruction store;
            store.opcode = OpCode::StoreVariable;
            store.text = "payloadSize";
            Instruction ret;
            ret.opcode = OpCode::Return;
            result.program.instructions = { push, store, ret };
            result.program.eventEntries["init"] = 0;
            return result;
        });

        int reloadEvents = 0;
        bool lastOk = false;
        std::string lastMessage;
        hr.set_listener([&](const std::filesystem::path&, bool ok, const std::string& msg) {
            ++reloadEvents;
            lastOk = ok;
            lastMessage = msg;
        });

        ScriptVM vm;
        vm.set_variable("persisted", ScriptValue{ int64_t(42) });

        CHECK(hr.watch(script));
        CHECK(hr.watched_count() == 1);
        CHECK(!hr.file_changed(script));   // unchanged
        CHECK(hr.poll(vm) == 0);           // no change → no reload
        CHECK(reloadEvents == 0);

        {
            std::ofstream out(script, std::ios::trunc);
            out << contentB;
        }
        CHECK(hr.file_changed(script));    // mtime/size differ
        CHECK(hr.changed_count() == 1);
        CHECK(hr.poll(vm) == 1);           // one successful reload
        CHECK(reloadEvents == 1 && lastOk);
        CHECK(lastCompiledSize == contentB.size());

        // Global state survived the program swap.
        const ScriptValue* persisted = vm.variable("persisted");
        CHECK(persisted && std::holds_alternative<int64_t>(*persisted) &&
              std::get<int64_t>(*persisted) == 42);

        // The new program is live: running it stores the new payload size.
        CHECK(vm.start_event("init"));
        CHECK(vm.run(0.016f) == VMStatus::Completed);
        CHECK(vm.float_variable("payloadSize") == static_cast<double>(contentB.size()));

        // Broken script: poll detects the change, compile fails, listener is
        // notified with the error, and the previous program stays active.
        {
            std::ofstream out(script, std::ios::trunc);
            out << contentBad;
        }
        CHECK(hr.file_changed(script));
        CHECK(hr.poll(vm) == 0);
        CHECK(reloadEvents == 2 && !lastOk);
        CHECK(lastMessage.find("BAD") != std::string::npos);
        CHECK(vm.float_variable("payloadSize") == static_cast<double>(contentB.size()));
        CHECK(vm.variable("persisted") != nullptr);

        CHECK(hr.unwatch(script));
        CHECK(hr.watched_count() == 0);
        CHECK(!hr.watch(tmp / "does_not_exist.script"));

        std::filesystem::remove_all(tmp);
    }

    section("hot reload: snapshot/restore globals helpers");
    {
        ScriptVM vm;
        vm.set_variable("hp", ScriptValue{ 100.0 });
        vm.set_variable("ammo", ScriptValue{ int64_t(30) });
        const auto globals = ScriptHotReload::snapshot_globals(vm);
        CHECK(globals.size() == 2);

        ScriptVM fresh;
        ScriptHotReload::restore_globals(fresh, globals);
        CHECK(fresh.variable("hp") != nullptr);
        CHECK(fresh.variable("ammo") != nullptr);
        CHECK(fresh.float_variable("hp") == 100.0);
        CHECK(fresh.float_variable("ammo") == 30.0);
    }

    // =====================================================================
    // D) Play-mode isolation — PlayModeManager (canonical editor contract).
    // The legacy EditorRuntimeBridge adapter duplicated this state machine
    // with 0 product consumers (test-only) and was REMOVED; the product
    // drives PlayModeManager directly. The play world is a separate copy
    // (clone_for_play) and the editor scene stays intact.
    // =====================================================================
    const auto scene_signature = [](const Scene& scene) -> std::string {
        std::vector<Engine::UUID> ids;
        ids.reserve(scene.get_entities().size());
        for (const auto& [id, entity] : scene.get_entities()) {
            (void)entity;
            ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        std::string signature;
        for (const auto& id : ids) {
            const Entity* entity = scene.find_entity_by_id_const(id);
            if (!entity) continue;
            signature += id.to_string();
            signature += '|';
            signature += entity->get_name();
            signature += '|';
            const auto it = scene.transformComponents.find(id);
            if (it != scene.transformComponents.end()) {
                const auto& t = it->second;
                char buffer[160];
                std::snprintf(buffer, sizeof(buffer),
                              "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g;",
                              t.position.x, t.position.y, t.position.z,
                              t.rotation.x, t.rotation.y, t.rotation.z,
                              t.scale.x, t.scale.y, t.scale.z);
                signature += buffer;
            }
            signature += '\n';
        }
        return std::to_string(std::hash<std::string>{}(signature));
    };

    section("play-mode: Play World is a separate copy; editor stays intact");
    {
        PlayModeManager manager;
        Scene editor("Editor Scene");
        const Entity cube = editor.create_entity("Cube");
        editor.transformComponents[cube.get_id()].position = { 1.0f, 2.0f, 3.0f };
        CHECK(editor.get_entities().size() == 1);

        const std::string editorSignature = scene_signature(editor);
        manager.set_editor_scene(&editor);
        CHECK(manager.get_state() == PlayState::Edit);

        int tickCount = 0;
        auto tickOnce = [&](Scene* world, float) {
            ++tickCount;
            if (!world->transformComponents.empty()) {
                auto& t = world->transformComponents.begin()->second;
                t.position.x += 1.0f;
            }
        };

        // start_play clones the editor scene into a separate Play World.
        manager.start_play(&editor);
        CHECK(manager.get_state() == PlayState::Play);
        Scene* playWorld = manager.get_active_scene();
        CHECK(playWorld != nullptr && playWorld != &editor);
        CHECK(playWorld->get_name() == "Editor Scene [PLAY]");
        CHECK(playWorld->get_entities().size() == 1);
        CHECK(near(playWorld->transformComponents.begin()->second.position.x, 1.0f));
        CHECK(near(editor.transformComponents.at(cube.get_id()).position.x, 1.0f));
        CHECK(scene_signature(editor) == editorSignature);

        // Pause + step_frame runs exactly one tick on the Play World only.
        manager.pause_play();
        CHECK(manager.get_state() == PlayState::Pause);
        manager.step_frame(1.0f / 60.0f, tickOnce);
        CHECK(tickCount == 1);
        CHECK(near(playWorld->transformComponents.begin()->second.position.x, 2.0f));
        CHECK(near(editor.transformComponents.at(cube.get_id()).position.x, 1.0f));
        CHECK(scene_signature(editor) == editorSignature);

        // Pause toggles back to Play (the editor's play/pause button path).
        manager.pause_play();
        CHECK(manager.get_state() == PlayState::Play);
        manager.stop_play();
        CHECK(manager.get_state() == PlayState::Edit);
        CHECK(manager.get_active_scene() == nullptr); // play world destroyed
        CHECK(near(editor.transformComponents.at(cube.get_id()).position.x, 1.0f));
        CHECK(scene_signature(editor) == editorSignature);

        // N-tick burst from Edit: clone → pause → tick N times → restore Edit.
        manager.start_play(&editor);
        playWorld = manager.get_active_scene();
        manager.pause_play();
        CHECK(manager.get_state() == PlayState::Pause);
        for (int i = 0; i < 5; ++i) manager.step_frame(1.0f / 60.0f, tickOnce);
        CHECK(tickCount == 6);
        CHECK(near(playWorld->transformComponents.begin()->second.position.x, 6.0f));
        CHECK(near(editor.transformComponents.at(cube.get_id()).position.x, 1.0f));
        manager.stop_play();
        CHECK(manager.get_state() == PlayState::Edit);

        // External editor edits are detected via the scene signature.
        manager.start_play(&editor);
        editor.transformComponents.at(cube.get_id()).position.x = 99.0f;
        CHECK(scene_signature(editor) != editorSignature);
        editor.transformComponents.at(cube.get_id()).position.x = 1.0f;
        CHECK(scene_signature(editor) == editorSignature);
        manager.stop_play();
        CHECK(manager.get_state() == PlayState::Edit);

        // Simulate mode round-trips through the canonical manager.
        manager.start_simulate(&editor);
        CHECK(manager.get_state() == PlayState::Simulate);
        manager.pause_play();
        CHECK(manager.get_state() == PlayState::Pause);
        manager.pause_play(); // toggle resumes (canonical manager behavior)
        CHECK(manager.get_state() == PlayState::Play);
        manager.stop_play();
        CHECK(manager.get_state() == PlayState::Edit);
    }

    std::cout << "All script debugger / hot reload / bridge checks passed.\n";
    return EXIT_SUCCESS;
}
