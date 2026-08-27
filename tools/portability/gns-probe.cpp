// gns-probe.cpp — proves GameNetworkingSockets C API is usable
// Returns exit 0 if initialization succeeds.

#include <cstdio>
#include <cstdlib>

#include "steam/steamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"

int main() {
    printf("[gns-probe] GameNetworkingSockets probe v3\n");

    // 1. Init (no identity — defaults)
    printf("[gns-probe] calling GameNetworkingSockets_Init...\n");
    SteamNetworkingErrMsg errMsg;
    bool ok = GameNetworkingSockets_Init(nullptr, errMsg);
    if (!ok) {
        printf("[gns-probe] FAIL: Init: %s\n", errMsg);
        return 1;
    }
    printf("[gns-probe] OK: initialized\n");

    // 2. Get default interface
    ISteamNetworkingSockets* iface = SteamNetworkingSockets();
    if (!iface) {
        printf("[gns-probe] FAIL: SteamNetworkingSockets() null\n");
        return 2;
    }
    printf("[gns-probe] OK: interface created\n");

    // 3. Get utils
    ISteamNetworkingUtils* utils = SteamNetworkingUtils_LibV4();
    if (!utils) {
        printf("[gns-probe] FAIL: SteamNetworkingUtils_LibV4() null\n");
        return 3;
    }
    printf("[gns-probe] OK: utils interface created\n");

    // 4. Cleanup
    GameNetworkingSockets_Kill();
    printf("[gns-probe] OK: cleanup done\n");

    printf("[gns-probe] ALL CHECKS PASSED\n");
    return 0;
}
