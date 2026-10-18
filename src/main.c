/*
 * main.c - Entry point: argument parsing, loading data, launching the UI.
 *
 *   bank [--data-dir DIR] [--demo] [--help] [--version]
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#define VERSION "2.0.0"
#define DEFAULT_DATA_DIR "data"

static void usage(const char *prog)
{
    printf("%s - console banking system v%s\n\n", BANK_NAME, VERSION);
    printf("Usage: %s [options]\n\n", prog);
    puts("Options:");
    puts("  --data-dir DIR   Store the database in DIR (default: ./" DEFAULT_DATA_DIR ")");
    puts("  --demo           Create a demo database with sample accounts (only if none exists)");
    puts("  --version        Print version and exit");
    puts("  --help           Show this help");
}

/* Sample data for trying the app out. Credentials are printed on creation. */
static bool seed_demo(App *app)
{
    uint32_t n;
    time_t now = time(NULL);
    if (bank_set_admin_password(&app->bank, "Admin@1234") != BANK_OK) return false;
    if (bank_open_account(&app->bank, "Aliasghar", ACC_SAVINGS, 163400, "Demo@1234", now, &n) != BANK_OK) return false;
    if (bank_open_account(&app->bank, "Hussain", ACC_CURRENT, 1002500, "Demo@1234", now, &n) != BANK_OK) return false;
    return storage_save(&app->storage, &app->bank) == STORAGE_OK;
}

int main(int argc, char **argv)
{
    App app;
    const char *dir = DEFAULT_DATA_DIR;
    bool demo = false;
    StorageStatus st;
    int bad_line = 0, i, rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) { usage(argv[0]); return 0; }
        if (strcmp(argv[i], "--version") == 0) { printf("%s\n", VERSION); return 0; }
        if (strcmp(argv[i], "--demo") == 0) { demo = true; continue; }
        if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) { dir = argv[++i]; continue; }
        fprintf(stderr, "Unknown or incomplete option: %s\n\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    bank_init(&app.bank);
    if (!storage_init(&app.storage, dir)) {
        fprintf(stderr, "Error: %s\n", storage_strerror(STORAGE_ERR_PATH));
        return 1;
    }

    st = storage_load(&app.storage, &app.bank, &bad_line);
    if (st == STORAGE_NOT_FOUND) {
        if (demo) {
            if (!seed_demo(&app)) { fprintf(stderr, "Error: could not create demo data.\n"); bank_free(&app.bank); return 1; }
            puts("Demo data created.");
            puts("  Admin password : Admin@1234");
            puts("  Accounts       : 10000001 (Aliasghar), 10000002 (Hussain)");
            puts("  Customer pass  : Demo@1234");
        }
    } else if (st != STORAGE_OK) {
        fprintf(stderr, "Error loading '%s': %s", app.storage.db, storage_strerror(st));
        if (bad_line > 0) fprintf(stderr, " (line %d)", bad_line);
        fprintf(stderr, "\nThe file was NOT modified. Fix or restore it from '%s'.\n", app.storage.bak);
        bank_free(&app.bank);
        return 1;
    } else if (demo) {
        puts("Existing database found; --demo ignored.");
    }

    rc = ui_run(&app);
    bank_free(&app.bank);
    return rc;
}
