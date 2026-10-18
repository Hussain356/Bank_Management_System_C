/*
 * ui.c - Interactive console front-end: menus, prompts and screens.
 * All business rules live in bank.c; this file only talks to the user.
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef enum { FLOW_OK = 0, FLOW_LOGOUT, FLOW_EOF } Flow;

/* Early-return on EOF, print a note and return on cancel. */
#define REQUIRE_INPUT(call)                                      \
    do {                                                         \
        InputResult r_ = (call);                                 \
        if (r_ == INPUT_EOF) return FLOW_EOF;                    \
        if (r_ == INPUT_CANCEL) { puts("  Cancelled."); return FLOW_OK; } \
    } while (0)

/* ---------------------------------------------------------------- helpers */

static void rule(void) { puts("------------------------------------------------------------"); }

static void banner(const char *title)
{
    puts("");
    puts("============================================================");
    printf("  %s  |  %s\n", BANK_NAME, title);
    puts("============================================================");
}

static void print_error(BankStatus st) { printf("  x %s\n", bank_strerror(st)); }

static void print_money_line(const char *label, money_t amount)
{
    char buf[48];
    money_format(amount, buf, sizeof(buf));
    printf("  %-22s %s %s\n", label, BANK_CURRENCY, buf);
}

/* Saves to disk; warns loudly if that fails. */
static bool persist(App *app)
{
    StorageStatus st = storage_save(&app->storage, &app->bank);
    if (st != STORAGE_OK) {
        printf("  ! WARNING: could not save data (%s). Recent changes may be lost.\n",
               storage_strerror(st));
        return false;
    }
    return true;
}

/* "Aliasghar Hussain" -> "A******** H******" */
static void mask_name(const char *name, char *out, size_t size)
{
    size_t i, j = 0;
    bool word_start = true;
    for (i = 0; name[i] && j + 1 < size; i++) {
        if (name[i] == ' ') { out[j++] = ' '; word_start = true; }
        else if (word_start) { out[j++] = name[i]; word_start = false; }
        else out[j++] = '*';
    }
    out[j] = '\0';
}

/* Asks for a new password twice, enforcing the policy. Empty input cancels. */
static InputResult prompt_new_password(char *out, size_t size)
{
    char again[AUTH_PASSWORD_MAX + 64], reason[128];
    printf("  Password rules: %d-%d chars, upper + lower case letter + digit, no spaces.\n",
           AUTH_PASSWORD_MIN, AUTH_PASSWORD_MAX);
    for (;;) {
        InputResult r = input_password("  New password (Enter to cancel): ", out, size);
        if (r != INPUT_OK) return r;
        if (out[0] == '\0') return INPUT_CANCEL;
        if (!auth_password_acceptable(out, reason, sizeof(reason))) {
            printf("  x %s\n", reason);
            continue;
        }
        r = input_password("  Confirm password            : ", again, sizeof(again));
        if (r != INPUT_OK) { auth_wipe(again, sizeof(again)); return r; }
        if (strcmp(out, again) != 0) {
            puts("  x Passwords do not match. Try again.");
            auth_wipe(again, sizeof(again));
            continue;
        }
        auth_wipe(again, sizeof(again));
        return INPUT_OK;
    }
}

static void print_account_summary(const Bank *b, const Account *a)
{
    char created[32];
    money_t avail = a->balance - bank_min_balance(a->type);
    money_t remaining = DAILY_OUTGOING_LIMIT - bank_daily_outgoing(b, a->number, time(NULL));
    format_time(a->created, created, sizeof(created));
    rule();
    printf("  %-22s %lu (%s)\n", "Account number:", (unsigned long)a->number, account_type_name(a->type));
    printf("  %-22s %s\n", "Holder:", a->name);
    printf("  %-22s %s\n", "Opened:", created);
    print_money_line("Balance:", a->balance);
    print_money_line("Available to withdraw:", avail < 0 ? 0 : avail);
    print_money_line("Daily limit remaining:", remaining < 0 ? 0 : remaining);
    rule();
}

