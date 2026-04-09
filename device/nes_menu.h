/*
 * ThumbyNES — generic in-game pause menu.
 *
 * A small reusable UI module that takes a list of items + current
 * values and runs a modal event loop on top of the existing 128×128
 * framebuffer. The frozen game frame stays visible behind a darkened
 * overlay so the user has context.
 *
 * Items are config-driven: each runner builds its own list with
 * pointers into its own state, hands the list to nes_menu_run(),
 * and gets back either NES_MENU_RESUME (close menu, resume cart) or
 * NES_MENU_ACTION (with the action_id of whichever Action item the
 * user activated). The menu mutates *value_ptr in place for
 * Toggle / Slider / Choice items so the runner sees the updated
 * values as soon as the menu returns.
 */
#ifndef THUMBYNES_MENU_H
#define THUMBYNES_MENU_H

#include <stdbool.h>
#include <stdint.h>

#define NES_MENU_MAX_ITEMS  16

typedef enum {
    NES_MENU_KIND_ACTION,    /* A activates; returns action_id to caller */
    NES_MENU_KIND_TOGGLE,    /* bool: LEFT/RIGHT or A flips           */
    NES_MENU_KIND_SLIDER,    /* int with min/max                       */
    NES_MENU_KIND_CHOICE,    /* int index into named choices array     */
} nes_menu_kind_t;

typedef struct {
    nes_menu_kind_t   kind;
    const char       *label;
    int              *value_ptr;       /* TOGGLE / SLIDER / CHOICE */
    int               min, max;        /* SLIDER */
    const char *const *choices;        /* CHOICE — array of label strings */
    int               num_choices;
    bool              enabled;         /* greyed-out + unselectable when false */
    int               action_id;       /* ACTION — returned to caller */
    const char       *suffix;          /* optional trailing hint, e.g. "next launch" */
} nes_menu_item_t;

typedef enum {
    NES_MENU_RESUME = 0,    /* close menu, resume cart            */
    NES_MENU_ACTION = 1,    /* an Action item fired; check action_id */
} nes_menu_result_kind_t;

typedef struct {
    nes_menu_result_kind_t kind;
    int                    action_id;
} nes_menu_result_t;

/* Run the modal menu loop. `fb` is the live 128×128 framebuffer; on
 * entry it should hold the most recent game frame so the darkened
 * overlay has something interesting underneath. The menu draws every
 * frame at ~60 fps and pumps tud_task() between frames so USB stays
 * alive while paused. Returns when the user picks Resume / activates
 * an Action / presses MENU again. */
nes_menu_result_t nes_menu_run(uint16_t          *fb,
                                const char        *title,
                                const char        *subtitle,
                                nes_menu_item_t   *items,
                                int                n_items);

#endif
