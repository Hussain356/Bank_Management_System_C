/*
 * utils.h - Safe console input, money parsing/formatting, hex and time helpers.
 */
#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef int64_t money_t; /* amounts are stored as integer minor units (paisa/cents) */

#define MONEY_SCALE 100

typedef enum {
    INPUT_OK = 0,
    INPUT_CANCEL,   /* user submitted an empty line where cancelling is allowed */
    INPUT_EOF       /* stdin closed */
} InputResult;

/* ---- money ---- */
bool money_parse(const char *text, money_t *out);              /* "1250.5" -> 125050 */
void money_format(money_t amount, char *out, size_t size);     /* 125050 -> "1,250.50" */

/* ---- console input (all re-prompt on invalid input) ---- */
InputResult input_line(const char *prompt, char *buf, size_t size);       /* empty allowed */
InputResult input_text(const char *prompt, char *buf, size_t size);       /* empty => CANCEL */
InputResult input_choice(const char *prompt, int min, int max, int *out);
InputResult input_money(const char *prompt, money_t *out);                /* empty => CANCEL */
InputResult input_account_number(const char *prompt, uint32_t *out);      /* empty => CANCEL */
InputResult input_password(const char *prompt, char *buf, size_t size);   /* masked */
InputResult input_confirm(const char *prompt, bool *yes);                 /* y/n */

/* ---- misc ---- */
void  hex_encode(const uint8_t *data, size_t n, char *out);               /* out: 2n+1 bytes */
bool  hex_decode(const char *hex, uint8_t *out, size_t n);                /* exactly 2n chars */
void  format_time(time_t t, char *out, size_t size);                      /* "2026-10-18 14:30" */
void  trim_in_place(char *s);
bool  make_directory(const char *path);                                   /* true if exists/created */
void  press_enter_to_continue(void);

#endif /* UTILS_H */
