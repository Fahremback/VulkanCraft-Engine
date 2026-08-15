#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <GLFW/glfw3.h>

class Camera {
public:
    glm::vec3 position{ 0.0f, 40.0f, 0.0f };
    glm::vec3 front{ 0.0f, 0.0f, -1.0f };
    glm::vec3 up{ 0.0f, 1.0f, 0.0f };
    glm::vec3 right{ 1.0f, 0.0f, 0.0f };

    float yaw{ -90.0f };
    float pitch{ 0.0f };
    float moveSpeed{ 60.0f };
    float mouseSensitivity{ 0.15f };
    bool firstMouse{ true };
    float lastX{ 640.0f };
    float lastY{ 360.0f };

    glm::mat4 get_view_matrix() const {
        return glm::lookAt(position, position + front, up);
    }

    glm::mat4 get_projection_matrix(float aspect, float farPlane = 3500.0f) const {
        glm::mat4 proj = glm::perspective(glm::radians(70.0f), aspect, 0.2f, farPlane);
        proj[1][1] *= -1.0f; // Inversão do eixo Y no Vulkan
        return proj;
    }

    void process_keyboard(GLFWwindow* window, float deltaTime) {
        float velocity = moveSpeed * deltaTime;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
            position += front * velocity;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
            position -= front * velocity;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
            position -= right * velocity;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
            position += right * velocity;
        if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
            position += up * velocity;
        if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
            position -= up * velocity;
    }

    void process_mouse_movement(float xoffset, float yoffset) {
        xoffset *= mouseSensitivity;
        yoffset *= mouseSensitivity;

        yaw += xoffset;
        pitch += yoffset;

        if (pitch > 89.0f) pitch = 89.0f;
        if (pitch < -89.0f) pitch = -89.0f;

        update_camera_vectors();
    }

private:
    void update_camera_vectors() {
        glm::vec3 f;
        f.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        f.y = sin(glm::radians(pitch));
        f.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        front = glm::normalize(f);
        right = glm::normalize(glm::cross(front, glm::vec3(0.0f, 1.0f, 0.0f)));
        up = glm::normalize(glm::cross(right, front));
    }
};
