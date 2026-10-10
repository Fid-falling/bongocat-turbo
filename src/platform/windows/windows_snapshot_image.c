#include "windows_snapshot_internal.h"

#include <SDL3/SDL_opengl.h>

/* custom_destination = 调用方自己指定了目标矩形(拖动预览就是这种); 否则跟随源窗口的
   实际位置与大小。 */
void bongo_cat_windows_snapshot_geometry(BongoCatWindowsSnapshot *s,
    const SDL_FRect *rect) {
    if (!s) return;
    s->custom_destination = rect != NULL;
    if (rect) s->destination = *rect;
}

/*
 * 重绘预览。步骤: 算出图像该出现的位置 -> 几何没变且已经画过就直接返回(拖动时这个
 * 函数会被高频调用, 这一步能省掉大量无谓重绘) -> 把屏幕坐标换算成渲染目标的像素坐标
 * (渲染目标是整个虚拟桌面, 所以要减 desktop 原点再乘缩放) -> 清成全透明再画 -> 用
 * SetWindowRgn 把窗口区域收敛到图像范围。
 * SetWindowRgn 只影响命中测试与绘制区域, 不改变窗口原点 —— 窗口始终钉在桌面左上角,
 * 于是整段交互里 DWM 看到的都是同一块表面(见下面原有的英文注释)。
 */
bool bongo_cat_windows_snapshot_present(BongoCatWindowsSnapshot *s) {
    RECT rect;
    if (!s || !GetWindowRect(s->source_handle, &rect)) return false;
    SDL_FRect image = s->custom_destination ? s->destination :
        (SDL_FRect){(float)rect.left, (float)rect.top,
            (float)(rect.right - rect.left), (float)(rect.bottom - rect.top)};
    if (s->has_frame && image.x == s->presented.x && image.y == s->presented.y &&
        image.w == s->presented.w && image.h == s->presented.h) return true;
    int pixel_width, pixel_height;
    if (!SDL_GetRenderOutputSize(s->renderer, &pixel_width, &pixel_height)) return false;
    float sx = (float)pixel_width / (s->desktop.right - s->desktop.left);
    float sy = (float)pixel_height / (s->desktop.bottom - s->desktop.top);
    SDL_FRect destination = {(image.x - s->desktop.left) * sx,
        (image.y - s->desktop.top) * sy, image.w * sx, image.h * sy};
    SDL_SetRenderDrawColor(s->renderer, 0, 0, 0, 0);
    if (!SDL_RenderClear(s->renderer) ||
        !SDL_RenderTextureRotated(s->renderer, s->texture, NULL, &destination,
            0.0, NULL, SDL_FLIP_VERTICAL) || !SDL_RenderPresent(s->renderer))
        return false;
    /* Keep the native hit-test footprint local to the picture, while the
       swapchain and window origin remain fixed for the whole interaction. */
    HRGN region = CreateRectRgn((int)SDL_floorf(image.x - s->desktop.left),
        (int)SDL_floorf(image.y - s->desktop.top),
        (int)SDL_ceilf(image.x + image.w - s->desktop.left),
        (int)SDL_ceilf(image.y + image.h - s->desktop.top));
    if (!region) return false;
    if (!SetWindowRgn(s->handle, region, FALSE)) {
        DeleteObject(region);
        return false;
    }
    s->presented = image;
    s->has_frame = true;
    return true;
}

/*
 * 抓取源窗口的前缓冲。glReadPixels 受一堆全局状态影响, 所以先备份并清零
 * PACK_ALIGNMENT / ROW_LENGTH / SKIP_*, 并临时解绑 PIXEL_PACK_BUFFER(绑着 PBO 时
 * glReadPixels 会把数据写进 PBO 而不是我们给的指针), 读完逐项还原 —— 这些状态属于
 * 共享的 GL 上下文, 不还原会影响渲染器后续工作。
 * 末尾还要检查抓回来的东西是不是空的: 某些驱动拿不到带透明的前缓冲, 这时宁可让整个
 * 预览失败, 也不能把一只空猫替换到屏幕上(见下面原有的英文注释)。
 */
bool bongo_cat_windows_snapshot_capture(BongoCatWindowsSnapshot *s) {
    if (!SDL_GetWindowSizeInPixels(s->source, &s->width, &s->height) ||
        s->width <= 0 || s->height <= 0 ||
        (size_t)s->width * s->height > SNAPSHOT_MAX_PIXELS) return false;
    s->pixels = SDL_malloc((size_t)s->width * s->height * 4);
    if (!s->pixels) return false;
    typedef void (APIENTRY *BindBufferFn)(GLenum, GLuint);
    BindBufferFn bind_buffer = (BindBufferFn)SDL_GL_GetProcAddress("glBindBuffer");
    if (!bind_buffer) return false;
    GLint pack_buffer;
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    bind_buffer(GL_PIXEL_PACK_BUFFER, 0);
    GLint buffer, alignment, row_length, skip_pixels, skip_rows;
    glGetIntegerv(GL_READ_BUFFER, &buffer);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row_length);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skip_pixels);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skip_rows);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    glReadPixels(0, 0, s->width, s->height, GL_RGBA, GL_UNSIGNED_BYTE, s->pixels);
    GLenum error = glGetError();
    glPixelStorei(GL_PACK_SKIP_PIXELS, skip_pixels);
    glPixelStorei(GL_PACK_SKIP_ROWS, skip_rows);
    glPixelStorei(GL_PACK_ROW_LENGTH, row_length);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment);
    glReadBuffer((GLenum)buffer);
    bind_buffer(GL_PIXEL_PACK_BUFFER, (GLuint)pack_buffer);
    if (error != GL_NO_ERROR) return false;
    /* Some drivers cannot expose a transparent front buffer. Never replace
       a live pet with an empty capture in that case. */
    size_t pixels = (size_t)s->width * s->height;
    for (size_t i = 0; i < pixels; ++i)
        if (s->pixels[i * 4 + 3] > 8) return true;
    return false;
}
