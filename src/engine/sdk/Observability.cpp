// Observability.cpp — adapter do contrato IObservability
// (engine::observability, §6 item 6 — "Publicar logging/tracing/crash
// reporting/telemetry opt-in por interfaces substituíveis").
//
// Implementação determinística: sinks em mapa ordenado por id; roteamento
// opt-in (`set_enabled` — sem sink ativo, emit é no-op); buffer circular com
// os últimos N logs; spans com id global estritamente crescente e fechamento
// por id; contadores/gauges em mapa ordenado por nome; sequência global de
// eventos; JSON bit-exact all-or-nothing (load só comita se TODOS os
// registros forem válidos — session mismatch/id duplicado/nível inválido/
// span pai inexistente/sequências não-crescentes rejeitam o documento inteiro
// e deixam o estado anterior intacto). Self-contained (std only) — mesma
// convenção dos demais adapters do sdk.

#include "engine/observability/IObservability.hpp"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <map>

namespace engine::observability {

namespace {

std::string json_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

bool skip_ws(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    return i < s.size();
}

const char* level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info:  return "info";
    case LogLevel::Warn:  return "warn";
    case LogLevel::Error: return "error";
    }
    return "info";
}

bool level_from_name(const std::string& name, LogLevel& out) {
    if (name == "trace") { out = LogLevel::Trace; return true; }
    if (name == "debug") { out = LogLevel::Debug; return true; }
    if (name == "info")  { out = LogLevel::Info;  return true; }
    if (name == "warn")  { out = LogLevel::Warn;  return true; }
    if (name == "error") { out = LogLevel::Error; return true; }
    return false;
}

}  // namespace

namespace {

class ObservabilityImpl final : public IObservability {
public:
    ObservabilityImpl(const std::string& sessionId, std::size_t history)
        : session_id_(sessionId), history_(history < 1 ? 1 : history) {}

    const std::string& session_id() const override { return session_id_; }

    bool register_sink(const std::string& sinkId, ISink* sink,
                       std::string& errorOut) override {
        if (sinkId.empty()) { errorOut = "empty sink id"; return false; }
        if (sink == nullptr) { errorOut = "null sink"; return false; }
        sinks_[sinkId] = sink;
        errorOut.clear();
        return true;
    }

    void remove_sink(const std::string& sinkId) override {
        sinks_.erase(sinkId);
    }

    void set_enabled(bool enabled) override { enabled_ = enabled; }
    bool enabled() const override { return enabled_; }

    bool log(LogLevel level, const std::string& category,
             const std::string& message, std::string& errorOut) override {
        if (category.empty()) { errorOut = "empty category"; return false; }
        LogEvent e;
        e.sequence = ++sequence_;
        e.level = level;
        e.category = category;
        e.message = message;
        logs_.push_back(e);
        if (logs_.size() > history_) logs_.pop_front();
        ++total_logs_;
        if (enabled_) {
            char level_buf[8];
            std::snprintf(level_buf, sizeof(level_buf), "%d", static_cast<int>(level));
            const std::string line = "[log " + std::string(level_name(level)) + " " +
                                     std::to_string(e.sequence) + "] " + category + ": " + message;
            emit_line(line);
        }
        errorOut.clear();
        return true;
    }

    bool begin_span(const std::string& name, std::uint64_t parent_id,
                    std::uint64_t& span_idOut, std::string& errorOut) override {
        if (name.empty()) { errorOut = "empty span name"; return false; }
        if (parent_id != 0 && !spans_.count(parent_id)) {
            errorOut = "unknown parent span";
            return false;
        }
        const std::uint64_t id = ++span_id_;
        TraceSpan s;
        s.span_id = id;
        s.parent_id = parent_id;
        s.name = name;
        s.begin_seq = ++sequence_;
        s.closed = false;
        spans_[id] = s;
        span_idOut = id;
        ++total_spans_;
        if (enabled_) {
            emit_line("[span begin " + std::to_string(id) + " parent " +
                      std::to_string(parent_id) + "] " + name);
        }
        errorOut.clear();
        return true;
    }

