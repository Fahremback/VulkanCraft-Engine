# Test labels for suite-based gating (Agent 6 — §1 build graph)
# Usage: include(cmake/TestLabels.cmake) after enable_testing() and all add_test()

# --- unit: pure logic, no engine state, no GPU ---
set(_unit_tests
    thread_pool_tests
    gltf_geometry_tests
    radiance_cache_math_tests
    engine_foundation_tests
    ability_system_tests
    simulation_lod_tests
    mission_asset_tests
    script_debugger_tests
    deformable_tests
    tetra_cooking_tests
    timeline_tests
    timeline_causal_tests
    macro_micro_reconciler_tests
    world_director_tests
    episode_compiler_tests
    simulation_farm_tests
    semantic_api_tests
    semantic_episode_farm_tests
    episode_compiler_zstd_blake3_tests
    semantic_episode_farm_zstd_blake3_tests
    registry_json_tests
    motion_database_tests
    gait_planner_tests
    procedural_legs_tests
    motion_matcher_tests
    hair_tests
    animation_lod_tests
    ai_graph_validation_tests
    foot_placement_tests
    pose_warp_tests
    offline_farm_tests
)
foreach(_t ${_unit_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "unit")
    endif()
endforeach()

# --- voxel: world state, streaming, meshing, save/load ---
set(_voxel_tests
    terrain_generator_tests
    voxel_box_merger_tests
    voxel_tools_tests
    voxel_streaming_tests
    voxel_scheduler_tests
    voxel_sdk_tests
    history_tests
    revoxelize_tests
    multiscale_streaming_tests
)
foreach(_t ${_voxel_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "voxel")
    endif()
endforeach()

# --- physics: collision, destruction, vehicles ---
set(_physics_tests
    physics_backend_tests
    physics_streaming_tests
    physics_ownership_tests
    explosion_tests
    debris_tests
    destruction_tests
    fracture_tests
)
foreach(_t ${_physics_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "physics")
    endif()
endforeach()

# --- vehicle: vehicle subsystem ---
set(_vehicle_tests
    vehicle_adapter_tests
    vehicle_asset_tests
    vehicle_kinds_tests
    beam_vehicle_tests
    vehicle_lifecycle_tests
    vehicle_power_tests
    vehicle_replication_tests
    vehicle_asset_gate_tests
    vehicle_provider_tests
)
foreach(_t ${_vehicle_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "vehicle")
    endif()
endforeach()

# --- skeleton: ragdoll, skeleton mapping ---
set(_skeleton_tests
    skeleton_mapper_tests
    active_ragdoll_tests
)
foreach(_t ${_skeleton_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "skeleton")
    endif()
endforeach()

# --- rendering: GPU-dependent ---
set(_rendering_tests
    rendering_tools_tests
    advanced_systems_tests
    visual_authoring_tests
)
foreach(_t ${_rendering_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "rendering")
    endif()
endforeach()

# --- integration: multi-system, connectivity, replication ---
set(_integration_tests
    connectivity_tests
    world_replication_tests
    world_ref_tests
    origin_rebase_tests
    local_space_tests
)
foreach(_t ${_integration_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "integration")
    endif()
endforeach()

# --- gameplay: gameplay framework, runtime, MCP ---
set(_gameplay_tests
    gameplay_framework_tests
    gameplay_runtime_tests
    mcp_registry_gate_tests
)
foreach(_t ${_gameplay_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "gameplay")
    endif()
endforeach()

# --- architecture: boundary, empty project ---
set(_arch_tests
    architecture_boundary_tests
    empty_project_tests
)
foreach(_t ${_arch_tests})
    if(TEST ${_t})
        set_tests_properties(${_t} PROPERTIES LABELS "architecture")
    endif()
endforeach()

# --- benchmark: performance measurement ---
if(TEST far_terrain_sampling_benchmark)
    set_tests_properties(far_terrain_sampling_benchmark PROPERTIES LABELS "benchmark;far-terrain")
endif()
