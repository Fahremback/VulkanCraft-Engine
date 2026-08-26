// Replay.cpp — adapter do contrato IReplay (engine::gameplay).
// Implementação determinística: frames em vetor (ordem de gravação),
// cursor de reprodução, trim do fim, JSON bit-exact all-or-nothing.

#include "engine/gameplay/IReplay.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace engine::gameplay {

namespace {

// Escapa uma string para JSON (mesma convenção dos demais adapters do sdk).
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

bool json_field(const std::string& s, std::size_t& i, std::string& key) {
    // lê "chave":
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    key.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) { key += s[i + 1]; i += 2; }
        else { key += s[i]; ++i; }
    }
    if (i >= s.size()) return false;
    ++i;  // fecha aspas
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size() || s[i] != ':') return false;
    ++i;
    return true;
}

bool json_number(const std::string& s, std::size_t& i, double& out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    const std::size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; any = true; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { ++i; any = true; }
    }
    if (!any) return false;
    out = std::strtod(s.substr(start, i - start).c_str(), nullptr);
    return true;
}

bool json_string(const std::string& s, std::size_t& i, std::string& out) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\' && i + 1 < s.size()) { out += s[i + 1]; i += 2; }
        else { out += s[i]; ++i; }
    }
    if (i >= s.size()) return false;
    ++i;
    return true;
}

bool json_array_start(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    return true;
}

bool json_array_end(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size() || s[i] != ']') return false;
    ++i;
    return true;
}

bool json_comma(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    if (i >= s.size() || s[i] != ',') return false;
    ++i;
    return true;
}

// inputs JSON: array de números 0..255. Retorna false em qualquer forma
// inválida (não-número, fora de faixa, decimal).
bool json_bytes(const std::string& s, std::size_t& i, std::vector<std::uint8_t>& out) {
    out.clear();
    if (!json_array_start(s, i)) return false;
    while (true) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        double value = 0.0;
        if (!json_number(s, i, value)) return false;
        if (value < 0.0 || value > 255.0 || std::floor(value) != value) return false;
        out.push_back(static_cast<std::uint8_t>(value));
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        return false;
    }
}

std::string bytes_json(const std::vector<std::uint8_t>& bytes) {
    std::string out = "[";
    for (std::size_t n = 0; n < bytes.size(); ++n) {
        if (n) out += ",";
        out += std::to_string(static_cast<unsigned>(bytes[n]));
    }
    out += "]";
    return out;
}

}  // namespace

class ReplayImpl final : public IReplay {
public:
    explicit ReplayImpl(std::size_t maxFrames) : maxFrames_(maxFrames) {}

    bool record_tick(std::uint64_t tick, std::uint32_t seed,
                     const std::vector<std::uint8_t>& inputs,
                     std::string& errorOut) override {
        if (!frames_.empty() && tick <= frames_.back().tick) {
            errorOut = "replay: tick fora de ordem (tick " + std::to_string(tick) +
                       " <= último " + std::to_string(frames_.back().tick) + ")";
            return false;
        }
        if (maxFrames_ > 0 && frames_.size() >= maxFrames_) {
            errorOut = "replay: limite de frames atingido (" + std::to_string(maxFrames_) + ")";
            return false;
        }
        ReplayFrame frame;
        frame.tick = tick;
        frame.seed = seed;
        frame.inputs = inputs;
        frames_.push_back(std::move(frame));
        return true;
    }

    std::size_t frame_count() const override { return frames_.size(); }
    std::uint64_t first_tick() const override {
        return frames_.empty() ? 0 : frames_.front().tick;
    }
    std::uint64_t last_tick() const override {
        return frames_.empty() ? 0 : frames_.back().tick;
    }

    bool begin_replay(std::string& errorOut) override {
        (void)errorOut;
        cursor_ = 0;
        return true;
    }

    bool next_frame(ReplayFrame& out) override {
        if (cursor_ >= frames_.size()) return false;
        out = frames_[cursor_++];
        return true;
    }

    bool seek_tick(std::uint64_t tick) override {
        auto found = std::lower_bound(
            frames_.begin(), frames_.end(), tick,
            [](const ReplayFrame& frame, std::uint64_t value) { return frame.tick < value; });
        if (found == frames_.end()) return false;
        cursor_ = static_cast<std::size_t>(found - frames_.begin());
        return true;
    }

    bool truncate_after(std::uint64_t tick, std::string& errorOut) override {
        (void)errorOut;
        auto first = std::upper_bound(
            frames_.begin(), frames_.end(), tick,
            [](std::uint64_t value, const ReplayFrame& frame) { return value < frame.tick; });
        frames_.erase(first, frames_.end());
        if (cursor_ > frames_.size()) cursor_ = frames_.size();
        return true;
    }

