// Inertializer.cpp — adapter único de IInertializer (engine::animation).
// Envelope (1 + t/T)·e^(−t/T) sobre o resíduo capturado no reset; saída =
// alvo ⊕ resíduo·d. JSON bit-exact all-or-nothing.

#include "engine/animation/IInertializer.hpp"
#include "engine/animation/IAnimBudget.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <utility>
#include <vector>

namespace engine::animation {
namespace {

std::string json_escape(const std::string& text) {
    std::ostringstream out;
    for (char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out << "\\u" << std::hex << std::setw(4)
                        << std::setfill('0')
                        << static_cast<int>(static_cast<unsigned char>(c))
                        << std::dec;
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

struct ResidualBone {
    AnimVec3 pos;
    AnimQuat rot;
    AnimVec3 scale;
};

// Envelope criticamente amortecido: d(t) = (1 + t/T)·e^(−t/T).
double envelope(double t, double T) {
    return (1.0 + t / T) * std::exp(-t / T);
}

class Inertializer final : public IInertializer {
public:
    Inertializer() {
        decay_ = 0.25;
        time_ = 0.0;
        budget_ = create_anim_budget();
        if (budget_) {
            std::string ignored;
            (void)budget_->configure(0.25, 0.25, ignored);
        }
        budgetEntries_.reserve(1);
        budgetEntries_.push_back(AnimUpdateEntry{ "inertializer.entity", 1.0, 0.05 });
    }

    void set_decay_time(double seconds, std::string& errorOut) override {
        if (!std::isfinite(seconds) || seconds <= 0.0) {
            errorOut = "decay time must be finite and > 0";
            return;
        }
        decay_ = seconds;
        errorOut.clear();
    }

    double decay_time() const override { return decay_; }

    bool reset(const std::vector<BonePose>& current,
               const std::vector<BonePose>& target,
               std::string& errorOut) override {
        if (current.size() != target.size()) {
            errorOut = "current and target poses must have the same size";
            return false;
        }
        residual_.clear();
        residual_.reserve(current.size());
        targetIndex_.clear();
        targetIndex_.reserve(current.size());
        for (const BonePose& c : current) {
            std::size_t targetIndex = target.size();
            for (std::size_t i = 0; i < target.size(); ++i) {
                if (target[i].bone == c.bone) {
                    targetIndex = i;
                    break;
                }
            }
            if (targetIndex == target.size()) {
                errorOut = "bone \"" + c.bone +
                           "\" missing from target pose";
                return false;
            }
            const AnimTransform& t = target[targetIndex].local;
            ResidualBone r;
            r.pos = {c.local.position.x - t.position.x,
                     c.local.position.y - t.position.y,
                     c.local.position.z - t.position.z};
            r.rot = (c.local.rotation * t.rotation.inverse()).normalized();
            r.scale = {c.local.scale.x / t.scale.x,
                       c.local.scale.y / t.scale.y,
                       c.local.scale.z / t.scale.z};
            residual_.push_back(r);
            targetIndex_.push_back(targetIndex);
        }
        order_.clear();
        for (const BonePose& p : current) order_.push_back(p.bone);
        time_ = 0.0;
        errorOut.clear();
        return true;
    }

    InertializerResult tick(const std::vector<BonePose>& target, double dt,
                            std::string& errorOut) override {
        InertializerResult out;
        if (!std::isfinite(dt) || dt < 0.0) {
            errorOut = "dt must be finite and >= 0";
            return out;
        }
        if (residual_.empty()) {
            out.pose = target;
            out.settled = true;
            errorOut.clear();
            return out;
        }
        if (target.size() != residual_.size()) {
            errorOut = "target pose size mismatch with active residual";
            return out;
        }

        // This object is the live per-entity inertialization consumer used by
        // the game showcase. Run the public animation budget on that same
        // update. The cost model is deterministic and proportional to the
        // skeleton size; the single active entity normally fits, while the
        // budget/fairness state remains real and serializable/observable.
        if (budget_) {
            budgetEntries_[0].importance = is_active() ? 1.0 : 0.25;
            budgetEntries_[0].cost_ms =
                0.05 + std::min<std::size_t>(target.size(), 100u) * 0.001;
            std::string budgetError;
            const BudgetFrame frame = budget_->select(budgetEntries_, budgetError);
            if (!budgetError.empty()) {
                errorOut = "animation budget refused inertializer update: " + budgetError;
                return out;
            }
            lastBudgetUsedMs_ = frame.used_ms;
            lastBudgetSelected_ = !frame.selected.empty();
            lastBudgetPriority_ = budget_->effective_priority(budgetEntries_[0].id);
            if (!lastBudgetSelected_) {
                // Hold simulation time when this entity loses the frame
                // budget. The target pose is still returned so root motion,
                // sockets and collision authority are never stalled.
                out.pose = target;
                out.settled = false;
                errorOut.clear();
                return out;
            }
        }

        time_ += dt;
        const double d = envelope(time_, decay_);
        out.pose.reserve(target.size());
        for (std::size_t i = 0; i < residual_.size(); ++i) {
            if (i >= targetIndex_.size()) targetIndex_.resize(residual_.size(), target.size());
            if (targetIndex_[i] >= target.size() ||
                target[targetIndex_[i]].bone != order_[i]) {
                auto found = std::find_if(
                    target.begin(), target.end(), [&](const BonePose& pose) {
                        return pose.bone == order_[i];
                    });
                if (found == target.end()) {
                    errorOut = "bone \"" + order_[i] +
                               "\" missing from target pose";
                    return out;
                }
                // A serialized inertializer stores stable bone names, not
                // caller-local vector indices. Rebuild the index lazily when
                // a caller presents the same skeleton in another order.
                targetIndex_[i] = static_cast<std::size_t>(
                    std::distance(target.begin(), found));
            }
            const BonePose& t = target[targetIndex_[i]];
            const ResidualBone& r = residual_[i];
            BonePose p;
            p.bone = t.bone;
            p.local.position = {t.local.position.x + r.pos.x * d,
                                t.local.position.y + r.pos.y * d,
                                t.local.position.z + r.pos.z * d};
            p.local.rotation =
                (t.local.rotation *
                 AnimQuat::slerp(AnimQuat{}, r.rot, d))
                    .normalized();
            p.local.scale = {t.local.scale.x * (1.0 + (r.scale.x - 1.0) * d),
                             t.local.scale.y * (1.0 + (r.scale.y - 1.0) * d),
                             t.local.scale.z * (1.0 + (r.scale.z - 1.0) * d)};
            out.pose.push_back(p);
        }
        out.settled = time_ >= 4.0 * decay_;
        errorOut.clear();
        return out;
    }

    void clear() override {
        residual_.clear();
        order_.clear();
        targetIndex_.clear();
        time_ = 0.0;
        if (budget_) budget_->reset();
        lastBudgetUsedMs_ = 0.0;
        lastBudgetPriority_ = 0.0;
        lastBudgetSelected_ = false;
    }

    bool is_active() const override { return !residual_.empty(); }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"decay\":" << decay_ << ",\"time\":" << time_
            << ",\"budgetUsedMs\":" << lastBudgetUsedMs_
            << ",\"budgetLimitMs\":" << (budget_ ? budget_->budget_ms() : 0.0)
            << ",\"budgetPriority\":" << lastBudgetPriority_
            << ",\"budgetSelected\":" << (lastBudgetSelected_ ? "true" : "false")
            << ",\"residual\":[";
        for (std::size_t i = 0; i < residual_.size(); ++i) {
            if (i > 0) out << ",";
            const ResidualBone& r = residual_[i];
            out << "{\"bone\":\"" << json_escape(order_[i])
                << "\",\"pos\":[" << r.pos.x << "," << r.pos.y << ","
                << r.pos.z << "],\"rot\":[" << r.rot.x << "," << r.rot.y
                << "," << r.rot.z << "," << r.rot.w << "],\"scale\":["
                << r.scale.x << "," << r.scale.y << "," << r.scale.z << "]}";
        }
        out << "]}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "inertializer state must be an object";
            return false;
        }
        const sdk::JsonValue* decay = doc.field("decay");
        const sdk::JsonValue* time = doc.field("time");
        const sdk::JsonValue* residual = doc.field("residual");
        if (decay == nullptr || time == nullptr || residual == nullptr ||
            decay->kind != sdk::JsonValue::Kind::Number ||
            time->kind != sdk::JsonValue::Kind::Number ||
            !residual->is_array()) {
            errorOut = "state needs decay/time numbers and residual array";
            return false;
        }
        if (!std::isfinite(decay->number) || decay->number <= 0.0) {
            errorOut = "decay must be finite and > 0";
            return false;
        }

        double parsedBudgetUsedMs = 0.0;
        double parsedBudgetLimitMs = budget_ ? budget_->budget_ms() : 0.25;
        double parsedBudgetPriority = 0.0;
        bool parsedBudgetSelected = false;
        if (const sdk::JsonValue* v = doc.field("budgetUsedMs")) {
            if (v->kind != sdk::JsonValue::Kind::Number ||
                !std::isfinite(v->number) || v->number < 0.0) {
                errorOut = "budgetUsedMs must be a finite non-negative number";
                return false;
            }
            parsedBudgetUsedMs = v->number;
        }
        if (const sdk::JsonValue* v = doc.field("budgetLimitMs")) {
            if (v->kind != sdk::JsonValue::Kind::Number ||
                !std::isfinite(v->number) || v->number <= 0.0) {
                errorOut = "budgetLimitMs must be a finite positive number";
                return false;
            }
            parsedBudgetLimitMs = v->number;
        }
        if (const sdk::JsonValue* v = doc.field("budgetPriority")) {
            if (v->kind != sdk::JsonValue::Kind::Number || !std::isfinite(v->number)) {
                errorOut = "budgetPriority must be a finite number";
                return false;
            }
            parsedBudgetPriority = v->number;
        }
        if (const sdk::JsonValue* v = doc.field("budgetSelected")) {
            if (v->kind != sdk::JsonValue::Kind::Bool) {
                errorOut = "budgetSelected must be a boolean";
                return false;
            }
            parsedBudgetSelected = v->boolean;
        }

        std::vector<ResidualBone> parsedResidual;
        std::vector<std::string> parsedOrder;
        for (const sdk::JsonValue& item : residual->array) {
            if (!item.is_object()) {
                errorOut = "residual entry must be an object";
                return false;
            }
            const sdk::JsonValue* bone = item.field("bone");
            const sdk::JsonValue* pos = item.field("pos");
            const sdk::JsonValue* rot = item.field("rot");
            const sdk::JsonValue* scale = item.field("scale");
            if (bone == nullptr || pos == nullptr || rot == nullptr ||
                scale == nullptr ||
                bone->kind != sdk::JsonValue::Kind::String ||
                pos->kind != sdk::JsonValue::Kind::Array ||
                rot->kind != sdk::JsonValue::Kind::Array ||
                scale->kind != sdk::JsonValue::Kind::Array ||
                pos->array.size() != 3 || rot->array.size() != 4 ||
                scale->array.size() != 3) {
                errorOut = "residual entry malformed";
                return false;
            }
            ResidualBone r;
            r.pos = {pos->array[0].number, pos->array[1].number,
                     pos->array[2].number};
            r.rot = {rot->array[0].number, rot->array[1].number,
                     rot->array[2].number, rot->array[3].number};
            r.scale = {scale->array[0].number, scale->array[1].number,
                       scale->array[2].number};
            parsedResidual.push_back(r);
            parsedOrder.push_back(bone->string);
        }
        if (budget_) {
            std::string budgetError;
            if (!budget_->configure(parsedBudgetLimitMs, budget_->boost(), budgetError)) {
                errorOut = "invalid restored animation budget: " + budgetError;
                return false;
            }
        }
        decay_ = decay->number;
        time_ = time->number;
        residual_ = std::move(parsedResidual);
        order_ = std::move(parsedOrder);
        targetIndex_.resize(order_.size());
        for (std::size_t i = 0; i < targetIndex_.size(); ++i) targetIndex_[i] = i;
        lastBudgetUsedMs_ = parsedBudgetUsedMs;
        lastBudgetPriority_ = parsedBudgetPriority;
        lastBudgetSelected_ = parsedBudgetSelected;
        errorOut.clear();
        return true;
    }

private:
    double decay_ = 0.25;
    double time_ = 0.0;
    std::vector<ResidualBone> residual_;
    std::vector<std::string> order_;
    std::vector<std::size_t> targetIndex_;
    std::unique_ptr<IAnimBudget> budget_;
    std::vector<AnimUpdateEntry> budgetEntries_;
    double lastBudgetUsedMs_{ 0.0 };
    double lastBudgetPriority_{ 0.0 };
    bool lastBudgetSelected_{ false };
};

}  // namespace

std::unique_ptr<IInertializer> create_inertializer() {
    return std::unique_ptr<IInertializer>(new Inertializer());
}

}  // namespace engine::animation
