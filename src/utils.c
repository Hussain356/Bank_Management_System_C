/*
 * utils.c - Console input and formatting helpers.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "utils.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <conio.h>
#include <direct.h>
#include <io.h>
#define IS_TTY(f) _isatty(_fileno(f))
#else
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#define IS_TTY(f) isatty(fileno(f))
#endif

#define LINE_BUFFER 256
#define MAX_MONEY_WHOLE INT64_C(1000000000000) /* 1 trillion units: far below overflow */

/* ------------------------------------------------------------------ money */

bool money_parse(const char *text, money_t *out)
{
    int64_t whole = 0, frac = 0;
    int frac_digits = 0, whole_digits = 0;
    const char *p = text;

    if (!text || !*p) return false;
    while (isdigit((unsigned char)*p)) {
        whole = whole * 10 + (*p - '0');
        whole_digits++;
        if (whole > MAX_MONEY_WHOLE) return false;
        p++;
    }
    if (whole_digits == 0) return false;
    if (*p == '.') {
        p++;
        while (isdigit((unsigned char)*p)) {
            if (++frac_digits > 2) return false;
            frac = frac * 10 + (*p - '0');
            p++;
        }
        if (frac_digits == 0) return false;
        if (frac_digits == 1) frac *= 10;
    }
    if (*p != '\0') return false;
    *out = whole * MONEY_SCALE + frac;
    return true;
}

void money_format(money_t amount, char *out, size_t size)
{
    char digits[32], grouped[48];
    uint64_t mag = amount < 0 ? (uint64_t)(-(amount + 1)) + 1u : (uint64_t)amount;
    uint64_t whole = mag / MONEY_SCALE;
    unsigned frac = (unsigned)(mag % MONEY_SCALE);
    size_t len, i, j = 0;

    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)whole);
    len = strlen(digits);
    for (i = 0; i < len; i++) {
        if (i > 0 && (len - i) % 3 == 0) grouped[j++] = ',';
        grouped[j++] = digits[i];
    }
    grouped[j] = '\0';
    snprintf(out, size, "%s%s.%02u", amount < 0 ? "-" : "", grouped, frac);
}

/* ----------------------------------------------------------- raw reading */

typedef enum { READ_OK, READ_EOF, READ_TOO_LONG } ReadStatus;

static ReadStatus read_raw(char *buf, size_t size)
{
    size_t len;
    if (!fgets(buf, (int)size, stdin)) return READ_EOF;
    len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[--len] = '\0';
        if (len > 0 && buf[len - 1] == '\r') buf[--len] = '\0';
        return READ_OK;
    }
    if (feof(stdin)) return READ_OK; /* last line without newline */
    {   /* line longer than the buffer: discard the remainder */
        int c;
        while ((c = getchar()) != '\n' && c != EOF) { }
    }
    return READ_TOO_LONG;
}

void trim_in_place(char *s)
{
    size_t len = strlen(s), start = 0;
    while (len > 0 && isspace((unsigned char)s[len - 1])) s[--len] = '\0';
    while (s[start] && isspace((unsigned char)s[start])) start++;
    if (start) memmove(s, s + start, strlen(s + start) + 1);
}

InputResult input_line(const char *prompt, char *buf, size_t size)
{
    for (;;) {
        ReadStatus st;
        printf("%s", prompt);
        fflush(stdout);
        st = read_raw(buf, size);
        if (st == READ_EOF) return INPUT_EOF;
        if (st == READ_TOO_LONG) {
            printf("  ! Input too long (max %u characters).\n", (unsigned)(size - 1));
            continue;
        }
        trim_in_place(buf);
        return INPUT_OK;
    }
}

InputResult input_text(const char *prompt, char *buf, size_t size)
{
    InputResult r = input_line(prompt, buf, size);
    if (r == INPUT_OK && buf[0] == '\0') return INPUT_CANCEL;
    return r;
}

InputResult input_choice(const char *prompt, int min, int max, int *out)
{
    char line[LINE_BUFFER];
    for (;;) {
        InputResult r = input_line(prompt, line, sizeof(line));
        char *end;
        long v;
        if (r != INPUT_OK) return r;
        errno = 0;
        v = strtol(line, &end, 10);
        if (line[0] != '\0' && *end == '\0' && errno == 0 && v >= min && v <= max) {
            *out = (int)v;
            return INPUT_OK;
        }
        printf("  ! Please enter a number between %d and %d.\n", min, max);
    }
}

InputResult input_money(const char *prompt, money_t *out)
{
    char line[LINE_BUFFER];
    for (;;) {
        InputResult r = input_line(prompt, line, sizeof(line));
        if (r != INPUT_OK) return r;
        if (line[0] == '\0') return INPUT_CANCEL;
        if (money_parse(line, out) && *out > 0) return INPUT_OK;
        printf("  ! Enter a positive amount with at most 2 decimals (e.g. 2500 or 99.50), or press Enter to cancel.\n");
    }
}

