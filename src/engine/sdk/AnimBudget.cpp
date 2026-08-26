// AnimBudget.cpp — adapter único de IAnimBudget (engine::animation).
// Seleção determinística: ordena por (importance + owed) desc com empate por
// id, acumula até estourar o orçamento (sem item parcial); pulados ganham
// boost de fairness. JSON bit-exact all-or-nothing.

#include "engine/animation/IAnimBudget.hpp"

#include "engine/sdk/RegistryJson.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <sstream>
#include <utility>

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

class AnimBudget final : public IAnimBudget {
public:
    AnimBudget() {
        budget_ms_ = 4.0;
        boost_ = 1.0;
    }

    bool configure(double budget_ms, double boost,
                   std::string& errorOut) override {
        if (!std::isfinite(budget_ms) || budget_ms <= 0.0) {
            errorOut = "budget must be finite and > 0";
            return false;
        }
        if (!std::isfinite(boost) || boost < 0.0) {
            errorOut = "boost must be finite and >= 0";
            return false;
        }
        budget_ms_ = budget_ms;
        boost_ = boost;
        errorOut.clear();
        return true;
    }

    BudgetFrame select(const std::vector<AnimUpdateEntry>& entries,
                       std::string& errorOut) override {
        BudgetFrame out;
        std::map<std::string, bool> seen;
        struct Item {
            std::string id;
            double priority;
            double cost;
        };
        std::vector<Item> items;
        items.reserve(entries.size());
        for (const AnimUpdateEntry& e : entries) {
            if (e.id.empty()) {
                errorOut = "entry id must not be empty";
                return out;
            }
            if (seen.count(e.id) != 0) {
                errorOut = "duplicate entry id \"" + e.id + "\"";
                return out;
            }
            seen[e.id] = true;
            if (!std::isfinite(e.importance)) {
                errorOut = "importance must be finite";
                return out;
            }
            if (!std::isfinite(e.cost_ms) || e.cost_ms < 0.0) {
                errorOut = "cost must be finite and >= 0";
                return out;
            }
            const auto oit = owed_.find(e.id);
            const double owed = oit == owed_.end() ? 0.0 : oit->second;
            last_importance_[e.id] = e.importance;
            items.push_back({e.id, e.importance + owed, e.cost_ms});
        }
        // Ordenação estável: prioridade desc, depois id (empate determinístico).
        std::stable_sort(items.begin(), items.end(),
                         [](const Item& a, const Item& b) {
                             if (a.priority != b.priority) {
                                 return a.priority > b.priority;
                             }
                             return a.id < b.id;
                         });
        double used = 0.0;
        for (const Item& it : items) {
            if (it.cost <= 0.0 || used + it.cost <= budget_ms_ + 1e-12) {
                out.selected.push_back(it.id);
                used += it.cost;
                owed_[it.id] = 0.0;
            } else {
                out.skipped.push_back(it.id);
                owed_[it.id] = owed_.count(it.id) ? owed_[it.id] + boost_
                                                  : boost_;
            }
        }
        out.used_ms = used;
        errorOut.clear();
        return out;
    }

    double budget_ms() const override { return budget_ms_; }
    double boost() const override { return boost_; }

    double effective_priority(const std::string& id) const override {
        const auto iit = last_importance_.find(id);
        const double imp = iit == last_importance_.end() ? 0.0 : iit->second;
        const auto oit = owed_.find(id);
        const double owed = oit == owed_.end() ? 0.0 : oit->second;
        return imp + owed;
    }

    void reset() override {
        owed_.clear();
        last_importance_.clear();
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << std::setprecision(9);
        out << "{\"budget_ms\":" << budget_ms_ << ",\"boost\":" << boost_
            << ",\"owed\":{";
        bool first = true;
        for (const auto& kv : owed_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":" << kv.second;
        }
        out << "},\"importance\":{";
        first = true;
        for (const auto& kv : last_importance_) {
            if (!first) out << ",";
            first = false;
            out << "\"" << json_escape(kv.first) << "\":" << kv.second;
        }
        out << "}}";
        return out.str();
    }

    bool deserialize_state(const std::string& json,
                           std::string& errorOut) override {
        sdk::JsonValue doc;
        if (!sdk::json_parse(json, doc, errorOut)) return false;
        if (!doc.is_object()) {
            errorOut = "anim budget state must be an object";
            return false;
        }
        const sdk::JsonValue* budget = doc.field("budget_ms");
        const sdk::JsonValue* boost = doc.field("boost");
        const sdk::JsonValue* owed = doc.field("owed");
        const sdk::JsonValue* importance = doc.field("importance");
        if (budget == nullptr || boost == nullptr || owed == nullptr ||
            importance == nullptr ||
            budget->kind != sdk::JsonValue::Kind::Number ||
            boost->kind != sdk::JsonValue::Kind::Number ||
            !owed->is_object() || !importance->is_object()) {
            errorOut = "state needs budget_ms/boost numbers and owed/"
                       "importance objects";
            return false;
        }
        if (!std::isfinite(budget->number) || budget->number <= 0.0 ||
            !std::isfinite(boost->number) || boost->number < 0.0) {
            errorOut = "budget/boost out of range";
            return false;
        }
        std::map<std::string, double> parsedOwed;
        for (const auto& kv : owed->object) {
            if (kv.second.kind != sdk::JsonValue::Kind::Number ||
                !std::isfinite(kv.second.number) || kv.second.number < 0.0) {
                errorOut = "owed values must be finite and >= 0";
                return false;
            }
            parsedOwed[kv.first] = kv.second.number;
        }
        std::map<std::string, double> parsedImp;
        for (const auto& kv : importance->object) {
            if (kv.second.kind != sdk::JsonValue::Kind::Number ||
                !std::isfinite(kv.second.number)) {
                errorOut = "importance values must be finite";
                return false;
            }
            parsedImp[kv.first] = kv.second.number;
        }
        budget_ms_ = budget->number;
        boost_ = boost->number;
        owed_ = std::move(parsedOwed);
        last_importance_ = std::move(parsedImp);
        errorOut.clear();
        return true;
    }

private:
    double budget_ms_ = 4.0;
    double boost_ = 1.0;
    std::map<std::string, double> owed_;
    std::map<std::string, double> last_importance_;
};

}  // namespace

std::unique_ptr<IAnimBudget> create_anim_budget() {
    return std::unique_ptr<IAnimBudget>(new AnimBudget());
}

}  // namespace engine::animation