static void print_history(const Bank *b, uint32_t number, size_t max_rows)
{
    size_t total = 0, skip, i, shown = 0;
    char when[32], amt[48], signed_amt[52], bal[48], other[16];

    for (i = 0; i < b->n_txns; i++) if (b->txns[i].account == number) total++;
    skip = (max_rows && total > max_rows) ? total - max_rows : 0;

    if (total == 0) { puts("  No transactions yet."); return; }
    printf("  %-16s %-15s %15s %15s  %s\n", "Date", "Type", "Amount", "Balance", "Other acct");
    rule();
    for (i = 0; i < b->n_txns; i++) {
        const Transaction *t = &b->txns[i];
        if (t->account != number) continue;
        if (skip) { skip--; continue; }
        format_time(t->when, when, sizeof(when));
        money_format(t->amount, amt, sizeof(amt));
        money_format(t->balance_after, bal, sizeof(bal));
        if (t->counterparty) snprintf(other, sizeof(other), "%lu", (unsigned long)t->counterparty);
        else other[0] = '\0';
        snprintf(signed_amt, sizeof(signed_amt), "%c%s", txn_is_credit(t->type) ? '+' : '-', amt);
        printf("  %-16s %-15s %15s %15s  %s\n", when, txn_type_name(t->type), signed_amt, bal, other);
        shown++;
    }
    rule();
    printf("  Showing %lu of %lu transaction(s).\n", (unsigned long)shown, (unsigned long)total);
}

/* Re-checks the logged-in user's password before a sensitive action. */
static Flow reauthenticate(App *app, uint32_t number, bool *ok)
{
    char pw[AUTH_PASSWORD_MAX + 64];
    int left;
    bool modified;
    BankStatus st;

    *ok = false;
    if (input_password("  Re-enter your password to confirm: ", pw, sizeof(pw)) == INPUT_EOF) return FLOW_EOF;
    st = bank_login(&app->bank, number, pw, &left, &modified);
    auth_wipe(pw, sizeof(pw));
    if (modified) persist(app);
    if (st == BANK_OK) { *ok = true; return FLOW_OK; }
    if (st == BANK_ERR_LOCKED) {
        print_error(st);
        return FLOW_LOGOUT;
    }
    printf("  x Wrong password. %d attempt(s) left before the account locks.\n", left);
    return FLOW_OK;
}

/* ------------------------------------------------------ customer screens */

static Flow screen_deposit(App *app, uint32_t number)
{
    money_t amount;
    BankStatus st;
    Account *a;
    banner("Deposit");
    REQUIRE_INPUT(input_money("  Amount to deposit (Enter to cancel): ", &amount));
    st = bank_deposit(&app->bank, number, amount, time(NULL));
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    a = bank_find(&app->bank, number);
    puts("  + Deposit successful.");
    print_money_line("New balance:", a->balance);
    return FLOW_OK;
}

static Flow screen_withdraw(App *app, uint32_t number)
{
    money_t amount;
    BankStatus st;
    Account *a;
    banner("Withdraw");
    REQUIRE_INPUT(input_money("  Amount to withdraw (Enter to cancel): ", &amount));
    st = bank_withdraw(&app->bank, number, amount, time(NULL));
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    a = bank_find(&app->bank, number);
    puts("  + Withdrawal successful. Please collect your cash.");
    print_money_line("New balance:", a->balance);
    return FLOW_OK;
}

