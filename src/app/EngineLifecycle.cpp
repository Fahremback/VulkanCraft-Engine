#include "Engine.hpp"

#include <GLFW/glfw3.h>
#include <cmath>
#include <format>
#include <string>

void Engine::run() {
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
                    BlockType hitBlock = as_builtin_block(world.get_block_at(hit.hitBlockPos));
                    if (hitBlock != BlockType::Air && hitBlock != BlockType::Bedrock) {
                        // 1. Quebra de Bloco Nível Voxel World
                        world.set_block_at(hit.hitBlockPos, runtime_id(BlockType::Air));
                        
                        // 2. Timber V2: Derrubada de Árvores em Cadeia se for Madeira
                        if (hitBlock == BlockType::Wood || hitBlock == BlockType::WoodOak || hitBlock == BlockType::WoodBirch || hitBlock == BlockType::WoodSpruce) {
                        }

                        // 3. Dropz: Spawn de Item Entidade Física 3D Flutuante

                        // 4. Som Específico por Bloco
                        soundEngine.play_break_sound_for_block(hitBlock);
                    }
                }
            }

            if (rightDown && !rightWasPressed) {
                player.isSwinging = true;
                player.swingProgress = 0.0f;
                RaycastResult hit = player.perform_raycast(world, 7.0f);
                if (hit.hit && player.selectedBlock != BlockType::Air) {
                    BlockType airTarget = as_builtin_block(world.get_block_at(hit.placeBlockPos));
                    if (airTarget == BlockType::Air || airTarget == BlockType::Water) {
                        world.set_block_at(hit.placeBlockPos, runtime_id(player.selectedBlock));
                        soundEngine.play_place_sound();
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
            const std::string title = representedReach > 0
                ? std::format("VulkanCraft | {:.0f} FPS | {} chunk reach ({} LODs, FAR {:.1f} ms) | {} | {} detail | jobs {} | fluid queue {}",
                              fps, representedReach, worldRenderer.clipmap_level_count(),
                              worldRenderer.last_build_milliseconds(), lodState, world.chunks.size(),
                              world.pendingTasks.load(), world.activeFluidCells.size())
                : std::format("VulkanCraft | {:.0f} FPS | {} | {} detail | jobs {} | fluid queue {}",
                              fps, lodState, world.chunks.size(), world.pendingTasks.load(),
                              world.activeFluidCells.size());
            glfwSetWindowTitle(window, title.c_str());
            statsStart = statsNow;
            statsFrames = 0;
        }
    }
}

void Engine::cleanup() {
    if (isInitialized) {
        vkDeviceWaitIdle(device);

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

        worldRenderer.cleanup(true);

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
