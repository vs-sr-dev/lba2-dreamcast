// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// V2 Maple controller backend. Mirrors the bit-packed virtual-scancode contract
// used by the SDL gamepad backend (SOURCES/JOYSTICK.CPP) so the engine's
// CheckKey() / GetInput() consume DC pad state with no other call-site changes.
//
// In addition to the K_GAMEPAD_* virtual scancodes, we also poke the physical
// scancodes that the default LBA2.CFG bindings expect (K_ENTER, K_SPACE, K_ESC,
// K_UP/DOWN/LEFT/RIGHT) so dialogs advance and Twinsen moves out of the box
// without requiring the user to enter a remap menu they can't currently use.

#include "JOYSTICK.H"

#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/LOGPRINT.H>

#include <dc/maple.h>
#include <dc/maple/controller.h>

#ifdef __cplusplus
extern "C" {
#endif

S32 JoystickDeadzone = 32;  // KOS controller axes are signed 8-bit, range [-128, 127].

static U32 s_firstPressedThisFrame = 0;

// Mirror of KEYBOARD.CPP's bit-packing for virtual scancodes (>= 256).
#define PAD_KEY_INDEX(x) (256 - 32 + ((x) >> 3))
#define PAD_KEY_BIT(x)   (1 << ((x) & 7))

static inline void SetVirtualKeyDown(U32 scancode) {
    TabKeys[PAD_KEY_INDEX(scancode)] |= (U8)PAD_KEY_BIT(scancode);
}

static inline void ClearGamepadBits(void) {
    U32 first = PAD_KEY_INDEX(K_GAMEPAD_BASE);
    U32 last  = PAD_KEY_INDEX(K_GAMEPAD_BASE + K_GAMEPAD_COUNT - 1);
    for (U32 i = first; i <= last; i++) {
        TabKeys[i] = 0;
    }
}

// Resolve the first connected DC controller via Maple bus enumeration.
static maple_device_t *GetController(void) {
    return maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
}

void InitJoystick(void) {
    maple_device_t *c = GetController();
    if (c) {
        LogPrintf("Gamepad connected: %s (Maple port %c%d)\n",
                  c->info.product_name, 'A' + c->port, c->unit);
    } else {
        LogPuts("No gamepad detected on Maple bus.");
    }
}

void EndJoystick(void) {
    // Maple is owned by KOS; nothing to release.
}

void DetectJoys(void) {
    InitJoystick();
}

void HandleEventsJoystick(const void *event) {
    (void)event;  // KOS Maple polling is implicit; no SDL-style hot-plug events here.
}

void UpdateJoystick(void) {
    // Snapshot prior gamepad bits for edge detection.
    U32 firstByte = PAD_KEY_INDEX(K_GAMEPAD_BASE);
    U32 lastByte  = PAD_KEY_INDEX(K_GAMEPAD_BASE + K_GAMEPAD_COUNT - 1);
    U8 prevBits[8];
    for (U32 i = firstByte; i <= lastByte; i++) {
        prevBits[i - firstByte] = TabKeys[i];
    }
    ClearGamepadBits();
    s_firstPressedThisFrame = 0;

    maple_device_t *cont = GetController();
    if (!cont) return;
    cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
    if (!st) return;

    const uint32_t b = st->buttons;

    // Face buttons
    if (b & CONT_A)     SetVirtualKeyDown(K_GAMEPAD_A);
    if (b & CONT_B)     SetVirtualKeyDown(K_GAMEPAD_B);
    if (b & CONT_X)     SetVirtualKeyDown(K_GAMEPAD_X);
    if (b & CONT_Y)     SetVirtualKeyDown(K_GAMEPAD_Y);
    if (b & CONT_START) SetVirtualKeyDown(K_GAMEPAD_START);

    // V2.3 swap: DPad = movement, Stick = spell quick-cast.
    // Reason: real DC pad analog sticks have varying tolerances; jitter around
    // a held position binarizes into rapid LEFT/RIGHT flips and Twinsen ends
    // up rotating in place even when the stick is held steady. Digital DPad
    // is rock-solid for tank-control rotate/walk inputs.
    if (b & CONT_DPAD_UP)    SetVirtualKeyDown(K_GAMEPAD_LSTICK_UP);
    if (b & CONT_DPAD_DOWN)  SetVirtualKeyDown(K_GAMEPAD_LSTICK_DOWN);
    if (b & CONT_DPAD_LEFT)  SetVirtualKeyDown(K_GAMEPAD_LSTICK_LEFT);
    if (b & CONT_DPAD_RIGHT) SetVirtualKeyDown(K_GAMEPAD_LSTICK_RIGHT);

    // Analog stick → DPAD_* virtual keys, which in LBA2.CFG are bound to the
    // 4 magic-ball spells (DPAD_RIGHT=Pingouin, DOWN=Jetpack, UP=Protection,
    // LEFT=Foudre). Stick jitter on these is harmless — spells are one-shot
    // triggers consumed by the engine in a single tick.
    const int dz = JoystickDeadzone;
    if (st->joyx < -dz)  SetVirtualKeyDown(K_GAMEPAD_DPAD_LEFT);
    if (st->joyx >  dz)  SetVirtualKeyDown(K_GAMEPAD_DPAD_RIGHT);
    if (st->joyy < -dz)  SetVirtualKeyDown(K_GAMEPAD_DPAD_UP);
    if (st->joyy >  dz)  SetVirtualKeyDown(K_GAMEPAD_DPAD_DOWN);

    // Triggers (DC has analog L/R triggers, no separate shoulder buttons).
    // Map both the trigger scancode and the shoulder scancode so engine bindings
    // that target either work on DC.
    if (st->ltrig > 32) { SetVirtualKeyDown(K_GAMEPAD_LTRIGGER); SetVirtualKeyDown(K_GAMEPAD_LSHOULDER); }
    if (st->rtrig > 32) { SetVirtualKeyDown(K_GAMEPAD_RTRIGGER); SetVirtualKeyDown(K_GAMEPAD_RSHOULDER); }

    // Edge-detect first newly-pressed bit this frame.
    for (U32 i = firstByte; i <= lastByte; i++) {
        U8 pressed = (U8)(TabKeys[i] & ~prevBits[i - firstByte]);
        if (pressed) {
            for (U32 bit = 0; bit < 8; bit++) {
                if (pressed & (1u << bit)) {
                    s_firstPressedThisFrame = ((i - (256 - 32)) << 3) | bit;
                    break;
                }
            }
            if (s_firstPressedThisFrame) break;
        }
    }

    // Physical scancode mirror (K_ENTER/K_SPACE/K_ESC/arrows for default LBA2.CFG
    // ergonomics) lives in LIB386/SYSTEM/KOS_BACKEND.CPP::ManageKeyboard, which
    // runs AFTER UpdateJoystick and would otherwise wipe anything we wrote here.
}

void GetJoys(U32 *bitfield) {
    (void)bitfield;
    UpdateJoystick();
}

S32 JoystickIsPresent(void) {
    return GetController() ? 1 : 0;
}

S32 JoystickMenuAction(void) {
    maple_device_t *cont = GetController();
    if (!cont) return 0;
    cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
    if (!st) return 0;
    if (st->buttons & (CONT_A | CONT_START)) return K_ENTER;
    return JoystickMenuNavOnly();
}

S32 JoystickMenuNavOnly(void) {
    maple_device_t *cont = GetController();
    if (!cont) return 0;
    cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
    if (!st) return 0;
    const int dz = JoystickDeadzone;
    if ((st->buttons & CONT_DPAD_UP)    || st->joyy < -dz) return K_UP;
    if ((st->buttons & CONT_DPAD_DOWN)  || st->joyy >  dz) return K_DOWN;
    if ((st->buttons & CONT_DPAD_LEFT)  || st->joyx < -dz) return K_LEFT;
    if ((st->buttons & CONT_DPAD_RIGHT) || st->joyx >  dz) return K_RIGHT;
    return 0;
}

U32 JoystickFirstPressedScancode(void) {
    return s_firstPressedThisFrame;
}

#ifdef __cplusplus
}
#endif
