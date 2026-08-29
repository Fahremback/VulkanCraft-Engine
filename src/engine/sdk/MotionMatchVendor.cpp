// MotionMatchVendor.cpp — adapter do core de motion matching do clone vendido
// motion-matching (Daniel Holden, MIT, §8 DEPENDENCY_POLICY) atrás do
// contrato público engine/animation/IMotionMatchVendor.hpp. ÚNICO TU que
// inclui database.h/vec.h/quat.h do doador. O pipeline da BASE DE DADOS é o
// do doador (FK, features de pé/quadril/trajetória, normalização, bounds); a
// busca é a database_search() do doador com features normalizadas; o lado da
// CONSULTA reusa a matemática vendida (quat_inv/quat_mul_vec3) para montar o
// vetor de features no mesmo layout. Sem raylib (demo gráfico não promovido).

#include "engine/animation/IMotionMatchVendor.hpp"

#include "character.h"  // define os índices Bone_* usados pelo doador
#include "database.h"
#include "quat.h"
#include "vec.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace engine {
namespace animation {
namespace {

// Layout do esqueleto humanoide do doador (character.h).
constexpr int kRootBone = 0;
constexpr int kHipsBone = 1;
constexpr int kLeftFootBone = 4;
constexpr int kRightFootBone = 8;
constexpr int kFeatureCount = 27;  // 5 grupos de 3 + 6 + 6 (database_build_matching_features)

class MotionMatchVendor final : public IMotionMatchVendor {
public:
    MotionMatchVendor() = default;

