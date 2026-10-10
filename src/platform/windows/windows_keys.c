#include "windows_keys.h"

#ifdef _WIN32
#include <stdio.h>

/*
 * 原始键码 -> 事件名(用户在配置里看到的名字)与"设备内索引"。
 *
 * 名字是外部契约: 它与 shortcuts / sound_shortcut 的 token 表、以及用户已经写进
 * settings.json 的字符串一一对应, 只能新增不能改。
 * 索引用于给每台设备记录按键状态, 所以同一个物理键必须永远映射到同一个下标。
 */
unsigned bongo_cat_windows_key_index(const RAWKEYBOARD *key) {
    if (key->MakeCode && key->MakeCode < 256)
        return key->MakeCode + ((key->Flags & RI_KEY_E1) ? 512u :
            (key->Flags & RI_KEY_E0) ? 256u : 0u);
    return 768u + key->VKey;
}

/* 索引把 MakeCode 与 E0/E1 前缀拼在一起(上限见 windows_keys.h 的 1024), 这样小键盘
   与主键区、以及左右修饰键都能区分开; 没有有效 MakeCode 的报文退到 VKey 段。 */
const char *bongo_cat_windows_key_name(const RAWKEYBOARD *key, char output[16]) {
    if (!key || key->VKey < VK_BACK || key->VKey >= 255 ||
        key->MakeCode == KEYBOARD_OVERRUN_MAKE_CODE) return NULL;
    /* 先用 VKey 处理与布局无关的通用键(字母 / 数字 / 功能键), 再单独处理小键盘 ——
       小键盘在 NumLock 关闭时会伪装成方向键与编辑键, 所以那一段必须按 VKey 判断。 */
    unsigned code = key->VKey;
    if (code >= 'A' && code <= 'Z') {
        snprintf(output, 16, "Key%c", (char)code);
        return output;
    }
    if (code >= '0' && code <= '9') {
        snprintf(output, 16, "Num%c", (char)code);
        return output;
    }
    if (code >= VK_F1 && code <= VK_F24) {
        snprintf(output, 16, "F%u", code - VK_F1 + 1);
        return output;
    }
    if (!(key->Flags & RI_KEY_E0)) switch (code) {
    case VK_INSERT: return "Kp0"; case VK_END: return "Kp1";
    case VK_DOWN: return "Kp2"; case VK_NEXT: return "Kp3";
    case VK_LEFT: return "Kp4"; case VK_CLEAR: return "Kp5";
    case VK_RIGHT: return "Kp6"; case VK_HOME: return "Kp7";
    case VK_UP: return "Kp8"; case VK_PRIOR: return "Kp9";
    case VK_DELETE: return "KpDecimal"; default: break;
    }
    /* 主映射表。左右修饰键的位置很关键: 系统有时只给通用的 VK_SHIFT / VK_CONTROL /
       VK_MENU, 这时只能靠 MakeCode 或 E0 标志区分左右, 所以这几条要放在具体
       VK_LSHIFT 之类的分支之后、default 之前。 */
    switch (code) {
    case VK_ESCAPE: return "Escape";
    case VK_PAUSE: return "Pause";
    case VK_SNAPSHOT: return "PrintScreen";
    case VK_APPS: return "Apps";
    case VK_TAB: return "Tab";
    case VK_CAPITAL: return "CapsLock";
    case VK_SPACE: return "Space";
    case VK_BACK: return "Backspace";
    case VK_DELETE: return "Delete";
    case VK_INSERT: return "Insert";
    case VK_HOME: return "Home";
    case VK_END: return "End";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_UP: return "UpArrow";
    case VK_DOWN: return "DownArrow";
    case VK_LEFT: return "LeftArrow";
    case VK_RIGHT: return "RightArrow";
    case VK_LWIN: case VK_RWIN: return "Meta";
    case VK_LSHIFT: return "ShiftLeft";
    case VK_RSHIFT: return "ShiftRight";
    case VK_LCONTROL: return "ControlLeft";
    case VK_RCONTROL: return "ControlRight";
    case VK_LMENU: return "Alt";
    case VK_RMENU: return "AltGr";
    case VK_SHIFT: return key->MakeCode == 0x36 ? "ShiftRight" : "ShiftLeft";
    case VK_CONTROL: return key->Flags & RI_KEY_E0 ? "ControlRight" : "ControlLeft";
    case VK_MENU: return key->Flags & RI_KEY_E0 ? "AltGr" : "Alt";
    case VK_RETURN: return "Return";
    case VK_NUMLOCK: return "NumLock";
    case VK_SCROLL: return "ScrollLock";
    case VK_NUMPAD0: return "Kp0"; case VK_NUMPAD1: return "Kp1";
    case VK_NUMPAD2: return "Kp2"; case VK_NUMPAD3: return "Kp3";
    case VK_NUMPAD4: return "Kp4"; case VK_NUMPAD5: return "Kp5";
    case VK_NUMPAD6: return "Kp6"; case VK_NUMPAD7: return "Kp7";
    case VK_NUMPAD8: return "Kp8"; case VK_NUMPAD9: return "Kp9";
    case VK_MULTIPLY: return "KpMultiply"; case VK_ADD: return "KpPlus";
    case VK_SUBTRACT: return "KpMinus"; case VK_DECIMAL: return "KpDecimal";
    case VK_DIVIDE: return "KpDivide";
    case VK_OEM_3: return "BackQuote";
    case VK_OEM_MINUS: return "Minus";
    case VK_OEM_PLUS: return "Equal";
    case VK_OEM_4: return "BracketLeft";
    case VK_OEM_6: return "BracketRight";
    case VK_OEM_5: return "Backslash";
    case VK_OEM_1: return "Semicolon";
    case VK_OEM_7: return "Quote";
    case VK_OEM_COMMA: return "Comma";
    case VK_OEM_PERIOD: return "Period";
    case VK_OEM_2: return "Slash";
    default:
        /* 没见过的键退化成十进制 VKey: 仍然能被记录和显示, 只是在配置里不好写。
           所有分支返回的字符串都以 NUL 结尾, 调用方可以直接当字符串用。 */
        snprintf(output, 16, "%u", code);
        return output;
    }
}

bool bongo_cat_windows_right_button_down(void) {
    /* Native window drag/capture state, not global model input. */
    return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
}
#endif
