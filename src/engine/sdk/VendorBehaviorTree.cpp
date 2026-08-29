// VendorBehaviorTree.cpp — adapter do runtime de Behavior Trees do clone
// vendido behavior-tree-cpp (§8 DEPENDENCY_POLICY) atrás do contrato público
// engine/ai/IVendorBehaviorTree.hpp. ÚNICO TU que inclui headers do
// BehaviorTree.CPP. Árvores XML data-driven (composite/decorator/action/
// condition builtin), nós customizados por callback e blackboard — o runtime
// real do doador, sem vazar headers externos para a API pública.
//
// Notas de integração com o clone (4.10.0):
// - registerSimpleAction cria SyncActionNode, que PROÍBE RUNNING ("SyncActionNode
//   MUST never return RUNNING") — para expor RUNNING (trabalho em andamento) os
//   nós customizados derivam de ActionNodeBase/ConditionNode diretamente.
// - tree.tickOnce() re-executa o tick enquanto houver wake-up signal; o tick do
//   contrato usa tickExactlyOnce() = exatamente UM tick da raiz por chamada,
//   determinístico e sem loop interno.
// - set_blackboard grava a chave DUAS vezes: a 1ª cria entrada genérica fraca
//   (AnyTypeAllowed) e a 2ª a promove para fortemente tipada std::string. Sem a
//   promoção, o nó vendido SetBlackboard com valor {referência} cai no caminho
//   parseString de UndefinedAnyType e armazena nullptr (quirk real do clone).

#include "engine/ai/IVendorBehaviorTree.hpp"

#include <behaviortree_cpp/action_node.h>
#include <behaviortree_cpp/behavior_tree.h>
#include <behaviortree_cpp/blackboard.h>
#include <behaviortree_cpp/bt_factory.h>
#include <behaviortree_cpp/condition_node.h>
#include <behaviortree_cpp/tree_node.h>

#include <exception>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace engine {
namespace ai {
namespace {

VendorNodeStatus map_status(BT::NodeStatus status) {
    switch (status) {
        case BT::NodeStatus::SUCCESS: return VendorNodeStatus::SUCCESS;
        case BT::NodeStatus::FAILURE: return VendorNodeStatus::FAILURE;
        case BT::NodeStatus::RUNNING: return VendorNodeStatus::RUNNING;
        default: return VendorNodeStatus::FAILURE;
    }
}

BT::NodeStatus to_bt(VendorNodeStatus status) {
    switch (status) {
        case VendorNodeStatus::SUCCESS: return BT::NodeStatus::SUCCESS;
        case VendorNodeStatus::FAILURE: return BT::NodeStatus::FAILURE;
        case VendorNodeStatus::RUNNING: return BT::NodeStatus::RUNNING;
    }
    return BT::NodeStatus::FAILURE;
}

// Ação customizada com suporte a RUNNING (ActionNodeBase, não SyncActionNode).
class CallbackActionNode : public BT::ActionNodeBase {
public:
    CallbackActionNode(const std::string& name, const BT::NodeConfig& config,
                       VendorActionFn fn)
        : BT::ActionNodeBase(name, config), fn_(std::move(fn)) {}

    BT::NodeStatus tick() override {
        try {
            return to_bt(fn_(name()));
        } catch (...) {
            return BT::NodeStatus::FAILURE;
        }
    }

    void halt() override {}  // ActionNodeBase::halt é virtual pura

    static BT::PortsList providedPorts() { return {}; }

private:
    VendorActionFn fn_;
};

// Condição customizada (ConditionNode).
class CallbackConditionNode : public BT::ConditionNode {
public:
    CallbackConditionNode(const std::string& name, const BT::NodeConfig& config,
                          VendorConditionFn fn)
        : BT::ConditionNode(name, config), fn_(std::move(fn)) {}

    BT::NodeStatus tick() override {
        try {
            return fn_(name()) ? BT::NodeStatus::SUCCESS
                               : BT::NodeStatus::FAILURE;
        } catch (...) {
            return BT::NodeStatus::FAILURE;
        }
    }

    static BT::PortsList providedPorts() { return {}; }

private:
    VendorConditionFn fn_;
};

class VendorBehaviorTree final : public IVendorBehaviorTree {
public:
    VendorBehaviorTree() = default;

