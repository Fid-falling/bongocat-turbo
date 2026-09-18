#include "macos_keys.h"

#include <stdio.h>

typedef struct MacKeyName { CGKeyCode code; const char *name; } MacKeyName;

const char *bongo_cat_macos_key_name(CGKeyCode code, char output[16]) {
    if (code == 105) return "PrintScreen";
    if (code == 107) return "ScrollLock";
    if (code == 113) return "Pause";
    static const MacKeyName keys[] = {
        {0,"KeyA"},{1,"KeyS"},{2,"KeyD"},{3,"KeyF"},{4,"KeyH"},{5,"KeyG"},
        {6,"KeyZ"},{7,"KeyX"},{8,"KeyC"},{9,"KeyV"},{11,"KeyB"},{12,"KeyQ"},
        {13,"KeyW"},{14,"KeyE"},{15,"KeyR"},{16,"KeyY"},{17,"KeyT"},
        {18,"Num1"},{19,"Num2"},{20,"Num3"},{21,"Num4"},{22,"Num6"},
        {23,"Num5"},{24,"Equal"},{25,"Num9"},{26,"Num7"},{27,"Minus"},
        {28,"Num8"},{29,"Num0"},{30,"BracketRight"},{31,"KeyO"},{32,"KeyU"},
        {33,"BracketLeft"},{34,"KeyI"},{35,"KeyP"},{36,"Return"},{37,"KeyL"},
        {38,"KeyJ"},{39,"Quote"},{40,"KeyK"},{41,"Semicolon"},{42,"Backslash"},
        {43,"Comma"},{44,"Slash"},{45,"KeyN"},{46,"KeyM"},{47,"Period"},
        {48,"Tab"},{49,"Space"},{50,"BackQuote"},{51,"Backspace"},{53,"Escape"},
        {54,"Meta"},{55,"Meta"},{56,"ShiftLeft"},{57,"CapsLock"},{58,"Alt"},
        {59,"ControlLeft"},{60,"ShiftRight"},{61,"AltGr"},{62,"ControlRight"},
        {65,"KpDecimal"},{67,"KpMultiply"},{69,"KpPlus"},{71,"NumLock"},
        {75,"KpDivide"},{76,"Return"},{78,"KpMinus"},{81,"Equal"},
        {82,"Kp0"},{83,"Kp1"},{84,"Kp2"},{85,"Kp3"},{86,"Kp4"},
        {87,"Kp5"},{88,"Kp6"},{89,"Kp7"},{91,"Kp8"},{92,"Kp9"},
        {114,"Insert"},{115,"Home"},{116,"PageUp"},{117,"Delete"},{119,"End"},
        {121,"PageDown"},{123,"LeftArrow"},{124,"RightArrow"},{125,"DownArrow"},
        {126,"UpArrow"}
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (keys[i].code == code) return keys[i].name;
    static const CGKeyCode function_codes[] = {122,120,99,118,96,97,98,100,101,
        109,103,111,105,107,113,106,64,79,80,90};
    for (size_t i = 0; i < sizeof(function_codes) / sizeof(function_codes[0]); ++i) {
        if (function_codes[i] != code) continue;
        snprintf(output, 16, "F%zu", i + 1);
        return output;
    }
    return NULL;
}
