#include "windows_dialog.h"

#ifdef _WIN32

#include <SDL3/SDL.h>
#include <SDL3/SDL_properties.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>

/*
 * 文件夹选择对话框: 用现代的 IFileDialog 替换 SDL 的 SHBrowseForFolderW 实现(原因见
 * windows_dialog.h)。
 *
 * 关键约束: SDL 的对话框契约是异步的 —— 回调必须在 SDL 事件线程上被调用, 而 IFileDialog
 * 是模态的、必须在它自己的线程上跑。所以这里的做法是: 起一条线程跑模态对话框, 回调就
 * 在那条线程里调用(回调本身是线程安全的, SDL 内部会把结果转投事件队列)。
 * 参数被打包成堆对象交给线程, 由线程负责释放(见 folder_dialog_thread)。
 */
typedef struct BongoCatWindowsFolderDialogArgs {
    SDL_DialogFileCallback callback;
    void *userdata;
    HWND owner;
    char *default_location;
    bool allow_many;
    Uint64 started_ns;
} BongoCatWindowsFolderDialogArgs;

/* 取消或失败时按 SDL 的约定回调: files 数组里只有一个 NULL 元素, 表示"用户取消"。 */
static void notify_cancel(BongoCatWindowsFolderDialogArgs *args) {
    const char *files[] = {NULL};
    args->callback(args->userdata, files, -1);
}

static char *wide_to_utf8(const wchar_t *value) {
    if (!value) return NULL;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value, -1, NULL, 0, NULL, NULL);
    if (length <= 0) return NULL;
    char *result = SDL_malloc((size_t)length);
    if (!result || !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value, -1, result, length, NULL, NULL)) {
        SDL_free(result);
        return NULL;
    }
    return result;
}

static HWND owner_window(SDL_Window *window) {
    if (!window) return NULL;
    return (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
}

/*
 * 模态对话框本体(跑在工作线程上)。三个容易忽略的细节:
 *   - FOS_FORCEFILESYSTEM 保证返回真实文件系统路径, 否则可能拿到"网络位置"这类虚拟项,
 *     根本取不出路径;
 *   - FOS_NOCHANGEDIR 防止选择过程中进程的当前工作目录被改掉 —— 那会让之后所有相对
 *     路径解析出错;
 *   - 记住的上次位置可能已经不存在, SetFolder 失败只是忽略(见下面原有的英文注释),
 *     由 Windows 用自己的默认位置兜底。
 * done 标签处保证"回调恰好被调用一次": 任何提前 goto 都会走 notify_cancel。
 */
static bool show_folder_dialog(BongoCatWindowsFolderDialogArgs *args) {
    IFileOpenDialog *dialog = NULL;
    IShellItem *default_folder = NULL;
    IShellItem *item = NULL;
    IShellItemArray *items = NULL;
    wchar_t *default_location = NULL;
    LPWSTR path = NULL;
    char **files = NULL;
    bool callback_called = false;
    bool com_initialized = false;
    HRESULT result;

    result = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(result)) goto done;
    com_initialized = true;

    result = CoCreateInstance(&CLSID_FileOpenDialog, NULL,
        CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog);
    if (FAILED(result)) goto done;

    FILEOPENDIALOGOPTIONS options = 0;
    result = dialog->lpVtbl->GetOptions(dialog, &options);
    if (FAILED(result)) goto done;
    options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        FOS_NOCHANGEDIR;
    if (args->allow_many) options |= FOS_ALLOWMULTISELECT;
    result = dialog->lpVtbl->SetOptions(dialog, options);
    if (FAILED(result)) goto done;

    if (args->default_location && args->default_location[0]) {
        int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            args->default_location, -1, NULL, 0);
        if (length > 0) {
            default_location = SDL_malloc((size_t)length * sizeof(*default_location));
            if (!default_location || !MultiByteToWideChar(CP_UTF8,
                MB_ERR_INVALID_CHARS, args->default_location, -1,
                default_location, length)) goto done;
            result = SHCreateItemFromParsingName(default_location, NULL,
                &IID_IShellItem, (void **)&default_folder);
            if (SUCCEEDED(result)) {
                if (FAILED(dialog->lpVtbl->SetFolder(dialog, default_folder))) {
                    default_folder->lpVtbl->Release(default_folder);
                    default_folder = NULL;
                }
            }
            /* An invalid remembered location should not prevent opening the
             * picker; Windows will use its normal last-location fallback. */
            result = S_OK;
        }
    }

    result = dialog->lpVtbl->Show(dialog, args->owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        notify_cancel(args);
        callback_called = true;
        goto done;
    }
    if (FAILED(result)) goto done;

    if (args->allow_many) {
        DWORD count = 0;
        result = dialog->lpVtbl->GetResults(dialog, &items);
        if (FAILED(result) || !items ||
            FAILED(items->lpVtbl->GetCount(items, &count)) || count == 0)
            goto done;

        files = SDL_calloc((size_t)count + 1, sizeof(*files));
        if (!files) goto done;
        for (DWORD index = 0; index < count; ++index) {
            IShellItem *selected = NULL;
            result = items->lpVtbl->GetItemAt(items, index, &selected);
            if (FAILED(result) || !selected ||
                FAILED(selected->lpVtbl->GetDisplayName(selected,
                    SIGDN_FILESYSPATH, &path))) {
                if (selected) selected->lpVtbl->Release(selected);
                goto done;
            }
            files[index] = wide_to_utf8(path);
            CoTaskMemFree(path);
            path = NULL;
            selected->lpVtbl->Release(selected);
            if (!files[index]) goto done;
        }
        args->callback(args->userdata, (const char *const *)files, -1);
        callback_called = true;
    } else {
        result = dialog->lpVtbl->GetResult(dialog, &item);
        if (FAILED(result) || !item ||
            FAILED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH,
                &path))) goto done;
        char *file = wide_to_utf8(path);
        if (!file) goto done;
        const char *selected[] = {file, NULL};
        args->callback(args->userdata, selected, -1);
        callback_called = true;
        SDL_free(file);
        CoTaskMemFree(path);
        path = NULL;
    }

