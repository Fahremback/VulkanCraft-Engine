// PackageManagerTests — gate do contrato IPackageManager
// (engine::packaging, §6 item 5 — "Implementar packages/mods assináveis,
// manifestos, resolução de dependências e atualizações seguras").
// Prova: criação all-or-nothing (session vazia), registro com campos vazios
// recusado sem mutar, manifest de package instalado com conteúdo alterado
// recusado, resolução topológica determinística (deps antes do dependente,
// ordem de declaração, restrição violada/versão ausente/ciclo recusados),
// verificação de assinatura plugável (sem verificador recusada), gate de
// instalação all-or-nothing (sem verificador/signature inválida → nada muda;
// válida → instala root + deps não-instaladas em ordem topológica; reinstalar
// é no-op), uninstall com dependente instalado recusado, JSON round-trip
// bit-exact e rejeições all-or-nothing com estado intacto.

#include "engine/packaging/IPackageManager.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

engine::packaging::PackageManifest pkg(const char* name, const char* version,
                                       const char* hash,
                                       std::initializer_list<const char*> deps) {
    engine::packaging::PackageManifest m;
    m.name = name;
    m.version = version;
    m.content_hash = hash;
    // deps alternam name/constraint: {"a", "==1.0", "b", "*"}
    const std::vector<const char*> raw(deps);
    for (std::size_t i = 0; i + 1 < raw.size(); i += 2) {
        engine::packaging::PackageDependency d;
        d.name = raw[i];
        d.constraint = raw[i + 1];
        m.dependencies.push_back(d);
    }
    return m;
}

// Verificador fake: aceita assinatura "ok:<name>".
struct FakeVerifier final : engine::packaging::ISignatureVerifier {
    bool verify(const std::string& name, const std::string&, const std::string& sig) override {
        return sig == "ok:" + name;
    }
};

void test_creation() {
    std::printf("[creation]\n");
    std::string err;
    auto pm = engine::packaging::create_package_manager("pm-1", err);
    check(pm != nullptr, "manager criado com id válido");
    check(pm->session_id() == "pm-1", "session_id preservado");
    check(pm->states().empty(), "nasce vazio");

    auto empty = engine::packaging::create_package_manager("", err);
    check(empty == nullptr, "session vazia recusada (all-or-nothing)");
    check(!err.empty(), "erro nomeado na session vazia");
}

void test_register() {
    std::printf("[register]\n");
    std::string err;
    auto pm = engine::packaging::create_package_manager("pm-2", err);

    check(!pm->register_manifest(pkg("", "1.0", "h", {}), err), "nome vazio recusado");
    check(!pm->register_manifest(pkg("a", "", "h", {}), err), "versão vazia recusada");
    check(!pm->register_manifest(pkg("a", "1.0", "", {}), err), "hash vazio recusado");
    check(!pm->register_manifest(pkg("a", "1.0", "h", {"", "*"}), err), "dep sem nome recusado");
    check(!pm->register_manifest(pkg("a", "1.0", "h", {"b", ""}), err), "dep sem constraint recusado");
    check(pm->states().empty(), "nada mutou após recusas");

    check(pm->register_manifest(pkg("a", "1.0", "h1", {}), err), "package a registrado");
    check(pm->register_manifest(pkg("a", "1.0", "h1", {}), err), "re-registro idêntico ok");
    check(pm->states().size() == 1, "sem duplicata");
}

