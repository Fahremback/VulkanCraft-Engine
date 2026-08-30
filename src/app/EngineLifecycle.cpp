#include "VulkanEngineApp.hpp"

#include <GLFW/glfw3.h>
#include <cstdint>
#include <cmath>
#include <format>
#include <string>
#include <unordered_set>
#include <vector>

void VulkanEngineApp::run() {
    lastFrameTime = static_cast<float>(glfwGetTime());
    double statsStart = glfwGetTime();
    int statsFrames = 0;

    while (!glfwWindowShouldClose(window)) {
        float currentFrameTime = static_cast<float>(glfwGetTime());
        deltaTime = currentFrameTime - lastFrameTime;
        lastFrameTime = currentFrameTime;

        bool escDown = (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS);
        if (escDown && !escWasPressed) {
            if (isPaused && showGraphicsMenu) {
                showGraphicsMenu = false;
            } else {
                isPaused = !isPaused;
            }
            if (isPaused) {
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            } else {
                showGraphicsMenu = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                player.camera.firstMouse = true;
            }
        }
        escWasPressed = escDown;

        bool f5Down = glfwGetKey(window, GLFW_KEY_F5) == GLFW_PRESS;
        if (f5Down && !f5WasPressed && !isPaused) thirdPerson = !thirdPerson;
        f5WasPressed = f5Down;

        glfwPollEvents();

        // [L1 diag] heartbeat: prova que o loop itera e mede onde trava.
        {
            static std::uint64_t hbFrame = 0;
            static double hbLast = 0.0;
            ++hbFrame;
            const double hbNow = glfwGetTime();
            if (hbFrame <= 3 && hbLast == 0.0) {
                std::cout << "[HB] first-frame reached before draw, delta=" << deltaTime << '\n' << std::flush;
                hbLast = hbNow;
            } else if (hbNow - hbLast >= 5.0) {
                std::cout << "[HB] frame=" << hbFrame
                          << " delta=" << deltaTime
                          << " isPaused=" << (isPaused ? "1" : "0")
                          << " worldChunks=" << world.chunks.size()
                          << " runtimeTick=" << runtimeTick
                          << " blockEnts=" << world.block_entities().size()
                          << '\n' << std::flush;
                hbLast = hbNow;
            }
        }

        // Processamento de Interação do Jogador: Destruição e Colocação de Blocos com Física
        static bool leftWasPressed = false;
        static bool rightWasPressed = false;
        const bool leftDown = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS);
        const bool rightDown = (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);

        if (!isPaused) {
            if (leftDown && !leftWasPressed) {
                player.isSwinging = true;
                player.swingProgress = 0.0f;
                RaycastResult hit = player.perform_raycast(world, 7.0f);
                if (hit.hit) {
                    // A.2: registry-driven id — as_builtin_block mapped dynamic
                    // blocks to Air, so JSON-defined blocks were unbreakable
                    // through this gate. Air/Bedrock are builtin ids.
                    const RuntimeBlockId hitId =
                        world.get_block_at(hit.hitBlockPos);
                    const RuntimeBlockId airId = kRuntimeAirId;
                    const RuntimeBlockId bedrockId =
                        static_cast<RuntimeBlockId>(BlockType::Bedrock);
                    if (hitId != airId && hitId != bedrockId) {
                        // 1. Quebra de Bloco Nível Voxel World — via transação
                        // atômica (AGENTE 2 — gameplay showcase): valida e
                        // aplica all-or-nothing pelo caminho único de mutação
                        // (dirty do mesh, persistência, relight).
                        std::string txError;
                        auto tx = world.begin_transaction();
                        tx->remove_block(hit.hitBlockPos);
                        if (!tx->commit(txError)) {
                            std::cout << "[VulkanEngineApp] break refused (atomic "
                                         "transaction): "
                                      << txError << '\n';
                        } else {
                            // Journal the committed edit so the showcase save/
                            // load round trip persists the altered block.
                            const glm::ivec3 jPos(
                                static_cast<int>(std::floor(hit.hitBlockPos.x)),
                                static_cast<int>(std::floor(hit.hitBlockPos.y)),
                                static_cast<int>(std::floor(hit.hitBlockPos.z)));
                            showcaseBlockJournal.push_back(
                                { jPos, static_cast<uint32_t>(airId) });
                            ++showcaseJournalCommits;
                            // The committed edit changes surface heights on
                            // this column — drop the stale cached value.
                            invalidateShowcaseSurfaces();
                        
                        // 2. Timber V2: Derrubada de Árvores em Cadeia se for Madeira
                        // — a real chain felling: breaking a log flood-fills
                        // through 6-connected wood blocks (bounded at 64 so a
                        // giant log pile cannot stall the frame) and removes
                        // them all in ONE atomic transaction, journaling each
                        // edit for the showcase save/load round trip.
                        const RuntimeBlockId woodId =
                            static_cast<RuntimeBlockId>(BlockType::Wood);
                        const RuntimeBlockId birchId =
                            static_cast<RuntimeBlockId>(BlockType::WoodBirch);
                        const RuntimeBlockId spruceId =
                            static_cast<RuntimeBlockId>(BlockType::WoodSpruce);
                        const auto isWood = [&](RuntimeBlockId id) {
                            return id == woodId || id == birchId || id == spruceId;
                        };
                        if (isWood(hitId)) {
                            std::vector<glm::ivec3> queue = { jPos };
                            std::vector<glm::ivec3> fell;
                            std::unordered_set<std::int64_t> seen;
                            const auto cellKey = [](const glm::ivec3& p) {
                                return (static_cast<std::int64_t>(p.x) << 42) ^
                                       (static_cast<std::int64_t>(p.y) << 21) ^
                                       static_cast<std::int64_t>(p.z);
                            };
                            seen.insert(cellKey(jPos));
                            static const glm::ivec3 kDir[6] = {
                                { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
                                { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
                            while (!queue.empty() && fell.size() < 64) {
                                const glm::ivec3 cur = queue.back();
                                queue.pop_back();
                                for (const glm::ivec3& d : kDir) {
                                    const glm::ivec3 n = cur + d;
                                    const std::int64_t key = cellKey(n);
                                    if (seen.count(key)) continue;
                                    seen.insert(key);
                                    if (!isWood(world.get_block_at(n))) continue;
                                    fell.push_back(n);
                                    queue.push_back(n);
                                }
                            }
                            if (!fell.empty()) {
                                std::string txError2;
                                auto tx2 = world.begin_transaction();
                                for (const glm::ivec3& n : fell) {
                                    tx2->remove_block(n);
                                }
                                if (tx2->commit(txError2)) {
                                    // Journal ONLY after a successful atomic
                                    // commit — a rolled-back transaction must
                                    // not leave ghost edits that a later save/
                                    // load would re-apply as Air in the world.
                                    ++showcaseJournalCommits;
                                    for (const glm::ivec3& n : fell) {
                                        showcaseBlockJournal.push_back(
                                            { n, static_cast<uint32_t>(airId) });
                                    }
                                    // Chain felling changed surface heights —
                                    // drop stale cached columns.
                                    invalidateShowcaseSurfaces();
                                } else {
                                    std::cout << "[VulkanEngineApp] chain fell refused "
                                                 "(atomic transaction): "
                                              << txError2 << '\n';
                                }
                            }
                        }

                        // 3. Dropz: AGENTE 2 block G — breaking a block adds
                        // its item stack to the player's real Inventory
                        // (data-driven ItemRegistry; the legacy path left this
                        // empty). The registry may not know the builtin block
                        // id — when it does, the item is granted; when it
                        // does not, the drop is skipped (no guessed item).
                        if (playerItems && playerInventory) {
                            const char* itemName = block_type_item_name(hitId);
                            if (itemName != nullptr && itemName[0] != '\0') {
                                const auto* itemDef = playerItems->find_by_name(
                                    std::string("vulkancraft:") + itemName);
                                if (itemDef != nullptr) {
                                    std::string invError;
                                    const auto remainder =
                                        playerInventory->add(
                                            engine::registry::ItemStack{
                                                itemDef->namespaced(), 1 },
                                            *playerItems, invError);
                                    (void)remainder;  // inventory full: drop lost
                                    playerInventorySummary =
                                        playerInventory->serialize_json();
                                }
                            }
                        }

                        // AGENTE 2 block G.91: recompute the craftable recipe
                        // count from the LIVE hotbar (recipes_for over the
                        // Inventory after the drop) — the crafting table
                        // answers with real satisfiability, observable in the
                        // title as `craft N/M`.
                        if (playerRecipes && playerInventory && playerItems) {
                            craftableRecipeCount =
                                playerRecipes
                                    ->recipes_for(*playerInventory,
                                                  "vulkancraft:crafting",
                                                  *playerItems)
                                    .size();
                        }

                        // 4. Som Específico por Bloco
                        soundEngine.play_break_sound_for_block(hitId);

                        // 5. Barramento canônico (AGENTE 2 G.97): publica um
                        // evento REAL de gameplay no IGameplayEvents da
                        // composição — o IGameplayEventRouter (drenado no
                        // fixed tick do runtime) o traduz em trigger de
                        // áudio + métrica, sem callbacks paralelos.
                        if (runtimeEvents) {
                            runtimeEvents->publish(1, runtimeTick,
                                                   { static_cast<std::uint8_t>(hitId & 0xFF) });
                        }

                        // AGENTE 2 G.92 (ability effects): each real block
                        // break also emits the configured ability effect into
                        // the SAME canonical bus — the ability table is a
                        // real loop consumer of the running game.
                        if (abilityEffects && runtimeEvents) {
                            std::string xError;
                            // L92 cooldown: the kick ability is gated by a
                            // fixed-tick cooldown so rapid breaks emit at most
                            // once per window (real cooldown semantics).
                            if (runtimeTick - abilityLastKickTick <
                                kAbilityKickCooldownTicks) {
                                ++abilityCooldownBlocks;
                            } else {
                                if (abilityEffects->emit(*runtimeEvents, "kick",
                                                         runtimeTick, xError)) {
                                    ++abilityEffectCount;
                                    abilityLastKickTick = runtimeTick;
                                }
                            }
                        }
                        }  // end atomic-transaction success branch
                    }
                }
            }

            if (rightDown && !rightWasPressed) {
                player.isSwinging = true;
                player.swingProgress = 0.0f;
                RaycastResult hit = player.perform_raycast(world, 7.0f);
                if (hit.hit && player.selectedBlock != BlockType::Air) {
                    // A.2: registry-driven — placement allowed into air or
                    // fluid (water id is builtin Water). as_builtin_block
                    // collapsed dynamic blocks to Air here (false allows).
                    const RuntimeBlockId targetId =
                        world.get_block_at(hit.placeBlockPos);
                    const RuntimeBlockId airId = kRuntimeAirId;
                    const RuntimeBlockId waterId =
                        static_cast<RuntimeBlockId>(BlockType::Water);
                    if (targetId == airId || targetId == waterId ||
                        world.is_fluid_runtime_id(targetId)) {
                        // Colocação via transação atômica (AGENTE 2 — gameplay
                        // showcase): all-or-nothing pelo caminho único.
                        std::string txError;
                        auto tx = world.begin_transaction();
                        tx->set_block(hit.placeBlockPos,
                                      runtime_id(player.selectedBlock));
                        if (tx->commit(txError)) {
                            soundEngine.play_place_sound();

                            // Journal the committed edit (save/load round trip).
                            const glm::ivec3 jPos(
                                static_cast<int>(std::floor(hit.placeBlockPos.x)),
                                static_cast<int>(std::floor(hit.placeBlockPos.y)),
                                static_cast<int>(std::floor(hit.placeBlockPos.z)));
                            showcaseBlockJournal.push_back(
                                { jPos, runtime_id(player.selectedBlock) });
                            ++showcaseJournalCommits;
                            // The placed block changes surface heights on this
                            // column — drop the stale cached value.
                            invalidateShowcaseSurfaces();

                            // Barramento canônico (AGENTE 2 G.97): evento de
                            // colocação no mesmo bus que o break.
                            if (runtimeEvents) {
                                runtimeEvents->publish(2, runtimeTick, {});
                            }
                        } else {
                            std::cout << "[VulkanEngineApp] place refused "
                                         "(atomic transaction): "
                                      << txError << '\n';
                        }
                    }
                }
            }
        }
        leftWasPressed = leftDown;
        rightWasPressed = rightDown;

        const bool fullscreenDown = glfwGetKey(window, GLFW_KEY_F11) == GLFW_PRESS ||
            (glfwGetKey(window, GLFW_KEY_ENTER) == GLFW_PRESS &&
             (glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS ||
              glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS));
        if (fullscreenDown && !fullscreenWasPressed) {
            toggle_fullscreen();
        }
        fullscreenWasPressed = fullscreenDown;
        draw();
        ++statsFrames;
        const double statsNow = glfwGetTime();
        if (statsNow - statsStart >= 1.0) {
            const double fps = static_cast<double>(statsFrames) / (statsNow - statsStart);
            const int representedReach = worldRenderer.represented_reach_chunks();
            const float lodTarget = world.far_lod_endpoint_percent();
            const float lodApplied = worldRenderer.applied_endpoint_percent();
            const bool lodPending = worldRenderer.is_building() ||
                std::abs(lodTarget - lodApplied) > 0.00001f;
            const std::string lodState = lodPending
                ? std::format("LOD alvo {:.6g}% (aplicado {:.6g}%, PROCESSANDO)",
                              lodTarget, lodApplied)
                : std::format("LOD {:.6g}% APLICADO", lodApplied);
            const std::string invSummary = playerInventorySummary.empty()
                ? std::string("inv empty")
                : playerInventorySummary;
            // AGENTE 2 block G.91: the crafting table's live answer — how many
            // registered recipes are satisfiable from the real hotbar.
            const std::string craftSummary = playerRecipes
                ? std::format("craft {}/{}", craftableRecipeCount, recipeCount)
                : std::string("craft n/a");
            // AGENTE 2 block G.97 (observable metrics): the canonical bus
            // records a per-kind counter (events.block.break / events.block
            // .place) on every routed gameplay event; the game reads the real
            // snapshot and reports the aggregate — proof the router->metrics
            // path is alive and countable in the running game.
            std::size_t routedEventCount = 0;
            if (runtimeMetrics) {
                const auto metrics = runtimeMetrics->snapshot();
                for (const auto& m : metrics) {
                    if (m.name.rfind("events.", 0) == 0) {
                        routedEventCount += static_cast<std::size_t>(m.value);
                    }
                }
            }
            // AGENTE 2 block G (day/night): the canonical clock state is
            // observable in the window title — the same time_of_day the
            // runtime advances every frame drives the sun/lighting.
            const std::string clockSummary = dayNightCycle
                ? std::format("day {:04.1f}% ({:.2f})",
                              dayNightCycle->daylight_factor() * 100.0f,
                              dayNightCycle->time_of_day())
                : std::string("day n/a");
            // AGENTE 2 block J: the spatializer + adaptive-music cores report
            // their real per-frame state (sources from the live mob ECS,
            // music state from the day/night clock) — observable in the title.
            const std::string audioSummary =
                (spatialAudio || adaptiveMusic)
                    ? std::format("spatial {} ({} virt) | music {}",
                                  spatialActiveSources,
                                  spatialVirtualizedSources,
                                  adaptiveMusicState.empty()
                                      ? std::string("n/a")
                                      : adaptiveMusicState)
                    : std::string("audio n/a");
            const std::string steeringSummary =
                std::format("steer {:.1f} ({})", mobSteeringForce,
                            steeringMobCount);
            // AGENTE 2 block I.112: the player's sensor suite reports real
            // detections/memory of the hostility field — observable in title.
            const std::string perceptionSummary =
                (playerPerception)
                    ? std::format("percept {} det ({} mem, threat {:.0f})",
                                  perceptionDetections, perceptionMemory,
                                  nearestThreatDistance)
                    : std::string("percept n/a");
            const std::string fsmSummary =
                (mobFsm)
                    ? std::format("fsm {}<{}-{}", fsmState, fsmLastAction,
                                   fsmTicks)
                    : std::string("fsm n/a");
            const std::string treeSummary =
                (mobTree)
                    ? std::format("tree {}@{}", treeStatus, treeTrace)
                    : std::string("tree n/a");
            const std::string utilitySummary =
                (utilityAi)
                    ? std::format("util {} ({})", utilityAction, utilityChoice)
                    : std::string("util n/a");
            const std::string plannerSummary =
                (mobPlanner)
                    ? std::format("plan {}→{} ({})", plannerStep, plannerLength,
                                  plannerGoal)
                    : std::string("plan n/a");
            const std::string lodSummary =
                (aiLod)
                    ? std::format("lod {}+{}-{}", lodFull, lodReduced, lodDormant)
                    : std::string("lod n/a");
            const std::string aiBusSummary =
                (aiEventBus)
                    ? std::format("aibus {}", aiEventCount)
                    : std::string("aibus n/a");
            const std::string crowdSummary =
                (crowd)
                    ? std::format("crowd {}+{}-{}", crowdActive, crowdDormant,
                                  crowdWoken)
                    : std::string("crowd n/a");
            // AGENTE 1 block B.4: the public scene-culling core drove the real
            // chunk culling this frame — visible vs SDK-culled chunks, detail
            // chunks skipped by conservative occlusion, the LOD tier split and
            // the mob instance groups (merged AABB) culled before the draw.
            const std::string cullSummary =
                (sceneCulling)
                    ? std::format("cull {}-{} (occl {}, lod {}, inst {}/{}x{})",
                                  worldRenderer.last_visible_chunks(),
                                  worldRenderer.last_culled_chunks(),
                                  worldRenderer.last_occluded_detail_chunks(),
                                  worldRenderer.last_lod_split(),
                                  mobInstanceGroups, mobVisibleGroups,
                                  mobInstanceCount)
                    : std::string("cull n/a");
            const std::string fxSummary =
                (abilityEffects)
                    ? std::format("fx {}", abilityEffectCount)
                    : std::string("fx n/a");
            // AGENTE 2 block B.2/B.5 (spatial partition + sleeping by
            // relevance): the ISpatialIndex reports the near-candidate count
            // at the player's cell; the AI-LOD sleep authority reports how
            // many mobs are frozen (physics mirror paused, ECS state intact).
            const std::string spatialSummary = mobSpatial
                ? std::format("spatial {} ({})", spatialNearCount,
                              sleepingMobCount)
                : std::string("spatial n/a");
            // AGENTE 2 block H.107 (animation LOD): how many mobs the
            // IAnimationLod core asked to re-sample vs hold (frozen) this
            // frame — the per-entity animation budget in the running game.
            const std::string animLodSummary = animLod
                ? std::format("animlod {}/{}", animLodSampled, animLodFrozen)
                : std::string("animlod n/a");
            // AGENTE 2 block G (world director + weather): the director's
            // selected world event (storm / raid / festival) with its utility,
            // and the deterministic weather state derived from the SAME
            // day/night clock — both observable in the running game.
            const std::string directorSummary = worldDirector
                ? std::format("{} {} ({})", weatherState, directorEvent,
                              directorUtility)
                : std::string("director n/a");
            // AGENT-1 I.1 (render providers): the seven public rendering
            // factories wired into the executable report their real state —
            // public frame graph (passes/barriers), swapchain mirror (frame /
            // recreate count), quality preset, sparse volume bricks, ray-bake
            // AO mean and the camera geodetic + rebase count (B.7).
            const std::string renderProvidersSummary = std::format(
                "rgraph {}/{} | sw {} (r{}) | {} | sparse {} | bake {:.2f} | "
                "{:.1f}N {:.1f}E | rebase {}",
                renderGraphPassCount, renderGraphBarrierCount,
                swapchainManager ? swapchainManager->getInfo().frameIndex : 0,
                swapchainManager ? swapchainManager->getInfo().recreateCount : 0,
                presetName, sparseActiveBricks, rayBakeOpenMean, ellipsoidLat,
                ellipsoidLon, rebaseFiredCount);
            // AGENT-1 B.1/B.2 (render snapshot): the presentation layer reads
            // ONLY the immutable per-frame snapshot — camera local offset from
            // the rebase origin (small = no jitter), tick, entities, mobs and
            // visible chunks are the values captured once per frame in draw().
            const glm::dvec3 localCam =
                glm::dvec3(frameSnapshot.cameraPosition) - frameSnapshot.origin;
            const std::string snapshotSummary = std::format(
                "snap local {:.1f} | t{} e{} m{} v{}",
                std::sqrt(localCam.x * localCam.x + localCam.y * localCam.y +
                          localCam.z * localCam.z),
                frameSnapshot.tick, frameSnapshot.entities,
                frameSnapshot.mobs, frameSnapshot.visibleChunks);
            const std::string title = representedReach > 0
                ? std::format("VulkanCraft | {:.0f} FPS | {} chunk reach ({} LODs, FAR {:.1f} ms) | {} | {} detail | jobs {} | fluid queue {} | sim tick {} ({} ent, {} mobs, {} worlds) | {} events | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {}",
                              fps, representedReach, worldRenderer.clipmap_level_count(),
                              worldRenderer.last_build_milliseconds(), lodState, world.chunks.size(),
                              world.pendingTasks.load(), world.activeFluidCells.size(),
                              runtimeTick, runtimeEntities, mobPhysicsBodyCount,
                              runtimeWorldCount, routedEventCount, invSummary,
                              clockSummary, audioSummary, craftSummary, steeringSummary,
                              perceptionSummary, fsmSummary, treeSummary, utilitySummary,
                              plannerSummary, lodSummary, aiBusSummary, crowdSummary,
                              fxSummary, cullSummary, spatialSummary, animLodSummary,
                              directorSummary, renderProvidersSummary, snapshotSummary)
                : std::format("VulkanCraft | {:.0f} FPS | {} | {} detail | jobs {} | fluid queue {} | sim tick {} ({} ent, {} mobs, {} worlds) | {} events | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {}",
                              fps, lodState, world.chunks.size(), world.pendingTasks.load(),
                              world.activeFluidCells.size(), runtimeTick, runtimeEntities,
                              mobPhysicsBodyCount, runtimeWorldCount, routedEventCount,
                              invSummary, clockSummary, audioSummary, craftSummary,
                              steeringSummary, perceptionSummary, fsmSummary, treeSummary,
                              utilitySummary, plannerSummary, lodSummary, aiBusSummary,                              crowdSummary, fxSummary, cullSummary, spatialSummary, animLodSummary,
                              directorSummary, renderProvidersSummary, snapshotSummary);
            // AGENTE 2 (aceleracao — gameplay showcase): the canonical
            // character/animation/physics/simulation/gameplay factories report
            // their real per-fixed-tick state (controller flags, animation
            // stack, ragdoll/weapon, explosion falloff, AI, navigation,
            // simulation LOD, convex/multibody/shape physics).
            const std::string showcaseFinal =
                showcaseSummary.empty() ? std::string("showcase n/a")
                                        : showcaseSummary;
            glfwSetWindowTitle(window, (title + " | " + showcaseFinal).c_str());
            // [L1 diag] surface fps + core counters to stdout every second so
            // the loop's health is visible when run headless/terminal.
            std::cout << "[HB] fps=" << static_cast<int>(fps)
                      << " chunks=" << world.chunks.size()
                      << " tick=" << runtimeTick
                      << " blockEnts=" << world.block_entities().size()
                      << " l1=" << worldLote1GameShow
                      << "\n" << std::flush;
            statsStart = statsNow;
            statsFrames = 0;
        }
    }
}

void VulkanEngineApp::cleanup() {
    if (isInitialized) {
        vkDeviceWaitIdle(device);

        // AGENTE 2 block J.126 (voice lifecycle): release every spatial-audio
        // voice on shutdown — the deterministic spatializer's sources are
        // explicitly removed (the lifecycle the mixer would mirror on world
        // switch / time travel / pause). No voices leak into teardown.
        if (spatialAudio) {
            for (std::size_t i = 0; i < spatialActiveSources; ++i) {
                std::string voiceError;
                spatialAudio->remove_source("mob" + std::to_string(i),
                                            voiceError);
            }
            std::cout << "[VulkanEngineApp] spatial audio voices released ("
                      << spatialActiveSources << ")\n";
        }

        // AGENTE 2 block A: ordered teardown of the canonical composition
        // (reverse of bootstrap, flushes persistence) before render resources
        // are destroyed. Safe to call when the runtime never bound.
        if (worldRuntime) {
            worldRuntime->shutdown();
        }

        // AGENTE 2 (aceleracao — gameplay showcase): release the showcase
        // factory state (ragdoll/weapon references, fixed-tick accumulator)
        // before render resources are destroyed.
        showcase_gameplay_shutdown();

        textureManager.cleanup(device, allocator);

        if (armBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, armBuffer.buffer, armBuffer.allocation);
        }
        if (heldBlockBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, heldBlockBuffer.buffer, heldBlockBuffer.allocation);
        }
        if (characterBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, characterBuffer.buffer, characterBuffer.allocation);
        }
        if (showcasePoseBuffer.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, showcasePoseBuffer.buffer, showcasePoseBuffer.allocation);
        }

        worldRenderer.cleanup(true);
        destroy_gpu_feature_passes();
        destroy_mesh_shader_path();
        destroy_timestamp_queries();
        destroy_gpu_feature_binding();
        radianceCache.cleanup();

        vkDestroyPipeline(device, voxelPipeline, nullptr);
        vkDestroyPipeline(device, farSurfacePipeline, nullptr);
        vkDestroyPipeline(device, waterPipeline, nullptr);
        vkDestroyPipeline(device, grassPipeline, nullptr);
        vkDestroyPipeline(device, foliagePipeline, nullptr);
        vkDestroyPipeline(device, skyPipeline, nullptr);
        vkDestroyPipeline(device, postPipeline, nullptr);
        vkDestroyPipeline(device, shadowPipeline, nullptr);
        vkDestroyPipeline(device, shadowFarSurfacePipeline, nullptr);
        vkDestroyPipeline(device, shadowFoliagePipeline, nullptr);
        vkDestroyPipeline(device, shadowGrassPipeline, nullptr);
        vkDestroyPipelineLayout(device, voxelPipelineLayout, nullptr);
        vkDestroyPipelineLayout(device, postPipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, postDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, postDescriptorLayout, nullptr);
        vkDestroySampler(device, postSampler, nullptr);

        destroy_screen_targets();
        vkDestroySampler(device, shadowSampler, nullptr);
        vkDestroyImageView(device, shadowImageView, nullptr);
        vmaDestroyImage(allocator, shadowImage, shadowAllocation);
        vkDestroySampler(device, minimapSampler, nullptr);
        vkDestroyImageView(device, minimapImageView, nullptr);
        vmaDestroyImage(allocator, minimapImage, minimapAllocation);
        vkDestroyImageView(device, minimapDepthView, nullptr);
        vmaDestroyImage(allocator, minimapDepthImage, minimapDepthAllocation);
        vkDestroySampler(device, waterSceneSampler, nullptr);

        for (auto sem : renderSemaphores) {
            vkDestroySemaphore(device, sem, nullptr);
        }
        for (int i = 0; i < FRAME_OVERLAP; i++) {
            vkDestroyCommandPool(device, frames[i].commandPool, nullptr);
            vkDestroyFence(device, frames[i].renderFence, nullptr);
            vkDestroySemaphore(device, frames[i].swapchainSemaphore, nullptr);
        }

        for (auto view : swapchainImageViews) {
            vkDestroyImageView(device, view, nullptr);
        }
        vkDestroySwapchainKHR(device, swapchain, nullptr);

        mobRenderer.cleanup(device, allocator);
        vmaDestroyAllocator(allocator);

        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkb::destroy_debug_utils_messenger(instance, debugMessenger);
        vkDestroyInstance(instance, nullptr);

        glfwDestroyWindow(window);
        glfwTerminate();
    }
}
