/*
 * bank.c - Core banking domain logic.
 *
 * Every mutating operation validates first, reserves memory second and only
 * then changes state, so a failure never leaves the bank half-updated.
 */
#include "bank.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ lifecycle */

void bank_init(Bank *b)
{
    memset(b, 0, sizeof(*b));
    b->next_account_number = FIRST_ACCOUNT_NUMBER;
    b->next_txn_id = 1;
}

void bank_free(Bank *b)
{
    free(b->accounts);
    free(b->txns);
    auth_wipe(b, sizeof(*b));
}

/* --------------------------------------------------------------- memory */

static BankStatus reserve_accounts(Bank *b, size_t extra)
{
    if (b->n_accounts + extra > b->cap_accounts) {
        size_t cap = b->cap_accounts ? b->cap_accounts * 2 : 8;
        Account *p;
        while (cap < b->n_accounts + extra) cap *= 2;
        p = realloc(b->accounts, cap * sizeof(*p));
        if (!p) return BANK_ERR_NOMEM;
        b->accounts = p;
        b->cap_accounts = cap;
    }
    return BANK_OK;
}

static BankStatus reserve_txns(Bank *b, size_t extra)
{
    if (b->n_txns + extra > b->cap_txns) {
        size_t cap = b->cap_txns ? b->cap_txns * 2 : 32;
        Transaction *p;
        while (cap < b->n_txns + extra) cap *= 2;
        p = realloc(b->txns, cap * sizeof(*p));
        if (!p) return BANK_ERR_NOMEM;
        b->txns = p;
        b->cap_txns = cap;
    }
    return BANK_OK;
}

/* Capacity must have been reserved beforehand. */
static void push_txn(Bank *b, uint32_t acct, TxnType type, money_t amount,
                     money_t balance_after, uint32_t counterparty, time_t now)
{
    Transaction *t = &b->txns[b->n_txns++];
    t->id = b->next_txn_id++;
    t->when = now;
    t->account = acct;
    t->type = type;
    t->amount = amount;
    t->balance_after = balance_after;
    t->counterparty = counterparty;
}

BankStatus bank_add_account_raw(Bank *b, const Account *a)
{
    BankStatus st;
    if (bank_find(b, a->number)) return BANK_ERR_DUPLICATE;
    st = reserve_accounts(b, 1);
    if (st != BANK_OK) return st;
    b->accounts[b->n_accounts++] = *a;
    return BANK_OK;
}

BankStatus bank_add_txn_raw(Bank *b, const Transaction *t)
{
    BankStatus st = reserve_txns(b, 1);
    if (st != BANK_OK) return st;
    b->txns[b->n_txns++] = *t;
    return BANK_OK;
}

/* --------------------------------------------------------------- lookup */

Account *bank_find(const Bank *b, uint32_t number)
{
    size_t i;
    for (i = 0; i < b->n_accounts; i++) {
        if (b->accounts[i].number == number) return &b->accounts[i];
    }
    return NULL;
}

money_t bank_total_deposits(const Bank *b)
{
    money_t sum = 0;
    size_t i;
    for (i = 0; i < b->n_accounts; i++) sum += b->accounts[i].balance;
    return sum;
}

money_t bank_min_balance(AccountType type)
{
    return type == ACC_SAVINGS ? SAVINGS_MIN_BALANCE : 0;
}

static bool same_day(time_t a, time_t b)
{
    struct tm ta, tb, *p;
    p = localtime(&a); if (!p) return false; ta = *p;
    p = localtime(&b); if (!p) return false; tb = *p;
    return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

money_t bank_daily_outgoing(const Bank *b, uint32_t number, time_t now)
{
    money_t sum = 0;
    size_t i;
    for (i = 0; i < b->n_txns; i++) {
        const Transaction *t = &b->txns[i];
        if (t->account == number && (t->type == TXN_WITHDRAW || t->type == TXN_TRANSFER_OUT) &&
            same_day(t->when, now)) {
            sum += t->amount;
        }
    }
    return sum;
}

/* ----------------------------------------------------------- validation */

bool bank_valid_name(const char *name)
{
    size_t len = strlen(name), i;
    bool has_letter = false;
    if (len < NAME_MIN_LEN || len > NAME_MAX_LEN) return false;
    if (isspace((unsigned char)name[0]) || isspace((unsigned char)name[len - 1])) return false;
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (isalpha(c)) has_letter = true;
        else if (c != ' ' && c != '.' && c != '\'' && c != '-') return false;
    }
    return has_letter;
}