static Flow screen_transfer(App *app, uint32_t number)
{
    uint32_t to;
    money_t amount;
    Account *dst;
    bool yes, authed;
    Flow fl;
    BankStatus st;
    char masked[NAME_MAX_LEN + 1], amt[48];

    banner("Transfer");
    REQUIRE_INPUT(input_account_number("  Recipient account number (Enter to cancel): ", &to));
    if (to == number) { print_error(BANK_ERR_SAME_ACCOUNT); return FLOW_OK; }
    dst = bank_find(&app->bank, to);
    if (!dst) { print_error(BANK_ERR_NOT_FOUND); return FLOW_OK; }
    mask_name(dst->name, masked, sizeof(masked));
    printf("  Recipient: %s\n", masked);
    REQUIRE_INPUT(input_money("  Amount to transfer (Enter to cancel): ", &amount));

    money_format(amount, amt, sizeof(amt));
    printf("  Send %s %s to %lu (%s)?\n", BANK_CURRENCY, amt, (unsigned long)to, masked);
    if (input_confirm("  Confirm (y/N): ", &yes) == INPUT_EOF) return FLOW_EOF;
    if (!yes) { puts("  Cancelled."); return FLOW_OK; }

    fl = reauthenticate(app, number, &authed);
    if (fl != FLOW_OK || !authed) return fl;

    st = bank_transfer(&app->bank, number, to, amount, time(NULL));
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    puts("  + Transfer successful.");
    print_money_line("New balance:", bank_find(&app->bank, number)->balance);
    return FLOW_OK;
}

static Flow screen_change_password(App *app, uint32_t number)
{
    char old_pw[AUTH_PASSWORD_MAX + 64], new_pw[AUTH_PASSWORD_MAX + 64];
    bool modified;
    BankStatus st;
    InputResult r;

    banner("Change Password");
    if (input_password("  Current password: ", old_pw, sizeof(old_pw)) == INPUT_EOF) return FLOW_EOF;
    r = prompt_new_password(new_pw, sizeof(new_pw));
    if (r != INPUT_OK) {
        auth_wipe(old_pw, sizeof(old_pw));
        if (r == INPUT_EOF) return FLOW_EOF;
        puts("  Cancelled.");
        return FLOW_OK;
    }
    st = bank_change_password(&app->bank, number, old_pw, new_pw, &modified);
    auth_wipe(old_pw, sizeof(old_pw));
    auth_wipe(new_pw, sizeof(new_pw));
    if (modified) persist(app);
    if (st == BANK_OK) { puts("  + Password changed successfully."); return FLOW_OK; }
    print_error(st);
    return st == BANK_ERR_LOCKED ? FLOW_LOGOUT : FLOW_OK;
}

static Flow screen_export(App *app, uint32_t number)
{
    char path[STORAGE_PATH_MAX + 64];
    StorageStatus st;
    banner("Export Statement");
    st = storage_export_statement(&app->storage, &app->bank, bank_find(&app->bank, number),
                                  path, sizeof(path));
    if (st == STORAGE_OK) printf("  + Statement saved to: %s\n", path);
    else printf("  x Could not write statement (%s).\n", storage_strerror(st));
    return FLOW_OK;
}

static Flow customer_session(App *app, uint32_t number)
{
    for (;;) {
        Account *a = bank_find(&app->bank, number);
        int choice;
        Flow fl = FLOW_OK;
        if (!a) return FLOW_OK;

        banner("Customer Menu");
        printf("  Welcome, %s\n", a->name);
        puts("");
        puts("  1. Account summary");
        puts("  2. Deposit");
        puts("  3. Withdraw");
        puts("  4. Transfer to another account");
        puts("  5. Mini statement (last 10)");
        puts("  6. Full transaction history");
        puts("  7. Export statement to file");
        puts("  8. Change password");
        puts("  0. Log out");
        if (input_choice("\n  Choose an option: ", 0, 8, &choice) == INPUT_EOF) return FLOW_EOF;

        switch (choice) {
        case 1: banner("Account Summary"); print_account_summary(&app->bank, a); break;
        case 2: fl = screen_deposit(app, number); break;
        case 3: fl = screen_withdraw(app, number); break;
        case 4: fl = screen_transfer(app, number); break;
        case 5: banner("Mini Statement"); print_history(&app->bank, number, 10); break;
        case 6: banner("Transaction History"); print_history(&app->bank, number, 0); break;
        case 7: fl = screen_export(app, number); break;
        case 8: fl = screen_change_password(app, number); break;
        case 0: puts("  Logged out. Thank you for banking with us."); return FLOW_OK;
        }
        if (fl == FLOW_EOF) return FLOW_EOF;
        if (fl == FLOW_LOGOUT) { puts("  You have been logged out."); return FLOW_OK; }
    }
}

