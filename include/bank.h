/*
 * bank.h - Core banking domain logic. Pure logic: no console or file I/O,
 * which keeps it easy to unit-test and reuse.
 */
#ifndef BANK_H
#define BANK_H

#include "auth.h"
#include "utils.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* ------------------------------------------------------------ policy */
#define BANK_NAME                 "Secure Bank"
#define BANK_CURRENCY             "PKR"
#define FIRST_ACCOUNT_NUMBER      10000001u
#define NAME_MIN_LEN              2
#define NAME_MAX_LEN              49
#define MAX_LOGIN_ATTEMPTS        3
#define MIN_OPENING_DEPOSIT       ((money_t)50000)        /* 500.00  */
#define SAVINGS_MIN_BALANCE       ((money_t)50000)        /* 500.00  */
#define DAILY_OUTGOING_LIMIT      ((money_t)5000000)      /* 50,000.00 withdraw + transfer per day */
#define MAX_BALANCE               ((money_t)100000000000) /* 1,000,000,000.00 */

typedef enum { ACC_SAVINGS = 0, ACC_CURRENT = 1 } AccountType;

typedef enum {
    TXN_OPENING = 0,
    TXN_DEPOSIT,
    TXN_WITHDRAW,
    TXN_TRANSFER_OUT,
    TXN_TRANSFER_IN,
    TXN_CLOSURE,
    TXN_TYPE_COUNT
} TxnType;

typedef enum {
    BANK_OK = 0,
    BANK_ERR_NOMEM,
    BANK_ERR_INVALID_AMOUNT,
    BANK_ERR_INVALID_NAME,
    BANK_ERR_INVALID_TYPE,
    BANK_ERR_OPENING_DEPOSIT,
    BANK_ERR_WEAK_PASSWORD,
    BANK_ERR_NOT_FOUND,
    BANK_ERR_BAD_CREDENTIALS,
    BANK_ERR_LOCKED,
    BANK_ERR_INSUFFICIENT_FUNDS,
    BANK_ERR_MIN_BALANCE,
    BANK_ERR_DAILY_LIMIT,
    BANK_ERR_BALANCE_LIMIT,
    BANK_ERR_SAME_ACCOUNT,
    BANK_ERR_RNG,
    BANK_ERR_DUPLICATE
} BankStatus;

typedef struct {
    uint32_t    number;
    char        name[NAME_MAX_LEN + 1];
    AccountType type;
    money_t     balance;
    Credential  cred;
    int         failed_attempts;
    bool        locked;
    time_t      created;
} Account;

typedef struct {
    uint64_t id;
    time_t   when;
    uint32_t account;
    TxnType  type;
    money_t  amount;        /* always positive; direction is implied by `type` */
    money_t  balance_after;
    uint32_t counterparty;  /* other account for transfers, otherwise 0 */
} Transaction;

typedef struct {
    Account     *accounts;
    size_t       n_accounts, cap_accounts;
    Transaction *txns;
    size_t       n_txns, cap_txns;
    uint32_t     next_account_number;
    uint64_t     next_txn_id;
    Credential   admin;
    bool         has_admin;
} Bank;

/* ------------------------------------------------------ lifecycle */
void bank_init(Bank *b);
void bank_free(Bank *b);

/* ---------------------------------------------------------- lookup */
Account *bank_find(const Bank *b, uint32_t number);
money_t  bank_total_deposits(const Bank *b);
money_t  bank_min_balance(AccountType type);
money_t  bank_daily_outgoing(const Bank *b, uint32_t number, time_t now);

/* ------------------------------------------------- customer actions */
BankStatus bank_open_account(Bank *b, const char *name, AccountType type, money_t deposit,
                             const char *password, time_t now, uint32_t *out_number);
/* `modified` is set when persistent state changed (failed counter, lock). */
BankStatus bank_login(Bank *b, uint32_t number, const char *password,
                      int *attempts_left, bool *modified);
BankStatus bank_deposit(Bank *b, uint32_t number, money_t amount, time_t now);
BankStatus bank_withdraw(Bank *b, uint32_t number, money_t amount, time_t now);
BankStatus bank_transfer(Bank *b, uint32_t from, uint32_t to, money_t amount, time_t now);
BankStatus bank_change_password(Bank *b, uint32_t number, const char *old_pw,
                                const char *new_pw, bool *modified);

/* -------------------------------------------------- admin actions */
BankStatus bank_set_admin_password(Bank *b, const char *password);
bool       bank_admin_verify(const Bank *b, const char *password);
BankStatus bank_unlock(Bank *b, uint32_t number);
BankStatus bank_admin_reset_password(Bank *b, uint32_t number, const char *new_pw);
/* Closes the account; any remaining balance is recorded as a payout. */
BankStatus bank_close_account(Bank *b, uint32_t number, time_t now, money_t *payout);

/* -------------------------------------------- persistence helpers */
BankStatus bank_add_account_raw(Bank *b, const Account *a);
BankStatus bank_add_txn_raw(Bank *b, const Transaction *t);

/* --------------------------------------------------- presentation */
const char *bank_strerror(BankStatus s);
const char *account_type_name(AccountType t);
const char *txn_type_name(TxnType t);
bool        txn_is_credit(TxnType t);
bool        bank_valid_name(const char *name);

#endif /* BANK_H */