static bool amount_ok(money_t amount)
{
    return amount > 0 && amount <= MAX_BALANCE;
}

/* ----------------------------------------------------- customer actions */

BankStatus bank_open_account(Bank *b, const char *name, AccountType type, money_t deposit,
                             const char *password, time_t now, uint32_t *out_number)
{
    char reason[128];
    Account acc;
    BankStatus st;

    if (!bank_valid_name(name)) return BANK_ERR_INVALID_NAME;
    if (type != ACC_SAVINGS && type != ACC_CURRENT) return BANK_ERR_INVALID_TYPE;
    if (!amount_ok(deposit)) return BANK_ERR_INVALID_AMOUNT;
    if (deposit < MIN_OPENING_DEPOSIT) return BANK_ERR_OPENING_DEPOSIT;
    if (!auth_password_acceptable(password, reason, sizeof(reason))) return BANK_ERR_WEAK_PASSWORD;
    if (b->next_account_number == 0) return BANK_ERR_NOMEM; /* number space exhausted */

    st = reserve_accounts(b, 1);
    if (st != BANK_OK) return st;
    st = reserve_txns(b, 1);
    if (st != BANK_OK) return st;

    memset(&acc, 0, sizeof(acc));
    if (!auth_make_credential(password, &acc.cred)) return BANK_ERR_RNG;
    acc.number = b->next_account_number++;
    strncpy(acc.name, name, NAME_MAX_LEN);
    acc.type = type;
    acc.balance = deposit;
    acc.created = now;

    b->accounts[b->n_accounts++] = acc;
    push_txn(b, acc.number, TXN_OPENING, deposit, deposit, 0, now);
    if (out_number) *out_number = acc.number;
    return BANK_OK;
}

/* Shared password check that applies the failed-attempt counter and lockout. */
static BankStatus check_password(Account *a, const char *password, int *attempts_left, bool *modified)
{
    if (attempts_left) *attempts_left = 0;
    if (a->locked) return BANK_ERR_LOCKED;

    if (auth_verify(password, &a->cred)) {
        if (a->failed_attempts != 0) {
            a->failed_attempts = 0;
            if (modified) *modified = true;
        }
        return BANK_OK;
    }
    a->failed_attempts++;
    if (modified) *modified = true;
    if (a->failed_attempts >= MAX_LOGIN_ATTEMPTS) {
        a->locked = true;
        return BANK_ERR_LOCKED;
    }
    if (attempts_left) *attempts_left = MAX_LOGIN_ATTEMPTS - a->failed_attempts;
    return BANK_ERR_BAD_CREDENTIALS;
}

BankStatus bank_login(Bank *b, uint32_t number, const char *password,
                      int *attempts_left, bool *modified)
{
    Account *a = bank_find(b, number);
    if (modified) *modified = false;
    if (attempts_left) *attempts_left = 0;
    if (!a) {
        /* Same work as a real attempt so response time does not reveal which
         * account numbers exist. */
        static const Credential dummy = {{0}, {0}};
        (void)auth_verify(password, &dummy);
        return BANK_ERR_BAD_CREDENTIALS;
    }
    return check_password(a, password, attempts_left, modified);
}

