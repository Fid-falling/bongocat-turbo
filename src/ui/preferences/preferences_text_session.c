/*
 * 文本编辑会话: 把"哪个输入框正在编辑"这件事变成一个可跨帧存活的会话对象, 并负责把
 * SDL 事件翻译成编辑动作。
 *
 * 调用时序 (三个入口, 缺一不可):
 * begin    —— 用户点进输入框时建立会话 (记住目标缓冲的 id 与初始文本);
 * event    —— 每个 SDL 事件都喂进来, 返回 handled/finish/save 三个意图;
 * reset    —— finish 之后 (或关窗/取消时) 收尾清空。
 * 会话是"按 id 认领"的: 同一时间只有一个输入框处于编辑态, 这是单行编辑的简化点,
 * 也意味着**不能有两个输入框同时进入编辑**。
 *
 * 焦点丢失 (WINDOW_FOCUS_LOST) 被当成"回车保存": 用户切到别的窗口时, 不应该丢掉
 * 已经输入的内容。
 */
#include "preferences_text_session.h"

#include "preferences_text_edit.h"

#include <SDL3/SDL_clipboard.h>
#include <stdio.h>
#include <string.h>

static float measure(const void *userdata, const char *text, size_t length) {
    const struct nk_user_font *font = userdata;
    return font->width(font->userdata, font->height, text, (int)length);
}

/* 只有选中态才有"整段复制/剪切"的语义; 未选中时按下 Ctrl+C/X 不动剪贴板,
 * 但 Ctrl+V 始终可用 */
static void clipboard_event(BongoCatPreferencesTextSession *session,
    SDL_Keycode key) {
    if (key == SDLK_V) {
        char *clipboard = SDL_GetClipboardText();
        bongo_cat_text_edit_insert(session->text, sizeof(session->text),
            &session->cursor, &session->select_all, clipboard);
        SDL_free(clipboard);
        return;
    }
    if (session->select_all) SDL_SetClipboardText(session->text);
    if (key == SDLK_X && session->select_all)
        bongo_cat_text_edit_clear(session->text, &session->cursor,
            &session->select_all);
}

/* 会话是否活着: 靠 id 非空判断, 所以 reset 必须把 id 清成空串 (memset 全零即可) */
bool bongo_cat_preferences_text_session_active(
    const BongoCatPreferencesTextSession *session) {
    return session && session->id[0];
}

/* 进入编辑: 光标停在末尾而不是全选 —— 重命名时用户多半想接着改, 全选会一打字就
 * 把原名冲掉 */
void bongo_cat_preferences_text_session_begin(
    BongoCatPreferencesTextSession *session, const char *id,
    const char *text, struct nk_rect bounds) {
    if (!session) return;
    snprintf(session->id, sizeof(session->id), "%s", id ? id : "");
    snprintf(session->text, sizeof(session->text), "%s", text ? text : "");
    session->bounds = bounds;
    bongo_cat_text_edit_begin(session->text, &session->cursor,
        &session->select_all);
}

void bongo_cat_preferences_text_session_reset(
    BongoCatPreferencesTextSession *session) {
    if (session) memset(session, 0, sizeof(*session));
}

/*
 * 事件入口。返回的三个意图要分清:
 * handled  本事件已被本会话消费, 不要再给别的控件;
 * finish   编辑结束 (回车/ESC/点空白/丢焦点), save 表示"要保存"还是"放弃";
 * save     只有 finish 为真时才有意义。
 * 鼠标点击落在输入框外要按布局缩放换算坐标 (事件是物理像素, bounds 是逻辑像素),
 * 不换算的话高 DPI 下点空白处无法结束编辑。
 * 编辑期间 KEY_DOWN/KEY_UP 一律标记为 handled, 否则方向键会同时被页面滚动吃掉。
 */
BongoCatPreferencesTextSessionEvent bongo_cat_preferences_text_session_event(
    BongoCatPreferencesTextSession *session, const SDL_Event *event,
    float layout_scale, const struct nk_user_font *font, float text_inset) {
    BongoCatPreferencesTextSessionEvent result = {0};
    if (!bongo_cat_preferences_text_session_active(session) || !event)
        return result;
    if (event->type == SDL_EVENT_TEXT_INPUT) {
        bongo_cat_text_edit_insert(session->text, sizeof(session->text),
            &session->cursor, &session->select_all, event->text.text);
        result.handled = true;
        return result;
    }
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        result.finish = result.save = true;
        return result;
    }
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
        event->button.button == SDL_BUTTON_LEFT) {
        float scale = layout_scale > 0.0f ? layout_scale : 1.0f;
        float x = event->button.x / scale, y = event->button.y / scale;
        if (!NK_INBOX(x, y, session->bounds.x, session->bounds.y,
            session->bounds.w, session->bounds.h)) {
            result.finish = result.save = true;
            return result;
        }
        if (font) session->cursor = bongo_cat_text_edit_nearest(session->text,
            x - session->bounds.x - text_inset, measure, font);
        session->select_all = false;
        result.handled = true;
        return result;
    }
    if (event->type != SDL_EVENT_KEY_DOWN &&
        event->type != SDL_EVENT_KEY_UP) return result;
    result.handled = true;
    if (event->type == SDL_EVENT_KEY_UP) return result;
    SDL_Keycode key = event->key.key;
    bool control = (event->key.mod & SDL_KMOD_CTRL) != 0;
    if (key == SDLK_ESCAPE) {
        result.finish = true;
        return result;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        result.finish = result.save = true;
        return result;
    }
    if (control && key == SDLK_A)
        bongo_cat_text_edit_select_all(session->text, &session->cursor,
            &session->select_all);
    else if (control && (key == SDLK_C || key == SDLK_X || key == SDLK_V))
        clipboard_event(session, key);
    else if (key == SDLK_BACKSPACE || key == SDLK_DELETE)
        bongo_cat_text_edit_erase(session->text, &session->cursor,
            &session->select_all, key == SDLK_DELETE);
    else if (key == SDLK_LEFT || key == SDLK_RIGHT || key == SDLK_HOME ||
        key == SDLK_END)
        bongo_cat_text_edit_move(session->text, &session->cursor,
            &session->select_all,
            key == SDLK_LEFT ? BONGO_CAT_TEXT_EDIT_LEFT :
            key == SDLK_RIGHT ? BONGO_CAT_TEXT_EDIT_RIGHT :
            key == SDLK_HOME ? BONGO_CAT_TEXT_EDIT_HOME :
            BONGO_CAT_TEXT_EDIT_END);
    return result;
}
