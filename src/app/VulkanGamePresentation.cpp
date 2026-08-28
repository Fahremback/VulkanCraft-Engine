#include "VulkanGame.hpp"
#include "VulkanGameSupport.hpp"

void VulkanGame::mainLoop(){
        auto lastTime = std::chrono::high_resolution_clock::now();
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            auto currentTime = std::chrono::high_resolution_clock::now();
            float deltaTime = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - lastTime).count();
            lastTime = currentTime;
            update(deltaTime);
            drawFrame();
            updateHud(deltaTime);
        }
        vkDeviceWaitIdle(device);
    }

void VulkanGame::drawFrame(){
        vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
        uint32_t imageIndex;
        VkResult result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            framebufferResized = false;
            return;
        }
        // The acquired image must be free before we submit new work on it: the
        // swapchain may still be presenting the previous frame that used it.
        if (imagesInFlight[imageIndex] != VK_NULL_HANDLE) {
            vkWaitForFences(device, 1, &imagesInFlight[imageIndex], VK_TRUE, UINT64_MAX);
        }
        imagesInFlight[imageIndex] = inFlightFences[currentFrame];

        vkResetFences(device, 1, &inFlightFences[currentFrame]);
        vkResetCommandBuffer(commandBuffers[currentFrame], 0);

        VkCommandBuffer cb = commandBuffers[currentFrame];
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(cb, &beginInfo);

        // The whole frame runs through the compiled render graph: the executor
        // begins each pass on its real framebuffer, inserts the compiled
        // barriers between passes, and invokes the per-pass draw callbacks.
        renderGraphExecutor.record(cb, imageIndex, swapChainExtent);

        vkEndCommandBuffer(cb);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        VkSemaphore waitSemaphores[] = { imageAvailableSemaphores[currentFrame] };
        VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cb;
        VkSemaphore signalSemaphores[] = { renderFinishedSemaphores[currentFrame] };
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]);

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;
        VkSwapchainKHR swapChains[] = { swapChain };
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;
        presentInfo.pImageIndices = &imageIndex;
        vkQueuePresentKHR(graphicsQueue, &presentInfo);
        currentFrame = (currentFrame + 1) % imageAvailableSemaphores.size();
    }

void VulkanGame::cleanup(){
        if (ragdollBuilt) ragdoll.destroy(physics);
        destroyMeshResource(cubeMesh);
        for (auto& [id, mesh] : meshes) {
            (void)id;
            destroyMeshResource(mesh);
        }
        meshes.clear();
        renderGraphExecutor.shutdown();
        vkDestroyPipeline(device, compositePipeline, nullptr);
        vkDestroyPipelineLayout(device, compositePipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, compositeDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, compositeSetLayout, nullptr);
        vkDestroySampler(device, compositeSampler, nullptr);
        for (auto framebuffer : compositeFramebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
        compositeFramebuffers.clear();
        vkDestroyFramebuffer(device, sceneFramebuffer, nullptr);
        vkDestroyImageView(device, hdrImageView, nullptr);
        vmaDestroyImage(allocator, hdrImage, hdrAllocation);
        vkDestroyPipeline(device, shadowPipeline, nullptr);
        vkDestroyPipelineLayout(device, shadowPipelineLayout, nullptr);
        vkDestroyFramebuffer(device, shadowFramebuffer, nullptr);
        vkDestroyRenderPass(device, shadowRenderPass, nullptr);
        vkDestroySampler(device, shadowMapSampler, nullptr);
        vkDestroyImageView(device, shadowMapView, nullptr);
        vmaDestroyImage(allocator, shadowMapImage, shadowMapAllocation);
        vkDestroyPipeline(device, volumPipeline, nullptr);
        vkDestroyPipelineLayout(device, volumPipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, volumDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, volumSetLayout, nullptr);
        vkDestroySampler(device, volumSampler, nullptr);
        vkDestroySampler(device, volumDepthSampler, nullptr);
        vkDestroyFramebuffer(device, volumFramebuffer, nullptr);
        vkDestroyRenderPass(device, volumRenderPass, nullptr);
        vmaDestroyBuffer(allocator, volumUboBuffer, volumUboAllocation);
        vkDestroyImageView(device, volumImageView, nullptr);
        vmaDestroyImage(allocator, volumImage, volumAllocation);
        vkDestroyPipeline(device, skinnedPipeline, nullptr);
        vkDestroyPipelineLayout(device, skinnedPipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, skinnedDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, skinnedSetLayout, nullptr);
        vmaDestroyBuffer(allocator, skinnedBoneBuffer, skinnedBoneAllocation);
        vmaDestroyBuffer(allocator, skinnedCameraBuffer, skinnedCameraAllocation);
        if (skinnedVb != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, skinnedVb, skinnedVbAllocation);
        if (skinnedIb != VK_NULL_HANDLE) vmaDestroyBuffer(allocator, skinnedIb, skinnedIbAllocation);
        vkDestroyRenderPass(device, compositeRenderPass, nullptr);
        vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vmaDestroyBuffer(allocator, uboBuffer, uboAllocation);
        vmaDestroyBuffer(allocator, lightBuffer, lightAllocation);
        vkDestroyImageView(device, depthImageView, nullptr);
        vmaDestroyImage(allocator, depthImage, depthAllocation);
        vkDestroyRenderPass(device, renderPass, nullptr);
        for (auto imageView : swapChainImageViews) vkDestroyImageView(device, imageView, nullptr);
        vkDestroySwapchainKHR(device, swapChain, nullptr);
        vmaDestroyAllocator(allocator);
        for (size_t i = 0; i < 2; i++) {
            vkDestroySemaphore(device, renderFinishedSemaphores[i], nullptr);
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }
        vkDestroyCommandPool(device, commandPool, nullptr);
        vkDestroyDevice(device, nullptr);
        vkDestroySurfaceKHR(instance, surface, nullptr);
        destroyDebugMessenger();
        vkDestroyInstance(instance, nullptr);
        glfwDestroyWindow(window);
        glfwTerminate();
    }

