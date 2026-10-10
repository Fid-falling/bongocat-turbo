/*
 * 轮盘的菜单数据: 12 个根项 (其中"移除桌宠"由平台的可见性开关决定是否出现), 以及
 * 每个根项下"按需生成"的子项。
 *
 * 一条贯穿本文件的枚举约定, 改动时必须一起改:
 * 子项的命令值是**按偏移排布**的 (BONGO_CAT_MENU_SCALE_50 + i, ...), 而 i 是该页内
 * 的下标 (0..DIAL_PAGE-1)。所以 BongoCatMenuAction 里每个"块"的长度必须等于
 * DIAL_PAGE (16), 否则第二页的项会落到别的命令区间里去。
 *
 * 子项不是预先建好的数组, 而是 dial_child_item 现场拼出来的: 模型数/动作数都是运行时
 * 才知道的, 而且标签里的百分比要现算。代价是调用方必须提供一个栈上 text 缓冲接住标签。
 */
#include "dial_internal.h"
#include <stdio.h>

/*
 * 建根项表。表本身是局部常量数组, 拷贝进 d->items 时才按可见性截断 —— 这样"是否显示
 * 移除桌宠"只需要改 d->count, 不必维护两张表。
 * children > 0 的项是"容器" (尺寸/透明度/动作/表情/声音/模型), 它们的 command 是
 * BONGO_CAT_MENU_NONE, 点了是展开内圈而不是执行动作。
 * 颜色是每个根项固定的主题色 (0xAARRGGBB), 与 dial_scene 里的 tint 是两回事。
 */
void dial_items(Dial *d) {
    const BongoCatMenuLabels *l = d->labels;
    const DialItem items[] = {
        {l->preferences, BONGO_CAT_MENU_PREFERENCES, 0xff54aeff, 0, false, 0},
        {l->hide, BONGO_CAT_MENU_HIDE, 0xff60a5fa, 1, false, 0},
        {l->pass_through, BONGO_CAT_MENU_PASS_THROUGH, 0xff38bdf8, 2,
            l->pass_through_checked, 0},
        {l->always_on_top, BONGO_CAT_MENU_ALWAYS_ON_TOP, 0xfff77daa, 3,
            l->always_on_top_checked, 0},
        {l->window_size, BONGO_CAT_MENU_NONE, 0xff34d399, 4, false, 16},
        {l->opacity, BONGO_CAT_MENU_NONE, 0xff818cf8, 5, false, 10},
        {l->motion, BONGO_CAT_MENU_NONE, 0xfffbbf24, 6, false, l->motion_count},
        {l->expression, BONGO_CAT_MENU_NONE, 0xfff472b6, 7, false, l->expression_count},
        {l->audio, BONGO_CAT_MENU_NONE, 0xff4ade80, 8, false, l->audio_count},
        {l->model, BONGO_CAT_MENU_NONE, 0xffa78bfa, 9, false, l->model_count + 1},
        {l->exit, BONGO_CAT_MENU_EXIT, 0xffff453a, 10, false, 0},
        {l->remove_pet, BONGO_CAT_MENU_REMOVE_PET, 0xffff453a, 11, false, 0}
    };
    d->count = l->remove_pet_visible ? 12 : 11;
    for (int i = 0; i < d->count; ++i) d->items[i] = items[i];
}

/* 当前页里有几个子项: 总数减去已翻过的页, 再夹到一页的上限 DIAL_PAGE */
int dial_child_count(const Dial *d) {
    if (d->active < 0) return 0;
    int remaining = (int)d->items[d->active].children - d->page * DIAL_PAGE;
    return remaining < 0 ? 0 : (remaining > DIAL_PAGE ? DIAL_PAGE : remaining);
}

/*
 * 子项之间的角间距 (弧度)。0.32 用于尺寸/透明度 (10~16 项) 和任何超过 11 个子项的情况,
 * 否则用 0.50 —— 项少时排得开一些更好点。
 * 这个值同时决定扇形的宽度, 所以它一变 dial_child_paths 就会重建几何 (见那边的缓存键)。
 */
float dial_child_step(const Dial *d) {
    /* Preserve HTML spacing; large real catalogs use pages to avoid overlap. */
    return d->active == 4 || d->active == 5 || dial_child_count(d) > 11 ? .32f : .50f;
}

/*
 * 子项的角度: 以父扇形的中心角为对称轴均匀铺开。所以子项永远"长在"当前根项那一侧,
 * 不会绕到菜单另一边去。
 */