    bool end_span(std::uint64_t span_id, std::string& errorOut) override {
        const auto it = spans_.find(span_id);
        if (it == spans_.end()) { errorOut = "unknown span"; return false; }
        if (it->second.closed) { errorOut = "span already closed"; return false; }
        it->second.closed = true;
        it->second.end_seq = ++sequence_;
        if (enabled_) {
            emit_line("[span end " + std::to_string(span_id) + "] " + it->second.name);
        }
        errorOut.clear();
        return true;
    }

    bool increment_counter(const std::string& name, std::int64_t by,
                           std::string& errorOut) override {
        if (name.empty()) { errorOut = "empty counter name"; return false; }
        counters_[name] += by;
        if (enabled_) {
            emit_line("[counter " + json_escape(name) + " +" + std::to_string(by) + "] " +
                      std::to_string(counters_[name]));
        }
        errorOut.clear();
        return true;
    }

    bool set_gauge(const std::string& name, std::int64_t value,
                   std::string& errorOut) override {
        if (name.empty()) { errorOut = "empty gauge name"; return false; }
        counters_[name] = value;
        if (enabled_) {
            emit_line("[gauge " + json_escape(name) + " =] " + std::to_string(value));
        }
        errorOut.clear();
        return true;
    }

    CrashContext crash_context() const override {
        CrashContext ctx;
        ctx.recent_logs.assign(logs_.rbegin(), logs_.rend());
        for (const auto& kv : spans_) {
            if (!kv.second.closed) ctx.open_spans.push_back(kv.second);
        }
        for (const auto& kv : counters_) {
            ctx.counters.push_back(kv);
        }
        ctx.total_logs = total_logs_;
        ctx.total_spans = total_spans_;
        return ctx;
    }

    std::uint64_t total_logs() const override { return total_logs_; }
    std::uint64_t total_spans() const override { return total_spans_; }

