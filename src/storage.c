/*
 * storage.c - Text database with atomic save and backup.
 *
 * File format (one record per line, '|' separated):
 *   BANKDB|1
 *   META|<next_account_number>|<next_txn_id>
 *   ADMIN|<salt_hex>|<hash_hex>
 *   ACCOUNT|<number>|<name>|<type>|<balance>|<salt_hex>|<hash_hex>|<failed>|<locked>|<created>
 *   TXN|<id>|<time>|<account>|<type>|<amount>|<balance_after>|<counterparty>
 */
#include "storage.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_VERSION   1
#define LINE_MAX_LEN 1024
#define MAX_FIELDS   16

/* ------------------------------------------------------------------ paths */

bool storage_init(Storage *s, const char *dir)
{
    int n1, n2, n3, n4;
    size_t len = strlen(dir);
    while (len > 1 && (dir[len - 1] == '/' || dir[len - 1] == '\\')) len--;
    n1 = snprintf(s->dir, sizeof(s->dir), "%.*s", (int)len, dir);
    n2 = snprintf(s->db,  sizeof(s->db),  "%s/bank.db",     s->dir);
    n3 = snprintf(s->tmp, sizeof(s->tmp), "%s/bank.db.tmp", s->dir);
    n4 = snprintf(s->bak, sizeof(s->bak), "%s/bank.db.bak", s->dir);
    return n1 > 0 && n2 > 0 && n3 > 0 && n4 > 0 &&
           (size_t)n2 < sizeof(s->db) && (size_t)n3 < sizeof(s->tmp) && (size_t)n4 < sizeof(s->bak);
}

const char *storage_strerror(StorageStatus st)
{
    switch (st) {
    case STORAGE_OK:           return "OK";
    case STORAGE_NOT_FOUND:    return "No database found.";
    case STORAGE_ERR_IO:       return "File read/write error.";
    case STORAGE_ERR_CORRUPT:  return "Database file is corrupt.";
    case STORAGE_ERR_NOMEM:    return "Out of memory.";
    case STORAGE_ERR_PATH:     return "Data directory path is invalid or too long.";
    }
    return "Unknown storage error.";
}

/* ---------------------------------------------------------------- parsing */

static int split_fields(char *line, char *fields[], int max)
{
    int n = 0;
    char *p = line;
    fields[n++] = p;
    while (*p && n < max) {
        if (*p == '|') { *p = '\0'; fields[n++] = p + 1; }
        p++;
    }
    return n;
}

static bool parse_i64(const char *s, int64_t *out)
{
    char *end;
    long long v;
    if (!*s) return false;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0') return false;
    *out = (int64_t)v;
    return true;
}

