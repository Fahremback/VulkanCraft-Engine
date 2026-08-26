// PublishPipeline — adapter do contrato engine/editor IPublishPipeline.
//
// Máquina de estágios determinística do build do editor (cook → package →
// publish). O build_game() do editor dirige este contrato a cada etapa; as
// transições inválidas são recusadas sem mutar. Sem RNG/relógio/estado global.

#include "engine/editor/IPublishPipeline.hpp"

#include <sstream>

namespace engine::editor {

namespace {

const char* stage_name(PublishStage s) {
    switch (s) {
        case PublishStage::Idle: return "idle";
        case PublishStage::Cooking: return "cooking";
        case PublishStage::Packaging: return "packaging";
        case PublishStage::Publishing: return "publishing";
        case PublishStage::Done: return "done";
        case PublishStage::Failed: return "failed";
    }
    return "idle";
}

class PublishPipelineImpl : public IPublishPipeline {
public:
    PublishState state() const override { return m_state; }

    bool begin(const std::string& project) override {
        if (m_state.stage != PublishStage::Idle &&
            m_state.stage != PublishStage::Done &&
            m_state.stage != PublishStage::Failed) {
            return false;  // já em andamento
        }
        m_state = PublishState{};
        m_state.stage = PublishStage::Cooking;
        m_state.project = project;
        return true;
    }

    bool cooking_done(size_t imported, size_t failed) override {
        if (m_state.stage != PublishStage::Cooking) return false;
        m_state.imported = imported;
        m_state.failed = failed;
        m_state.stage = PublishStage::Packaging;
        return true;
    }

    bool packaging_done(size_t packaged) override {
        if (m_state.stage != PublishStage::Packaging) return false;
        m_state.packaged = packaged;
        m_state.stage = PublishStage::Publishing;
        return true;
    }

    bool publishing_done() override {
        if (m_state.stage != PublishStage::Publishing) return false;
        m_state.stage = PublishStage::Done;
        m_state.last_error.clear();
        return true;
    }

    bool fail(const std::string& error) override {
        if (m_state.stage == PublishStage::Done || m_state.stage == PublishStage::Idle) {
            return false;  // não há build para falhar
        }
        m_state.stage = PublishStage::Failed;
        m_state.last_error = error;
        return true;
    }

    void reset() override {
        m_state = PublishState{};
    }

    std::string to_json() const override {
        std::ostringstream out;
        out << "{\"stage\":\"" << stage_name(m_state.stage)
            << "\",\"imported\":" << m_state.imported
            << ",\"failed\":" << m_state.failed
            << ",\"packaged\":" << m_state.packaged
            << ",\"project\":\"" << m_state.project
            << "\",\"error\":\"" << m_state.last_error << "\"}";
        return out.str();
    }

private:
    PublishState m_state;
};

}  // namespace

std::unique_ptr<IPublishPipeline> create_publish_pipeline() {
    return std::make_unique<PublishPipelineImpl>();
}

}  // namespace engine::editor