    bool reset(std::string& errorOut) override {
        sinks_.clear();
        logs_.clear();
        spans_.clear();
        counters_.clear();
        sequence_ = 0;
        span_id_ = 0;
        total_logs_ = 0;
        total_spans_ = 0;
        enabled_ = true;
        errorOut.clear();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        // Documento: {"session": "...", "history": N, "enabled": bool,
        //  "sequence": N, "span_id": N, "total_logs": N, "total_spans": N,
        //  "logs": [ {seq, level, category, message}, ... ],
        //  "spans": [ {id, parent, name, begin, end, closed}, ... ],
        //  "counters": {name: value} }
        ObservabilityImpl fresh(session_id_, history_);
        std::string text;
        std::size_t i = 0;
        if (!skip_ws(json, i) || json[i] != '{') { errorOut = "not an object"; return false; }
        ++i;
        bool have_session = false, have_history = false;
        std::size_t parsed_history = 0;
        bool first_field = true;
        for (;;) {
            if (!skip_ws(json, i)) { errorOut = "unterminated"; return false; }
            if (json[i] == '}') { ++i; break; }
            if (!first_field) {
                if (json[i] != ',') { errorOut = "bad comma"; return false; }
                ++i;
            }
            first_field = false;
            if (!skip_ws(json, i) || json[i] != '"') { errorOut = "expected key"; return false; }
            const std::size_t ks = ++i;
            while (i < json.size() && json[i] != '"') ++i;
            if (i >= json.size()) { errorOut = "unterminated key"; return false; }
            const std::string key = json.substr(ks, i - ks);
            ++i;
            if (!skip_ws(json, i) || json[i] != ':') { errorOut = "expected ':'"; return false; }
            ++i;
            if (key == "session") {
                if (!read_string(json, i, text) || text != session_id_) {
                    errorOut = "session mismatch";
                    return false;
                }
                have_session = true;
            } else if (key == "history") {
                if (!read_size(json, i, parsed_history)) { errorOut = "bad history"; return false; }
                have_history = true;
            } else if (key == "enabled") {
                if (!read_bool(json, i, fresh.enabled_)) { errorOut = "bad enabled"; return false; }
            } else if (key == "sequence") {
                if (!read_u64(json, i, fresh.sequence_)) { errorOut = "bad sequence"; return false; }
            } else if (key == "span_id") {
                if (!read_u64(json, i, fresh.span_id_)) { errorOut = "bad span_id"; return false; }
            } else if (key == "total_logs") {
                if (!read_u64(json, i, fresh.total_logs_)) { errorOut = "bad total_logs"; return false; }
            } else if (key == "total_spans") {
                if (!read_u64(json, i, fresh.total_spans_)) { errorOut = "bad total_spans"; return false; }
            } else if (key == "logs") {
                if (!read_logs(json, i, fresh)) { errorOut = "bad logs"; return false; }
            } else if (key == "spans") {
                if (!read_spans(json, i, fresh)) { errorOut = "bad spans"; return false; }
            } else if (key == "counters") {
                if (!read_counters(json, i, fresh)) { errorOut = "bad counters"; return false; }
            } else {
                errorOut = "unknown field: " + key;
                return false;
            }
        }
        if (!have_session) { errorOut = "missing session"; return false; }
        if (have_history && parsed_history != history_) {
            errorOut = "history mismatch";
            return false;
        }
        // Rejeita conteúdo após o fechamento do objeto raiz (all-or-nothing).
        if (skip_ws(json, i) && i < json.size()) { errorOut = "trailing content"; return false; }
        // Valida invariantes: sequências estritamente crescentes em logs/spans.
        std::uint64_t prev = 0;
        for (const auto& e : fresh.logs_) {
            if (e.sequence <= prev) { errorOut = "log sequence not increasing"; return false; }
            prev = e.sequence;
        }
        prev = 0;
        for (const auto& kv : fresh.spans_) {
            const auto& s = kv.second;
            if (s.begin_seq <= prev) { errorOut = "span begin not increasing"; return false; }
            prev = s.begin_seq;
        }
        // Só comita no final (all-or-nothing).
        sinks_ = std::move(fresh.sinks_);
        logs_ = std::move(fresh.logs_);
        spans_ = std::move(fresh.spans_);
        counters_ = std::move(fresh.counters_);
        sequence_ = fresh.sequence_;
        span_id_ = fresh.span_id_;
        total_logs_ = fresh.total_logs_;
        total_spans_ = fresh.total_spans_;
        enabled_ = fresh.enabled_;
        return true;
    }

    std::string serialize_state() const override {
        std::string out = "{\"session\":\"";
        out += json_escape(session_id_);
        out += "\",\"history\":" + std::to_string(history_);
        out += ",\"enabled\":" + std::string(enabled_ ? "true" : "false");
        out += ",\"sequence\":" + std::to_string(sequence_);
        out += ",\"span_id\":" + std::to_string(span_id_);
        out += ",\"total_logs\":" + std::to_string(total_logs_);
        out += ",\"total_spans\":" + std::to_string(total_spans_);
        out += ",\"logs\":[";
        bool first = true;
        for (const auto& e : logs_) {
            if (!first) out += ',';
            first = false;
            out += "{\"seq\":" + std::to_string(e.sequence);
            out += ",\"level\":\"" + std::string(level_name(e.level)) + "\"";
            out += ",\"category\":\"" + json_escape(e.category) + "\"";
            out += ",\"message\":\"" + json_escape(e.message) + "\"}";
        }
        out += "],\"spans\":[";
        first = true;
        for (const auto& kv : spans_) {
            if (!first) out += ',';
            first = false;
            const auto& s = kv.second;
            out += "{\"id\":" + std::to_string(s.span_id);
            out += ",\"parent\":" + std::to_string(s.parent_id);
            out += ",\"name\":\"" + json_escape(s.name) + "\"";
            out += ",\"begin\":" + std::to_string(s.begin_seq);
            out += ",\"end\":" + std::to_string(s.end_seq);
            out += ",\"closed\":" + std::string(s.closed ? "true" : "false") + "}";
        }
        out += "],\"counters\":{";
        first = true;
        for (const auto& kv : counters_) {
            if (!first) out += ',';
            first = false;
            out += "\"" + json_escape(kv.first) + "\":" + std::to_string(kv.second);
        }
        out += "}}";
        return out;
    }

private:
    void emit_line(const std::string& line) {
        for (const auto& kv : sinks_) {
            if (kv.second != nullptr) kv.second->emit(line);
        }
    }

