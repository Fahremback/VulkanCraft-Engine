// sqlite-probe.c — probe de utilização do SQLite vendido (§7, finding #297).
// Prova que o external é utilizável SEM wiring de CMake: compila e roda contra
// o amalgamation gerado (`external/solutions/sqlite/sqlite3.c` + `sqlite3.h`),
// mesmo padrão do immer-probe #288. O probe usa a API pública (sqlite3_open,
// sqlite3_exec, sqlite3_prepare/step/finalize, sqlite3_bind) em um fluxo real:
// criar tabela, inserir com prepared statement, consultar com filtro e
// verificar os resultados. Exit 0 = utilizável.
//
// Compilação (exemplo):
//   gcc -O2 -I <sqlite-root> tools/portability/sqlite-probe.c <sqlite-root>/sqlite3.c -o sqlite-probe

#include <sqlite3.h>

#include <stdio.h>
#include <string.h>

int main(void) {
    sqlite3* db = NULL;
    char* err = NULL;
    int rc = 0;
    int failures = 0;

    // 1. Abre um banco em memória (determinístico, sem arquivo).
    rc = sqlite3_open(":memory:", &db);
    if (rc != SQLITE_OK) {
        printf("FAIL: open (%s)\n", sqlite3_errmsg(db));
        return 1;
    }

    // 2. Schema + insert via exec.
    rc = sqlite3_exec(db,
        "CREATE TABLE items(id INTEGER PRIMARY KEY, name TEXT NOT NULL, qty INTEGER NOT NULL);"
        "INSERT INTO items(name, qty) VALUES('apple', 3),('banana', 7),('cherry', 1);",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        printf("FAIL: exec (%s)\n", err ? err : "?");
        sqlite3_free(err);
        sqlite3_close(db);
        return 1;
    }

    // 3. Consulta parametrizada via prepared statement + bind.
    sqlite3_stmt* stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT name, qty FROM items WHERE qty >= ? ORDER BY name;",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("FAIL: prepare (%s)\n", sqlite3_errmsg(db));
        failures = 1;
    } else {
        sqlite3_bind_int(stmt, 1, 3);
        int rows = 0;
        int saw_apple = 0, saw_banana = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name = sqlite3_column_text(stmt, 0);
            const int qty = sqlite3_column_int(stmt, 1);
            if (name && strcmp((const char*)name, "apple") == 0 && qty == 3) saw_apple = 1;
            if (name && strcmp((const char*)name, "banana") == 0 && qty == 7) saw_banana = 1;
            ++rows;
        }
        sqlite3_finalize(stmt);
        if (rows != 2 || !saw_apple || !saw_banana) {
            printf("FAIL: query rows=%d apple=%d banana=%d\n", rows, saw_apple, saw_banana);
            failures = 1;
        } else {
            printf("query OK: 2 rows (apple=3, banana=7)\n");
        }
    }

    // 4. Consulta agregada.
    stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*), SUM(qty) FROM items;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        printf("FAIL: aggregate prepare (%s)\n", sqlite3_errmsg(db));
        failures = 1;
    } else {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const int count = sqlite3_column_int(stmt, 0);
            const int sum = sqlite3_column_int(stmt, 1);
            if (count != 3 || sum != 11) {
                printf("FAIL: aggregate count=%d sum=%d\n", count, sum);
                failures = 1;
            } else {
                printf("aggregate OK: count=3 sum=11\n");
            }
        } else {
            printf("FAIL: aggregate step\n");
            failures = 1;
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);

    if (failures == 0) {
        printf("sqlite-probe: ALL PASSED (vendored sqlite usable)\n");
        return 0;
    }
    printf("sqlite-probe: %d FAILURE(S)\n", failures);
    return 1;
}
