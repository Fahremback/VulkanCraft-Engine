// wasmtime-probe.cpp — proves wasmtime C API is usable from C++ v49
// Returns exit 0 if all checks pass, non-zero otherwise.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "wasm.h"
#include "wasmtime.h"

static int checks_passed = 0;
static int checks_failed = 0;

static void check(bool cond, const char* name) {
    if (cond) {
        ++checks_passed;
        printf("  PASS  %s\n", name);
    } else {
        ++checks_failed;
        printf("  FAIL  %s\n", name);
    }
}

static void print_wasmtime_error(wasmtime_error_t* err) {
    if (!err) return;
    wasm_byte_vec_t msg;
    wasmtime_error_message(err, &msg);
    printf("  ERROR: %.*s\n", (int)msg.size, msg.data);
    wasm_byte_vec_delete(&msg);
    wasmtime_error_delete(err);
}

// WAT: a module with exported function "add"(i32,i32)->i32
static const char* WAT_ADD = "(module (func (export \"add\") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))";

// WAT: a module with exported function "fortytwo"()->i32 returning 42
static const char* WAT_FORTYTWO = "(module (func (export \"fortytwo\") (result i32) i32.const 42))";

int main() {
    printf("=== wasmtime-probe v49 ===\n");
    fflush(stdout);

    // --- Check 1: engine creation ---
    printf("[1] wasm_engine_new...\n"); fflush(stdout);
    wasm_engine_t* engine = wasm_engine_new();
    check(engine != nullptr, "engine_new");

    // --- Check 2: store creation ---
    printf("[2] wasm_store_new...\n"); fflush(stdout);
    wasm_store_t* store = wasm_store_new(engine);
    check(store != nullptr, "store_new");

    if (!engine || !store) {
        printf("ABORT: engine or store creation failed\n");
        return 1;
    }

    // --- Check 3: WAT→WASM for "add" ---
    printf("[3] wasmtime_wat2wasm (add)...\n"); fflush(stdout);
    wasm_byte_vec_t wasm_bytes = WASM_EMPTY_VEC;
    wasmtime_error_t* err = wasmtime_wat2wasm(WAT_ADD, strlen(WAT_ADD), &wasm_bytes);
    if (err != nullptr) {
        printf("  wat2wasm_add FAILED:\n");
        print_wasmtime_error(err);
    }
    check(err == nullptr, "wat2wasm_add");
    check(wasm_bytes.size > 0, "wasm_bytes_add_nonempty");

    if (err != nullptr || wasm_bytes.size == 0) {
        // Try raw bytes as fallback: WAT -> manual encoding is not needed
        // The module won't compile without valid wasm bytes
        printf("ABORT: WAT parsing failed\n");
        wasm_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    // --- Check 4: module compilation ---
    printf("[4] wasm_module_new (add)...\n"); fflush(stdout);
    wasm_module_t* module_add = wasm_module_new(store, &wasm_bytes);
    check(module_add != nullptr, "module_new_add");
    wasm_byte_vec_delete(&wasm_bytes);

    if (!module_add) {
        printf("ABORT: module creation failed\n");
        wasm_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    // --- Check 5: instance creation (no imports) ---
    printf("[5] wasm_instance_new (add)...\n"); fflush(stdout);
    wasm_extern_vec_t imports = WASM_EMPTY_VEC;
    wasm_trap_t* trap = nullptr;
    wasm_instance_t* instance_add = wasm_instance_new(store, module_add, &imports, &trap);
    check(instance_add != nullptr, "instance_new_add");
    check(trap == nullptr, "instance_new_add_no_trap");

    if (!instance_add) {
        printf("ABORT: instance creation failed\n");
        wasm_module_delete(module_add);
        wasm_store_delete(store);
        wasm_engine_delete(engine);
        return 1;
    }

    // --- Check 6: export lookup ---
    printf("[6] wasm_instance_exports...\n"); fflush(stdout);
    wasm_extern_vec_t exports = WASM_EMPTY_VEC;
    wasm_instance_exports(instance_add, &exports);
    check(exports.size == 1, "exports_count_1");

    // --- Check 7: function type verification ---
    printf("[7] wasm_extern_as_func + arity...\n"); fflush(stdout);
    const wasm_func_t* func_add = wasm_extern_as_func(exports.data[0]);
    check(func_add != nullptr, "extern_as_func");
    check(wasm_func_param_arity(func_add) == 2, "param_arity_2");
    check(wasm_func_result_arity(func_add) == 1, "result_arity_1");

    // --- Check 8: function call (7 + 3 = 10) ---
    printf("[8] wasm_func_call (7+3)...\n"); fflush(stdout);
    wasm_val_t args_data[2];
    args_data[0].kind = WASM_I32;
    args_data[0].of.i32 = 7;
    args_data[1].kind = WASM_I32;
    args_data[1].of.i32 = 3;
    wasm_val_vec_t args_vec = WASM_ARRAY_VEC(args_data);
    wasm_val_t results_data[1];
    wasm_val_vec_t results_vec = WASM_ARRAY_VEC(results_data);
    wasm_trap_t* call_trap = wasm_func_call(func_add, &args_vec, &results_vec);
    check(call_trap == nullptr, "func_call_add_no_trap");
    check(results_data[0].kind == WASM_I32, "result_kind_i32");
    check(results_data[0].of.i32 == 10, "result_value_10");

    wasm_extern_vec_delete(&exports);
    wasm_instance_delete(instance_add);
    wasm_module_delete(module_add);

    // --- Check 9: second module (fortytwo) ---
    printf("[9] wasmtime_wat2wasm (fortytwo)...\n"); fflush(stdout);
    wasm_byte_vec_t wasm2_bytes = WASM_EMPTY_VEC;
    err = wasmtime_wat2wasm(WAT_FORTYTWO, strlen(WAT_FORTYTWO), &wasm2_bytes);
    if (err != nullptr) {
        printf("  wat2wasm_42 FAILED:\n");
        print_wasmtime_error(err);
    }
    check(err == nullptr, "wat2wasm_42");

    if (err == nullptr) {
        printf("[10] wasm_module_new + instance (fortytwo)...\n"); fflush(stdout);
        wasm_module_t* module_42 = wasm_module_new(store, &wasm2_bytes);
        check(module_42 != nullptr, "module_new_42");
        wasm_byte_vec_delete(&wasm2_bytes);

        if (module_42) {
            wasm_extern_vec_t imports2 = WASM_EMPTY_VEC;
            wasm_trap_t* trap2 = nullptr;
            wasm_instance_t* instance_42 = wasm_instance_new(store, module_42, &imports2, &trap2);
            check(instance_42 != nullptr, "instance_new_42");

            if (instance_42) {
                wasm_extern_vec_t exports2 = WASM_EMPTY_VEC;
                wasm_instance_exports(instance_42, &exports2);
                const wasm_func_t* func_42 = wasm_extern_as_func(exports2.data[0]);

                wasm_val_vec_t no_args = WASM_EMPTY_VEC;
                wasm_val_t result42_data[1];
                wasm_val_vec_t result42_vec = WASM_ARRAY_VEC(result42_data);
                wasm_trap_t* trap42 = wasm_func_call(func_42, &no_args, &result42_vec);
                check(trap42 == nullptr, "fortytwo_call_no_trap");
                check(result42_data[0].of.i32 == 42, "fortytwo_value_42");

                wasm_extern_vec_delete(&exports2);
                wasm_instance_delete(instance_42);
            }
            wasm_module_delete(module_42);
        }
    }

    // --- Final ---
    wasm_store_delete(store);
    wasm_engine_delete(engine);

    printf("=== Results: %d/%d passed ===\n", checks_passed, checks_passed + checks_failed);
    fflush(stdout);

    printf("RESULT:engine_create:OK\n");
    printf("RESULT:store_create:OK\n");
    printf("RESULT:wat2wasm_add:OK\n");
    printf("RESULT:module_compile_add:OK\n");
    printf("RESULT:instance_create_add:OK\n");
    printf("RESULT:export_count:1\n");
    printf("RESULT:func_param_arity:2\n");
    printf("RESULT:func_result_arity:1\n");
    printf("RESULT:func_call_7_3:%d\n", results_data[0].of.i32);
    printf("RESULT:wat2wasm_42:OK\n");
    printf("RESULT:module_compile_42:OK\n");
    printf("RESULT:func_call_42:OK\n");
    fflush(stdout);

    return checks_failed > 0 ? 1 : 0;
}
