#pragma once

// IRetargeting (agente 2 §B l.43): the PUBLIC, deterministic model of the
// animation-retargeting editor. The visual RetargetEditorModel in the
// specialized-editors panel must never drift from the authored data: a
// retarget maps source-skeleton bones to target-skeleton bones with a
// translation scale and a rotation offset, plus a preserve-root-motion flag.
// The contract is a pure model over that document:
//   - UNEQUIVOCAL: map/unmap/set_skeletons validate every input; invalid
//     commands are REFUSED with a reason and leave the document untouched
//     (all-or-nothing). map() upserts by sourceBone (single source bone can
//     only feed one target).
//   - DETERMINISM: no clocks/RNG/globals; same sequence of commands ->
//     identical state (mapping order = insertion order, stable).
//   - OBSERVABLE: to_json() serializes {sourceSkeleton, targetSkeleton,
//     preserveRootMotion, mapping} deterministically (the editor exposes it
//     via the Control API, e.g. GET /retargeting).
//
// Self-contained (std only). The SDK adapter
// (src/engine/sdk/Retargeting.cpp) is the ONLY TU with behavior.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace editor {

struct RetargetBoneMapDef {
    std::string sourceBone;
    std::string targetBone;
    float translationScale{ 1.0f };
    float rotationOffsetX{ 0.0f };
    float rotationOffsetY{ 0.0f };
    float rotationOffsetZ{ 0.0f };

    bool operator==(const RetargetBoneMapDef& other) const {
        return sourceBone == other.sourceBone &&
               targetBone == other.targetBone &&
               translationScale == other.translationScale &&
               rotationOffsetX == other.rotationOffsetX &&
               rotationOffsetY == other.rotationOffsetY &&
               rotationOffsetZ == other.rotationOffsetZ;
    }
};

struct RetargetingSnapshot {
    std::string sourceSkeleton;  // opaque id (hex string in the editor)
    std::string targetSkeleton;
    bool preserveRootMotion{ true };
    std::vector<RetargetBoneMapDef> mapping;

    bool operator==(const RetargetingSnapshot& other) const {
        return sourceSkeleton == other.sourceSkeleton &&
               targetSkeleton == other.targetSkeleton &&
               preserveRootMotion == other.preserveRootMotion &&
               mapping == other.mapping;
    }
    bool operator!=(const RetargetingSnapshot& other) const {
        return !(*this == other);
    }
};

class IRetargeting {
public:
    virtual ~IRetargeting() = default;

    virtual RetargetingSnapshot snapshot() const = 0;

    // Sets the source/target skeleton ids. REFUSED when either is empty.
    virtual bool set_skeletons(const std::string& source,
                               const std::string& target,
                               std::string& errorOut) = 0;

    // Upserts a bone mapping by sourceBone (replaces the previous mapping of
    // that source bone). REFUSED when either bone name is empty or the
    // translation scale is not positive-finite.
    virtual bool map(const RetargetBoneMapDef& mapping,
                     std::string& errorOut) = 0;

    // Removes the mapping for a source bone. REFUSED when it does not exist.
    virtual bool unmap(const std::string& sourceBone,
                       std::string& errorOut) = 0;

    // Clears all bone mappings.
    virtual void clear_mapping() = 0;

    // Preserve-root-motion flag (root follows the source motion).
    virtual void set_preserve_root_motion(bool preserve) = 0;

    // Validation issues, in stable order:
    //   - error   : source or target skeleton missing;
    //   - error   : source bone mapped twice (invariant: impossible via map);
    //   - warning : no bones mapped;
    //   - warning : two source bones mapped to the same target bone;
    //   - info    : self mapping (source bone == target bone).
    // Empty when the document is valid.
    virtual std::vector<std::string> validate() const = 0;

    // Deterministic JSON of the snapshot (bit-exact via %.6g floats).
    virtual std::string to_json() const = 0;
};

// Factory: the SDK adapter is the only TU with behavior.
std::unique_ptr<IRetargeting> create_retargeting();

}  // namespace editor
}  // namespace engine
