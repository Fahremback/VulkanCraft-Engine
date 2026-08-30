// WorldProcgenTests — WorldProcgen.RequiredFactoryFailureReported (Agente 6
// certification of A4-PROCGEN-FACTORY-FAILURE).
//
// The fix: WorldProcgen::init classifies 15 factories as REQUIRED and aborts
// (reports + returns) when one of them fails, instead of silently degrading
// with a null factory. `require_factory` is the exact reporting/abort decision
// used by every required call site in init(); this test drives it directly so
// the failure path is provable headlessly (no World boot needed).
//
// Calibration: reintroducing the old silent behavior (dropping the check, so
// a null required factory is ignored) makes `require_factory` return true for
// a failed factory and this test FAILS. Keeping the guard keeps it green.

#include "WorldProcgen.hpp"

#include <cassert>
#include <iostream>
#include <string>

int main() {
    // Required factory that failed -> reported + init must abort (false).
    // require_factory must return false so WorldProcgen::init returns early
    // instead of continuing with a null factory.
    {
        std::string diag = "invalid noise graph spec";
        const bool ok = app::WorldProcgen::require_factory(
            "heightGraph", diag, /*has_value=*/false);
        assert(!ok);  // failed required factory must abort init
    }

    // Required factory with an empty diagnostic still aborts.
    {
        const bool ok = app::WorldProcgen::require_factory(
            "temperature", "", /*has_value=*/false);
        assert(!ok);
    }

    // Optional/successful factory -> init continues (true).
    {
        const bool ok = app::WorldProcgen::require_factory(
            "decoratorSetJson", "", /*has_value=*/true);
        assert(ok);
    }

    std::cout << "WorldProcgen.RequiredFactoryFailureReported PASS\n";
    return 0;
}
