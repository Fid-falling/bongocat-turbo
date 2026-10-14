/*
 * 更新通道的平台策略层: 回答本平台能否检查更新、是否只能用商店、当前 exe 是否装在
 * 安装目录里、该匹配哪个发布资产 (asset); Windows 与 Unix 各一套同名实现, 由 _WIN32 切开。
 * 时序: supported/store/installed 只在 bongo_cat_update_create() 判定一次并固化进
 * service; asset 每次检查都要重取, 所以必须无 I/O、无分配。
 * 最易改错: asset 字符串要与 core/update_release.c 拼出的后缀严格一致 (windows-x64 等),
 * 否则解析成功而下载链接为空, 界面静默退化成打开发布页。
 */
#include "update_internal.h"
#include "bongo_cat/i18n.h"
#include "preferences_notice.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include "windows_package.h"
#include <windows.h>

bool bongo_cat_update_platform_supported(void) {
    return true;
}

bool bongo_cat_update_platform_store(void) {
    return bongo_cat_windows_is_packaged();
}

/* 取当前进程 exe 的全路径; 失败或长度超出 capacity 都算失败, 不做截断式重试。 */
static bool current_executable(wchar_t *path, size_t capacity) {
    DWORD length = GetModuleFileNameW(NULL, path, (DWORD)capacity);
    return length > 0 && length < capacity;
}

/* 语义是当前 exe 正好位于安装目录, 不是曾经装过: 与卸载表的 InstallLocation
 * 拼上 exe 文件名后不区分大小写地比较, 便携版必然为 false。
 * 两个键覆盖 Inno Setup (BongoCat_is1) 与旧安装器 (BongoCat), 两个 view 覆盖
 * 32/64 位注册表视图; 安装器只写 HKCU (见 packaging/windows/install-lifecycle.iss),
 * 这里也只查 HKCU, 免得为读注册表去提权。
 * 两个 exe 名也都要试: 1.16.0 起产物叫 BongoCatTurbo.exe, 而 1.15.x 及更早的安装目录
 * 里只有 BongoCat.exe —— 只认新名字会把老安装误判成便携版, 用户会拿到便携包而不是
 * 安装器升级。iss 里那个 Software\BongoCat\BongoCat 兜底位置这里没认, 老安装可能被当成便携版。 */
bool bongo_cat_update_platform_installed(void) {
    wchar_t running[2048] = {0};
    if (!current_executable(running, _countof(running))) return false;
    const DWORD views[] = {
        RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY
    };
    const wchar_t *keys[] = {
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BongoCat_is1",
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\BongoCat"
    };
    const wchar_t *names[] = {
        L"\\BongoCatTurbo.exe", L"\\BongoCat.exe"
    };
    for (size_t key = 0; key < _countof(keys); ++key) {
        for (size_t index = 0; index < _countof(views); ++index) {
            wchar_t install_root[2048] = {0};
            DWORD size = sizeof(install_root);
            LSTATUS result = RegGetValueW(HKEY_CURRENT_USER,
                keys[key],
                L"InstallLocation", RRF_RT_REG_SZ | views[index], NULL,
                install_root, &size);
            if (result != ERROR_SUCCESS || !install_root[0]) continue;
            size_t length = wcslen(install_root);
            while (length && (install_root[length - 1] == L'\\' ||
                install_root[length - 1] == L'/')) install_root[--length] = L'\0';
            for (size_t name = 0; name < _countof(names); ++name) {
                size_t name_length = wcslen(names[name]);
                if (length + name_length + 1 > _countof(install_root)) continue;
                memcpy(install_root + length, names[name],
                    (name_length + 1) * sizeof(wchar_t));
                if (_wcsicmp(running, install_root) == 0) return true;
            }
        }
    }
    return false;
}

const char *bongo_cat_update_platform_asset(void) {
#ifdef _WIN64
    return "windows-x64";
#else
    return "windows-x86";
#endif
}

/* Windows 上"下载 -> 校验 -> 原地替换自己 -> 重启"这条链路是通的 (见
   platform/windows/windows_update_apply.c), 所以手动检查发现新版本后可以直接一键更新,
   不必把用户丢到浏览器里。 */
bool bongo_cat_update_platform_self_update(void) {
    return true;
}

#else

/* Unix packages are published as a single archive for each architecture.
 * They do not have an installer registration to inspect, but they can still
 * use the release API and open the matching archive when an update exists. */
bool bongo_cat_update_platform_supported(void) {
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__) || defined(__x86_64__)
    return true;
#else
    return false;