static bool parse_u32(const char *s, uint32_t *out)
{
    int64_t v;
    if (!parse_i64(s, &v) || v < 0 || v > (int64_t)UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_credential(const char *salt_hex, const char *hash_hex, Credential *c)
{
    return hex_decode(salt_hex, c->salt, AUTH_SALT_BYTES) &&
           hex_decode(hash_hex, c->hash, AUTH_HASH_BYTES);
}

static bool parse_account(char *f[], int n, Account *a)
{
    int64_t balance, failed, locked, created;
    uint32_t type;
    if (n != 10) return false;
    memset(a, 0, sizeof(*a));
    if (!parse_u32(f[1], &a->number) || a->number == 0) return false;
    if (!bank_valid_name(f[2])) return false;
    strncpy(a->name, f[2], NAME_MAX_LEN);
    if (!parse_u32(f[3], &type) || type > ACC_CURRENT) return false;
    a->type = (AccountType)type;
    if (!parse_i64(f[4], &balance) || balance < 0 || balance > MAX_BALANCE) return false;
    a->balance = balance;
    if (!parse_credential(f[5], f[6], &a->cred)) return false;
    if (!parse_i64(f[7], &failed) || failed < 0 || failed > 1000) return false;
    a->failed_attempts = (int)failed;
    if (!parse_i64(f[8], &locked) || (locked != 0 && locked != 1)) return false;
    a->locked = locked == 1;
    if (!parse_i64(f[9], &created)) return false;
    a->created = (time_t)created;
    return true;
}

static bool parse_txn(char *f[], int n, Transaction *t)
{
    int64_t id, when, amount, bal;
    uint32_t type;
    if (n != 8) return false;
    memset(t, 0, sizeof(*t));
    if (!parse_i64(f[1], &id) || id <= 0) return false;
    t->id = (uint64_t)id;
    if (!parse_i64(f[2], &when)) return false;
    t->when = (time_t)when;
    if (!parse_u32(f[3], &t->account)) return false;
    if (!parse_u32(f[4], &type) || type >= TXN_TYPE_COUNT) return false;
    t->type = (TxnType)type;
    if (!parse_i64(f[5], &amount) || amount < 0) return false;
    t->amount = amount;
    if (!parse_i64(f[6], &bal) || bal < 0) return false;
    t->balance_after = bal;
    if (!parse_u32(f[7], &t->counterparty)) return false;
    return true;
}

/* ------------------------------------------------------------------- load */

static StorageStatus load_file(const char *path, Bank *bank, int *bad_line)
{
    FILE *fp = fopen(path, "r");
    char line[LINE_MAX_LEN];
    int lineno = 0;
    bool header_seen = false;
    StorageStatus result = STORAGE_OK;

    if (!fp) return errno == ENOENT ? STORAGE_NOT_FOUND : STORAGE_ERR_IO;

    while (fgets(line, sizeof(line), fp)) {
        char *f[MAX_FIELDS];
        size_t len = strlen(line);
        int n;
        lineno++;
        if (len == sizeof(line) - 1 && line[len - 1] != '\n') { result = STORAGE_ERR_CORRUPT; break; }
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        n = split_fields(line, f, MAX_FIELDS);
        if (!header_seen) {
            int64_t ver;
            if (strcmp(f[0], "BANKDB") != 0 || n != 2 || !parse_i64(f[1], &ver) || ver != DB_VERSION) {
                result = STORAGE_ERR_CORRUPT; break;
            }
            header_seen = true;
        } else if (strcmp(f[0], "META") == 0) {
            int64_t next_txn;
            if (n != 3 || !parse_u32(f[1], &bank->next_account_number) ||
                !parse_i64(f[2], &next_txn) || next_txn <= 0) { result = STORAGE_ERR_CORRUPT; break; }
            bank->next_txn_id = (uint64_t)next_txn;
        } else if (strcmp(f[0], "ADMIN") == 0) {
            if (n != 3 || !parse_credential(f[1], f[2], &bank->admin)) { result = STORAGE_ERR_CORRUPT; break; }
            bank->has_admin = true;
        } else if (strcmp(f[0], "ACCOUNT") == 0) {
            Account a;
            BankStatus st;
            if (!parse_account(f, n, &a)) { result = STORAGE_ERR_CORRUPT; break; }
            st = bank_add_account_raw(bank, &a);
            if (st == BANK_ERR_NOMEM) { result = STORAGE_ERR_NOMEM; break; }
            if (st != BANK_OK) { result = STORAGE_ERR_CORRUPT; break; }
        } else if (strcmp(f[0], "TXN") == 0) {
            Transaction t;
            if (!parse_txn(f, n, &t)) { result = STORAGE_ERR_CORRUPT; break; }
            if (bank_add_txn_raw(bank, &t) != BANK_OK) { result = STORAGE_ERR_NOMEM; break; }
        } else {
            result = STORAGE_ERR_CORRUPT; break;
        }
    }
    if (result == STORAGE_OK && (ferror(fp) || !header_seen)) result = header_seen ? STORAGE_ERR_IO : STORAGE_ERR_CORRUPT;
    if (result != STORAGE_OK && bad_line) *bad_line = lineno;
    fclose(fp);
    return result;
}

/* Keep the in-memory counters ahead of anything already on disk. */
static void fix_counters(Bank *b)
{
    size_t i;
    for (i = 0; i < b->n_accounts; i++)
        if (b->accounts[i].number >= b->next_account_number && b->accounts[i].number != UINT32_MAX)
            b->next_account_number = b->accounts[i].number + 1;
    for (i = 0; i < b->n_txns; i++)
        if (b->txns[i].id >= b->next_txn_id) b->next_txn_id = b->txns[i].id + 1;
}

StorageStatus storage_load(const Storage *s, Bank *bank, int *bad_line)
{
    StorageStatus st;
    if (bad_line) *bad_line = 0;

    st = load_file(s->db, bank, bad_line);
    if (st == STORAGE_NOT_FOUND) {
        /* Interrupted save: fall back to the backup if there is one. */
        bank_free(bank);
        bank_init(bank);
        st = load_file(s->bak, bank, bad_line);
    }
    if (st != STORAGE_OK) {
        bank_free(bank);
        bank_init(bank);
        return st;
    }
    fix_counters(bank);
    return STORAGE_OK;
}

/* ------------------------------------------------------------------- save */

static bool write_db(FILE *fp, const Bank *b)
{
    char salt[AUTH_SALT_BYTES * 2 + 1], hash[AUTH_HASH_BYTES * 2 + 1];
    size_t i;

    fprintf(fp, "BANKDB|%d\n", DB_VERSION);
    fprintf(fp, "META|%lu|%llu\n", (unsigned long)b->next_account_number,
            (unsigned long long)b->next_txn_id);
    if (b->has_admin) {
        hex_encode(b->admin.salt, AUTH_SALT_BYTES, salt);
        hex_encode(b->admin.hash, AUTH_HASH_BYTES, hash);
        fprintf(fp, "ADMIN|%s|%s\n", salt, hash);
    }
    for (i = 0; i < b->n_accounts; i++) {
        const Account *a = &b->accounts[i];
        hex_encode(a->cred.salt, AUTH_SALT_BYTES, salt);
        hex_encode(a->cred.hash, AUTH_HASH_BYTES, hash);
        fprintf(fp, "ACCOUNT|%lu|%s|%d|%lld|%s|%s|%d|%d|%lld\n",
                (unsigned long)a->number, a->name, (int)a->type, (long long)a->balance,
                salt, hash, a->failed_attempts, a->locked ? 1 : 0, (long long)a->created);
    }
    for (i = 0; i < b->n_txns; i++) {
        const Transaction *t = &b->txns[i];
        fprintf(fp, "TXN|%llu|%lld|%lu|%d|%lld|%lld|%lu\n",
                (unsigned long long)t->id, (long long)t->when, (unsigned long)t->account,
                (int)t->type, (long long)t->amount, (long long)t->balance_after,
                (unsigned long)t->counterparty);
    }
    return !ferror(fp);
}

StorageStatus storage_save(const Storage *s, const Bank *bank)
{
    FILE *fp;
    bool ok;

    if (!make_directory(s->dir)) return STORAGE_ERR_IO;
    fp = fopen(s->tmp, "w");
    if (!fp) return STORAGE_ERR_IO;
    ok = write_db(fp, bank);
    ok = (fflush(fp) == 0) && ok;
    ok = (fclose(fp) == 0) && ok;
    if (!ok) { remove(s->tmp); return STORAGE_ERR_IO; }

    /* Rotate: db -> bak, tmp -> db. (remove() first because rename() will not
     * overwrite an existing file on Windows.) */
    remove(s->bak);
    if (rename(s->db, s->bak) != 0 && errno != ENOENT) { remove(s->tmp); return STORAGE_ERR_IO; }
    if (rename(s->tmp, s->db) != 0) {
        rename(s->bak, s->db); /* try to restore the previous database */
        remove(s->tmp);
        return STORAGE_ERR_IO;
    }
    return STORAGE_OK;
}

/* -------------------------------------------------------------- statement */

StorageStatus storage_export_statement(const Storage *s, const Bank *bank, const Account *acct,
                                       char *path_out, size_t path_size)
{
    FILE *fp;
    size_t i;
    char when[32], amt[48], signed_amt[52], bal[48], gen[32];
    int n;

    n = snprintf(path_out, path_size, "%s/statement_%lu.txt", s->dir, (unsigned long)acct->number);
    if (n < 0 || (size_t)n >= path_size) return STORAGE_ERR_PATH;
    if (!make_directory(s->dir)) return STORAGE_ERR_IO;
    fp = fopen(path_out, "w");
    if (!fp) return STORAGE_ERR_IO;

    format_time(time(NULL), gen, sizeof(gen));
    money_format(acct->balance, bal, sizeof(bal));
    fprintf(fp, "%s - ACCOUNT STATEMENT\n", BANK_NAME);
    fprintf(fp, "==============================================================\n");
    fprintf(fp, "Account holder : %s\n", acct->name);
    fprintf(fp, "Account number : %lu\n", (unsigned long)acct->number);
    fprintf(fp, "Account type   : %s\n", account_type_name(acct->type));
    fprintf(fp, "Generated      : %s\n", gen);
    fprintf(fp, "Current balance: %s %s\n", BANK_CURRENCY, bal);
    fprintf(fp, "--------------------------------------------------------------\n");
    fprintf(fp, "%-16s  %-15s  %14s  %14s\n", "Date", "Type", "Amount", "Balance");
    fprintf(fp, "--------------------------------------------------------------\n");
    for (i = 0; i < bank->n_txns; i++) {
        const Transaction *t = &bank->txns[i];
        if (t->account != acct->number) continue;
        format_time(t->when, when, sizeof(when));
        money_format(t->amount, amt, sizeof(amt));
        money_format(t->balance_after, bal, sizeof(bal));
        snprintf(signed_amt, sizeof(signed_amt), "%c%s", txn_is_credit(t->type) ? '+' : '-', amt);
        fprintf(fp, "%-16s  %-15s  %14s  %14s\n", when, txn_type_name(t->type), signed_amt, bal);
    }
    fprintf(fp, "==============================================================\n");
    fprintf(fp, "End of statement\n");

    n = ferror(fp) ? -1 : 0;
    if (fclose(fp) != 0) n = -1;
    return n == 0 ? STORAGE_OK : STORAGE_ERR_IO;
}
