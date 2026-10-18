# Secure Bank — Console Banking System in C

A complete, menu-driven banking application written in **portable C11** with zero external dependencies.
It started as a 150-line two-account prototype and was rebuilt into a modular, tested, security-conscious project:
password authentication, salted password hashing, account lockout, transfers, transaction history,
an administrator console and crash-safe storage.

Runs on **Windows, Linux and macOS**.

---

## Features

**Customers**
- Open a Savings or Current account (name, opening deposit, password)
- Log in with account number + **masked password**
- Deposit, withdraw, **transfer** to another account (re-authenticates with password before sending)
- Account summary, mini statement (last 10), full transaction history
- **Export a statement** to a text file
- Change password

**Administrator**
- Separate admin login (password is created on first run — there is no default password)
- List / inspect accounts, bank-wide summary
- **Unlock** locked accounts, **reset** forgotten customer passwords
- **Close** accounts (any remaining balance is recorded as a cash payout)
- Change the admin password

**Banking rules** (all configurable in `include/bank.h`)

| Rule | Default |
|---|---|
| Minimum opening deposit | PKR 500.00 |
| Savings minimum balance | PKR 500.00 (Current accounts may reach zero) |
| Daily outgoing limit (withdrawals + transfers) | PKR 50,000.00 |
| Failed logins before account lock | 3 |
| Account numbers | Sequential from 10000001, **never reused** |

---

## Quick start

### Linux / macOS
```bash
make            # build ./bank
make test       # run the unit tests
./bank          # run
make demo       # run with sample data (see below)
```

### Windows
Using **MinGW / MSYS2**:
```bash
gcc -std=c11 -Iinclude src/*.c -o bank.exe
bank.exe
```
Or open `BankingSystem.cbp` in **Code::Blocks** and press *Build and run*.

### Options
```
bank [--data-dir DIR] [--demo] [--version] [--help]
```
`--demo` creates a sample database (only if none exists) and prints the credentials:
admin `Admin@1234`, accounts `10000001` / `10000002`, password `Demo@1234`.

On a normal first run you are asked to create the administrator password.

---

## Sample session

```
============================================================
  Secure Bank  |  Mini Statement
============================================================
  Date             Type                      Amount         Balance  Other acct
------------------------------------------------------------
  2026-10-18 14:37 Opening                +1,000.00        1,000.00
  2026-10-18 14:37 Deposit                  +250.75        1,250.75
  2026-10-18 14:37 Withdrawal               -100.00        1,150.75
  2026-10-18 14:37 Transfer out             -300.00          850.75  10000002
------------------------------------------------------------
  Showing 4 of 4 transaction(s).
```

---

## Project structure

```
include/        public headers
src/
  main.c        entry point, CLI arguments
  ui.c          console menus and screens (no business rules)
  bank.c        core banking logic — pure, no I/O, fully unit-tested
  auth.c        password policy, salted+iterated hashing, constant-time compare
  sha256.c      dependency-free SHA-256 (verified against NIST test vectors)
  storage.c     text database, atomic saves, backups, statement export
  utils.c       safe input, masked passwords, money parsing/formatting
tests/
  test_bank.c   150 unit checks, no framework needed
.github/workflows/ci.yml   build + tests on Linux, macOS, Windows
```

The layers only depend downwards (`ui → bank/storage → auth/utils`), so the banking engine can be reused
behind a different front-end (GUI, web API) without changes.

---

## Engineering decisions

- **Money is integer minor units (`int64_t`), never `float`.** Floating point cannot represent values like
  `0.10` exactly and drifts over many operations. Amounts are parsed and printed with exact decimal logic.
- **Safe input everywhere.** All input goes through `fgets` with length limits and validation — no bare
  `scanf`. Over-long lines, non-numeric text, EOF (Ctrl+D / Ctrl+Z) and empty input are all handled.
- **All-or-nothing operations.** Operations validate first, reserve memory second and mutate state last, so a
  failure can never leave an account half-updated. A transfer always creates both ledger entries or neither.
- **Transaction ledger.** Every balance change is recorded with timestamp, type, amount, resulting balance and
  counterparty. A test asserts that transfers conserve the total amount of money.
- **Crash-safe persistence.** Saves write to a temp file, keep the previous database as `bank.db.bak`, then swap
  the new file in. If the main file is missing the backup is loaded. Corrupt files are *detected and reported
  with a line number* and are never overwritten.
- **Strict compilation.** Builds clean with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`, and the test-suite
  runs clean under AddressSanitizer and UBSan (`make debug`).

## Security notes

- Passwords are never stored. Each password gets a **random 16-byte salt** (OS CSPRNG: `/dev/urandom` /
  `rand_s`) and is hashed with **50,000 iterations of SHA-256**; verification is **constant-time**.
- **Password policy:** 8–64 characters, upper-case + lower-case + digit, no spaces.
- **Lockout:** 3 wrong passwords lock the account (persisted across restarts). Only the administrator can unlock it.
- Unknown account numbers and wrong passwords produce the **same message and same work**, so the login form does
  not reveal which account numbers exist. Transfer recipients are shown with a masked name.
- Sensitive buffers are wiped after use; the terminal echo is restored even on Ctrl+C.

**Honest limitations** — this is a learning/portfolio project, not production banking software:
iterated SHA-256 is far better than plain hashing but a production system should use a vetted KDF such as
Argon2id or bcrypt; the database is a plain-text file with no encryption at rest or multi-process locking;
and the admin login has no persistent rate-limit (only a per-attempt limit per menu visit).

## Data file format

Human-readable and diff-friendly, pipe-delimited (`data/bank.db`):

```
BANKDB|1
META|<next_account_number>|<next_txn_id>
ADMIN|<salt_hex>|<hash_hex>
ACCOUNT|<number>|<name>|<type>|<balance_minor_units>|<salt_hex>|<hash_hex>|<failed>|<locked>|<created>
TXN|<id>|<time>|<account>|<type>|<amount>|<balance_after>|<counterparty>
```

## Testing

```bash
make test     # 150 checks
make debug    # same tests under AddressSanitizer + UBSan
```
Covers SHA-256 known-answer vectors, money parsing/formatting, password policy, hashing, account rules, lockout,
daily limits, transfers, password changes, account closure, and storage round-trips including corrupted files
and backup recovery.

## Roadmap ideas

Interest accrual for Savings, fixed-deposit accounts, per-account PINs for ATM mode, CSV export,
a stronger KDF (Argon2id) behind the same `auth.h` interface, and a GUI / REST front-end over `bank.c`.

## License

MIT — see [LICENSE](LICENSE).
