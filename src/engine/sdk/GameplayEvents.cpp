// GameplayEvents.cpp — adapter do contrato IGameplayEvents.
// Fila FIFO determinística: publish no fim; com capacity finita e fila
// cheia, descarta o MAIS ANTIGO (drop). drain() esvazia na ordem exata de
// publicação. Sem RNG, sem estado global.

#include "engine/gameplay/IGameplayEvents.hpp"

#include <deque>

namespace engine::gameplay {

class GameplayEventsImpl final : public IGameplayEvents {
public:
    explicit GameplayEventsImpl(std::size_t capacity) : capacity_(capacity) {}

    void publish(std::uint16_t kind, std::uint64_t tick,
                 const std::vector<std::uint8_t>& payload) override {
        GameplayEvent event;
        event.kind = kind;
        event.tick = tick;
        event.payload = payload;
        if (capacity_ > 0 && queue_.size() >= capacity_) {
            queue_.pop_front();  // descarta o mais antigo
            ++dropped_;
        }
        queue_.push_back(std::move(event));
    }

    std::vector<GameplayEvent> drain(std::size_t maxCount) override {
        std::vector<GameplayEvent> out;
        const std::size_t take = (maxCount == 0) ? queue_.size()
                                                 : (maxCount < queue_.size() ? maxCount
                                                                             : queue_.size());
        out.reserve(take);
        for (std::size_t n = 0; n < take; ++n) {
            out.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return out;
    }

    std::size_t pending_count() const override { return queue_.size(); }
    std::size_t dropped_count() const override { return dropped_; }
    void reset() override {
        queue_.clear();
        dropped_ = 0;
    }

private:
    std::size_t capacity_{ 0 };
    std::size_t dropped_{ 0 };
    std::deque<GameplayEvent> queue_;
};

std::unique_ptr<IGameplayEvents> create_gameplay_events(std::size_t capacity) {
    return std::make_unique<GameplayEventsImpl>(capacity);
}

}  // namespace engine::gameplay
