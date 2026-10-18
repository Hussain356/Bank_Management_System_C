/*
 * test_bank.c - Dependency-free unit tests. Run with `make test`.
 */
#include "bank.h"
#include "sha256.h"
#include "storage.h"
#include "utils.h"

#include <stdio.h>
#include <string.h>

static int g_run = 0, g_failed = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_run++;                                                                 \
        if (!(cond)) {                                                           \
            g_failed++;                                                          \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
        }                                                                        \
    } while (0)

#define PW "Passw0rdOK"
#define NOW ((time_t)1760000000)

static void hex_of(const char *msg, char out[65])
{
    uint8_t d[32];
    sha256((const uint8_t *)msg, strlen(msg), d);
    hex_encode(d, 32, out);
}

static void test_sha256(void)
{
    char h[65];
    puts("sha256");
    hex_of("", h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    hex_of("abc", h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    { /* one million 'a' exercises multi-block streaming */
        Sha256Ctx c; uint8_t d[32]; uint8_t block[1000]; int i;
        memset(block, 'a', sizeof(block));
        sha256_init(&c);
        for (i = 0; i < 1000; i++) sha256_update(&c, block, sizeof(block));
        sha256_final(&c, d);
        hex_encode(d, 32, h);
        CHECK(strcmp(h, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0);
    }
}

static void test_money(void)
{
    money_t m; char buf[48];
    puts("money");
    CHECK(money_parse("0", &m) && m == 0);
    CHECK(money_parse("1250", &m) && m == 125000);
    CHECK(money_parse("1250.5", &m) && m == 125050);
    CHECK(money_parse("99.99", &m) && m == 9999);
    CHECK(!money_parse("", &m));
    CHECK(!money_parse("-5", &m));
    CHECK(!money_parse("1.234", &m));
    CHECK(!money_parse("1.", &m));
    CHECK(!money_parse(".5", &m));
    CHECK(!money_parse("12a", &m));
    CHECK(!money_parse("1,000", &m));
    CHECK(!money_parse("99999999999999999999999", &m));
    money_format(0, buf, sizeof(buf));         CHECK(strcmp(buf, "0.00") == 0);
    money_format(5, buf, sizeof(buf));         CHECK(strcmp(buf, "0.05") == 0);
    money_format(163400, buf, sizeof(buf));    CHECK(strcmp(buf, "1,634.00") == 0);
    money_format(123456789, buf, sizeof(buf)); CHECK(strcmp(buf, "1,234,567.89") == 0);
    money_format(-250, buf, sizeof(buf));      CHECK(strcmp(buf, "-2.50") == 0);
}

static void test_password_policy(void)
{
    char why[128];
    puts("password policy");
    CHECK(auth_password_acceptable("Abcdef12", why, sizeof(why)));
    CHECK(!auth_password_acceptable("Ab1", why, sizeof(why)));
    CHECK(!auth_password_acceptable("alllowercase1", why, sizeof(why)));
    CHECK(!auth_password_acceptable("ALLUPPERCASE1", why, sizeof(why)));
    CHECK(!auth_password_acceptable("NoDigitsHere", why, sizeof(why)));
    CHECK(!auth_password_acceptable("Has Space1a", why, sizeof(why)));
    CHECK(!auth_password_acceptable("", why, sizeof(why)));
    {
        char longpw[100];
        memset(longpw, 'a', sizeof(longpw)); longpw[99] = '\0'; longpw[0] = 'A'; longpw[1] = '1';
        CHECK(!auth_password_acceptable(longpw, why, sizeof(why)));
    }
}

static void test_credentials(void)
{
    Credential a, b;
    puts("credentials");
    CHECK(auth_make_credential(PW, &a));
    CHECK(auth_make_credential(PW, &b));
    CHECK(auth_verify(PW, &a));
    CHECK(!auth_verify("Passw0rdNO", &a));
    CHECK(!auth_verify("", &a));
    CHECK(memcmp(a.salt, b.salt, sizeof(a.salt)) != 0);   /* unique salts */
    CHECK(memcmp(a.hash, b.hash, sizeof(a.hash)) != 0);   /* => unique hashes */
}

static void test_open_account(void)
{
    Bank b; uint32_t n1 = 0, n2 = 0;
    puts("open account");
    bank_init(&b);
    CHECK(bank_open_account(&b, "Ali Raza", ACC_SAVINGS, 100000, PW, NOW, &n1) == BANK_OK);
    CHECK(n1 == FIRST_ACCOUNT_NUMBER);
    CHECK(bank_open_account(&b, "Sara-Khan", ACC_CURRENT, 50000, PW, NOW, &n2) == BANK_OK);
    CHECK(n2 == n1 + 1);
    CHECK(bank_open_account(&b, "", ACC_SAVINGS, 100000, PW, NOW, NULL) == BANK_ERR_INVALID_NAME);
    CHECK(bank_open_account(&b, "Bad|Name", ACC_SAVINGS, 100000, PW, NOW, NULL) == BANK_ERR_INVALID_NAME);
    CHECK(bank_open_account(&b, "R2D2", ACC_SAVINGS, 100000, PW, NOW, NULL) == BANK_ERR_INVALID_NAME);
    CHECK(bank_open_account(&b, "Ok Name", ACC_SAVINGS, 100, PW, NOW, NULL) == BANK_ERR_OPENING_DEPOSIT);
    CHECK(bank_open_account(&b, "Ok Name", ACC_SAVINGS, 0, PW, NOW, NULL) == BANK_ERR_INVALID_AMOUNT);
    CHECK(bank_open_account(&b, "Ok Name", ACC_SAVINGS, 100000, "weak", NOW, NULL) == BANK_ERR_WEAK_PASSWORD);
    CHECK(bank_open_account(&b, "Ok Name", (AccountType)9, 100000, PW, NOW, NULL) == BANK_ERR_INVALID_TYPE);
    CHECK(b.n_accounts == 2);       /* failures created nothing */
    CHECK(b.n_txns == 2);
    bank_free(&b);
}

static void test_login_lockout(void)
{
    Bank b; uint32_t n; int left = 0; bool mod = false;
    puts("login & lockout");
    bank_init(&b);
    bank_open_account(&b, "Ali Raza", ACC_SAVINGS, 100000, PW, NOW, &n);

    CHECK(bank_login(&b, n, PW, &left, &mod) == BANK_OK && !mod);
    CHECK(bank_login(&b, n, "Wrong1pass", &left, &mod) == BANK_ERR_BAD_CREDENTIALS && left == 2 && mod);
    CHECK(bank_login(&b, n, "Wrong2pass", &left, &mod) == BANK_ERR_BAD_CREDENTIALS && left == 1);
    CHECK(bank_login(&b, n, PW, &left, &mod) == BANK_OK);          /* success resets counter */
    CHECK(bank_find(&b, n)->failed_attempts == 0);

    CHECK(bank_login(&b, n, "x", &left, &mod) == BANK_ERR_BAD_CREDENTIALS);
    CHECK(bank_login(&b, n, "x", &left, &mod) == BANK_ERR_BAD_CREDENTIALS);
    CHECK(bank_login(&b, n, "x", &left, &mod) == BANK_ERR_LOCKED);  /* 3rd strike */
    CHECK(bank_login(&b, n, PW, &left, &mod) == BANK_ERR_LOCKED);   /* even correct pw refused */

    CHECK(bank_unlock(&b, n) == BANK_OK);
    CHECK(bank_login(&b, n, PW, &left, &mod) == BANK_OK);
    CHECK(bank_login(&b, 424242, PW, &left, &mod) == BANK_ERR_BAD_CREDENTIALS); /* unknown acct */
    CHECK(bank_unlock(&b, 424242) == BANK_ERR_NOT_FOUND);
    bank_free(&b);
}

static void test_deposit_withdraw(void)
{
    Bank b; uint32_t n;
    puts("deposit & withdraw");
    bank_init(&b);
    bank_open_account(&b, "Ali Raza", ACC_SAVINGS, 100000, PW, NOW, &n);   /* 1,000.00 */

    CHECK(bank_deposit(&b, n, 25050, NOW) == BANK_OK);
    CHECK(bank_find(&b, n)->balance == 125050);
    CHECK(bank_deposit(&b, n, 0, NOW) == BANK_ERR_INVALID_AMOUNT);
    CHECK(bank_deposit(&b, n, -5, NOW) == BANK_ERR_INVALID_AMOUNT);
    CHECK(bank_deposit(&b, 999, 100, NOW) == BANK_ERR_NOT_FOUND);
    CHECK(bank_deposit(&b, n, MAX_BALANCE, NOW) == BANK_ERR_BALANCE_LIMIT);

    CHECK(bank_withdraw(&b, n, 50000, NOW) == BANK_OK);
    CHECK(bank_find(&b, n)->balance == 75050);
    CHECK(bank_withdraw(&b, n, 99999999, NOW) == BANK_ERR_INSUFFICIENT_FUNDS);
    CHECK(bank_withdraw(&b, n, 70000, NOW) == BANK_ERR_MIN_BALANCE);       /* would leave 50.50 < 500 */
    CHECK(bank_withdraw(&b, n, 25050, NOW) == BANK_OK);                    /* exactly to the minimum */
    CHECK(bank_find(&b, n)->balance == SAVINGS_MIN_BALANCE);
    CHECK(bank_withdraw(&b, n, 1, NOW) == BANK_ERR_MIN_BALANCE);
    bank_free(&b);
}

static void test_current_and_limits(void)
{
    Bank b; uint32_t n;
    puts("current account & daily limit");
    bank_init(&b);
    bank_open_account(&b, "Big Spender", ACC_CURRENT, 20000000, PW, NOW, &n);  /* 200,000.00 */

    CHECK(bank_withdraw(&b, n, 4000000, NOW) == BANK_OK);                      /* 40,000 */
    CHECK(bank_daily_outgoing(&b, n, NOW) == 4000000);
    CHECK(bank_withdraw(&b, n, 1000001, NOW) == BANK_ERR_DAILY_LIMIT);         /* 50,000.01 total */
    CHECK(bank_withdraw(&b, n, 1000000, NOW) == BANK_OK);                      /* exactly 50,000 */
    CHECK(bank_withdraw(&b, n, 1, NOW) == BANK_ERR_DAILY_LIMIT);
    CHECK(bank_withdraw(&b, n, 1000, NOW + 3 * 86400) == BANK_OK);             /* new day resets */
    {   /* current accounts may be emptied completely */
        Bank c; uint32_t m;
        bank_init(&c);
        bank_open_account(&c, "Small Fry", ACC_CURRENT, 50000, PW, NOW, &m);
        CHECK(bank_withdraw(&c, m, 50000, NOW) == BANK_OK);
        CHECK(bank_find(&c, m)->balance == 0);
        bank_free(&c);
    }
    bank_free(&b);
}

static void test_transfer(void)
{
    Bank b; uint32_t a, c;
    puts("transfer");
    bank_init(&b);
    bank_open_account(&b, "Alice", ACC_CURRENT, 100000, PW, NOW, &a);
    bank_open_account(&b, "Carol", ACC_SAVINGS, 100000, PW, NOW, &c);

    CHECK(bank_transfer(&b, a, c, 30000, NOW) == BANK_OK);
    CHECK(bank_find(&b, a)->balance == 70000);
    CHECK(bank_find(&b, c)->balance == 130000);
    CHECK(bank_total_deposits(&b) == 200000);                  /* money is conserved */
    CHECK(bank_transfer(&b, a, a, 100, NOW) == BANK_ERR_SAME_ACCOUNT);
    CHECK(bank_transfer(&b, a, 777, 100, NOW) == BANK_ERR_NOT_FOUND);
    CHECK(bank_transfer(&b, 777, a, 100, NOW) == BANK_ERR_NOT_FOUND);
    CHECK(bank_transfer(&b, a, c, 70001, NOW) == BANK_ERR_INSUFFICIENT_FUNDS);
    CHECK(bank_transfer(&b, c, a, 90000, NOW) == BANK_ERR_MIN_BALANCE);
    CHECK(bank_transfer(&b, a, c, 0, NOW) == BANK_ERR_INVALID_AMOUNT);
    CHECK(bank_total_deposits(&b) == 200000);
    /* two txns per transfer, linked by counterparty */
    CHECK(b.n_txns == 4);
    CHECK(b.txns[2].type == TXN_TRANSFER_OUT && b.txns[2].counterparty == c);
    CHECK(b.txns[3].type == TXN_TRANSFER_IN  && b.txns[3].counterparty == a);
    bank_free(&b);
}

static void test_passwords_and_admin(void)
{
    Bank b; uint32_t n; bool mod; int left;
    puts("password change & admin");
    bank_init(&b);
    bank_open_account(&b, "Ali Raza", ACC_SAVINGS, 100000, PW, NOW, &n);

    CHECK(bank_change_password(&b, n, PW, "NewPassw0rd", &mod) == BANK_OK && mod);
    CHECK(bank_login(&b, n, PW, &left, &mod) == BANK_ERR_BAD_CREDENTIALS);
    CHECK(bank_login(&b, n, "NewPassw0rd", &left, &mod) == BANK_OK);
    CHECK(bank_change_password(&b, n, "WrongOld1x", "Another1Pass", &mod) == BANK_ERR_BAD_CREDENTIALS);
    CHECK(bank_change_password(&b, n, "NewPassw0rd", "weak", &mod) == BANK_ERR_WEAK_PASSWORD);
    CHECK(bank_login(&b, n, "NewPassw0rd", &left, &mod) == BANK_OK);   /* unchanged by weak attempt */

    CHECK(!b.has_admin && !bank_admin_verify(&b, "Admin@1234"));
    CHECK(bank_set_admin_password(&b, "short") == BANK_ERR_WEAK_PASSWORD);
    CHECK(bank_set_admin_password(&b, "Admin@1234") == BANK_OK);
    CHECK(bank_admin_verify(&b, "Admin@1234"));
    CHECK(!bank_admin_verify(&b, "admin@1234"));

    bank_find(&b, n)->locked = true;
    CHECK(bank_admin_reset_password(&b, n, "Reset1Pass") == BANK_OK);
    CHECK(!bank_find(&b, n)->locked);
    CHECK(bank_login(&b, n, "Reset1Pass", &left, &mod) == BANK_OK);
    bank_free(&b);
}

static void test_close_account(void)
{
    Bank b; uint32_t a, c; money_t payout = -1;
    puts("close account");
    bank_init(&b);
    bank_open_account(&b, "Alice", ACC_CURRENT, 100000, PW, NOW, &a);
    bank_open_account(&b, "Carol", ACC_CURRENT, 70000, PW, NOW, &c);
    CHECK(bank_close_account(&b, a, NOW, &payout) == BANK_OK && payout == 100000);
    CHECK(bank_find(&b, a) == NULL);
    CHECK(bank_find(&b, c) != NULL && bank_find(&b, c)->balance == 70000);
    CHECK(b.n_accounts == 1);
    CHECK(bank_close_account(&b, a, NOW, &payout) == BANK_ERR_NOT_FOUND);
    {   /* numbers are never reused */
        uint32_t d;
        bank_open_account(&b, "Dave", ACC_CURRENT, 70000, PW, NOW, &d);
        CHECK(d == c + 1);
    }
    bank_free(&b);
}

static void make_sample(Bank *b, uint32_t *a, uint32_t *c)
{
    bank_init(b);
    bank_set_admin_password(b, "Admin@1234");
    bank_open_account(b, "Aliasghar", ACC_SAVINGS, 163400, PW, NOW, a);
    bank_open_account(b, "Hussain", ACC_CURRENT, 1002500, "Other1Pass", NOW, c);
    bank_deposit(b, *a, 5000, NOW);
    bank_transfer(b, *c, *a, 12345, NOW);
}

static void test_storage(void)
{
    Bank b, l; Storage s; uint32_t a, c; int bad = 0, left; bool mod;
    FILE *f; char line[2048]; size_t i;
    puts("storage");

    CHECK(make_directory("build"));
    CHECK(storage_init(&s, "build/test_data"));
    remove(s.db); remove(s.tmp); remove(s.bak);

    bank_init(&l);
    CHECK(storage_load(&s, &l, &bad) == STORAGE_NOT_FOUND);
    bank_free(&l);

    make_sample(&b, &a, &c);
    CHECK(storage_save(&s, &b) == STORAGE_OK);

    bank_init(&l);
    CHECK(storage_load(&s, &l, &bad) == STORAGE_OK);
    CHECK(l.n_accounts == b.n_accounts && l.n_txns == b.n_txns);
    CHECK(l.has_admin && bank_admin_verify(&l, "Admin@1234"));
    CHECK(bank_find(&l, a) && strcmp(bank_find(&l, a)->name, "Aliasghar") == 0);
    CHECK(bank_find(&l, a)->balance == b.accounts[0].balance);
    CHECK(bank_find(&l, c)->balance == b.accounts[1].balance);
    CHECK(bank_login(&l, a, PW, &left, &mod) == BANK_OK);          /* hashes survive the round trip */
    CHECK(l.next_account_number == b.next_account_number && l.next_txn_id == b.next_txn_id);
    for (i = 0; i < b.n_txns; i++) {
        CHECK(l.txns[i].id == b.txns[i].id && l.txns[i].amount == b.txns[i].amount &&
              l.txns[i].type == b.txns[i].type && l.txns[i].counterparty == b.txns[i].counterparty);
    }
    bank_free(&l);

    /* second save creates a backup */
    bank_deposit(&b, a, 100, NOW);
    CHECK(storage_save(&s, &b) == STORAGE_OK);
    f = fopen(s.bak, "r"); CHECK(f != NULL); if (f) fclose(f);
    f = fopen(s.tmp, "r"); CHECK(f == NULL); if (f) fclose(f);       /* no temp file left behind */

    /* missing main db falls back to backup */
    remove(s.db);
    bank_init(&l);
    CHECK(storage_load(&s, &l, &bad) == STORAGE_OK && l.n_accounts == 2);
    bank_free(&l);

    /* corruption is detected, not silently accepted */
    storage_save(&s, &b);
    f = fopen(s.db, "r");
    CHECK(f != NULL);
    if (f) {
        FILE *g = fopen("build/test_data/corrupt.db", "w");
        while (fgets(line, sizeof(line), f)) {
            if (g && strncmp(line, "ACCOUNT|", 8) == 0) {   /* wreck the balance field */
                fputs("ACCOUNT|10000001|Aliasghar|0|notanumber|00|00|0|0|0\n", g);
                continue;
            }
            if (g) fputs(line, g);
        }
        if (g) fclose(g);
        fclose(f);
    }
    {
        Storage cs;
        storage_init(&cs, "build/test_data");
        snprintf(cs.db, sizeof(cs.db), "build/test_data/corrupt.db");
        snprintf(cs.bak, sizeof(cs.bak), "build/test_data/none.bak");
        bank_init(&l);
        CHECK(storage_load(&cs, &l, &bad) == STORAGE_ERR_CORRUPT && bad > 0);
        CHECK(l.n_accounts == 0);
        bank_free(&l);
    }

    /* garbage header */
    f = fopen("build/test_data/garbage.db", "w");
    if (f) { fputs("hello world\n", f); fclose(f); }
    {
        Storage gs;
        storage_init(&gs, "build/test_data");
        snprintf(gs.db, sizeof(gs.db), "build/test_data/garbage.db");
        snprintf(gs.bak, sizeof(gs.bak), "build/test_data/none.bak");
        bank_init(&l);
        CHECK(storage_load(&gs, &l, &bad) == STORAGE_ERR_CORRUPT);
        bank_free(&l);
    }

    /* statement export */
    {
        char path[600]; char text[4096]; size_t got;
        CHECK(storage_export_statement(&s, &b, bank_find(&b, a), path, sizeof(path)) == STORAGE_OK);
        f = fopen(path, "r");
        CHECK(f != NULL);
        if (f) {
            got = fread(text, 1, sizeof(text) - 1, f); text[got] = '\0'; fclose(f);
            CHECK(strstr(text, "Aliasghar") != NULL && strstr(text, "ACCOUNT STATEMENT") != NULL);
            CHECK(strstr(text, "Transfer in") != NULL);
        }
    }
    bank_free(&b);
}

static void test_hex(void)
{
    uint8_t in[4] = {0x00, 0xab, 0xff, 0x10}, out[4]; char h[9];
    puts("hex");
    hex_encode(in, 4, h);
    CHECK(strcmp(h, "00abff10") == 0);
    CHECK(hex_decode(h, out, 4) && memcmp(in, out, 4) == 0);
    CHECK(!hex_decode("00abff1", out, 4));
    CHECK(!hex_decode("00abffzz", out, 4));
}

int main(void)
{
    test_sha256();
    test_money();
    test_password_policy();
    test_credentials();
    test_open_account();
    test_login_lockout();
    test_deposit_withdraw();
    test_current_and_limits();
    test_transfer();
    test_passwords_and_admin();
    test_close_account();
    test_hex();
    test_storage();

    printf("\n%d checks, %d failed\n", g_run, g_failed);
    return g_failed ? 1 : 0;
}
