#include "VulkanEngineApp.hpp"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <stdexcept>

void VulkanEngineApp::mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    auto* instancePtr = static_cast<VulkanEngineApp*>(glfwGetWindowUserPointer(window));
    if (!instancePtr || instancePtr->isPaused) return;
    float xPosF = static_cast<float>(xpos);
    float yPosF = static_cast<float>(ypos);

    if (instancePtr->player.camera.firstMouse) {
        instancePtr->player.camera.lastX = xPosF;
        instancePtr->player.camera.lastY = yPosF;
        instancePtr->player.camera.firstMouse = false;
    }

    float xoffset = xPosF - instancePtr->player.camera.lastX;
    float yoffset = instancePtr->player.camera.lastY - yPosF;

    instancePtr->player.camera.lastX = xPosF;
    instancePtr->player.camera.lastY = yPosF;

    instancePtr->player.camera.process_mouse_movement(xoffset, yoffset);
}

void VulkanEngineApp::mouse_button_callback(GLFWwindow* window, int button, int action, int mods) {
    auto* instancePtr = static_cast<VulkanEngineApp*>(glfwGetWindowUserPointer(window));
    if (!instancePtr || action != GLFW_PRESS) return;

    if (instancePtr->isPaused) {
        if (button != GLFW_MOUSE_BUTTON_LEFT && button != GLFW_MOUSE_BUTTON_RIGHT) return;
        double mouseX = 0.0, mouseY = 0.0;
        int width = 1, height = 1;
        glfwGetCursorPos(window, &mouseX, &mouseY);
        glfwGetWindowSize(window, &width, &height);
        const float u = static_cast<float>(mouseX / (std::max)(width, 1));
        const float v = 1.0f - static_cast<float>(mouseY / (std::max)(height, 1));
        const bool insideX = u >= 0.34f && u <= 0.66f;

        if (!instancePtr->showGraphicsMenu) {
            if (button != GLFW_MOUSE_BUTTON_LEFT) return;
            if (insideX && v >= 0.515f && v <= 0.605f) {
                instancePtr->isPaused = false;
                glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
                instancePtr->player.camera.firstMouse = true;
            } else if (insideX && v >= 0.395f && v <= 0.485f) {
                instancePtr->showGraphicsMenu = true;
            } else if (insideX && v >= 0.275f && v <= 0.365f) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }
        } else {
            if (button == GLFW_MOUSE_BUTTON_LEFT && insideX && v >= 0.530f && v <= 0.620f) {
                instancePtr->cinematicEffects = !instancePtr->cinematicEffects;
            } else if (button == GLFW_MOUSE_BUTTON_LEFT && insideX && v >= 0.430f && v <= 0.520f) {
                instancePtr->depthOfFieldEnabled = !instancePtr->depthOfFieldEnabled;
            } else if (insideX && v >= 0.330f && v <= 0.420f) {
                const int direction = button == GLFW_MOUSE_BUTTON_RIGHT || u < 0.5f ? -1 : 1;
                instancePtr->world.cycle_chunk_budget(direction);
            } else if (button == GLFW_MOUSE_BUTTON_LEFT && insideX && v >= 0.230f && v <= 0.320f) {
                instancePtr->showGraphicsMenu = false;
            }
        }
        return;
    }

    instancePtr->player.trigger_swing();

    RaycastResult res = instancePtr->player.perform_raycast(instancePtr->world);
    if (res.hit) {
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            // A.2: registry-driven id (see SoundEngine) — dynamic blocks get
            // their own break sound path instead of collapsing to Air.
            instancePtr->soundEngine.play_break_sound_for_block(
                instancePtr->world.get_block_at(res.hitBlockPos));
            instancePtr->world.set_block_at(res.hitBlockPos, kRuntimeAirId);
            instancePtr->world.force_fluid_tick();
        } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
            instancePtr->soundEngine.play_place_sound();
            instancePtr->world.set_block_at(res.placeBlockPos, runtime_id(instancePtr->player.selectedBlock));
            instancePtr->world.force_fluid_tick();
        }
    }
}

void VulkanEngineApp::framebuffer_size_callback(GLFWwindow* window, int width, int height) {
    auto* instancePtr = static_cast<VulkanEngineApp*>(glfwGetWindowUserPointer(window));
    if (!instancePtr || width <= 0 || height <= 0) return;
    instancePtr->framebufferResized = true;
}


void VulkanEngineApp::init_window() {
    if (!glfwInit()) {
        throw std::runtime_error("Failed to initialize GLFW");
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    window = glfwCreateWindow(windowExtent.width, windowExtent.height, "VulkanCraft - Minecraft VulkanEngineApp C++", nullptr, nullptr);
    if (!window) {

        throw std::runtime_error("Failed to create GLFW Window");
    }

    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetWindowUserPointer(window, this);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetMouseButtonCallback(window, mouse_button_callback);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwGetWindowPos(window, &windowedX, &windowedY);
    glfwGetWindowSize(window, &windowedWidth, &windowedHeight);
}


void VulkanEngineApp::toggle_fullscreen() {
    if (!fullscreen) {
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedWidth, &windowedHeight);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
        if (!monitor || !mode) return;
        fullscreen = true;
        glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        fullscreen = false;
        glfwSetWindowMonitor(window, nullptr, windowedX, windowedY,
                             (std::max)(windowedWidth, 640), (std::max)(windowedHeight, 360),
                             GLFW_DONT_CARE);
    }
    framebufferResized = true;
    player.camera.firstMouse = true;
}


