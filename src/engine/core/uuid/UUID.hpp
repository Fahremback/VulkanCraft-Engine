#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <random>

namespace Engine {

class UUID {
public:
    UUID();
    UUID(uint64_t high, uint64_t low);
    UUID(const std::string& uuidString);

    uint64_t get_high() const { return m_high; }
    uint64_t get_low() const { return m_low; }

    std::string to_string() const;
    static UUID from_string(const std::string& str);

    bool operator==(const UUID& other) const {
        return m_high == other.m_high && m_low == other.m_low;
    }

    bool operator!=(const UUID& other) const {
        return !(*this == other);
    }

    bool operator<(const UUID& other) const {
        if (m_high != other.m_high) return m_high < other.m_high;
        return m_low < other.m_low;
    }

    bool is_valid() const { return m_high != 0 || m_low != 0; }

private:
    uint64_t m_high{ 0 };
    uint64_t m_low{ 0 };
};

} // namespace Engine

namespace std {
    template<>
    struct hash<Engine::UUID> {
        std::size_t operator()(const Engine::UUID& uuid) const noexcept {
            return std::hash<uint64_t>{}(uuid.get_high()) ^ (std::hash<uint64_t>{}(uuid.get_low()) << 1);
        }
    };
}
