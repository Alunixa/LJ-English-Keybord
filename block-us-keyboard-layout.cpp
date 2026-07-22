// ==WindhawkMod==
// @id              block-us-keyboard-layout
// @name            Block US Keyboard Layout
// @name:zh-CN      拦截美式键盘布局
// @description     Keeps English language features but removes and blocks the standard US keyboard layout.
// @description:zh-CN 保留英文语言功能，但移除并阻止“英语（美国）- 美式键盘”。
// @version         1.0.0
// @author          Gardenia
// @license         MIT
// @include         explorer.exe
// @include         ctfmon.exe
// @include         TextInputHost.exe
// @include         ShellExperienceHost.exe
// @include         ShellHost.exe
// @include         StartMenuExperienceHost.exe
// @include         SearchHost.exe
// @include         ApplicationFrameHost.exe
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# 拦截美式键盘布局

本模组针对标准美式键盘配置文件：

- 语言 ID：`0409`
- 键盘布局 ID：`00000409`
- 完整 TIP：`0409:00000409`

功能：

1. 保留英语语言包、英文显示语言和英文区域功能。
2. 禁用“英语（美国）- 美式键盘”输入配置文件。
3. 阻止 Windows 输入宿主重新加载和激活该布局。
4. 定时检查并清理被应用程序重新添加的美式键盘。
5. 当当前窗口停留在美式键盘时，自动切换到其他已安装输入法。

本模组不会删除 `en-US` 语言包。

必须至少安装一个其他输入法，例如：

- 中文（简体，中国）- 微软拼音
- 中文（繁体）输入法
- 其他非 `00000409` 的键盘布局

Windhawk 只能在用户登录并启动相关进程后工作，不能注入 Windows 登录界面或安全桌面。
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- CheckIntervalMs: 1000
  $name: 检查间隔（毫秒）
  $description: 定期检查美式键盘是否被 Windows 或应用程序重新添加。建议保持 1000。
- BlockLoading: true
  $name: 阻止加载
  $description: 阻止 Windows Shell 和输入宿主调用 LoadKeyboardLayout 加载美式键盘。
- BlockActivation: true
  $name: 阻止激活
  $description: 阻止 Windows Shell 和输入宿主激活美式键盘。
- RemoveFromProfile: true
  $name: 从当前用户输入配置中移除
  $description: 使用 Windows Text Services Framework 禁用美式键盘配置文件，但不删除英语语言包。
- LogActions: false
  $name: 记录操作
  $description: 在 Windhawk 日志中记录拦截和清理操作。
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include <atomic>

namespace {

constexpr DWORD kUsKlid = 0x00000409;
constexpr wchar_t kUsTip[] = L"0x0409:0x00000409";

// InstallLayoutOrTip 没有公开导入库，需要从 input.dll 动态获取。
constexpr DWORD ILOT_UNINSTALL = 0x00000001;

struct Settings {
    DWORD checkIntervalMs;
    bool blockLoading;
    bool blockActivation;
    bool removeFromProfile;
    bool logActions;
};

Settings g_settings{};
HANDLE g_stopEvent = nullptr;
HANDLE g_workerThread = nullptr;
HMODULE g_inputDll = nullptr;

using InstallLayoutOrTip_t = BOOL(WINAPI*)(LPCWSTR psz, DWORD dwFlags);
InstallLayoutOrTip_t g_installLayoutOrTip = nullptr;

using LoadKeyboardLayoutW_t = HKL(WINAPI*)(LPCWSTR pwszKLID, UINT flags);
using LoadKeyboardLayoutA_t = HKL(WINAPI*)(LPCSTR pszKLID, UINT flags);
using ActivateKeyboardLayout_t = HKL(WINAPI*)(HKL hkl, UINT flags);

LoadKeyboardLayoutW_t g_originalLoadKeyboardLayoutW = nullptr;
LoadKeyboardLayoutA_t g_originalLoadKeyboardLayoutA = nullptr;
ActivateKeyboardLayout_t g_originalActivateKeyboardLayout = nullptr;

std::atomic_bool g_workerStarted{false};

void Log(PCWSTR text) {
    if (g_settings.logActions) {
        Wh_Log(L"%s", text);
    }
}

void LoadSettings() {
    int interval = Wh_GetIntSetting(L"CheckIntervalMs");

    if (interval < 250) {
        interval = 250;
    } else if (interval > 60000) {
        interval = 60000;
    }

    g_settings.checkIntervalMs = static_cast<DWORD>(interval);
    g_settings.blockLoading = Wh_GetIntSetting(L"BlockLoading") != 0;
    g_settings.blockActivation = Wh_GetIntSetting(L"BlockActivation") != 0;
    g_settings.removeFromProfile =
        Wh_GetIntSetting(L"RemoveFromProfile") != 0;
    g_settings.logActions = Wh_GetIntSetting(L"LogActions") != 0;
}

bool IsExplorerProcess() {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));

    if (!length || length >= ARRAYSIZE(path)) {
        return false;
    }

    PCWSTR fileName = path;

    for (PCWSTR p = path; *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            fileName = p + 1;
        }
    }

    return _wcsicmp(fileName, L"explorer.exe") == 0;
}

