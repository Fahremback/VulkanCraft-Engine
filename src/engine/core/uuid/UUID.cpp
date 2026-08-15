#include "UUID.hpp"
#include <random>
#include <sstream>
#include <iomanip>

namespace Engine {

static std::random_device s_RandomDevice;
static std::mt19937_64 s_Engine(s_RandomDevice());
static std::uniform_int_distribution<uint64_t> s_UniformDistribution;

UUID::UUID()
    : m_high(s_UniformDistribution(s_Engine)), m_low(s_UniformDistribution(s_Engine)) {
}

UUID::UUID(uint64_t high, uint64_t low)
    : m_high(high), m_low(low) {
}

UUID::UUID(const std::string& uuidString) {
    *this = from_string(uuidString);
}

std::string UUID::to_string() const {
    std::stringstream ss;
    ss << std::hex << std::setfill('0')
       << std::setw(16) << m_high
       << std::setw(16) << m_low;
    std::string hexStr = ss.str();

    // Format as 8-4-4-4-12 UUID standard string
    return hexStr.substr(0, 8) + "-" +
           hexStr.substr(8, 4) + "-" +
           hexStr.substr(12, 4) + "-" +
           hexStr.substr(16, 4) + "-" +
           hexStr.substr(20, 12);
}

UUID UUID::from_string(const std::string& str) {
    std::string clean;
    for (char c : str) {
        if (c != '-') clean += c;
    }
    if (clean.length() != 32) return UUID(0, 0);

    try {
        uint64_t high = std::stoull(clean.substr(0, 16), nullptr, 16);
        uint64_t low = std::stoull(clean.substr(16, 16), nullptr, 16);
        return UUID(high, low);
    } catch (...) {
        return UUID(0, 0);
    }
}

} // namespace Engine