void test_resolve() {
    std::printf("[resolve]\n");
    std::string err;
    auto pm = engine::packaging::create_package_manager("pm-3", err);

    // a -> b, c ; b -> d ; c -> d
    pm->register_manifest(pkg("a", "1.0", "h", {"b", "*", "c", "*"}), err);
    pm->register_manifest(pkg("b", "2.0", "h", {"d", "==1.5"}), err);
    pm->register_manifest(pkg("c", "1.0", "h", {"d", "*"}), err);
    pm->register_manifest(pkg("d", "1.5", "h", {}), err);

    auto r = pm->resolve("a");
    check(r.ok, "resolução de a ok");
    check(r.order.size() == 4, "fechamento transitivo de 4");
    // d antes de b e c; b/c antes de a.
    std::size_t idx_d = 99, idx_b = 99, idx_c = 99, idx_a = 99;
    for (std::size_t i = 0; i < r.order.size(); ++i) {
        if (r.order[i] == "d") idx_d = i;
        if (r.order[i] == "b") idx_b = i;
        if (r.order[i] == "c") idx_c = i;
        if (r.order[i] == "a") idx_a = i;
    }
    check(idx_d < idx_b && idx_d < idx_c, "d antes de b e c");
    check(idx_b < idx_a && idx_c < idx_a, "b e c antes de a");
    check(r.order[0] == "d", "d é a raiz da ordem");

    // Restrição violada.
    pm->register_manifest(pkg("e", "1.0", "h", {"b", "==9.9"}), err);
    auto rv = pm->resolve("e");
    check(!rv.ok && rv.error.find("constraint") != std::string::npos,
          "restrição violada recusada com erro nomeado");

    // Versão ausente.
    pm->register_manifest(pkg("f", "1.0", "h", {"ghost", "*"}), err);
    auto rm = pm->resolve("f");
    check(!rm.ok && rm.error.find("missing") != std::string::npos,
          "dependência ausente recusada");

    // Ciclo.
    pm->register_manifest(pkg("x", "1.0", "h", {"y", "*"}), err);
    pm->register_manifest(pkg("y", "1.0", "h", {"x", "*"}), err);
    auto rc = pm->resolve("x");
    check(!rc.ok && rc.error.find("cycle") != std::string::npos, "ciclo detectado");

    // Desconhecido.
    auto rn = pm->resolve("nope");
    check(!rn.ok && rn.error.find("unknown") != std::string::npos, "package desconhecido recusado");
}

void test_signature_install() {
    std::printf("[signature/install]\n");
    std::string err;
    auto pm = engine::packaging::create_package_manager("pm-4", err);
    FakeVerifier verifier;

    pm->register_manifest(pkg("core", "1.0", "hc", {}), err);
    pm->register_manifest(pkg("mod", "1.0", "hm", {"core", "==1.0"}), err);

    // Sem verificador: verify e install recusados.
    check(!pm->verify_signature("core", "x", err), "verify sem verificador recusada");
    check(!pm->install("core", "x", err), "install sem verificador recusada");
    check(pm->installed().empty(), "nada instalado");

    pm->set_verifier(&verifier);

    // Assinatura inválida: nada muda.
    check(!pm->verify_signature("core", "bad", err), "assinatura inválida recusada");
    check(!pm->install("core", "bad", err), "install com assinatura inválida recusado");
    check(pm->installed().empty(), "nada instalado após recusa");

    // Dep não verificada bloqueia o install do root (all-or-nothing).
    check(!pm->install("mod", "ok:mod", err),
          "install com dep não verificada recusado");
    check(err.find("not verified") != std::string::npos, "erro nomeado da dep não verificada");
    check(pm->installed().empty(), "nada instalado após recusa de dep");

    // Válida: verifica core explicitamente, depois instala root + deps em
    // ordem topológica.
    check(pm->verify_signature("core", "ok:core", err), "verify de core ok");
    check(pm->install("mod", "ok:mod", err), "install de mod ok");
    auto inst = pm->installed();
    check(inst.size() == 2, "core + mod instalados");
    if (inst.size() == 2) {
        check(inst[0].name == "core" && inst[1].name == "mod",
              "ordem de instalação: core antes de mod");
    }

    // Reinstalar é no-op.
    check(pm->install("mod", "ok:mod", err), "reinstall no-op");
    check(pm->installed().size() == 2, "sem duplicar");

    // Content hash de package instalado não pode mudar.
    check(!pm->register_manifest(pkg("core", "1.0", "DIFFERENT", {}), err),
          "content de instalado não muda sem uninstall");

    // Uninstall com dependente instalado recusado.
    check(!pm->uninstall("core", err), "uninstall de core com dependente recusado");
    check(pm->uninstall("mod", err), "uninstall de mod ok");
    check(pm->uninstall("core", err), "uninstall de core ok");
    check(pm->installed().empty(), "tudo desinstalado");
    check(pm->uninstall("core", err), "uninstall repetido no-op");
}

