/*
 * ui.h - Interactive console front-end.
 */
#ifndef UI_H
#define UI_H

#include "bank.h"
#include "storage.h"

typedef struct {
    Bank    bank;
    Storage storage;
} App;

/* Runs the interactive application until the user exits. Returns exit code. */
int ui_run(App *app);

#endif /* UI_H */