bool IsStandardUsLayout(HKL hkl) {
    const ULONG_PTR value = reinterpret_cast<ULONG_PTR>(hkl);
    return static_cast<DWORD>(value & 0xFFFFFFFFull) == kUsKlid;
}

bool IsStandardUsKlidW(LPCWSTR klid) {
    if (!klid || !*klid) {
        return false;
    }

    while (*klid == L' ' || *klid == L'\t') {
        klid++;
    }

    if (klid[0] == L'0' && (klid[1] == L'x' || klid[1] == L'X')) {
        klid += 2;
    }

    wchar_t* end = nullptr;
    unsigned long value = wcstoul(klid, &end, 16);

    if (end == klid) {
        return false;
    }

    while (*end == L' ' || *end == L'\t') {
        end++;
    }

    return *end == L'\0' && static_cast<DWORD>(value) == kUsKlid;
}

bool IsStandardUsKlidA(LPCSTR klid) {
    if (!klid || !*klid) {
        return false;
    }

    while (*klid == ' ' || *klid == '\t') {
        klid++;
    }

    if (klid[0] == '0' && (klid[1] == 'x' || klid[1] == 'X')) {
        klid += 2;
    }

    char* end = nullptr;
    unsigned long value = strtoul(klid, &end, 16);

    if (end == klid) {
        return false;
    }

    while (*end == ' ' || *end == '\t') {
        end++;
    }

    return *end == '\0' && static_cast<DWORD>(value) == kUsKlid;
}

HKL FindFallbackLayout() {
    const int count = GetKeyboardLayoutList(0, nullptr);

    if (count <= 0) {
        return nullptr;
    }

    HKL stackLayouts[32];
    HKL* layouts = stackLayouts;
    HANDLE heapBuffer = nullptr;

    if (count > static_cast<int>(ARRAYSIZE(stackLayouts))) {
        const SIZE_T bytes = sizeof(HKL) * static_cast<SIZE_T>(count);

        heapBuffer = HeapAlloc(GetProcessHeap(), 0, bytes);
        if (!heapBuffer) {
            return nullptr;
        }

        layouts = static_cast<HKL*>(heapBuffer);
    }

    HKL fallback = nullptr;
    const int received = GetKeyboardLayoutList(count, layouts);

    for (int i = 0; i < received; i++) {
        if (layouts[i] && !IsStandardUsLayout(layouts[i])) {
            fallback = layouts[i];
            break;
        }
    }

    if (heapBuffer) {
        HeapFree(GetProcessHeap(), 0, heapBuffer);
    }

    return fallback;
}

