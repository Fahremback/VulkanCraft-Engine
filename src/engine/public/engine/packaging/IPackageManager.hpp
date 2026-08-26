#pragma once
// IPackageManager — packages/mods assináveis, manifestos, resolução de
// dependências e atualizações seguras, para o domínio `engine/packaging/`
// (§6 item 5 — "Implementar packages/mods assináveis, manifestos, resolução
// de dependências e atualizações seguras").
//
// Núcleo headless do gerenciador de packages: manifestos são registrados com
// nome + versão + dependências (restrições de versão opacas: `*`, `==X`,
// `>=X`) + hash de conteúdo; a RESOLUÇÃO de dependências é determinística
// (ordem topológica, ciclo detectado e recusado, restrição violada recusada,
// versão ausente recusada); a VERIFICAÇÃO de assinatura é plugável via
// `ISignatureVerifier` (o contrato NÃO conhece criptografia — o chamador
// pluga libsodium/ed25519/etc.); a INSTALAÇÃO é um gate all-or-nothing
// (todas as dependências presentes E assinatura válida → comita; senão nada
// muda) — é isso que torna as atualizações seguras. Mesmo espírito dos
// contratos de networking/assets/observability: dados opacos, ordem
// determinística, persistência JSON bit-exact e all-or-nothing.
//
// Self-contained (std only), headless, determinístico.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace engine::packaging {

// Uma dependência de um package.
struct PackageDependency {
    std::string name;        // nome do package dependido
    std::string constraint;  // restrição de versão opaca: "*", "==1.2.0", ">=2.0"
};

// Manifesto de um package registrado.
struct PackageManifest {
    std::string name;                 // chave única
    std::string version;              // versão opaca: "1.2.0"
    std::vector<PackageDependency> dependencies;  // ordem declarada preservada
    std::string content_hash;         // hash do conteúdo (opaco, do chamador)
};

// Estado observável de um package no gerenciador.
struct PackageState {
    std::string name;
    std::string version;
    std::vector<PackageDependency> dependencies;
    std::string content_hash;
    bool installed{ false };          // instalado (gate de instalação comitou)
    bool signature_valid{ false };    // última verificação passou
    std::uint64_t install_seq{ 0 };   // ordem de instalação (0 = não instalado)
};

// Resultado da resolução de dependências (ordem topológica determinística).
struct ResolutionResult {
    bool ok{ false };
    std::string error;                 // motivo da recusa (vazio quando ok)
    std::vector<std::string> order;    // nomes em ordem topológica (deps primeiro)
};

// Verificador de assinatura plugável. `packageName` + `contentHash` são as
// credenciais do manifesto; `signature` é opaca (formato do chamador). O
// contrato chama `verify` no gate de instalação e na verificação manual.
struct ISignatureVerifier {
    virtual ~ISignatureVerifier() = default;
    virtual bool verify(const std::string& packageName,
                        const std::string& contentHash,
                        const std::string& signature) = 0;
};

class IPackageManager {
public:
    virtual ~IPackageManager() = default;

    // Identificador fixo da sessão de packages.
    virtual const std::string& session_id() const = 0;

    // Registra/atualiza um manifesto. Nome/versão/hash vazios → false e NADA
    // muda (all-or-nothing). Atualização sobrescreve sem duplicar; um
    // package INSTALADO não pode ser re-registrado com conteúdo diferente
    // (hash mudou → false; exige uninstall primeiro).
    virtual bool register_manifest(const PackageManifest& manifest,
                                   std::string& errorOut) = 0;

    // Define/substitui o verificador de assinatura plugável. `nullptr`
    // desliga a verificação (assinatura é aceita sem checar — NÃO recomendado
    // para produção; o contrato avisa via `signature_valid=false` quando não
    // há verificador).
    virtual void set_verifier(ISignatureVerifier* verifier) = 0;

    // Verifica manualmente a assinatura de um package (não instala).
    // Ausente → false com erro nomeado. Sem verificador → false com erro
    // (verificação exige verificador — opt-in por design).
    virtual bool verify_signature(const std::string& name,
                                  const std::string& signature,
                                  std::string& errorOut) = 0;

    // Resolve as dependências de um package (fechamento transitivo) em ordem
    // topológica determinística: dependências antes de dependentes, ordem de
    // declaração como desempate, nomes iguais por restrição satisfeita.
    // Ausente/ciclo/restrição violada/versão ausente → ok=false com erro.
    virtual ResolutionResult resolve(const std::string& name) const = 0;

    // Gate de instalação all-or-nothing: resolve() ok E assinatura válida
    // (verificador plugado E verify=true; sem verificador → instalação
    // recusada — assinatura é obrigatória) → instala o package E todas as
    // dependências ainda não instaladas (em ordem topológica); senão NADA
    // muda. Reinstalar o mesmo package é no-op (já instalado).
    virtual bool install(const std::string& name, const std::string& signature,
                         std::string& errorOut) = 0;

    // Desinstala um package. Dependentes instalados → recusado (all-or-
    // nothing: não quebra o grafo). Ausente = no-op.
    virtual bool uninstall(const std::string& name, std::string& errorOut) = 0;

    // Estados de todos os packages, em ordem crescente de nome.
    virtual std::vector<PackageState> states() const = 0;

    // Packages instalados em ordem de instalação (install_seq crescente).
    virtual std::vector<PackageState> installed() const = 0;

    // Descarta tudo (nova sessão). Sempre ok.
    virtual bool reset(std::string& errorOut) = 0;

    // --- Persistência (bit-exact, all-or-nothing) ---
    virtual bool load_from_json(const std::string& json, std::string& errorOut) = 0;
    virtual std::string serialize_state() const = 0;
};

// Cria um gerenciador de packages. `sessionId` deve ser não-vazio
// (all-or-nothing).
std::unique_ptr<IPackageManager> create_package_manager(const std::string& sessionId,
                                                        std::string& errorOut);

}  // namespace engine::packaging
