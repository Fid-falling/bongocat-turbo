/*
 * 轮盘的输入: SDL 事件 -> 选中状态 -> (可选) 预览回调。
 *
 * 本文件最重要的不变量: 命中测试与绘制必须用**同一套**坐标与变换。
 * dial_draw 把逻辑坐标变成屏幕坐标的式子是
 *     screen = center + (logical*zoom + offset) * (scale * opening)
 * 所以这里反向换算时要先除以 (scale*opening) 再减中心, 而 dial_transform 里的
 * zoom/offset 由 dial_hit 自己按选中项重算 (它用的是布局公式, 不是绘制时的动画值)。
 * 一旦两边不同步, 症状就是"菜单展开动画期间点击落点偏移"。
 *
 * 选择状态机只有三个变量: active (选中的根项) / child (选中的子项) / child_focus
 * (是否已进入内圈)。pressed 只在按下时记录目标, 松开时必须落回同一个目标才生效 ——
 * 这样"按下后拖走再松手"不会误触发。
 */
#include "dial_internal.h"

/*
 * 把屏幕坐标换成逻辑坐标并更新选中, click 表示"这是一次点击判定"而不是悬停。
 * 两者的差别在下面第二段: 悬停时允许 dial_hit 的兜底逻辑保留当前根项 (划过缝隙不会
 * 掉选中), 但**点击**时不允许 —— 点在扇形之间的空隙上必须判定为"点空白", 从而关掉
 * 菜单。少了这段, 用户点缝隙会发现菜单不关, 像是卡住了。
 * 另外半径小于内圈起点时主动退出 child_focus: 指针回到中心就是"我要返回上一层"。
 */
static void pointer(Dial *d, float px, float py, bool click, int *root, int *child) {
    float scale = d->scale*d->opening;
    float x = (px-(float)d->width/2)/scale, y = (py-(float)d->height/2)/scale;
    float radius = hypotf(x,y);
    if (d->child_focus && radius < 190) { d->child_focus = false; d->dirty = true; }
    dial_hit(d,x,y,root,child);
    if (click && *child < 0 && *root >= 0) {
        float angle = atan2f(y,x)-(-DIAL_PI/2+(float)*root*2*DIAL_PI/(float)d->count);
        if (radius < 73 || radius > 198 ||
            fabsf(atan2f(sinf(angle),cosf(angle))) > DIAL_PI/(float)d->count) *root = -1;
    }
}

/*
 * 确认当前选中的项。
 * 有子项 -> 钻进内圈 (dial_select 会把 page 归零并重建几何);
 * 有命令 -> 记下结果并结束模态循环, 真正的执行在 context_menu 返回之后由主程序做。
 * 局部 text 缓冲只是为了接住 dial_child_item 现场格式化的标签 (比如 "70%"),
 * 它的 label 指针指向这个栈上缓冲, 所以不能把它存到别处。
 */
static void activate(Dial *d) {
    if (d->active < 0) return;
    char text[32];
    DialItem item = d->child < 0 ? d->items[d->active] : dial_child_item(d,d->child,text,sizeof(text));
    if (item.children) { dial_select(d,d->active,0); return; }
    if (item.command != BONGO_CAT_MENU_NONE) { d->result = item.command; d->done = true; }
}

/*
 * 内圈翻页 (滚轮 / PageUp/Down)。页数是向上取整的, 取模时先加 pages 再取模,
 * 这样 direction 为 -1 时也不会出现负数下标 (C 的 % 对负数返回负值)。
 * 翻页要顺手清掉预览: 上一页的预览动作已经不代表当前选项了, 留着会让宠物保持一个
 * 用户已经看不到的效果。
 */
static void page(Dial *d, int direction) {
    if (d->active < 0 || d->items[d->active].children <= DIAL_PAGE) return;
    int pages = ((int)d->items[d->active].children+DIAL_PAGE-1)/DIAL_PAGE;
    d->page = (d->page+direction+pages)%pages;
    d->child = -1;
    if (d->preview != BONGO_CAT_MENU_NONE) dial_preview(d,BONGO_CAT_MENU_NONE);
    d->preview = BONGO_CAT_MENU_NONE;
    d->changed_at = SDL_GetTicks();
    dial_child_paths(d); d->dirty = true;
}

/*
 * 键盘导航。ESC 是分层的: 在内圈时先退回外圈, 已经在外圈才关菜单 —— 和图形界面的
 * 惯例一致, 也让用户能一层层退出而不会一下子全关掉。
 * 方向键/Shift+Tab 在内圈里移动子项选择, 在外圈里移动根项选择 (环形取模, 所以从左端
 * 继续按会绕到右端)。
 * Backspace 等于"返回上一层": 退出 child_focus 但保留当前根项。
 */
