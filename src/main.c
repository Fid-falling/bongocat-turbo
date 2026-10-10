#include "bongo_cat/app.h"
#include "bongo_cat/platform.h"
#include <SDL3/SDL_main.h>

int main(int argc, char **argv) {
    /*
     * 不是管理员就以管理员身份重启自己 (ShellExecuteExW 的 runas 动词), 旧进程
     * 立刻退出; 用户取消 UAC 或提权失败时返回 false, 照常以普通权限运行。
     * 提权后的新进程带着 BONGO_CAT_ELEVATION_TRIED 标记, 不会反复重启。
     */
    if (bongo_cat_platform_relaunch_elevated_if_needed()) return 0;
    return bongo_cat_app_run(argc, argv);
}