BankStatus bank_deposit(Bank *b, uint32_t number, money_t amount, time_t now)
{
    Account *a = bank_find(b, number);
    BankStatus st;
    if (!a) return BANK_ERR_NOT_FOUND;
    if (!amount_ok(amount)) return BANK_ERR_INVALID_AMOUNT;
    if (a->balance > MAX_BALANCE - amount) return BANK_ERR_BALANCE_LIMIT;
    st = reserve_txns(b, 1);
    if (st != BANK_OK) return st;
    a->balance += amount;
    push_txn(b, number, TXN_DEPOSIT, amount, a->balance, 0, now);
    return BANK_OK;
}

/* Validates that `amount` can leave `a` right now. */
static BankStatus check_outgoing(const Bank *b, const Account *a, money_t amount, time_t now)
{
    if (!amount_ok(amount)) return BANK_ERR_INVALID_AMOUNT;
    if (amount > a->balance) return BANK_ERR_INSUFFICIENT_FUNDS;
    if (a->balance - amount < bank_min_balance(a->type)) return BANK_ERR_MIN_BALANCE;
    if (bank_daily_outgoing(b, a->number, now) + amount > DAILY_OUTGOING_LIMIT) return BANK_ERR_DAILY_LIMIT;
    return BANK_OK;
}

BankStatus bank_withdraw(Bank *b, uint32_t number, money_t amount, time_t now)
{
    Account *a = bank_find(b, number);
    BankStatus st;
    if (!a) return BANK_ERR_NOT_FOUND;
    st = check_outgoing(b, a, amount, now);
    if (st != BANK_OK) return st;
    st = reserve_txns(b, 1);
    if (st != BANK_OK) return st;
    a->balance -= amount;
    push_txn(b, number, TXN_WITHDRAW, amount, a->balance, 0, now);
    return BANK_OK;
}

BankStatus bank_transfer(Bank *b, uint32_t from, uint32_t to, money_t amount, time_t now)
{
    Account *src, *dst;
    BankStatus st;
    if (from == to) return BANK_ERR_SAME_ACCOUNT;
    src = bank_find(b, from);
    dst = bank_find(b, to);
    if (!src || !dst) return BANK_ERR_NOT_FOUND;
    st = check_outgoing(b, src, amount, now);
    if (st != BANK_OK) return st;
    if (dst->balance > MAX_BALANCE - amount) return BANK_ERR_BALANCE_LIMIT;
    st = reserve_txns(b, 2);
    if (st != BANK_OK) return st;
    src->balance -= amount;
    dst->balance += amount;
    push_txn(b, from, TXN_TRANSFER_OUT, amount, src->balance, to, now);
    push_txn(b, to, TXN_TRANSFER_IN, amount, dst->balance, from, now);
    return BANK_OK;
}

BankStatus bank_change_password(Bank *b, uint32_t number, const char *old_pw,
                                const char *new_pw, bool *modified)
{
    Account *a = bank_find(b, number);
    char reason[128];
    BankStatus st;
    Credential fresh;
    if (modified) *modified = false;
    if (!a) return BANK_ERR_NOT_FOUND;
    st = check_password(a, old_pw, NULL, modified);
    if (st != BANK_OK) return st;
    if (!auth_password_acceptable(new_pw, reason, sizeof(reason))) return BANK_ERR_WEAK_PASSWORD;
    if (!auth_make_credential(new_pw, &fresh)) return BANK_ERR_RNG;
    a->cred = fresh;
    if (modified) *modified = true;
    return BANK_OK;
}

/* -------------------------------------------------------- admin actions */

BankStatus bank_set_admin_password(Bank *b, const char *password)
{
    char reason[128];
    Credential fresh;
    if (!auth_password_acceptable(password, reason, sizeof(reason))) return BANK_ERR_WEAK_PASSWORD;
    if (!auth_make_credential(password, &fresh)) return BANK_ERR_RNG;
    b->admin = fresh;
    b->has_admin = true;
    return BANK_OK;
}

bool bank_admin_verify(const Bank *b, const char *password)
{
    if (!b->has_admin) return false;
    return auth_verify(password, &b->admin);
}

