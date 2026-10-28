#include "process.h"
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mcc::client {

#ifdef _WIN32
static HANDLE g_server;

bool start_server(const char *dir, int port) {
    char exe[1024], cmd[1200];
    snprintf(exe, sizeof exe, "%smcc_debug.exe", dir);
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) return false;
    snprintf(cmd, sizeof cmd, "\"%s\" %d", exe, port);
    STARTUPINFOA si = {};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWMINNOACTIVE; /* 服务端窗口最小化，日志还能看 */
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    g_server = pi.hProcess;
    return true;
}

void stop_server() {
    if (!g_server) return;
    TerminateProcess(g_server, 0);
    CloseHandle(g_server);
    g_server = nullptr;
}
#else
static pid_t g_server;

bool start_server(const char *dir, int port) {
    char exe[1024], ps[16];
    snprintf(exe, sizeof exe, "%smcc_debug", dir);
    if (access(exe, X_OK) != 0) return false;
    snprintf(ps, sizeof ps, "%d", port);
    pid_t p = fork();
    if (p < 0) return false;
    if (p == 0) {
        execl(exe, exe, ps, (char *)nullptr);
        _exit(127);
    }
    g_server = p;
    return true;
}

void stop_server() {
    if (g_server <= 0) return;
    kill(g_server, SIGTERM);
    waitpid(g_server, nullptr, 0);
    g_server = 0;
}
#endif

} // namespace mcc::client