void test_persistence() {
    std::printf("[persistence]\n");
    std::string err;
    auto pm = engine::packaging::create_package_manager("pm-5", err);
    FakeVerifier verifier;
    pm->set_verifier(&verifier);
    pm->register_manifest(pkg("core", "1.0", "hc", {}), err);
    pm->register_manifest(pkg("mod", "2.0", "hm", {"core", ">=1.0"}), err);
    pm->verify_signature("core", "ok:core", err);
    pm->install("mod", "ok:mod", err);
    const std::string snap = pm->serialize_state();

    auto pm2 = engine::packaging::create_package_manager("pm-5", err);
    check(pm2->load_from_json(snap, err), "load aceita o próprio snapshot");
    check(pm2->serialize_state() == snap, "round-trip bit-exact");
    check(pm2->installed().size() == 2, "estado de instalação preservado");
    auto st = pm2->states();
    check(st.size() == 2, "dois estados");
    if (st.size() == 2) {
        check(st[0].name == "core" && st[1].name == "mod", "ordem por nome preservada");
    }

    // Rejeições all-or-nothing (estado intacto).
    check(!pm2->load_from_json("{}", err), "documento sem session rejeitado");
    check(pm2->serialize_state() == snap, "estado intacto após rejeição 1");

    std::string bad2 = snap;
    const std::size_t pos = bad2.find("\"pm-5\"");
    bad2.replace(pos, 7, "\"other\"");
    check(!pm2->load_from_json(bad2, err), "session mismatch rejeitado");
    check(pm2->serialize_state() == snap, "estado intacto após rejeição 2");

    std::string bad3 = snap + ",\"bogus\":1}";
    check(!pm2->load_from_json(bad3, err), "campo desconhecido rejeitado");
    check(pm2->serialize_state() == snap, "estado intacto após rejeição 3");

    std::string bad4 = snap;
    const std::size_t pos4 = bad4.find("\"install_seq\":");
    bad4.replace(pos4 + std::string("\"install_seq\":").size(), 1, "x");
    check(!pm2->load_from_json(bad4, err), "install_seq malformado rejeitado");
    check(pm2->serialize_state() == snap, "estado intacto após rejeição 4");
}

void test_determinism() {
    std::printf("[determinism]\n");
    std::string err;
    auto p1 = engine::packaging::create_package_manager("pm-6", err);
    auto p2 = engine::packaging::create_package_manager("pm-6", err);
    p1->register_manifest(pkg("a", "1.0", "h", {"b", "*"}), err);
    p2->register_manifest(pkg("a", "1.0", "h", {"b", "*"}), err);
    p1->register_manifest(pkg("b", "1.0", "h", {}), err);
    p2->register_manifest(pkg("b", "1.0", "h", {}), err);
    check(p1->serialize_state() == p2->serialize_state(),
          "snapshot determinístico cross-instance");
    check(p1->resolve("a").order == p2->resolve("a").order,
          "resolução determinística cross-instance");
}

void test_reset() {
    std::printf("[reset]\n");
    std::string err;
    auto pm = engine::packaging::create_package_manager("pm-7", err);
    pm->register_manifest(pkg("a", "1.0", "h", {}), err);
    check(pm->reset(err), "reset ok");
    check(pm->states().empty(), "reset limpa manifestos");
    check(pm->installed().empty(), "reset limpa instalações");
}

}  // namespace

int main() {
    test_creation();
    test_register();
    test_resolve();
    test_signature_install();
    test_persistence();
    test_determinism();
    test_reset();
    if (failures == 0) {
        std::printf("PackageManagerTests: ALL PASSED\n");
        return 0;
    }
    std::printf("PackageManagerTests: %d FAILURE(S)\n", failures);
    return 1;
}