BankStatus bank_unlock(Bank *b, uint32_t number)
{
    Account *a = bank_find(b, number);
    if (!a) return BANK_ERR_NOT_FOUND;
    a->locked = false;
    a->failed_attempts = 0;
    return BANK_OK;
}

BankStatus bank_admin_reset_password(Bank *b, uint32_t number, const char *new_pw)
{
    Account *a = bank_find(b, number);
    char reason[128];
    Credential fresh;
    if (!a) return BANK_ERR_NOT_FOUND;
    if (!auth_password_acceptable(new_pw, reason, sizeof(reason))) return BANK_ERR_WEAK_PASSWORD;
    if (!auth_make_credential(new_pw, &fresh)) return BANK_ERR_RNG;
    a->cred = fresh;
    a->locked = false;
    a->failed_attempts = 0;
    return BANK_OK;
}

BankStatus bank_close_account(Bank *b, uint32_t number, time_t now, money_t *payout)
{
    Account *a = bank_find(b, number);
    BankStatus st;
    size_t idx;
    money_t remaining;
    if (!a) return BANK_ERR_NOT_FOUND;
    st = reserve_txns(b, 1);
    if (st != BANK_OK) return st;
    remaining = a->balance;
    push_txn(b, number, TXN_CLOSURE, remaining, 0, 0, now);
    idx = (size_t)(a - b->accounts);
    memmove(&b->accounts[idx], &b->accounts[idx + 1], (b->n_accounts - idx - 1) * sizeof(Account));
    b->n_accounts--;
    if (payout) *payout = remaining;
    return BANK_OK;
}

/* ------------------------------------------------------- presentation */

const char *bank_strerror(BankStatus s)
{
    switch (s) {
    case BANK_OK:                    return "Success.";
    case BANK_ERR_NOMEM:             return "Out of memory.";
    case BANK_ERR_INVALID_AMOUNT:    return "Invalid amount.";
    case BANK_ERR_INVALID_NAME:      return "Name must be 2-49 characters: letters, spaces, . ' - only.";
    case BANK_ERR_INVALID_TYPE:      return "Invalid account type.";
    case BANK_ERR_OPENING_DEPOSIT:   return "Opening deposit is below the required minimum.";
    case BANK_ERR_WEAK_PASSWORD:     return "Password does not meet the security policy.";
    case BANK_ERR_NOT_FOUND:         return "Account not found.";
    case BANK_ERR_BAD_CREDENTIALS:   return "Invalid account number or password.";
    case BANK_ERR_LOCKED:            return "This account is locked. Please contact the bank administrator.";
    case BANK_ERR_INSUFFICIENT_FUNDS:return "Insufficient funds.";
    case BANK_ERR_MIN_BALANCE:       return "This would take a Savings account below its minimum balance.";
    case BANK_ERR_DAILY_LIMIT:       return "Daily withdrawal/transfer limit exceeded.";
    case BANK_ERR_BALANCE_LIMIT:     return "Transaction would exceed the maximum allowed balance.";
    case BANK_ERR_SAME_ACCOUNT:      return "Cannot transfer to the same account.";
    case BANK_ERR_RNG:               return "Could not obtain secure random data.";
    case BANK_ERR_DUPLICATE:         return "Duplicate account number.";
    }
    return "Unknown error.";
}

const char *account_type_name(AccountType t)
{
    return t == ACC_SAVINGS ? "Savings" : "Current";
}

const char *txn_type_name(TxnType t)
{
    switch (t) {
    case TXN_OPENING:      return "Opening";
    case TXN_DEPOSIT:      return "Deposit";
    case TXN_WITHDRAW:     return "Withdrawal";
    case TXN_TRANSFER_OUT: return "Transfer out";
    case TXN_TRANSFER_IN:  return "Transfer in";
    case TXN_CLOSURE:      return "Closure payout";
    default:               return "Unknown";
    }
}

bool txn_is_credit(TxnType t)
{
    return t == TXN_OPENING || t == TXN_DEPOSIT || t == TXN_TRANSFER_IN;
}