float dial_child_angle(const Dial *d, int index) {
    return -DIAL_PI / 2 + d->active * 2 * DIAL_PI / d->count +
        (index - (dial_child_count(d) - 1) / 2.0f) * dial_child_step(d);
}

/*
 * 现场生成第 child 个子项 (从根项的模板复制过来再改写)。
 * i 是**全局**下标 (d->page*DIAL_PAGE + child), 因为命令值也是按全局顺序排的; 但
 * 标签数组 (motion_names 等) 也是按全局下标索引的, 两者必须一致。
 * 每个分支负责三件事: 设置标签 (可能需要 snprintf 到调用方的 text 里)、算出命令值、
 * 以及标出 checked (与 labels 里的当前状态比对)。
 * 模型那一项的最后多出一个"添加模型" (i == model_count), 所以根的 children 是
 * model_count + 1 —— 这个 +1 只出现在 dial_items 里, 别处不要再加。
 */
DialItem dial_child_item(const Dial *d, int child, char *text, size_t capacity) {
    DialItem item = {0};
    if (child < 0 || child >= dial_child_count(d)) return item;
    const BongoCatMenuLabels *l = d->labels;
    size_t i = (size_t)(d->page * DIAL_PAGE + child);
    item = d->items[d->active];
    item.children = 0;
    switch (d->active) {
    case 4:
        snprintf(text, capacity, "%d%%", 50 + (int)i * 10);
        item.label = text;
        item.command = (BongoCatMenuAction)(BONGO_CAT_MENU_SCALE_50 + i);
        item.checked = fabsf(l->scale_percent - (50 + (int)i * 10)) < .5f;
        break;
    case 5:
        snprintf(text, capacity, "%d%%", 10 + (int)i * 10);
        item.label = text;
        item.command = (BongoCatMenuAction)(BONGO_CAT_MENU_OPACITY_10 + i);
        item.checked = fabsf(l->opacity_percent - (10 + (int)i * 10)) < .5f;
        break;
    case 6:
        item.label = l->motion_names[i];
        item.command = (BongoCatMenuAction)(BONGO_CAT_MENU_MOTION_FIRST + i);
        item.checked = l->motion_checked && l->motion_checked[i];
        break;
    case 7:
        item.label = l->expression_names[i];
        item.command = (BongoCatMenuAction)(BONGO_CAT_MENU_EXPRESSION_FIRST + i);
        item.checked = i == l->current_expression;
        break;
    case 8:
        item.label = l->audio_names[i];
        item.command = (BongoCatMenuAction)(BONGO_CAT_MENU_AUDIO_FIRST + i);
        item.checked = l->audio_checked && l->audio_checked[i];
        break;
    case 9:
        item.label = i == l->model_count ? l->add_model : l->model_names[i];
        item.command = i == l->model_count ? BONGO_CAT_MENU_MODEL_ADD :
            (BongoCatMenuAction)(BONGO_CAT_MENU_MODEL_FIRST + i);
        item.checked = i < l->model_count && i == l->current_model;
        break;
    }
    return item;
}

/*
 * 选中状态的唯一写入口 (绘制、键盘、鼠标都走这里)。
 * 三件事按顺序做:
 *   1. 早点返回: 什么都没变就不做任何事 (鼠标移动是高频事件, 每次都重算几何会掉帧);
 *   2. child_focus 的维护: 换了根项一定退出内圈; 选中了子项一定进入内圈 ——
 *      这个标志决定 dial_scene 是画全部根项还是只画当前那个;
 *   3. 预览只在"实际生效的动作"变化时才回调: 鼠标在同一个子项上小幅移动不该反复
 *      触发试听/试穿, 那样会把宠物画面刷成一片乱。
 * 换根项时 page 归零并重建内圈几何 (几何只与根项/子项数有关)。
 */
void dial_select(Dial *d, int root, int child) {
    if (d->active == root && d->child == child) return;
    bool changed = d->active != root;
    d->active = root;
    d->child = child;
    if (changed) d->child_focus = false;
    if (child >= 0) d->child_focus = true;
    if (changed) {
        d->page = 0;
        d->changed_at = SDL_GetTicks();
        dial_child_paths(d);
    }
    char text[32];
    BongoCatMenuAction action = child >= 0 ?
        dial_child_item(d, child, text, sizeof(text)).command : BONGO_CAT_MENU_NONE;
    if (action != d->preview) {
        d->preview = action;
        dial_preview(d, action);
    }
    d->dirty = true;
}