static Flow screen_customer_login(App *app)
{
    uint32_t number;
    char pw[AUTH_PASSWORD_MAX + 64];

    banner("Customer Login");
    REQUIRE_INPUT(input_account_number("  Account number (Enter to cancel): ", &number));
    for (;;) {
        int left;
        bool modified;
        BankStatus st;

        if (input_password("  Password (Enter to cancel): ", pw, sizeof(pw)) == INPUT_EOF) return FLOW_EOF;
        if (pw[0] == '\0') { puts("  Cancelled."); return FLOW_OK; }
        st = bank_login(&app->bank, number, pw, &left, &modified);
        auth_wipe(pw, sizeof(pw));
        if (modified) persist(app);

        if (st == BANK_OK) return customer_session(app, number);
        print_error(st);
        if (st == BANK_ERR_LOCKED) return FLOW_OK;
        if (left > 0 && bank_find(&app->bank, number)) printf("  %d attempt(s) left before the account locks.\n", left);
        else if (!bank_find(&app->bank, number)) return FLOW_OK; /* do not reveal more */
    }
}

static Flow screen_open_account(App *app)
{
    char name[128], pw[AUTH_PASSWORD_MAX + 64], dep[48], min[48];
    int type_choice;
    money_t deposit;
    uint32_t number;
    BankStatus st;
    InputResult r;

    banner("Open New Account");
    for (;;) {
        REQUIRE_INPUT(input_text("  Full name (Enter to cancel): ", name, sizeof(name)));
        if (bank_valid_name(name)) break;
        print_error(BANK_ERR_INVALID_NAME);
    }
    puts("  1. Savings  (must keep a minimum balance of PKR 500.00)");
    puts("  2. Current  (no minimum balance)");
    if (input_choice("  Account type: ", 1, 2, &type_choice) == INPUT_EOF) return FLOW_EOF;

    money_format(MIN_OPENING_DEPOSIT, min, sizeof(min));
    printf("  Minimum opening deposit: %s %s\n", BANK_CURRENCY, min);
    for (;;) {
        REQUIRE_INPUT(input_money("  Opening deposit (Enter to cancel): ", &deposit));
        if (deposit >= MIN_OPENING_DEPOSIT) break;
        print_error(BANK_ERR_OPENING_DEPOSIT);
    }

    r = prompt_new_password(pw, sizeof(pw));
    if (r == INPUT_EOF) return FLOW_EOF;
    if (r == INPUT_CANCEL) { puts("  Cancelled."); return FLOW_OK; }

    st = bank_open_account(&app->bank, name, type_choice == 1 ? ACC_SAVINGS : ACC_CURRENT,
                           deposit, pw, time(NULL), &number);
    auth_wipe(pw, sizeof(pw));
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);

    money_format(deposit, dep, sizeof(dep));
    puts("");
    puts("  + Account created successfully!");
    rule();
    printf("  Account number : %lu   <-- keep this safe, you need it to log in\n", (unsigned long)number);
    printf("  Holder         : %s\n", name);
    printf("  Type           : %s\n", account_type_name(type_choice == 1 ? ACC_SAVINGS : ACC_CURRENT));
    printf("  Opening balance: %s %s\n", BANK_CURRENCY, dep);
    rule();
    return FLOW_OK;
}

/* --------------------------------------------------------- admin screens */

static void print_account_table(const Bank *b)
{
    size_t i;
    char bal[48];
    if (b->n_accounts == 0) { puts("  No accounts yet."); return; }
    printf("  %-10s %-24s %-8s %16s  %s\n", "Account", "Holder", "Type", "Balance", "Status");
    rule();
    for (i = 0; i < b->n_accounts; i++) {
        const Account *a = &b->accounts[i];
        money_format(a->balance, bal, sizeof(bal));
        printf("  %-10lu %-24.24s %-8s %16s  %s\n", (unsigned long)a->number, a->name,
               account_type_name(a->type), bal, a->locked ? "LOCKED" : "Active");
    }
    rule();
    printf("  %lu account(s).\n", (unsigned long)b->n_accounts);
}