#endif
#elif defined(__linux__) && (defined(__x86_64__) || defined(__amd64__))
    return true;
#else
    return false;
#endif
}
/* Unix 上没有商店也没有安装登记: 两个判断恒为 false, 更新一律走 portable 归档。 */
bool bongo_cat_update_platform_store(void) { return false; }
bool bongo_cat_update_platform_installed(void) { return false; }
/* Unix 上没有"替换正在运行的自己"这套做法 (AppImage/包管理/bundle 替换都需要外部进程
   参与), 所以自动更新不在这里做: 手动检查仍然只打开对应的下载页。 */
bool bongo_cat_update_platform_self_update(void) {
    return false;
}

const char *bongo_cat_update_platform_asset(void) {
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return "macos-arm64";
#else
    return "macos-x64";
#endif
#elif defined(__x86_64__) || defined(__amd64__)
    return "linux-x64";
#else
    return "unsupported";
#endif
}

#endif

/* i18n 查表, 缺 key 时回退英文; 返回的是 i18n 内部的借用指针, 调用方不要 free。 */
static const char *tr(BongoCatUpdateService *service, const char *key,
    const char *fallback) {
    return bongo_cat_i18n_get(service->app->i18n, key, fallback);
}

/*
 * "已下载到 <路径>, 请手动替换" —— 更新链路里唯一必须带参数的文案。
 * 先看文案里有没有 %s 再决定要不要传参数: 某个语言的翻译漏了占位符时, 直接把路径追加在
 * 末尾, 而不是拿一个不匹配的格式串去 snprintf (那是未定义行为)。
 * 路径是用户唯一的出路, 任何情况下都不能丢。
 */
static void manual_replace_message(BongoCatUpdateService *service,
    const BongoCatUpdateSnapshot *snapshot, char *output, size_t capacity) {
    const char *format = tr(service, "native.support.updateManualReplace",
        "Unable to replace the running program automatically. The new version "
        "was downloaded to %s; close BongoCat and replace the file manually.");
    if (format && strstr(format, "%s"))
        snprintf(output, capacity, format, snapshot->manual_path);
    else snprintf(output, capacity, "%s (%s)",
        format ? format : "Update downloaded", snapshot->manual_path);
}

/* 手动检查完成后的回执: 只有 manual 检查会走到这里 (自动检查静默, 免得每次启动
 * 都弹提示), 而且 app->preferences 为空时底层直接丢弃, 不会崩。
 * 三种情况:
 *   1. "替换没能自动完成": 新文件已经在磁盘上 (manual_path), 唯一的出路是告诉用户它在
 *      哪、让他手动替换 —— 这是本条分支存在的全部理由, 所以走危险色;
 *   2. ERROR: 人话交给 core/update_error.c (它认识 update.h 里那几个阶段原文, 也认识
 *      "GitHub returned HTTP status N" 这种限流原文), 界面不出现状态码与英文原文;
 *   3. CURRENT/AVAILABLE: 一句结果提示。
 * 下载中/校验中/准备重启这些进行中的阶段不弹提示: 进度由关于页的状态条持续显示,
 * 弹一次 2.6 秒的 toast 反而会让人以为已经结束了。
 * message/detail 都是固定栈缓冲, 超长只截断不报错。 */
void bongo_cat_update_show_completion(BongoCatUpdateService *service) {
    BongoCatUpdateSnapshot snapshot;
    bongo_cat_update_snapshot(service, &snapshot);
    char message[384];
    if (snapshot.manual_replace && snapshot.manual_path[0]) {
        manual_replace_message(service, &snapshot, message, sizeof(message));
        bongo_cat_preferences_notice_show(service->app, message, true);
        return;
    }
    if (snapshot.status == BONGO_CAT_UPDATE_CURRENT) {
        snprintf(message, sizeof(message), "%s v%s", tr(service,
            "native.support.latest", "Already up to date"), BONGO_CAT_VERSION);
        bongo_cat_preferences_notice_show(service->app, message, false);
    } else if (snapshot.status == BONGO_CAT_UPDATE_AVAILABLE) {
        snprintf(message, sizeof(message), "%s v%s", tr(service,
            "native.support.updateAvailable", "New version available:"),
            snapshot.release.version);
        bongo_cat_preferences_notice_show(service->app, message, false);
    } else if (snapshot.status == BONGO_CAT_UPDATE_ERROR) {
        bongo_cat_preferences_notice_show(service->app,
            bongo_cat_update_error_text(service->app->i18n, snapshot.error),
            true);
    }
}
