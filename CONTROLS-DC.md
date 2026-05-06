# Controls — Dreamcast (V2.9.6)

The DC layout is hardcoded in `SOURCES/INPUT.CPP`; the engine's `LBA2.CFG`
gamepad lines are ignored on this target because the Dreamcast has no remap
UI and a stale CFG written by a prior build silently overrode the
compiled-in defaults.

## Gameplay

| Button             | Action                            |
|--------------------|-----------------------------------|
| **D-Pad ←/→**      | Rotate Twinsen                    |
| **D-Pad ↑**        | Forward                           |
| **D-Pad ↓**        | Backward                          |
| **A**              | Camera recenter (`I_RETURN`)      |
| **B**              | Dodge (`I_ESQUIVE`)               |
| **X**              | Action / Pick up (`I_ACTION_M`)   |
| **Y**              | Inventory                         |
| **Start**          | Pause menu                        |
| **R trigger**      | Up + Throw (jump up + throw item) |
| **L trigger**      | Behaviour cycle (`I_COMPORTEMENT`)|
| **Stick ↑**        | Shortcut: Protection spell        |
| **Stick ↓**        | Shortcut: Jetpack spell           |
| **Stick ←/→**      | Camera (`I_RETURN`/`I_ACTION_ALWAYS`)|

Notes:

- Only **two** spell shortcuts are wired on the stick (Protection, Jetpack).
  The other two LBA2 spells (Pingouin, Foudre) are situational and were
  dropped from the layout.
- The **L trigger** is bound to `I_COMPORTEMENT` only. An earlier build
  also bound it to `I_DOWN`, which deadlocked `COMPORTE.CPP`'s
  `while (Input & I_JOY)` wait loop because the DC analog trigger fires
  *both* `K_GAMEPAD_LTRIGGER` and `K_GAMEPAD_LSHOULDER` virtual scancodes
  on the same physical press — see DEVLOG.

## Menus

| Button   | Action                                                |
|----------|-------------------------------------------------------|
| **D-Pad ↑/↓** | Navigate                                         |
| **A**    | Confirm                                               |
| **B**    | Back / close menu                                     |
| **X**    | Confirm (alt)                                         |
| **Start**| Close menu                                            |

## Soft keyboard (save name input)

Triggered when saving a new game. The DC has no real keyboard support
wired in, so character entry is one-glyph-at-a-time:

| Button       | Action                                              |
|--------------|-----------------------------------------------------|
| **D-Pad / Stick ←/→** | Cycle the current glyph (A-Z, 0-9, space) |
| **A**        | Append the current glyph to the name                |
| **B**        | Backspace                                           |
| **X**        | Confirm and save                                    |
| **Start**    | Cancel                                              |

The hint string at the bottom of the screen currently overflows the visible
area on both sides — cosmetic, on the known-issues list.

## Save system

- VMU slot **A1** only.
- Saves use the `vmu_pkg` format and are recognized by the Dreamcast BIOS as
  *"LBA2 Save"*.
- File names are mangled from `<player>.lba` → `LBA2_<6-upper>` to fit the
  12-char VMU constraint.
- **Manual saves only.** Auto-save was disabled on DC because VMU flash
  writes at 1–5 KB/s would freeze the engine for 6–7 s on every Start press.
- Loading reads the VMU directory once at boot and caches presence; per-menu
  navigation no longer pays the ~50–100 ms Maple-bus lookup for missing
  files.
