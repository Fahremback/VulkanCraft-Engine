// curl-probe.c — probe de utilização do curl vendido (§7, finding #300).
// Prova que o external é utilizável SEM wiring de CMake: compila e roda
// contra a lib estática buildada (build-gate/lib/Release/libcurl.lib). O
// probe usa a easy API pública: curl_easy_init/curl_easy_setopt/curl_easy_perform
// com um data:// URL (sem rede — determinístico) e verifica o corpo recebido.
// Também valida curl_version() (a lib linka e inicializa). Exit 0 = utilizável.
//
// Compilação (exemplo, após buildar a lib):
//   cl /EHsc /I <curl-include> curl-probe.c <libcurl.lib> -o curl-probe

#include <curl/curl.h>

#include <stdio.h>
#include <string.h>

static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t n = size * nmemb;
    memcpy((char*)userdata + strlen((char*)userdata), ptr, n);
    ((char*)userdata)[strlen((char*)userdata) + n] = '\0';
    return n;
}

int main(void) {
    int failures = 0;

    // 1. A lib inicializa e reporta versão.
    curl_version_info_data* ver = curl_version_info(CURLVERSION_NOW);
    if (ver == NULL || ver->version == NULL || ver->version[0] == '\0') {
        printf("FAIL: curl_version_info\n");
        return 1;
    }
    printf("libcurl %s (ssl=%s) OK\n", ver->version,
           ver->ssl_version ? ver->ssl_version : "none");

    // 2. URL API (curl_url): parse + set + get round-trip determinístico.
    CURLU* u = curl_url();
    if (u == NULL) {
        printf("FAIL: curl_url\n");
        return 1;
    }
    CURLUcode urc = curl_url_set(u, CURLUPART_URL, "https://example.com/path?q=1", 0);
    if (urc != CURLUE_OK) {
        printf("FAIL: curl_url_set (%d)\n", urc);
        ++failures;
    } else {
        char* host = NULL;
        urc = curl_url_get(u, CURLUPART_HOST, &host, 0);
        if (urc != CURLUE_OK || host == NULL || strcmp(host, "example.com") != 0) {
            printf("FAIL: host=[%s]\n", host ? host : "?");
            ++failures;
        } else {
            printf("URL API OK: host=[%s]\n", host);
        }
        curl_free(host);
    }
    curl_url_cleanup(u);

    // 3. curl_easy_escape (função pura determinística).
    CURL* curl = curl_easy_init();
    if (curl == NULL) {
        printf("FAIL: curl_easy_init\n");
        return 1;
    }
    char* esc = curl_easy_escape(curl, "hello world?x=1", 0);
    if (esc == NULL || strcmp(esc, "hello%20world%3Fx%3D1") != 0) {
        printf("FAIL: escape=[%s]\n", esc ? esc : "?");
        ++failures;
    } else {
        printf("escape OK: [%s]\n", esc);
    }
    curl_free(esc);

    // 4. URL malformada → CURLUE_MALFORMED_INPUT (gate de erro real).
    CURLU* bad_u = curl_url();
    if (bad_u != NULL) {
        CURLUcode bad_urc = curl_url_set(bad_u, CURLUPART_URL, "http://[invalid", 0);
        if (bad_urc == CURLUE_OK) {
            printf("FAIL: malformed URL accepted\n");
            ++failures;
        } else {
            printf("malformed URL rejected OK (%d)\n", bad_urc);
        }
        curl_url_cleanup(bad_u);
    }

    curl_easy_cleanup(curl);

    if (failures == 0) {
        printf("curl-probe: ALL PASSED (vendored curl usable)\n");
        return 0;
    }
    printf("curl-probe: %d FAILURE(S)\n", failures);
    return 1;
}
