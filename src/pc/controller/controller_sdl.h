#ifndef CONTROLLER_SDL_H
#define CONTROLLER_SDL_H

#include "controller_api.h"

#define VK_BASE_SDL_GAMEPAD 0x1000

// mouse buttons are also in the controller namespace, just offset 0x100
#define VK_OFS_SDL_MOUSE 0x0100
#define VK_BASE_SDL_MOUSE (VK_BASE_SDL_GAMEPAD + VK_OFS_SDL_MOUSE)
#define MWHEELUP    0x20
#define MWHEELDOWN  0x40

#ifdef TARGET_VITA
// PS Vita: the corners of the front touch screen and of the rear touch pad act as eight extra
// gamepad buttons that can be bound like any other (VK_BASE_SDL_GAMEPAD + these indices)
#define VITA_TOUCH_BUTTON_BASE 0x20
enum VitaTouchButton {
    VITA_TOUCH_FRONT_TOP_LEFT,
    VITA_TOUCH_FRONT_TOP_RIGHT,
    VITA_TOUCH_FRONT_BOTTOM_LEFT,
    VITA_TOUCH_FRONT_BOTTOM_RIGHT,
    VITA_TOUCH_REAR_TOP_LEFT,
    VITA_TOUCH_REAR_TOP_RIGHT,
    VITA_TOUCH_REAR_BOTTOM_LEFT,
    VITA_TOUCH_REAR_BOTTOM_RIGHT,
    VITA_TOUCH_BUTTON_COUNT
};
#endif

extern struct ControllerAPI controller_sdl;

#endif
