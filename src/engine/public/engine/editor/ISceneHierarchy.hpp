#pragma once
// ISceneHierarchy — contrato público do modelo de hierarquia da cena do
// editor (agente 2 §B, "Integrar Hierarchy, Content Browser, Inspector").
//
// Constrói, de forma determinística, a lista plana exibível da cena a partir
// de entidades (id + nome) e relações de parentesco: filhos logo após o pai,
// ordem de inserção estável, profundidade calculada, com busca por nome
// (substring, case-insensitive — transformação documentada e determinística).
// O painel visual (draw_hierarchy_panel) pode consumir este modelo em vez de
// montar a lista ad-hoc; o editor expõe o modelo das entidades REAIS via
// GET /hierarchy. SEM RNG/relógio/estado global. Self-contained (std apenas).

#include <memory>
#include <string>
#include <vector>

namespace engine::editor {

// Uma entidade de entrada (id estável + nome para exibição).
struct HierarchyEntity {
    std::string id;     // UUID string
    std::string name;   // nome exibido
};

// Relação de parentesco: parent_id vazio = raiz.
struct HierarchyLink {
    std::string child_id;
    std::string parent_id;
};

// Uma linha da lista plana exibível.
struct HierarchyRow {
    std::string id;
    std::string name;
    int depth = 0;        // 0 = raiz, 1 = filho de raiz, ...
    std::size_t index = 0;  // ordem estável na lista
};

// Contrato do modelo de hierarquia.
struct ISceneHierarchy {
    virtual ~ISceneHierarchy() = default;

    // Constrói a lista plana. Entidades sem link = raízes, na ordem dada.
    // Filhos aparecem imediatamente após o pai (DFS), ordem de inserção
    // estável. Ciclos são quebrados tratando o membro como raiz (determinístico).
    // query vazio = lista completa; senão filtra por substring
    // case-insensitive do nome (raízes com filhos casando entram junto).
    virtual std::vector<HierarchyRow> build(
        const std::vector<HierarchyEntity>& entities,
        const std::vector<HierarchyLink>& links,
        const std::string& query) const = 0;

    // JSON determinístico da lista: [{"id","name","depth","index"},...].
    virtual std::string to_json(const std::vector<HierarchyRow>& rows) const = 0;
};

// Factory do adapter (implementada em src/engine/sdk/SceneHierarchy.cpp).
std::unique_ptr<ISceneHierarchy> create_scene_hierarchy();

}  // namespace engine::editor
