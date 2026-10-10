#include "windows_input_internal.h"

#ifdef _WIN32
#include <stddef.h>

/*
 * 原始输入报文的解析入口(接收线程)。两处防御必须先做: header.dwSize 必须等于调用方
 * 给出的字节数, 并且整段报文必须装得下对应类型的负载 —— 这两个字段来自内核/驱动
 * 边界, 不能盲信; 不合格的报文只计数(diagnostic_invalid), 不崩溃。
 * 键盘报文里 RIM_INPUTSINK 表示"窗口不在前台时收到的输入", 单独计数: 那种情况下
 * 按键可能既不该触发动作也不该点亮模型。
 */
void bongo_cat_windows_input_packet(WindowsInputState *state,
    const RAWINPUT *packet, UINT bytes) {
    if (!state || !packet) return;
    if (bytes < sizeof(RAWINPUTHEADER) || packet->header.dwSize != bytes) {
        state->diagnostic_invalid++;
        return;
    }
    UINT payload;
    switch (packet->header.dwType) {
    case RIM_TYPEKEYBOARD: payload = sizeof(RAWKEYBOARD); break;
    case RIM_TYPEMOUSE: payload = sizeof(RAWMOUSE); break;
    default: return;
    }
    if (bytes < offsetof(RAWINPUT, data) + payload) {
        state->diagnostic_invalid++;
        return;
    }
    if (packet->header.dwType == RIM_TYPEKEYBOARD) {
        state->diagnostic_keys++;
        if (GET_RAWINPUT_CODE_WPARAM(packet->header.wParam) == RIM_INPUTSINK)
            state->diagnostic_background_keys++;
    }
    else state->diagnostic_mouse++;
    WindowsRawDevice *device = bongo_cat_windows_input_device(state,
        packet->header.hDevice);
    if (!device) { state->diagnostic_device_drops++; return; }
    if (packet->header.dwType == RIM_TYPEKEYBOARD) {
        bongo_cat_windows_input_key(state, device, &packet->data.keyboard);
        return;
    }
    const RAWMOUSE *mouse = &packet->data.mouse;
    bongo_cat_windows_input_buttons(state, device, mouse->usButtonFlags);
    RECT bounds = {0};
    /* 绝对坐标设备给出的是归一化到 [0,65535] 的值, 要映射到 bounds 上; bounds 由
       MOUSE_VIRTUAL_DESKTOP 决定 —— 触摸板、数位板、远程桌面都走这条路径, bounds 取
       错就会把光标算到屏幕外面。相对模式不需要 bounds, 所以保持全 0。 */
    if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE) {
        bool virtual_desktop = (mouse->usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
        bounds.left = virtual_desktop ? GetSystemMetrics(SM_XVIRTUALSCREEN) : 0;
        bounds.top = virtual_desktop ? GetSystemMetrics(SM_YVIRTUALSCREEN) : 0;
        bounds.right = bounds.left + GetSystemMetrics(virtual_desktop ?
            SM_CXVIRTUALSCREEN : SM_CXSCREEN);
        bounds.bottom = bounds.top + GetSystemMetrics(virtual_desktop ?
            SM_CYVIRTUALSCREEN : SM_CYSCREEN);
    }
    bongo_cat_windows_input_motion(state, device, mouse, &bounds);
    state->wake_pending = true;
}
#endif