    bool register_action(const std::string& name, VendorActionFn fn,
                         std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "vendor behavior tree: action name must be non-empty";
            return false;
        }
        if (fn == nullptr) {
            errorOut = "vendor behavior tree: action callback null";
            return false;
        }
        if (actions_.count(name) != 0 || conditions_.count(name) != 0) {
            errorOut = "vendor behavior tree: duplicate node name '" + name + "'";
            return false;
        }
        try {
            factory_.registerNodeType<CallbackActionNode>(name, std::move(fn));
        } catch (const std::exception& ex) {
            errorOut = std::string("vendor behavior tree: register action failed: ") +
                       ex.what();
            return false;
        }
        actions_[name] = std::move(fn);
        return true;
    }

    bool register_condition(const std::string& name, VendorConditionFn fn,
                            std::string& errorOut) override {
        if (name.empty()) {
            errorOut = "vendor behavior tree: condition name must be non-empty";
            return false;
        }
        if (fn == nullptr) {
            errorOut = "vendor behavior tree: condition callback null";
            return false;
        }
        if (actions_.count(name) != 0 || conditions_.count(name) != 0) {
            errorOut = "vendor behavior tree: duplicate node name '" + name + "'";
            return false;
        }
        try {
            factory_.registerNodeType<CallbackConditionNode>(name, std::move(fn));
        } catch (const std::exception& ex) {
            errorOut = std::string("vendor behavior tree: register condition failed: ") +
                       ex.what();
            return false;
        }
        conditions_[name] = std::move(fn);
        return true;
    }

    bool load_from_xml(const std::string& xml, std::string& errorOut) override {
        if (xml.empty()) {
            errorOut = "vendor behavior tree: xml empty";
            return false;
        }
        BT::Tree tree;
        try {
            tree = factory_.createTreeFromText(xml, blackboard_);
        } catch (const std::exception& ex) {
            errorOut = std::string("vendor behavior tree: load failed: ") + ex.what();
            return false;
        } catch (...) {
            errorOut = "vendor behavior tree: load failed (unknown error)";
            return false;
        }
        tree_ = std::move(tree);
        return true;
    }

    VendorNodeStatus tick(std::string& errorOut) override {
        if (tree_.rootNode() == nullptr) {
            errorOut = "vendor behavior tree: no tree loaded";
            return VendorNodeStatus::FAILURE;
        }
        try {
            return map_status(tree_.tickExactlyOnce());
        } catch (const std::exception& ex) {
            errorOut = std::string("vendor behavior tree: tick failed: ") + ex.what();
            return VendorNodeStatus::FAILURE;
        } catch (...) {
            errorOut = "vendor behavior tree: tick failed (unknown error)";
            return VendorNodeStatus::FAILURE;
        }
    }

    bool set_blackboard(const std::string& key, const std::string& value,
                        std::string& errorOut) override {
        if (key.empty()) {
            errorOut = "vendor behavior tree: blackboard key empty";
            return false;
        }
        try {
            // 1ª gravação: entrada genérica; 2ª: promove a std::string forte
            // (ver nota do arquivo — quirk do SetBlackboard vendido).
            blackboard_->set(key, value);
            blackboard_->set(key, value);
            return true;
        } catch (const std::exception& ex) {
            errorOut = std::string("vendor behavior tree: set blackboard failed: ") +
                       ex.what();
            return false;
        }
    }

    bool get_blackboard(const std::string& key, std::string& value) const override {
        try {
            return blackboard_->get(key, value);
        } catch (...) {
            return false;
        }
    }

    std::vector<std::string> builtin_node_names() const override {
        const std::set<std::string>& builtin = factory_.builtinNodes();
        return std::vector<std::string>(builtin.begin(), builtin.end());
    }

private:
    BT::BehaviorTreeFactory factory_;
    BT::Blackboard::Ptr blackboard_{ BT::Blackboard::create() };
    BT::Tree tree_;
    std::map<std::string, VendorActionFn> actions_;
    std::map<std::string, VendorConditionFn> conditions_;
};

}  // namespace

std::unique_ptr<IVendorBehaviorTree> create_vendor_behavior_tree(
    std::string& errorOut) {
    return std::make_unique<VendorBehaviorTree>();
}

}  // namespace ai
}  // namespace engine