static Flow admin_view_account(App *app)
{
    uint32_t number;
    Account *a;
    banner("Account Details");
    REQUIRE_INPUT(input_account_number("  Account number (Enter to cancel): ", &number));
    a = bank_find(&app->bank, number);
    if (!a) { print_error(BANK_ERR_NOT_FOUND); return FLOW_OK; }
    print_account_summary(&app->bank, a);
    printf("  Status: %s   Failed logins: %d\n\n", a->locked ? "LOCKED" : "Active", a->failed_attempts);
    print_history(&app->bank, number, 10);
    return FLOW_OK;
}

static Flow admin_unlock(App *app)
{
    uint32_t number;
    BankStatus st;
    banner("Unlock Account");
    REQUIRE_INPUT(input_account_number("  Account number (Enter to cancel): ", &number));
    st = bank_unlock(&app->bank, number);
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    puts("  + Account unlocked and failed-login counter reset.");
    return FLOW_OK;
}

static Flow admin_reset_password(App *app)
{
    uint32_t number;
    char pw[AUTH_PASSWORD_MAX + 64];
    BankStatus st;
    InputResult r;
    banner("Reset Customer Password");
    REQUIRE_INPUT(input_account_number("  Account number (Enter to cancel): ", &number));
    if (!bank_find(&app->bank, number)) { print_error(BANK_ERR_NOT_FOUND); return FLOW_OK; }
    puts("  Set a temporary password and give it to the customer securely.");
    r = prompt_new_password(pw, sizeof(pw));
    if (r == INPUT_EOF) return FLOW_EOF;
    if (r == INPUT_CANCEL) { puts("  Cancelled."); return FLOW_OK; }
    st = bank_admin_reset_password(&app->bank, number, pw);
    auth_wipe(pw, sizeof(pw));
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    puts("  + Password reset. The account is unlocked.");
    return FLOW_OK;
}

static Flow admin_close_account(App *app)
{
    uint32_t number, typed;
    Account *a;
    bool yes;
    money_t payout;
    BankStatus st;
    char bal[48];

    banner("Close Account");
    REQUIRE_INPUT(input_account_number("  Account number (Enter to cancel): ", &number));
    a = bank_find(&app->bank, number);
    if (!a) { print_error(BANK_ERR_NOT_FOUND); return FLOW_OK; }
    money_format(a->balance, bal, sizeof(bal));
    printf("  Account %lu - %s - balance %s %s\n", (unsigned long)number, a->name, BANK_CURRENCY, bal);
    puts("  This permanently closes the account. Any balance is paid out in cash.");
    REQUIRE_INPUT(input_account_number("  Re-type the account number to confirm (Enter to cancel): ", &typed));
    if (typed != number) { puts("  x Numbers do not match. Nothing was changed."); return FLOW_OK; }
    if (input_confirm("  Are you sure? (y/N): ", &yes) == INPUT_EOF) return FLOW_EOF;
    if (!yes) { puts("  Cancelled."); return FLOW_OK; }
    st = bank_close_account(&app->bank, number, time(NULL), &payout);
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    puts("  + Account closed.");
    print_money_line("Cash payout due:", payout);
    return FLOW_OK;
}

static Flow admin_change_password(App *app)
{
    char pw[AUTH_PASSWORD_MAX + 64];
    InputResult r;
    BankStatus st;
    banner("Change Admin Password");
    r = prompt_new_password(pw, sizeof(pw));
    if (r == INPUT_EOF) return FLOW_EOF;
    if (r == INPUT_CANCEL) { puts("  Cancelled."); return FLOW_OK; }
    st = bank_set_admin_password(&app->bank, pw);
    auth_wipe(pw, sizeof(pw));
    if (st != BANK_OK) { print_error(st); return FLOW_OK; }
    persist(app);
    puts("  + Admin password updated.");
    return FLOW_OK;
}