void MoveForegroundWindowAwayFromUs(HKL fallback) {
    if (!fallback) {
        return;
    }

    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return;
    }

    DWORD foregroundThread =
        GetWindowThreadProcessId(foreground, nullptr);

    if (!foregroundThread) {
        return;
    }

    HKL foregroundLayout = GetKeyboardLayout(foregroundThread);

    if (!IsStandardUsLayout(foregroundLayout)) {
        return;
    }

    DWORD_PTR ignored = 0;

    // 窗口可以通过 DefWindowProc 接受此请求并切换输入布局。
    SendMessageTimeoutW(
        foreground,
        WM_INPUTLANGCHANGEREQUEST,
        INPUTLANGCHANGE_SYSCHARSET,
        reinterpret_cast<LPARAM>(fallback),
        SMTO_ABORTIFHUNG | SMTO_NORMAL,
        250,
        &ignored
    );

    Log(L"已请求当前窗口离开美式键盘布局");
}

bool HasFallbackLayout() {
    return FindFallbackLayout() != nullptr;
}

void DisableUsKeyboardProfile() {
    HKL fallback = FindFallbackLayout();

    // 防止用户只剩美式键盘时删除最后一个可用输入法。
    if (!fallback) {
        Log(L"未找到其他输入法，因此暂不禁用美式键盘");
        return;
    }

    MoveForegroundWindowAwayFromUs(fallback);

    // 让 explorer 自身也离开美式键盘。
    if (IsStandardUsLayout(GetKeyboardLayout(0))) {
        ActivateKeyboardLayout(fallback, KLF_SETFORPROCESS);
    }

    if (g_settings.removeFromProfile && g_installLayoutOrTip) {
        BOOL result =
            g_installLayoutOrTip(kUsTip, ILOT_UNINSTALL);

        if (result) {
            Log(L"已从当前用户输入配置中禁用美式键盘");
        } else if (g_settings.logActions) {
            Wh_Log(
                L"InstallLayoutOrTip 失败，GetLastError=%lu",
                GetLastError()
            );
        }
    }

    // 尝试从当前进程已加载布局列表中卸载残留 HKL。
    const int count = GetKeyboardLayoutList(0, nullptr);

    if (count > 0 && count <= 128) {
        HKL layouts[128];
        const int received =
            GetKeyboardLayoutList(ARRAYSIZE(layouts), layouts);

        for (int i = 0; i < received; i++) {
            if (IsStandardUsLayout(layouts[i])) {
                UnloadKeyboardLayout(layouts[i]);
            }
        }
    }
}

