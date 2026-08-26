#pragma once
// IAssetPipeline — pipeline público de assets (import→validate→cook→cache→
// package) para o domínio `engine/assets/` (§6 item 1 — "pipeline público
// import→validate→cook→cache→package com operações incrementais e
// determinísticas").
//
// Núcleo headless do pipeline de assets: fontes são importadas com nome +
// kind + versão + bytes; a validação e o cook são determinísticos POR KIND
// (kinds embutidos: `raw` = passthrough, `json` = valida parse + re-emite
// JSON canônico compacto com chaves ordenadas, `text` = valida UTF-8 e
// normaliza EOL); o cache é content-addressado (hash FNV-1a da fonte +
// versão) — re-cook da MESMA fonte+versão é um CACHE HIT sem recomputar;
// package produz um manifesto JSON bit-exact ordenado por nome. O contrato
// NÃO conhece o disco nem formatos externos: bytes entram e saem por valor.
// Mesmo espírito dos contratos de networking: dados opacos, ordem
// determinística, persistência JSON bit-exact e all-or-nothing.
//
// Self-contained (std only), headless, determinístico.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::assets {

// Uma fonte de asset importada no pipeline.
struct AssetSource {
    std::string name;                 // chave única (nome do asset)
    std::string kind;                 // kind do cooker/validator ("raw"/"json"/"text")
    std::string version;              // versão da fonte (opaca, do chamador)
    std::vector<std::uint8_t> bytes;  // conteúdo bruto da fonte
};

// Resultado da validação de uma fonte.
struct AssetValidation {
    bool valid{ false };
    std::string error;  // motivo da recusa (vazio quando ok)
};

// Resultado do cook de uma fonte.
struct AssetCookResult {
    bool ok{ false };
    std::string error;
    std::vector<std::uint8_t> artifact;  // bytes cozidos (determinísticos)
    std::uint64_t artifact_hash{ 0 };    // hash FNV-1a do artefato
    bool cache_hit{ false };             // true = servido do cache sem recomputar
};

// Estado observável de um asset no pipeline.
struct AssetState {
    std::string name;
    std::string kind;
    std::string version;
    std::uint64_t source_hash{ 0 };    // hash FNV-1a da fonte importada
    bool validated{ false };           // última validação passou
    bool cooked{ false };              // já houve cook (artefato disponível)
    std::uint64_t artifact_hash{ 0 };  // hash do último artefato cozido
};

// Manifesto de package (determinístico: ordenado por nome).
struct AssetManifest {
    std::vector<AssetState> assets;  // assets empacotáveis, em ordem de nome
};

class IAssetPipeline {
public:
    virtual ~IAssetPipeline() = default;

    // Importa/atualiza uma fonte. Nome vazio, kind desconhecido ou bytes
    // inválidos para o kind → false com erro e NADA muda (all-or-nothing).
    // Re-importar a mesma fonte (mesmo nome) SUBSTITUI; fonte/versão iguais
    // aos já presentes não recomputa (cache preservado); fonte ou versão
    // DIFERENTES invalidam o artefato anterior (cooked=false até re-cook).
    virtual bool import_source(const AssetSource& source, std::string& errorOut) = 0;

    // Valida a fonte atual de um asset. Ausente → false com erro nomeado.
    virtual AssetValidation validate(const std::string& name) const = 0;

    // Cozinha a fonte atual (validator do kind roda ANTES; inválida → false
    // sem mutar). Mesma (fonte, versão) já cozida → cache_hit=true e o
    // artefato vem do cache (não recomputa). Ausente → false com erro.
    virtual AssetCookResult cook(const std::string& name) = 0;

    // Remove um asset (fonte + artefato). Ausente = no-op.
    virtual void remove(const std::string& name) = 0;

    // Estados de todos os assets, em ordem crescente de nome.
    virtual std::vector<AssetState> states() const = 0;

    // Manifesto de package: assets com artefato disponível, ordenados por
    // nome. Nunca falha (estado derivado).
    virtual AssetManifest package() const = 0;

    // Contagem acumulada de cache hits (observabilidade).
    virtual std::size_t cache_hits() const = 0;

    // Descarta tudo. Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria um pipeline vazio. `seed` deve ser não-vazio (all-or-nothing) — chave
// de particionamento do cache (múltiplos pipelines não compartilham estado).
std::unique_ptr<IAssetPipeline> create_asset_pipeline(const std::string& seed,
                                                      std::string& errorOut);

}  // namespace engine::assets