static Flow admin_session(App *app)
{
    for (;;) {
        int choice, locked = 0;
        size_t i;
        Flow fl = FLOW_OK;

        for (i = 0; i < app->bank.n_accounts; i++) if (app->bank.accounts[i].locked) locked++;
        banner("Administrator");
        printf("  %lu account(s), %d locked\n\n", (unsigned long)app->bank.n_accounts, locked);
        puts("  1. List all accounts");
        puts("  2. View account details");
        puts("  3. Unlock account");
        puts("  4. Reset customer password");
        puts("  5. Close account");
        puts("  6. Bank summary");
        puts("  7. Change admin password");
        puts("  0. Log out");
        if (input_choice("\n  Choose an option: ", 0, 7, &choice) == INPUT_EOF) return FLOW_EOF;

        switch (choice) {
        case 1: banner("All Accounts"); print_account_table(&app->bank); break;
        case 2: fl = admin_view_account(app); break;
        case 3: fl = admin_unlock(app); break;
        case 4: fl = admin_reset_password(app); break;
        case 5: fl = admin_close_account(app); break;
        case 6:
            banner("Bank Summary");
            rule();
            printf("  %-22s %lu\n", "Accounts:", (unsigned long)app->bank.n_accounts);
            printf("  %-22s %lu\n", "Transactions:", (unsigned long)app->bank.n_txns);
            print_money_line("Total deposits held:", bank_total_deposits(&app->bank));
            rule();
            break;
        case 7: fl = admin_change_password(app); break;
        case 0: puts("  Administrator logged out."); return FLOW_OK;
        }
        if (fl == FLOW_EOF) return FLOW_EOF;
    }
}

static Flow screen_admin_login(App *app)
{
    int attempt;
    char pw[AUTH_PASSWORD_MAX + 64];

    banner("Administrator Login");
    for (attempt = 1; attempt <= MAX_LOGIN_ATTEMPTS; attempt++) {
        bool ok;
        if (input_password("  Admin password (Enter to cancel): ", pw, sizeof(pw)) == INPUT_EOF) return FLOW_EOF;
        if (pw[0] == '\0') { puts("  Cancelled."); return FLOW_OK; }
        ok = bank_admin_verify(&app->bank, pw);
        auth_wipe(pw, sizeof(pw));
        if (ok) return admin_session(app);
        printf("  x Incorrect password (%d/%d).\n", attempt, MAX_LOGIN_ATTEMPTS);
    }
    puts("  Too many failed attempts. Returning to the main menu.");
    return FLOW_OK;
}

/* ------------------------------------------------------------ first run */

static Flow first_run_setup(App *app)
{
    char pw[AUTH_PASSWORD_MAX + 64];
    InputResult r;
    BankStatus st;

    banner("First-Time Setup");
    puts("  No administrator exists yet. Create the administrator password.");
    for (;;) {
        r = prompt_new_password(pw, sizeof(pw));
        if (r == INPUT_EOF) return FLOW_EOF;
        if (r == INPUT_CANCEL) { puts("  An administrator password is required."); continue; }
        st = bank_set_admin_password(&app->bank, pw);
        auth_wipe(pw, sizeof(pw));
        if (st != BANK_OK) { print_error(st); continue; }
        break;
    }
    persist(app);
    puts("  + Administrator account created.");
    return FLOW_OK;
}

/* ------------------------------------------------------------ main loop */

int ui_run(App *app)
{
    Flow fl = FLOW_OK;

    if (!app->bank.has_admin) fl = first_run_setup(app);

    while (fl != FLOW_EOF) {
        int choice;
        banner("Main Menu");
        puts("  1. Customer login");
        puts("  2. Open a new account");
        puts("  3. Administrator login");
        puts("  0. Exit");
        if (input_choice("\n  Choose an option: ", 0, 3, &choice) == INPUT_EOF) break;

        switch (choice) {
        case 1: fl = screen_customer_login(app); break;
        case 2: fl = screen_open_account(app); break;
        case 3: fl = screen_admin_login(app); break;
        case 0:
            puts("  Goodbye!");
            return 0;
        }
    }
    puts("\n  Input closed. Goodbye!");
    return 0;
}