HKL WINAPI LoadKeyboardLayoutW_Hook(
    LPCWSTR pwszKLID,
    UINT flags
) {
    if (g_settings.blockLoading &&
        IsStandardUsKlidW(pwszKLID) &&
        HasFallbackLayout()) {
        Log(L"已拦截 LoadKeyboardLayoutW 加载美式键盘");
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    return g_originalLoadKeyboardLayoutW(pwszKLID, flags);
}

HKL WINAPI LoadKeyboardLayoutA_Hook(
    LPCSTR pszKLID,
    UINT flags
) {
    if (g_settings.blockLoading &&
        IsStandardUsKlidA(pszKLID) &&
        HasFallbackLayout()) {
        Log(L"已拦截 LoadKeyboardLayoutA 加载美式键盘");
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    return g_originalLoadKeyboardLayoutA(pszKLID, flags);
}

HKL WINAPI ActivateKeyboardLayout_Hook(
    HKL hkl,
    UINT flags
) {
    if (g_settings.blockActivation &&
        IsStandardUsLayout(hkl) &&
        HasFallbackLayout()) {
        Log(L"已拦截 ActivateKeyboardLayout 激活美式键盘");
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }

    return g_originalActivateKeyboardLayout(hkl, flags);
}

DWORD WINAPI WorkerThreadProc(void*) {
    // 第一次立即执行，确保 explorer 登录启动后尽快清理。
    DisableUsKeyboardProfile();

    while (true) {
        DWORD result = WaitForSingleObject(
            g_stopEvent,
            g_settings.checkIntervalMs
        );

        if (result != WAIT_TIMEOUT) {
            break;
        }

        DisableUsKeyboardProfile();
    }

    return 0;
}

void InitializeInputDll() {
    if (g_inputDll) {
        return;
    }

    // 明确从 System32 加载，避免普通 DLL 搜索路径。
    g_inputDll = LoadLibraryExW(
        L"input.dll",
        nullptr,
        LOAD_LIBRARY_SEARCH_SYSTEM32
    );

    if (!g_inputDll) {
        // 兼容少数 LoadLibraryEx 标志不可用的环境。
        wchar_t systemDirectory[MAX_PATH];

        UINT length = GetSystemDirectoryW(
            systemDirectory,
            ARRAYSIZE(systemDirectory)
        );

        if (length &&
            length < ARRAYSIZE(systemDirectory) - 12) {
            wcscat_s(
                systemDirectory,
                ARRAYSIZE(systemDirectory),
                L"\\input.dll"
            );

            g_inputDll = LoadLibraryW(systemDirectory);
        }
    }

    if (g_inputDll) {
        g_installLayoutOrTip =
            reinterpret_cast<InstallLayoutOrTip_t>(
                GetProcAddress(
                    g_inputDll,
                    "InstallLayoutOrTip"
                )
            );
    }
}

bool InstallUser32Hooks() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");

    if (!user32) {
        user32 = LoadLibraryW(L"user32.dll");
    }

    if (!user32) {
        return false;
    }

    bool success = true;

    void* loadW = reinterpret_cast<void*>(
        GetProcAddress(user32, "LoadKeyboardLayoutW")
    );

    void* loadA = reinterpret_cast<void*>(
        GetProcAddress(user32, "LoadKeyboardLayoutA")
    );

    void* activate = reinterpret_cast<void*>(
        GetProcAddress(user32, "ActivateKeyboardLayout")
    );

    if (loadW) {
        success &= Wh_SetFunctionHook(
            loadW,
            reinterpret_cast<void*>(LoadKeyboardLayoutW_Hook),
            reinterpret_cast<void**>(
                &g_originalLoadKeyboardLayoutW
            )
        );
    }

    if (loadA) {
        success &= Wh_SetFunctionHook(
            loadA,
            reinterpret_cast<void*>(LoadKeyboardLayoutA_Hook),
            reinterpret_cast<void**>(
                &g_originalLoadKeyboardLayoutA
            )
        );
    }

    if (activate) {
        success &= Wh_SetFunctionHook(
            activate,
            reinterpret_cast<void*>(
                ActivateKeyboardLayout_Hook
            ),
            reinterpret_cast<void**>(
                &g_originalActivateKeyboardLayout
            )
        );
    }

    return success;
}

} // namespace

BOOL Wh_ModInit() {
    LoadSettings();
    InitializeInputDll();

    if (!InstallUser32Hooks()) {
        Wh_Log(L"一个或多个键盘布局 Hook 安装失败");
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    // 只让 explorer 创建清理线程，避免多个输入宿主同时修改配置。
    if (!IsExplorerProcess()) {
        return;
    }

    bool expected = false;

    if (!g_workerStarted.compare_exchange_strong(
            expected,
            true)) {
        return;
    }

    g_stopEvent = CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr
    );

    if (!g_stopEvent) {
        g_workerStarted = false;
        return;
    }

    g_workerThread = CreateThread(
        nullptr,
        0,
        WorkerThreadProc,
        nullptr,
        0,
        nullptr
    );

    if (!g_workerThread) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
        g_workerStarted = false;
    }
}

void Wh_ModBeforeUninit() {
    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }

    if (g_workerThread) {
        WaitForSingleObject(g_workerThread, 3000);
        CloseHandle(g_workerThread);
        g_workerThread = nullptr;
    }

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }

    g_workerStarted = false;
}

void Wh_ModUninit() {
    if (g_inputDll) {
        FreeLibrary(g_inputDll);
        g_inputDll = nullptr;
        g_installLayoutOrTip = nullptr;
    }
}

BOOL Wh_ModSettingsChanged(BOOL* bReload) {
    // Hook 开关和检查间隔都通过重新载入可靠应用。
    *bReload = TRUE;
    return TRUE;
}