    bool load_from_json(const std::string& json, std::string& errorOut) override {
        std::vector<ReplayFrame> parsed;
        std::size_t i = 0;
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
        if (i >= json.size() || json[i] != '{') { errorOut = "replay: documento não é objeto"; return false; }
        ++i;

        bool sawFrames = false;
        std::string key;
        while (true) {
            if (!json_field(json, i, key)) {
                // fim do objeto?
                while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
                if (i < json.size() && json[i] == '}') { ++i; break; }
                errorOut = "replay: campo inválido";
                return false;
            }
            if (key == "frames") {
                sawFrames = true;
                if (!json_array_start(json, i)) { errorOut = "replay: frames não é array"; return false; }
                while (true) {
                    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
                    if (i < json.size() && json[i] == ']') { ++i; break; }
                    ReplayFrame frame;
                    if (!parse_frame(json, i, frame, errorOut)) return false;
                    if (!parsed.empty() && frame.tick <= parsed.back().tick) {
                        errorOut = "replay: frame fora de ordem no documento";
                        return false;
                    }
                    parsed.push_back(std::move(frame));
                    while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
                    if (i < json.size() && json[i] == ',') { ++i; continue; }
                    if (i < json.size() && json[i] == ']') { ++i; break; }
                    errorOut = "replay: separador inválido em frames";
                    return false;
                }
            } else {
                errorOut = "replay: campo desconhecido \"" + key + "\"";
                return false;
            }
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
            if (i < json.size() && json[i] == '}') { ++i; break; }
            if (!json_comma(json, i)) { errorOut = "replay: separador inválido"; return false; }
        }
        if (!sawFrames) { errorOut = "replay: campo frames ausente"; return false; }
        if (maxFrames_ > 0 && parsed.size() > maxFrames_) {
            errorOut = "replay: documento excede o limite de frames";
            return false;
        }
        frames_ = std::move(parsed);
        cursor_ = 0;
        return true;
    }

    std::string serialize_state() const override {
        std::ostringstream out;
        out << "{\"frames\":[";
        for (std::size_t n = 0; n < frames_.size(); ++n) {
            if (n) out << ",";
            const ReplayFrame& frame = frames_[n];
            out << "{\"tick\":" << frame.tick
                << ",\"seed\":" << frame.seed
                << ",\"inputs\":" << bytes_json(frame.inputs) << "}";
        }
        out << "]}";
        return out.str();
    }

private:
    bool parse_frame(const std::string& s, std::size_t& i, ReplayFrame& out, std::string& errorOut) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
        if (i >= s.size() || s[i] != '{') { errorOut = "replay: frame não é objeto"; return false; }
        ++i;
        bool sawTick = false, sawSeed = false, sawInputs = false;
        std::string key;
        while (true) {
            if (!json_field(s, i, key)) {
                while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
                if (i < s.size() && s[i] == '}') { ++i; break; }
                errorOut = "replay: campo de frame inválido";
                return false;
            }
            if (key == "tick") {
                double value = 0.0;
                if (!json_number(s, i, value) || value < 0.0 || std::floor(value) != value) {
                    errorOut = "replay: tick inválido";
                    return false;
                }
                out.tick = static_cast<std::uint64_t>(value);
                sawTick = true;
            } else if (key == "seed") {
                double value = 0.0;
                if (!json_number(s, i, value) || value < 0.0 || std::floor(value) != value) {
                    errorOut = "replay: seed inválido";
                    return false;
                }
                out.seed = static_cast<std::uint32_t>(value);
                sawSeed = true;
            } else if (key == "inputs") {
                if (!json_bytes(s, i, out.inputs)) {
                    errorOut = "replay: inputs inválidos";
                    return false;
                }
                sawInputs = true;
            } else {
                errorOut = "replay: campo de frame desconhecido \"" + key + "\"";
                return false;
            }
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
            if (i < s.size() && s[i] == '}') { ++i; break; }
            if (!json_comma(s, i)) { errorOut = "replay: separador de frame inválido"; return false; }
        }
        if (!sawTick || !sawSeed || !sawInputs) {
            errorOut = "replay: frame incompleto (tick/seed/inputs obrigatórios)";
            return false;
        }
        return true;
    }

    std::vector<ReplayFrame> frames_;
    std::size_t cursor_{ 0 };
    std::size_t maxFrames_{ 0 };
};

std::unique_ptr<IReplay> create_replay(std::size_t maxFrames) {
    return std::make_unique<ReplayImpl>(maxFrames);
}

}  // namespace engine::gameplay