    static bool read_string(const std::string& s, std::size_t& i, std::string& out) {
        if (!skip_ws(s, i) || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char e = s[i + 1];
                out += (e == 'n') ? '\n' : (e == 'r') ? '\r' : (e == 't') ? '\t' : e;
                i += 2;
            } else {
                out += s[i++];
            }
        }
        if (i >= s.size()) return false;
        ++i;
        return true;
    }

    static bool read_u64(const std::string& s, std::size_t& i, std::uint64_t& out) {
        if (!skip_ws(s, i)) return false;
        out = 0;
        bool any = false;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { out = out * 10 + (s[i] - '0'); any = true; ++i; }
        return any;
    }

    static bool read_size(const std::string& s, std::size_t& i, std::size_t& out) {
        std::uint64_t v;
        if (!read_u64(s, i, v)) return false;
        out = static_cast<std::size_t>(v);
        return true;
    }

    static bool read_bool(const std::string& s, std::size_t& i, bool& out) {
        if (!skip_ws(s, i)) return false;
        if (s.compare(i, 4, "true") == 0) { out = true; i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { out = false; i += 5; return true; }
        return false;
    }

    bool read_logs(const std::string& s, std::size_t& i, ObservabilityImpl& fresh) {
        if (!skip_ws(s, i) || s[i] != '[') return false;
        ++i;
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == ']') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            if (!skip_ws(s, i) || s[i] != '{') return false;
            ++i;
            LogEvent e;
            bool have_level = false;
            bool first_member = true;
            for (;;) {
                if (!skip_ws(s, i)) return false;
                if (s[i] == '}') { ++i; break; }
                if (!first_member) {
                    if (s[i] != ',') return false;
                    ++i;
                }
                first_member = false;
                std::string k, v;
                if (!read_key_value(s, i, k, v)) return false;
                if (k == "seq") {
                    std::uint64_t sv;
                    if (!read_u64_from(v, sv)) return false;
                    e.sequence = sv;
                } else if (k == "level") {
                    if (!level_from_name(v, e.level)) return false;
                    have_level = true;
                } else if (k == "category") {
                    e.category = v;
                } else if (k == "message") {
                    e.message = v;
                } else {
                    return false;
                }
            }
            if (!have_level) return false;
            fresh.logs_.push_back(e);
            if (fresh.logs_.size() > fresh.history_) fresh.logs_.pop_front();
        }
    }

    bool read_spans(const std::string& s, std::size_t& i, ObservabilityImpl& fresh) {
        if (!skip_ws(s, i) || s[i] != '[') return false;
        ++i;
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == ']') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            if (!skip_ws(s, i) || s[i] != '{') return false;
            ++i;
            TraceSpan sp;
            bool have_id = false, have_begin = false;
            bool first_member = true;
            for (;;) {
                if (!skip_ws(s, i)) return false;
                if (s[i] == '}') { ++i; break; }
                if (!first_member) {
                    if (s[i] != ',') return false;
                    ++i;
                }
                first_member = false;
                std::string k, v;
                if (!read_key_value(s, i, k, v)) return false;
                std::uint64_t nv;
                if (k == "id") { if (!read_u64_from(v, nv)) return false; sp.span_id = nv; have_id = true; }
                else if (k == "parent") { if (!read_u64_from(v, nv)) return false; sp.parent_id = nv; }
                else if (k == "name") { sp.name = v; }
                else if (k == "begin") { if (!read_u64_from(v, nv)) return false; sp.begin_seq = nv; have_begin = true; }
                else if (k == "end") { if (!read_u64_from(v, nv)) return false; sp.end_seq = nv; }
                else if (k == "closed") {
                    if (v == "true") sp.closed = true;
                    else if (v == "false") sp.closed = false;
                    else return false;
                } else {
                    return false;
                }
            }
            if (!have_id || !have_begin) return false;
            // Valida: id duplicado e pai conhecido.
            if (fresh.spans_.count(sp.span_id)) return false;
            if (sp.parent_id != 0 && !fresh.spans_.count(sp.parent_id)) return false;
            fresh.spans_[sp.span_id] = sp;
        }
    }

    bool read_counters(const std::string& s, std::size_t& i, ObservabilityImpl& fresh) {
        if (!skip_ws(s, i) || s[i] != '{') return false;
        ++i;
        bool first = true;
        for (;;) {
            if (!skip_ws(s, i)) return false;
            if (s[i] == '}') { ++i; return true; }
            if (!first) {
                if (s[i] != ',') return false;
                ++i;
            }
            first = false;
            std::string k, v;
            if (!read_key_value(s, i, k, v)) return false;
            // valor pode ser negativo (gauge)
            std::int64_t nv;
            if (!read_i64_from(v, nv)) return false;
            fresh.counters_[k] = nv;
        }
    }

    // Lê `"key":<valor>` no ponto atual e devolve o valor como STRING
    // (número ou string entre aspas — suficiente para os campos deste formato).
    static bool read_key_value(const std::string& s, std::size_t& i,
                               std::string& key, std::string& value) {
        if (!skip_ws(s, i) || s[i] != '"') return false;
        const std::size_t ks = ++i;
        while (i < s.size() && s[i] != '"') ++i;
        if (i >= s.size()) return false;
        key = s.substr(ks, i - ks);
        ++i;
        if (!skip_ws(s, i) || s[i] != ':') return false;
        ++i;
        if (!skip_ws(s, i)) return false;
        if (s[i] == '"') {
            return read_string(s, i, value);
        }
        const std::size_t vs = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
        value = s.substr(vs, i - vs);
        return true;
    }

    static bool read_u64_from(const std::string& v, std::uint64_t& out) {
        out = 0;
        bool any = false;
        for (const char c : v) {
            if (c < '0' || c > '9') return false;
            out = out * 10 + (c - '0');
            any = true;
        }
        return any;
    }

    static bool read_i64_from(const std::string& v, std::int64_t& out) {
        std::size_t idx = 0;
        bool neg = false;
        if (idx < v.size() && (v[idx] == '-' || v[idx] == '+')) {
            neg = (v[idx] == '-');
            ++idx;
        }
        std::uint64_t mag = 0;
        bool any = false;
        for (; idx < v.size(); ++idx) {
            const char c = v[idx];
            if (c < '0' || c > '9') return false;
            mag = mag * 10 + (c - '0');
            any = true;
        }
        if (!any) return false;
        out = neg ? -static_cast<std::int64_t>(mag) : static_cast<std::int64_t>(mag);
        return true;
    }

    std::string session_id_;
    std::size_t history_;
    std::map<std::string, ISink*> sinks_;
    std::deque<LogEvent> logs_;
    std::map<std::uint64_t, TraceSpan> spans_;
    std::map<std::string, std::int64_t> counters_;
    std::uint64_t sequence_{ 0 };
    std::uint64_t span_id_{ 0 };
    std::uint64_t total_logs_{ 0 };
    std::uint64_t total_spans_{ 0 };
    bool enabled_{ true };
};

}  // namespace

std::unique_ptr<IObservability> create_observability(const std::string& sessionId,
                                                     std::size_t history,
                                                     std::string& errorOut) {
    if (sessionId.empty()) {
        errorOut = "session id must be non-empty";
        return nullptr;
    }
    errorOut.clear();
    return std::unique_ptr<IObservability>(new ObservabilityImpl(sessionId, history));
}

}  // namespace engine::observability
