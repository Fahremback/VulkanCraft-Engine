#pragma once
// IReplicationSecurity — segurança e resiliência da pilha (seção H).
// Determinístico, transport-free: valida payload por schema-limite ANTES de
// alocar/aplicar efeito, aplica proteção contra spam/amplificação, e mantém um
// journal autoritativo de transações relevantes para recovery/replay.
// O replay é determinístico: as mesmas entradas na mesma ordem produzem a mesma
// sequência de chamadas — jamais reexecuta efeitos não-determinísticos.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace engine {
namespace networking {

enum class FieldKind : std::uint8_t { U8, U16, U32, I32, F32, FixedBytes };

// Regra de campo de um schema: ocupa `size` bytes na posição especificada e os
// valores validados respeitam range (min/max válidos apenas para numéricos; para
// FixedBytes, `min_value == 0` é ignorado). `range_ok` valida antes de aplicar.
struct SchemaFieldRule {
    std::string name;
    FieldKind kind{ FieldKind::U8 };
    std::uint16_t size{ 0 };       // bytes ocupados (FixedBytes => fixed len)
    std::int64_t min_value{ 0 };   // num: menor valor aceito (inclusive)
    std::int64_t max_value{ 0 };   // num: maior valor aceito (inclusive)
    std::int64_t range_ok{ 0 };    // 0 => sem verificação de faixa nesse campo
};

// Schema de uma mensagem replicada/RPC/edital: verifica tamanho máximo e as
// faixas/enums/limites especificados ANTES de o chamador alocar o efeito.
struct PayloadSchema {
    std::string name;
    std::size_t max_size{ 1 << 20 };
    std::vector<SchemaFieldRule> fields;
};

// Limites anti-spam/amplificação por conexão (por janela de tempo).
struct SecurityLimits {
    std::size_t max_messages_per_window{ 60 };
    std::uint64_t window_millis{ 1000 };
    std::size_t max_payload{ 1 << 20 };
    bool amplification_guard{ true };  // bloqueia respostas muito maiores que request
    std::size_t max_response_ratio{ 8 };  // resp<=ratio*request
    // Teto do journal autoritativo (janela de recovery/replay): quando
    // atingido, a entrada mais antiga é descartada — memória limitada em
    // servidor de longa duração. Sequências continuam estritamente crescentes
    // (sem reuso); o replay cobre apenas as entradas retidas.
    std::size_t journal_max_entries{ 65536 };
};

// Entrada do journal autoritativo (período recovery/replay).
struct JournalEntry {
    std::uint64_t tick{ 0 };
    std::uint64_t sequence{ 0 };   // global estritamente crescente
    std::string kind;              // "entity_spawn", "block_edit", "inventory", ...
    std::vector<std::uint8_t> data;
    std::uint32_t crc{ 0 };        // checksum armazenado p/ detecção de corrupção
};

using ReplayConsumer = std::function<void(const JournalEntry&)>;

class IReplicationSecurity {
public:
    virtual ~IReplicationSecurity() = default;

    // ---- schema validation (before allocation) ----
    virtual bool register_schema(const PayloadSchema& schema, std::string& errorOut) = 0;
    virtual bool validate(const std::string& schemaName, const std::uint8_t* data,
                          std::size_t size, std::string& errorOut) const = 0;

    // ---- anti-spam / amplification (per connection, per window) ----
    // Inicia uma nova janela (frequentemente por tick/carro). Só pode ser
    // chamado quando o relógio avançou para uma nova janela.
    virtual bool advance_window(std::uint64_t now_ms) = 0;
    // Observa uma mensagem recebida; false => o servidor deve DROPAR
    // (fora do budget de spam ou payload acima do limite).
    virtual bool observe_incoming(std::uint64_t connection_id, std::size_t bytes) = 0;
    // Resposta amplificada: false => o servidor RECUSA responder (domeştigar).
    virtual bool amplification_ok(std::uint64_t connection_id, std::size_t req_bytes,
                                  std::size_t resp_bytes) const = 0;

    // ---- authoritative journal (recovery/replay) ----
    virtual bool journal_record(const std::string& kind, const std::uint8_t* data,
                                std::size_t size, std::uint64_t tick,
                                std::uint64_t& out_sequence, std::string& errorOut) = 0;
    virtual std::vector<JournalEntry> journal_since(std::uint64_t after_sequence) const = 0;
    // Replay determinístico: emite entradas em ordem de sequence para o
    // consumidor. `after_sequence` permite retomar a partir de um checkpoint.
    virtual std::size_t replay(std::uint64_t after_sequence,
                               ReplayConsumer consumer) const = 0;
    virtual std::size_t journal_size() const = 0;
    virtual std::uint64_t last_journal_sequence() const = 0;

    // Métricas.
    virtual std::size_t dropped_spam() const noexcept = 0;
    virtual std::size_t dropped_amplification() const noexcept = 0;

    virtual bool reset(std::string& errorOut) = 0;
};

std::unique_ptr<IReplicationSecurity> create_replication_security(
    const SecurityLimits& limits, std::string& errorOut);

}  // namespace networking
}  // namespace engine