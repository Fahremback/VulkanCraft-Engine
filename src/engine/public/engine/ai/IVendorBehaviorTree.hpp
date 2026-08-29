// IVendorBehaviorTree — runtime de Behavior Trees do clone vendido
// behavior-tree-cpp (MIT, §8 DEPENDENCY_POLICY) atrás de superfície pública.
// Superfície self-contained: nenhum header do BehaviorTree.CPP aparece aqui;
// o adapter (src/engine/sdk/VendorBehaviorTree.cpp) é o ÚNICO TU que inclui
// <behaviortree_cpp/...>. Entrega o subset útil do doador — árvores
// data-driven em XML (composite/decorator/action/condition builtin), nós
// customizados por callback, blackboard compartilhado e tick — sem acoplar o
// jogo ao runtime externo. Tick é determinístico para árvores determinísticas
// (sem relógio/RNG no contrato; quem precisa de aleatoriedade injeta por nó).

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace ai {

enum class VendorNodeStatus : std::int32_t {
    SUCCESS = 0,
    FAILURE = 1,
    RUNNING = 2,
};

// Callbacks dos nós customizados. Recebem o nome do nó (para associação com
// o blackboard/config) e devolvem o status. Sem exceções: o adapter converte
// qualquer throw em FAILURE + errorOut no próximo tick.
using VendorActionFn = std::function<VendorNodeStatus(const std::string& nodeName)>;
using VendorConditionFn = std::function<bool(const std::string& nodeName)>;

class IVendorBehaviorTree {
public:
    virtual ~IVendorBehaviorTree() = default;

    // Registra nós customizados ANTES de load_from_xml. All-or-nothing:
    // nome vazio/duplicado → false + errorOut, registro anterior preservado.
    virtual bool register_action(const std::string& name, VendorActionFn fn,
                                 std::string& errorOut) = 0;
    virtual bool register_condition(const std::string& name,
                                    VendorConditionFn fn,
                                    std::string& errorOut) = 0;

    // Carrega a árvore de XML (BehaviorTree.CPP v4; suporta uma única
    // <BehaviorTree> ou main_tree_to_execute). All-or-nothing: XML malformado,
    // tipo de nó desconhecido, porta obrigatória ausente → false + errorOut,
    // e a árvore anterior permanece intacta.
    virtual bool load_from_xml(const std::string& xml, std::string& errorOut) = 0;

    // Executa UM tick da árvore carregada. Árvore não carregada → FAILURE.
    virtual VendorNodeStatus tick(std::string& errorOut) = 0;

    // Blackboard compartilhado (entrada/saída entre nós e o chamador).
    virtual bool set_blackboard(const std::string& key, const std::string& value,
                                std::string& errorOut) = 0;
    virtual bool get_blackboard(const std::string& key, std::string& value) const = 0;

    // Nomes dos tipos builtin disponíveis no runtime vendido (tooling).
    virtual std::vector<std::string> builtin_node_names() const = 0;
};

std::unique_ptr<IVendorBehaviorTree> create_vendor_behavior_tree(std::string& errorOut);

}  // namespace ai
}  // namespace engine
