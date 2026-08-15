#pragma once

#include <glm/glm.hpp>

struct Frustum {
    glm::vec4 planes[6];

    void update(const glm::mat4& mvp) {
        // Left
        planes[0] = glm::vec4(mvp[0][3] + mvp[0][0], mvp[1][3] + mvp[1][0], mvp[2][3] + mvp[2][0], mvp[3][3] + mvp[3][0]);
        // Right
        planes[1] = glm::vec4(mvp[0][3] - mvp[0][0], mvp[1][3] - mvp[1][0], mvp[2][3] - mvp[2][0], mvp[3][3] - mvp[3][0]);
        // Bottom
        planes[2] = glm::vec4(mvp[0][3] + mvp[0][1], mvp[1][3] + mvp[1][1], mvp[2][3] + mvp[2][1], mvp[3][3] + mvp[3][1]);
        // Top
        planes[3] = glm::vec4(mvp[0][3] - mvp[0][1], mvp[1][3] - mvp[1][1], mvp[2][3] - mvp[2][1], mvp[3][3] - mvp[3][1]);
        // Near
        planes[4] = glm::vec4(mvp[0][2], mvp[1][2], mvp[2][2], mvp[3][2]);
        // Far
        planes[5] = glm::vec4(mvp[0][3] - mvp[0][2], mvp[1][3] - mvp[1][2], mvp[2][3] - mvp[2][2], mvp[3][3] - mvp[3][2]);

        for (int i = 0; i < 6; i++) {
            float length = glm::length(glm::vec3(planes[i]));
            if (length > 0.0f) planes[i] /= length;
        }
    }

    bool is_box_visible(const glm::vec3& minBound, const glm::vec3& maxBound) const {
        for (int i = 0; i < 6; i++) {
            glm::vec3 p = minBound;
            if (planes[i].x >= 0) p.x = maxBound.x;
            if (planes[i].y >= 0) p.y = maxBound.y;
            if (planes[i].z >= 0) p.z = maxBound.z;

            if (glm::dot(glm::vec3(planes[i]), p) + planes[i].w < 0.0f) {
                return false;
            }
        }
        return true;
    }
};