    bool build_database(const std::vector<VendorPose>& poses,
                        std::string& errorOut) override {
        if (poses.empty()) {
            errorOut = "motion match vendor: empty pose list";
            return false;
        }
        const std::size_t nBones = poses.front().bonePositions.size() / 3;
        if (nBones < 9) {
            errorOut = "motion match vendor: need >= 9 bones (humanoid layout)";
            return false;
        }
        for (const VendorPose& pose : poses) {
            if (!validate_pose(pose, nBones, errorOut)) return false;
        }

        ::database db;
        try {
            const int nFrames = static_cast<int>(poses.size());
            db.bone_positions.resize(nFrames, static_cast<int>(nBones));
            db.bone_rotations.resize(nFrames, static_cast<int>(nBones));
            db.bone_velocities.resize(nFrames, static_cast<int>(nBones));
            db.bone_angular_velocities.resize(nFrames, static_cast<int>(nBones));
            db.bone_parents.resize(static_cast<int>(nBones));
            db.range_starts.resize(1);
            db.range_stops.resize(1);
            db.contact_states.resize(nFrames, static_cast<int>(nBones));

            for (std::size_t b = 0; b < nBones; ++b) {
                db.bone_parents(static_cast<int>(b)) =
                    poses.front().boneParents[b];
            }
            for (int i = 0; i < nFrames; ++i) {
                const VendorPose& pose = poses[static_cast<std::size_t>(i)];
                for (std::size_t b = 0; b < nBones; ++b) {
                    const float* p = pose.bonePositions.data() + b * 3;
                    const float* r = pose.boneRotations.data() + b * 4;
                    const float* v = pose.boneVelocities.data() + b * 3;
                    db.bone_positions(i, static_cast<int>(b)) =
                        vec3(p[0], p[1], p[2]);
                    db.bone_rotations(i, static_cast<int>(b)) =
                        quat(r[3], r[0], r[1], r[2]);
                    db.bone_velocities(i, static_cast<int>(b)) =
                        vec3(v[0], v[1], v[2]);
                    db.bone_angular_velocities(i, static_cast<int>(b)) =
                        vec3(0.0f, 0.0f, 0.0f);
                }
                db.contact_states(i, 0) = false;
            }
            db.range_starts(0) = 0;
            db.range_stops(0) = nFrames;

            // Features + normalização + bounds: pipeline completo do doador.
            database_build_matching_features(db, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
        } catch (...) {
            errorOut = "motion match vendor: build failed (invalid pose data)";
            return false;
        }

        db_ = std::move(db);
        return true;
    }

    bool query(const VendorQuery& query, std::int32_t currentFrame,
               float transitionCost, std::int32_t& frameIndex, float& cost,
               std::string& errorOut) override {
        if (frame_count() == 0) {
            errorOut = "motion match vendor: database not built";
            return false;
        }
        const std::size_t nBones = query.worldPositions.size() / 3;
        if (nBones < 9 || query.worldRotations.size() / 4 != nBones ||
            query.worldVelocities.size() / 3 != nBones ||
            query.trajectoryPositions.size() != 9 ||
            query.trajectoryRotations.size() != 12) {
            errorOut = "motion match vendor: query layout incompatible";
            return false;
        }
        if (transitionCost < 0.0f || !(transitionCost == transitionCost)) {
            errorOut = "motion match vendor: transitionCost must be >= 0 and finite";
            return false;
        }
        if (currentFrame < -1 || currentFrame >=
            static_cast<std::int32_t>(frame_count())) {
            errorOut = "motion match vendor: currentFrame out of range";
            return false;
        }

        float raw[kFeatureCount];
        if (!build_query_features(query, raw, errorOut)) return false;

        int best = currentFrame >= 0 ? currentFrame : -1;
        // O doador usa FLT_MAX como semente: quando best == -1 o search NÃO
        // inicializa best_cost por conta própria (só quando best != -1, com o
        // custo do próprio current frame). Seedar 0 faria todo frame perder
        // no prune (curr_cost >= best_cost).
        float bestCost = std::numeric_limits<float>::max();
        ::slice1d<float> querySlice(kFeatureCount, raw);
        database_search(best, bestCost, db_, querySlice, transitionCost,
                        /*ignore_range_end*/ 0, /*ignore_surrounding*/ 0);

        if (best < 0) {
            errorOut = "motion match vendor: search returned no frame";
            return false;
        }
        frameIndex = best;
        cost = bestCost;
        return true;
    }

    std::size_t frame_count() const override { return db_.nframes(); }
    std::size_t feature_count() const override { return db_.nfeatures(); }

private:
    static bool validate_pose(const VendorPose& pose, std::size_t nBones,
                              std::string& errorOut) {
        if (pose.bonePositions.size() / 3 != nBones ||
            pose.boneRotations.size() / 4 != nBones ||
            pose.boneVelocities.size() / 3 != nBones ||
            pose.boneParents.size() != nBones) {
            errorOut = "motion match vendor: inconsistent bone counts in poses";
            return false;
        }
        for (std::size_t b = 0; b < nBones; ++b) {
            const std::int32_t parent = pose.boneParents[b];
            if (parent < -1 || parent >= static_cast<std::int32_t>(nBones) ||
                (b != 0 && parent == static_cast<std::int32_t>(b))) {
                errorOut = "motion match vendor: invalid bone parent";
                return false;
            }
        }
        return true;
    }

    // Monta os 27 features da consulta no MESMO layout do doador, usando a
    // matemática vendida (quat_inv/quat_mul_vec3) sobre o estado mundial.
    static bool build_query_features(const VendorQuery& query, float* raw,
                                     std::string& errorOut) {
        const vec3 rootPos = vec3_at(query.worldPositions, kRootBone);
        const quat rootRot = quat_at(query.worldRotations, kRootBone);

        int offset = 0;
        // Posição dos pés root-relativa (features 0..5).
        for (int bone : { kLeftFootBone, kRightFootBone }) {
            const vec3 pos = vec3_at(query.worldPositions, bone);
            const vec3 local = quat_mul_vec3(quat_inv(rootRot), pos - rootPos);
            raw[offset++] = local.x;
            raw[offset++] = local.y;
            raw[offset++] = local.z;
        }
        // Velocidade dos pés e do quadril (features 6..14).
        for (int bone : { kLeftFootBone, kRightFootBone, kHipsBone }) {
            const vec3 vel = vec3_at(query.worldVelocities, bone);
            const vec3 local = quat_mul_vec3(quat_inv(rootRot), vel);
            raw[offset++] = local.x;
            raw[offset++] = local.y;
            raw[offset++] = local.z;
        }
        // Trajetória: posições 2D (x,z) e direções 2D (features 15..26).
        for (int t = 0; t < 3; ++t) {
            const float* tp = query.trajectoryPositions.data() + t * 3;
            const vec3 futurePos(tp[0], tp[1], tp[2]);
            const vec3 localPos =
                quat_mul_vec3(quat_inv(rootRot), futurePos - rootPos);
            raw[offset++] = localPos.x;
            raw[offset++] = localPos.z;
        }
        for (int t = 0; t < 3; ++t) {
            const quat futureRot =
                quat(query.trajectoryRotations[t * 4 + 3],
                     query.trajectoryRotations[t * 4 + 0],
                     query.trajectoryRotations[t * 4 + 1],
                     query.trajectoryRotations[t * 4 + 2]);
            const vec3 dir =
                quat_mul_vec3(quat_inv(rootRot),
                              quat_mul_vec3(futureRot, vec3(0.0f, 0.0f, 1.0f)));
            raw[offset++] = dir.x;
            raw[offset++] = dir.z;
        }
        if (offset != kFeatureCount) {
            errorOut = "motion match vendor: internal feature layout mismatch";
            return false;
        }
        return true;
    }

    static vec3 vec3_at(const std::vector<float>& data, int bone) {
        const float* p = data.data() + bone * 3;
        return vec3(p[0], p[1], p[2]);
    }
    static quat quat_at(const std::vector<float>& data, int bone) {
        const float* r = data.data() + bone * 4;
        return quat(r[3], r[0], r[1], r[2]);
    }

    ::database db_;
};

}  // namespace

std::unique_ptr<IMotionMatchVendor> create_motion_match_vendor() {
    return std::make_unique<MotionMatchVendor>();
}

}  // namespace animation
}  // namespace engine