InputResult input_account_number(const char *prompt, uint32_t *out)
{
    char line[LINE_BUFFER];
    for (;;) {
        InputResult r = input_line(prompt, line, sizeof(line));
        char *end;
        unsigned long v;
        if (r != INPUT_OK) return r;
        if (line[0] == '\0') return INPUT_CANCEL;
        errno = 0;
        v = strtoul(line, &end, 10);
        if (isdigit((unsigned char)line[0]) && *end == '\0' && errno == 0 && v > 0 && v <= 4294967295ul) {
            *out = (uint32_t)v;
            return INPUT_OK;
        }
        printf("  ! Account numbers contain digits only. Press Enter to cancel.\n");
    }
}

InputResult input_confirm(const char *prompt, bool *yes)
{
    char line[LINE_BUFFER];
    for (;;) {
        InputResult r = input_line(prompt, line, sizeof(line));
        if (r != INPUT_OK) return r;
        if (line[0] == 'y' || line[0] == 'Y') { *yes = true;  return INPUT_OK; }
        if (line[0] == 'n' || line[0] == 'N' || line[0] == '\0') { *yes = false; return INPUT_OK; }
        printf("  ! Please answer y or n.\n");
    }
}

/* ------------------------------------------------------- masked password */

#ifdef _WIN32

InputResult input_password(const char *prompt, char *buf, size_t size)
{
    for (;;) {
        size_t len = 0;
        if (!IS_TTY(stdin)) {
            ReadStatus st;
            printf("%s", prompt);
            fflush(stdout);
            st = read_raw(buf, size);
            if (st == READ_EOF) return INPUT_EOF;
            if (st == READ_TOO_LONG) { printf("  ! Input too long.\n"); continue; }
            return INPUT_OK;
        }
        printf("%s", prompt);
        fflush(stdout);
        for (;;) {
            int c = _getch();
            if (c == '\r' || c == '\n') break;
            if (c == 3) { printf("\n"); exit(130); }         /* Ctrl+C */
            if (c == 0 || c == 224) { (void)_getch(); continue; } /* function keys */
            if (c == '\b') {
                if (len > 0) { len--; printf("\b \b"); }
                continue;
            }
            if (len + 1 < size && isprint(c)) { buf[len++] = (char)c; putchar('*'); }
        }
        buf[len] = '\0';
        printf("\n");
        return INPUT_OK;
    }
}

#else

static struct termios g_saved_termios;
static volatile sig_atomic_t g_echo_disabled = 0;

static void restore_terminal(void)
{
    if (g_echo_disabled) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
        g_echo_disabled = 0;
    }
}

static void on_sigint(int sig)
{
    restore_terminal();
    signal(sig, SIG_DFL);
    raise(sig);
}

InputResult input_password(const char *prompt, char *buf, size_t size)
{
    for (;;) {
        ReadStatus st;
        bool tty = IS_TTY(stdin);
        void (*old_handler)(int) = SIG_DFL;

        printf("%s", prompt);
        fflush(stdout);
        if (tty && tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
            struct termios t = g_saved_termios;
            t.c_lflag &= (tcflag_t)~ECHO;
            old_handler = signal(SIGINT, on_sigint);
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) == 0) g_echo_disabled = 1;
        }
        st = read_raw(buf, size);
        if (g_echo_disabled) {
            restore_terminal();
            signal(SIGINT, old_handler);
            printf("\n");
        }
        if (st == READ_EOF) return INPUT_EOF;
        if (st == READ_TOO_LONG) { printf("  ! Input too long.\n"); continue; }
        return INPUT_OK;
    }
}

#endif

void press_enter_to_continue(void)
{
    char line[LINE_BUFFER];
    printf("\nPress Enter to continue...");
    fflush(stdout);
    (void)read_raw(line, sizeof(line));
}

/* ------------------------------------------------------------ hex / time */

void hex_encode(const uint8_t *data, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; i++) {
        out[i * 2]     = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 0x0f];
    }
    out[n * 2] = '\0';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hex_decode(const char *hex, uint8_t *out, size_t n)
{
    size_t i;
    if (strlen(hex) != n * 2) return false;
    for (i = 0; i < n; i++) {
        int hi = hex_val(hex[i * 2]), lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void format_time(time_t t, char *out, size_t size)
{
    struct tm *tm = localtime(&t);
    if (!tm || strftime(out, size, "%Y-%m-%d %H:%M", tm) == 0) {
        snprintf(out, size, "unknown");
    }
}

bool make_directory(const char *path)
{
#ifdef _WIN32
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0700) == 0) return true;
#endif
    return errno == EEXIST;
}
