// SceneHierarchy — adapter do contrato engine/editor ISceneHierarchy.
//
// Lista plana determinística da cena: DFS estável (filhos logo após o pai,
// ordem de inserção), profundidade calculada, busca por substring
// case-insensitive do nome. Ciclos quebrados tratando o membro como raiz.
// Sem RNG/relógio/estado global.

#include "engine/editor/ISceneHierarchy.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace engine::editor {

namespace {

// lowercase copy (busca case-insensitive, transformação documentada).
std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

class SceneHierarchyImpl : public ISceneHierarchy {
public:
    std::vector<HierarchyRow> build(
        const std::vector<HierarchyEntity>& entities,
        const std::vector<HierarchyLink>& links,
        const std::string& query) const override {
        std::vector<HierarchyRow> rows;

        // parent de cada child (último link vence; determinístico).
        std::unordered_map<std::string, std::string> parent_of;
        for (const HierarchyLink& l : links) {
            parent_of[l.child_id] = l.parent_id;
        }
        // filhos de cada pai, na ordem em que aparecem nos links.
        std::unordered_map<std::string, std::vector<std::string>> children_of;
        for (const HierarchyLink& l : links) {
            children_of[l.parent_id].push_back(l.child_id);
        }

        // Detecta membros de ciclos: seguindo a cadeia de pais a partir de um
        // membro retorna a ele próprio. Membros de ciclo são tratados como
        // raízes (sem filhos via link do ciclo) — semântica documentada.
        std::unordered_set<std::string> cycle_members;
        for (const HierarchyEntity& e : entities) {
            std::unordered_set<std::string> path;
            std::string cur = e.id;
            bool in_cycle = false;
            for (std::size_t step = 0; step <= entities.size(); ++step) {
                if (!path.insert(cur).second) {
                    in_cycle = true;
                    break;
                }
                const auto it = parent_of.find(cur);
                if (it == parent_of.end() || it->second.empty()) break;
                cur = it->second;
            }
            if (in_cycle) {
                for (const std::string& n : path) cycle_members.insert(n);
            }
        }
        // Remove links cujo child OU parent é membro de ciclo.
        if (!cycle_members.empty()) {
            std::unordered_map<std::string, std::vector<std::string>> filtered;
            for (const HierarchyLink& l : links) {
                if (cycle_members.count(l.child_id) || cycle_members.count(l.parent_id)) continue;
                filtered[l.parent_id].push_back(l.child_id);
            }
            children_of.swap(filtered);
        }

        // nome por id (para busca + exibição).
        std::unordered_map<std::string, std::string> name_of;
        for (const HierarchyEntity& e : entities) {
            name_of[e.id] = e.name;
        }

        const std::string q = to_lower(query);
        // ids que casam a busca (para incluir pais de filhos casando).
        std::unordered_set<std::string> matching;
        if (!q.empty()) {
            for (const HierarchyEntity& e : entities) {
                if (to_lower(e.name).find(q) != std::string::npos) {
                    matching.insert(e.id);
                }
            }
            // pais de um filho casando entram junto (DFS completa).
            bool changed = true;
            while (changed) {
                changed = false;
                for (const HierarchyLink& l : links) {
                    if (matching.count(l.child_id) && !matching.count(l.parent_id) &&
                        !l.parent_id.empty()) {
                        matching.insert(l.parent_id);
                        changed = true;
                    }
                }
            }
        }

        // raízes = entidades sem link ou cujo parent não é entidade
        // (ciclo → membro tratado como raiz). Ordem de inserção estável.
        std::unordered_set<std::string> all_ids;
        for (const HierarchyEntity& e : entities) all_ids.insert(e.id);
        std::unordered_set<std::string> is_child;
        for (const HierarchyLink& l : links) {
            if (all_ids.count(l.parent_id)) is_child.insert(l.child_id);
        }

        std::unordered_set<std::string> visited;
        std::size_t index = 0;
        const auto emit = [&](const std::string& id, int depth) {
            if (visited.count(id)) return;
            visited.insert(id);
            if (q.empty() || matching.count(id)) {
                HierarchyRow r;
                r.id = id;
                r.name = name_of.count(id) ? name_of.at(id) : id;
                r.depth = depth;
                r.index = index++;
                rows.push_back(std::move(r));
            }
        };
        const auto dfs = [&](const auto& self, const std::string& id, int depth) -> void {
            emit(id, depth);
            const auto it = children_of.find(id);
            if (it == children_of.end()) return;
            for (const std::string& c : it->second) {
                if (!visited.count(c)) self(self, c, depth + 1);
            }
        };

        for (const HierarchyEntity& e : entities) {
            const bool is_root = !is_child.count(e.id);
            if (is_root) dfs(dfs, e.id, 0);
        }
        // qualquer entidade não visitada (pai inexistente mas filho listado
        // antes da raiz nos links) → raiz residual, determinístico.
        for (const HierarchyEntity& e : entities) {
            if (!visited.count(e.id)) dfs(dfs, e.id, 0);
        }
        return rows;
    }

    std::string to_json(const std::vector<HierarchyRow>& rows) const override {
        std::ostringstream out;
        out << "[";
        for (std::size_t i = 0; i < rows.size(); ++i) {
            if (i > 0) out << ",";
            out << "{\"id\":\"" << rows[i].id << "\",\"name\":\""
                << rows[i].name << "\",\"depth\":" << rows[i].depth
                << ",\"index\":" << rows[i].index << "}";
        }
        out << "]";
        return out.str();
    }
};

}  // namespace

std::unique_ptr<ISceneHierarchy> create_scene_hierarchy() {
    return std::make_unique<SceneHierarchyImpl>();
}

}  // namespace engine::editor
