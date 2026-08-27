#include "engine/networking/ITransport.hpp"

#include <algorithm>
#include <deque>

namespace engine::networking {
namespace {
class LoopbackTransport final : public ITransport {
public:
    explicit LoopbackTransport(TransportConfig config) : config_(std::move(config)) {}

    bool start(const TransportConfig& config, std::string& error) override {
        if (config.max_message_bytes == 0 || config.poll_budget == 0) {
            error = "invalid_transport_config";
            return false;
        }
        config_ = config;
        state_ = TransportState::Listening;
        queue_.clear();
        sent_ = received_ = next_ = 0;
        return true;
    }

    bool connect(const TransportEndpoint& endpoint, std::string& error) override {
        if (state_ != TransportState::Listening && state_ != TransportState::Connected) {
            error = "transport_not_started";
            return false;
        }
        if (endpoint.port == 0) {
            error = "invalid_endpoint";
            return false;
        }
        endpoint_ = endpoint;
        state_ = TransportState::Connected;
        return true;
    }

    bool send(const std::vector<std::uint8_t>& payload, std::string& error) override {
        if (state_ != TransportState::Connected) { error = "transport_not_connected"; return false; }
        if (payload.empty() || payload.size() > config_.max_message_bytes) {
            error = "invalid_payload";
            return false;
        }
        queue_.push_back(TransportMessage{next_++, payload});
        ++sent_;
        return true;
    }

    std::vector<TransportMessage> poll(std::string&) override {
        std::vector<TransportMessage> result;
        const auto limit = std::min<std::size_t>(config_.poll_budget, queue_.size());
        result.reserve(limit);
        for (std::size_t i = 0; i < limit; ++i) {
            result.push_back(std::move(queue_.front()));
            queue_.pop_front();
            ++received_;
        }
        return result;
    }

    bool cancel(std::string&) override { queue_.clear(); return true; }
    void stop() noexcept override { queue_.clear(); state_ = TransportState::Stopped; }
    TransportState state() const noexcept override { return state_; }
    std::uint64_t sent_count() const noexcept override { return sent_; }
    std::uint64_t received_count() const noexcept override { return received_; }

private:
    TransportConfig config_;
    TransportEndpoint endpoint_;
    TransportState state_{TransportState::Stopped};
    std::deque<TransportMessage> queue_;
    std::uint64_t next_{0}, sent_{0}, received_{0};
};
}

std::unique_ptr<ITransport> create_transport(const TransportConfig& config, std::string& error) {
    if (config.kind != TransportKind::Loopback) {
        error = "transport_backend_unavailable";
        return {};
    }
    if (config.max_message_bytes == 0 || config.poll_budget == 0) {
        error = "invalid_transport_config";
        return {};
    }
    return std::make_unique<LoopbackTransport>(config);
}
}
