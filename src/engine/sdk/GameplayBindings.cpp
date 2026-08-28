#include "engine/gameplay/IGameplayBindings.hpp"

#include <algorithm>
#include <sstream>

namespace engine::gameplay {
namespace {

bool has_domain(const std::vector<GameplayBinding>& values, GameplayDomain domain) {
    return std::find_if(values.begin(), values.end(), [domain](const auto& value) {
        return value.domain == domain;
    }) != values.end();
}

bool has_solution(const std::vector<GameplayExternalBinding>& values,
                 const std::string& id) {
    return std::find_if(values.begin(), values.end(), [&id](const auto& value) {
        return value.solutionId == id;
    }) != values.end();
}

class Bindings final : public IGameplayBindings {
public:
    bool configure(const std::vector<GameplayBinding>& values,
                   std::string& errorOut) override {
        if (values.empty()) {
            errorOut = "at least one gameplay binding is required";
            return false;
        }
        std::vector<GameplayBinding> next;
        next.reserve(values.size());
        for (const auto& value : values) {
            if (value.producerId.empty() || value.consumerId.empty()) {
                errorOut = "producer and consumer ids are required";
                return false;
            }
            if (has_domain(next, value.domain)) {
                errorOut = "duplicate gameplay domain";
                return false;
            }
            next.push_back(value);
        }
        bindings_ = std::move(next);
        errorOut.clear();
        return true;
    }

    bool configure_external(const std::vector<GameplayExternalBinding>& values,
                            std::string& errorOut) override {
        if (values.empty()) {
            errorOut = "at least one external solution binding is required";
            return false;
        }
        std::vector<GameplayExternalBinding> next;
        next.reserve(values.size());
        for (const auto& value : values) {
            if (value.solutionId.empty() || value.adapterId.empty() || value.contractId.empty()) {
                errorOut = "external solution, adapter and contract ids are required";
                return false;
            }
            if (has_solution(next, value.solutionId)) {
                errorOut = "duplicate external solution binding";
                return false;
            }
            next.push_back(value);
        }
        external_ = std::move(next);
        errorOut.clear();
        return true;
    }

    bool bind_producer(GameplayDomain domain, const std::string& id,
                       std::string& errorOut) override {
        return bind(domain, id, &GameplayBinding::producerId, errorOut);
    }
    bool bind_consumer(GameplayDomain domain, const std::string& id,
                       std::string& errorOut) override {
        return bind(domain, id, &GameplayBinding::consumerId, errorOut);
    }
    bool bind_persistence(GameplayDomain domain, const std::string& id,
                          std::string& errorOut) override {
        return bind(domain, id, &GameplayBinding::persistenceId, errorOut);
    }
    bool bind_replication(GameplayDomain domain, const std::string& id,
                          std::string& errorOut) override {
        return bind(domain, id, &GameplayBinding::replicationId, errorOut);
    }

    bool complete(std::string& errorOut) const override {
        if (bindings_.empty()) {
            errorOut = "gameplay bindings are not configured";
            return false;
        }
        for (const auto& value : bindings_) {
            if (value.producerId.empty() || value.consumerId.empty()) {
                errorOut = "producer and consumer bindings are required";
                return false;
            }
            if (requires_persistence(value.domain) && value.persistenceId.empty()) {
                errorOut = "persistence binding is required";
                return false;
            }
            if (requires_replication(value.domain) && value.replicationId.empty()) {
                errorOut = "replication binding is required";
                return false;
            }
        }
        static const char* required[] = {
            "behavior-tree-cpp", "ceres-solver", "deepmimic", "minecraft-spider",
            "motion-matching", "mujoco", "mujoco-mpc", "ozz-animation",
            "or-tools", "opus", "recast-navigation", "steam-audio", "acl"};
        if (external_.size() != 13) {
            errorOut = "exactly 13 external solution bindings are required";
            return false;
        }
        for (const auto* solution : required) {
            if (!has_solution(external_, solution)) {
                errorOut = std::string("missing external solution binding: ") + solution;
                return false;
            }
        }
        errorOut.clear();
        return true;
    }

    std::vector<GameplayBinding> bindings() const override { return bindings_; }
    std::vector<GameplayExternalBinding> external_bindings() const override { return external_; }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"bindings\":[";
        for (std::size_t i = 0; i < bindings_.size(); ++i) {
            if (i != 0) out << ',';
            const auto& value = bindings_[i];
            out << "{\"domain\":" << static_cast<unsigned>(value.domain)
                << ",\"producer\":\"" << value.producerId
                << "\",\"consumer\":\"" << value.consumerId
                << "\",\"persistence\":\"" << value.persistenceId
                << "\",\"replication\":\"" << value.replicationId << "\"}";
        }
        out << "],\"external\":[";
        for (std::size_t i = 0; i < external_.size(); ++i) {
            if (i != 0) out << ',';
            const auto& value = external_[i];
            out << "{\"solution\":\"" << value.solutionId
                << "\",\"domain\":" << static_cast<unsigned>(value.domain)
                << ",\"adapter\":\"" << value.adapterId
                << "\",\"contract\":\"" << value.contractId
                << "\",\"runtimeOptional\":"
                << (value.runtimeOptional ? "true" : "false") << "}";
        }
        out << "]}";
        return out.str();
    }

private:
    using Field = std::string GameplayBinding::*;

    bool bind(GameplayDomain domain, const std::string& id, Field field,
              std::string& errorOut) {
        if (id.empty()) {
            errorOut = "binding id is required";
            return false;
        }
        const auto it = std::find_if(bindings_.begin(), bindings_.end(), [domain](const auto& value) {
            return value.domain == domain;
        });
        if (it == bindings_.end()) {
            errorOut = "gameplay domain is not configured";
            return false;
        }
        (*it).*field = id;
        errorOut.clear();
        return true;
    }

    static bool requires_persistence(GameplayDomain domain) {
        return domain == GameplayDomain::Ecs || domain == GameplayDomain::Animation ||
               domain == GameplayDomain::Worlds || domain == GameplayDomain::Multiplayer;
    }
    static bool requires_replication(GameplayDomain domain) {
        return domain == GameplayDomain::Ecs || domain == GameplayDomain::Physics ||
               domain == GameplayDomain::Voxel || domain == GameplayDomain::Worlds ||
               domain == GameplayDomain::Multiplayer;
    }

    std::vector<GameplayBinding> bindings_;
    std::vector<GameplayExternalBinding> external_;
};

} // namespace

std::unique_ptr<IGameplayBindings> create_gameplay_bindings() {
    return std::make_unique<Bindings>();
}

} // namespace engine::gameplay