static void key(Dial *d, const SDL_KeyboardEvent *event) {
    SDL_Keycode value = event->key;
    if (value == SDLK_ESCAPE) {
        if (d->active >= 0 && dial_child_count(d)) dial_select(d,-1,-1);
        else d->done = true;
    } else if (value == SDLK_RETURN || value == SDLK_SPACE) activate(d);
    else if (value == SDLK_PAGEUP || value == SDLK_PAGEDOWN) page(d,value == SDLK_PAGEDOWN ? 1 : -1);
    else if (value == SDLK_LEFT || value == SDLK_UP || value == SDLK_RIGHT ||
        value == SDLK_DOWN || value == SDLK_TAB) {
        int direction = value == SDLK_LEFT || value == SDLK_UP ||
            (value == SDLK_TAB && (event->mod & SDL_KMOD_SHIFT)) ? -1 : 1;
        int count = dial_child_count(d);
        if (d->child >= 0 && count) dial_select(d,d->active,(d->child+direction+count)%count);
        else dial_select(d,d->active < 0 ? 0 : (d->active+direction+d->count)%d->count,-1);
    } else if (value == SDLK_BACKSPACE) {
        d->child_focus = false; d->dirty = true; dial_select(d,d->active,-1);
    }
}

/*
 * 事件分发。几处刻意的行为:
 *   - 非左键按下直接关菜单: 右键再按一次、中键之类都应当被理解为"取消";
 *   - 松开时要求 pressed 与当前目标一致, pressed 用 DIAL_ROOTS+child 编码子项,
 *     是为了让"第 3 个根项"和"第 3 个子项"不会互相冒充;
 *   - 鼠标离开窗口 = 清空选中 (但不关闭), 指针回来会重新选中;
 *   - 失焦/隐藏导致的关闭都带 d->shown 条件: 窗口创建和显示过程本身会排进一两个
 *     瞬时的失焦事件, 不加这个条件菜单会在刚打开时立刻自己关掉;
 *   - 尺寸/像素密度变化统一走一段重算 scale/raster_scale 的代码 (见那里的英文注释):
 *     创建窗口时 SDL 也会发 DISPLAY_SCALE_CHANGED, 不能当成错误处理。
 */
void dial_event(Dial *d, const SDL_Event *e) {
    int root, child;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        pointer(d,e->motion.x,e->motion.y,false,&root,&child);
        dial_select(d,root,child); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e->button.button != SDL_BUTTON_LEFT) { d->done = true; break; }
        pointer(d,e->button.x,e->button.y,true,&root,&child);
        if (root < 0) { d->done = true; break; }
        dial_select(d,root,child);
        d->pressed = child >= 0 ? DIAL_ROOTS+child : root; d->dirty = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (e->button.button != SDL_BUTTON_LEFT) break;
        int pressed = d->pressed; d->pressed = -1; d->dirty = true;
        pointer(d,e->button.x,e->button.y,true,&root,&child);
        if (root >= 0 && pressed == (child >= 0 ? DIAL_ROOTS+child : root)) {
            dial_select(d,root,child); activate(d);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        float delta = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        if (delta != 0) page(d,delta < 0 ? 1 : -1);
        break;
    }
    case SDL_EVENT_KEY_DOWN: key(d,&e->key); break;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: dial_select(d,-1,-1); break;
    case SDL_EVENT_WINDOW_SHOWN: d->shown = true; d->dirty = true; break;
    case SDL_EVENT_WINDOW_HIDDEN:
        if (d->shown && (SDL_GetWindowFlags(d->window) & SDL_WINDOW_HIDDEN)) d->done = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* Creation/show may queue a transient loss before focus settles. */
        if (d->shown && SDL_GetKeyboardFocus() != d->window) d->done = true;
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED: d->done = true; break;
    case SDL_EVENT_WINDOW_EXPOSED: d->dirty = true; break;
    case SDL_EVENT_WINDOW_RESIZED: case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        if (!SDL_GetWindowSize(d->window,&d->width,&d->height) ||
            !SDL_GetWindowSizeInPixels(d->window,&d->pixel_width,&d->pixel_height) ||
            d->width < 1 || d->height < 1) { d->done = true; break; }
        d->scale = (float)SDL_min(d->width,d->height)/608.0f;
        d->raster_scale = (float)d->pixel_width/(float)d->width;
        /* SDL also sends DISPLAY_SCALE_CHANGED during window creation.
           Keep the menu open; existing textures remain valid and new covers
           use the updated density. Fonts are rebaked on the next opening. */
        d->dirty = true;
        break;
    }
}