done:
    if (!callback_called) notify_cancel(args);
    if (path) CoTaskMemFree(path);
    if (files) {
        for (char **file = files; *file; ++file) SDL_free(*file);
        SDL_free(files);
    }
    if (item) item->lpVtbl->Release(item);
    if (items) items->lpVtbl->Release(items);
    if (default_folder) default_folder->lpVtbl->Release(default_folder);
    if (dialog) dialog->lpVtbl->Release(dialog);
    SDL_free(default_location);
    if (com_initialized) CoUninitialize();
    return callback_called;
}

/* 对话框线程入口: 跑完模态循环后记一条耗时日志, 然后释放参数 —— 参数所有权属于这条
   线程, 所以选完或取消之后主线程不能再碰它。 */
static int SDLCALL folder_dialog_thread(void *userdata) {
    BongoCatWindowsFolderDialogArgs *args = userdata;
    show_folder_dialog(args);
    SDL_Log("[runtime] Folder dialog: stage=completed owner=%p elapsed_ms=%.2f",
        (void *)args->owner,
        (double)(SDL_GetTicksNS() - args->started_ns) / 1000000.0);
    SDL_free(args->default_location);
    SDL_free(args);
    return 0;
}

/* 异步入口: 复制参数 -> 起线程 -> 立刻返回。所有失败路径(分配失败、线程创建失败)都
   必须以"取消"回调收场, 否则调用方会永远等不到回调。线程是 detach 的, 生命周期由它
   自己管理。 */
void bongo_cat_windows_show_open_folder_dialog(
    SDL_DialogFileCallback callback, void *userdata, SDL_Window *window,
    const char *default_location, bool allow_many) {
    if (!callback) return;
    BongoCatWindowsFolderDialogArgs *args = SDL_calloc(1, sizeof(*args));
    if (!args) {
        const char *files[] = {NULL};
        callback(userdata, files, -1);
        return;
    }
    args->callback = callback;
    args->userdata = userdata;
    args->owner = owner_window(window);
    args->allow_many = allow_many;
    args->started_ns = SDL_GetTicksNS();
    if (default_location) {
        args->default_location = SDL_strdup(default_location);
        if (!args->default_location) {
            SDL_free(args);
            const char *files[] = {NULL};
            callback(userdata, files, -1);
            return;
        }
    }
    SDL_Thread *thread = SDL_CreateThread(folder_dialog_thread,
        "BongoCat_WindowsFolderDialog", args);
    if (!thread) {
        SDL_free(args->default_location);
        SDL_free(args);
        const char *files[] = {NULL};
        callback(userdata, files, -1);
        return;
    }
    SDL_Log("[runtime] Folder dialog: stage=opened owner=%p allow_many=%d",
        (void *)args->owner, allow_many);
    SDL_DetachThread(thread);
}

#endif /* _WIN32 */
