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
#include "bongo_cat/log.h"

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
    /*
     * 键盘导航的日志。这一块踩过"按键完全收不到"的坑(代理分层呈现下焦点不在菜单窗口
     * 上, 见 dial_window.c 的 collect_event), 出问题时这一行是分辨"没收到按键"与
     * "收到了没处理"的唯一依据。
     */
    SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
        "Radial menu key: key=0x%x active=%d child=%d", (unsigned)value,
        d->active, d->child);
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
 * 光标此刻是否还在菜单窗口的矩形内。窗口是无边框的, 所以窗口位置就是客户区原点。
 *
 * 为什么需要它: 代理分层呈现(仅 Windows, 见 windows_layered.c)下真正显示画面的是
 * 代理窗口, 真实窗口被压成全透明; 系统认为光标在代理窗口上, 而 SDL 只认识真实窗口
 * —— 代理窗口每转投一条 WM_MOUSEMOVE, SDL 都会带出一对事件: 先 MOUSE_ENTER, 紧接着
 * (因为光标并不在真实窗口上, TrackMouseEvent 立刻报 LEAVE)再来一条 MOUSE_LEAVE。
 * 风暴的源头(同一个坐标被系统反复重发)已经在 windows_layered_window.c 里挡掉了, 但
 * 只要光标在动, 这个"motion 后面永远跟着一条 LEAVE"就一直成立。
 *
 * 老逻辑对每一条 MOUSE_LEAVE 都清空选中, 于是代理模式下悬停高亮会被立刻抹掉、方向键
 * 永远从"什么都没选"开始(实测 0 -> 1 -> 2 变成 -1 -> 0 -> 1; 非代理模式下同一套按键
 * 是好的, 这正是它被漏掉的原因)。判据必须是"光标真的在窗口外", 否则用户把指针移出
 * 菜单时高亮会留在原地。
 */
static bool pointer_inside_window(const Dial *d) {
    float x = 0, y = 0;
    int window_x = 0, window_y = 0;
    /* SDL_GetGlobalMouseState 返回的是按键掩码(没按键时就是 0), 不是成败 —— 别拿它
       当布尔用: 那样"没按住鼠标"会被误判成"问不到光标位置", 整个过滤永远不生效。 */
    SDL_GetGlobalMouseState(&x, &y);
    if (!SDL_GetWindowPosition(d->window, &window_x, &window_y)) return false;
    x -= (float)window_x; y -= (float)window_y;
    return x >= 0 && y >= 0 && x < (float)d->width && y < (float)d->height;
}

/*
 * 事件分发。几处刻意的行为:
 *   - 非左键按下直接关菜单: 右键再按一次、中键之类都应当被理解为"取消";
 *   - 松开时要求 pressed 与当前目标一致, pressed 用 DIAL_ROOTS+child 编码子项,
 *     是为了让"第 3 个根项"和"第 3 个子项"不会互相冒充;
 *   - 鼠标离开窗口 = 清空选中 (但不关闭), 指针回来会重新选中; 代理分层模式下"离开"
 *     事件基本全是假的, 只在光标真的出了窗口矩形时才清 (见 pointer_inside_window);
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
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        /*
         * 代理分层模式下这条事件基本全是假的(见 pointer_inside_window): 光标还在窗口
         * 里就把它当噪声丢掉。实测少了这一条, 悬停高亮会被"紧跟每条真实 motion 而来
         * 的 LEAVE"抹掉 —— 方向键于是永远从"什么都没选"开始(0 -> 1 -> 2 变成
         * -1 -> 0 -> 1), 截图里也看不到任何高亮。
         * 日志只写一次: 这个分支本身是高频的, 每来一次写一行等于自己造风暴。
         */
        if (d->layered && pointer_inside_window(d)) {
            if (!d->leave_filtered) {
                d->leave_filtered = true;
                SDL_LogInfo(BONGO_CAT_LOG_LIFECYCLE,
                    "[probe] 轮盘: 忽略代理模式下的假 MOUSE_LEAVE (光标仍在窗口内)");
            }
            break;
        }
        dial_select(d,-1,-1); break;
    case SDL_EVENT_WINDOW_SHOWN: d->shown = true; d->dirty = true; break;
    case SDL_EVENT_WINDOW_HIDDEN:
        if (d->shown && (SDL_GetWindowFlags(d->window) & SDL_WINDOW_HIDDEN)) d->done = true;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        /* Creation/show may queue a transient loss before focus settles. */
        /* 代理分层呈现(仅 Windows, 见 windows_layered.c)下这条整体不适用: 真正显示画面的
           是代理窗口, 它带 WS_EX_NOACTIVATE, 点它必然把键盘焦点交给别的窗口(通常是猫),
           于是每次点菜单都会先收到一条失焦事件 —— 按老逻辑就变成"点哪都关菜单、动作不
           执行"。代理模式下菜单的关闭改由"点在扇区外"的点击或 ESC 决定。 */
        if (d->shown && !d->layered && SDL_GetKeyboardFocus() != d->window) d->done = true;
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
