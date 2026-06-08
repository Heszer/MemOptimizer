#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <shlobj.h>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <algorithm>
#include <thread>
#include <sstream>
#include <fstream>
#include <cctype>
#include <cwctype>
#include <stdlib.h>
#include <deque>
#include <gdiplus.h>

#include "resource.h"
#include <pathcch.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "pathcch.lib")
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

// 托盘菜单命令 ID
#define ID_TRAY_OPEN_WINDOW         3001
#define ID_TRAY_DISABLE_OPT         3002
#define ID_TRAY_ALL_OPT             3003
#define ID_TRAY_MEM_OPT             3004
#define ID_TRAY_VRAM_OPT            3005
#define ID_TRAY_JVM_OPT             3006
#define ID_TRAY_EXIT                3007

// 待机列表清除 API
typedef LONG NTSTATUS;
typedef enum _MEMORY_LIST_COMMAND { MemoryPurgeStandbyList = 4 } MEMORY_LIST_COMMAND;
typedef struct _SYSTEM_MEMORY_LIST_INFORMATION {
    HANDLE              MemoryListHandle;
    MEMORY_LIST_COMMAND MemoryListCommand;
} SYSTEM_MEMORY_LIST_INFORMATION;
typedef NTSTATUS(WINAPI* PNtSetSystemInformation)(ULONG, PVOID, ULONG);
const ULONG SystemMemoryListInformationClass = 80;

bool ClearStandbyList() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    auto NtSetSystemInformation = (PNtSetSystemInformation)GetProcAddress(ntdll, "NtSetSystemInformation");
    if (!NtSetSystemInformation) return false;
    SYSTEM_MEMORY_LIST_INFORMATION info = { 0, MemoryPurgeStandbyList };
    return NtSetSystemInformation(SystemMemoryListInformationClass, &info, sizeof(info)) >= 0;
}

std::wstring ToLower(const std::wstring& str) {
    std::wstring lower = str;
    for (auto& c : lower) c = towlower(c);
    return lower;
}

std::wstring GetConfigFilePath() {
    static std::wstring cachedPath;
    if (!cachedPath.empty()) return cachedPath;
    WCHAR appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring folder = std::wstring(appDataPath) + L"\\MemoryOptimizer";
        CreateDirectoryW(folder.c_str(), NULL);
        cachedPath = folder + L"\\Optimizer.ini";
    }
    else {
        WCHAR tempPath[MAX_PATH];
        GetTempPathW(MAX_PATH, tempPath);
        cachedPath = std::wstring(tempPath) + L"MemoryOptimizer.ini";
    }
    return cachedPath;
}

std::wstring ReadIni(const std::wstring& section, const std::wstring& key, const std::wstring& def = L"") {
    WCHAR buf[4096] = { 0 };
    GetPrivateProfileStringW(section.c_str(), key.c_str(), def.c_str(), buf, 4096, GetConfigFilePath().c_str());
    return buf;
}

void WriteIni(const std::wstring& section, const std::wstring& key, const std::wstring& value) {
    WritePrivateProfileStringW(section.c_str(), key.c_str(), value.c_str(), GetConfigFilePath().c_str());
}

HINSTANCE g_hInst;
HWND g_hSimpleDlg = nullptr;
HWND g_hAdvancedDlg = nullptr;

// 全局设置
std::set<std::wstring> g_whiteSet;          // 用户可见白名单（高级窗口显示）
std::set<std::wstring> g_defaultWhiteSet;   // 隐藏默认白名单（自动保护，不显示，包括自身和系统关键进程）
bool g_optimizationEnabled = true;
bool g_focusTrackingEnabled = false;
int g_maxFocusCount = 1;
int g_xmx = 2048;
bool g_hideToTray = true;

// 高级窗口控件
HWND g_hProcList = nullptr, g_hWhiteList = nullptr;
std::wstring g_searchFilter;
HFONT g_hBigFont = nullptr;
HIMAGELIST g_hImageList = nullptr;
HWND g_hTooltip = nullptr;
std::map<std::wstring, int> g_iconCache;
int g_defaultIconIndex = -1;
int g_leftSortCol = 1;
bool g_leftSortAsc = true;
int g_rightSortCol = 1;
bool g_rightSortAsc = true;

// 窗口聚焦
HWINEVENTHOOK g_hWinEventHook = nullptr;
std::deque<std::wstring> g_focusList;
std::set<std::wstring> g_focusSet;

// 背景图资源 (嵌入)
HBITMAP g_hBgSimple = NULL;
HBITMAP g_hBgAdvanced = NULL;
HBRUSH  g_hEmptyBrush = NULL;

// GDI+ 初始化 token
ULONG_PTR g_gdiplusToken = 0;

// 编辑框子类化过程
WNDPROC g_oldSearchEditProc = nullptr;
WNDPROC g_oldCountEditProc = nullptr;

// 托盘相关（仅简约窗口管理）
NOTIFYICONDATAW g_nid = { 0 };
HMENU g_hTrayMenu = NULL;
bool g_trayIconAdded = false;

// 前向声明
void UpdateSimpleDialogControls();
void UpdateAdvancedDialogControls();
void ExitProgram();

bool IsElevated() {
    BOOL fIsElevated = FALSE;
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD dwSize;
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &dwSize)) {
            fIsElevated = elevation.TokenIsElevated;
        }
        CloseHandle(hToken);
    }
    return fIsElevated == TRUE;
}

// 从资源提取 EmptyStandbyList.exe
std::wstring ExtractResourceToTemp(int resourceId, LPCWSTR resourceType, const std::wstring& desiredName) {
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(resourceId), resourceType);
    if (!hRes) return L"";
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData) return L"";
    DWORD dataSize = SizeofResource(NULL, hRes);
    LPVOID pData = LockResource(hData);
    if (!pData) return L"";

    WCHAR tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    WCHAR tempFile[MAX_PATH];
    GetTempFileNameW(tempPath, L"ESL", 0, tempFile);
    WCHAR* ext = wcsrchr(tempFile, L'.');
    if (ext) *ext = 0;
    wcscat_s(tempFile, L".exe");

    HANDLE hFile = CreateFileW(tempFile, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return L"";
    DWORD written;
    WriteFile(hFile, pData, dataSize, &written, NULL);
    CloseHandle(hFile);
    if (written != dataSize) {
        DeleteFileW(tempFile);
        return L"";
    }
    return tempFile;
}

bool RunEmptyStandbyList() {
    std::wstring exePath = ExtractResourceToTemp(IDR_EMPTY_STANDBY_LIST, RT_RCDATA, L"EmptyStandbyList.exe");
    if (exePath.empty()) return false;

    WCHAR cmdLine[MAX_PATH + 20];
    swprintf_s(cmdLine, L"\"%s\" standbylist", exePath.c_str());

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL success = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (success) {
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    DeleteFileW(exePath.c_str());
    return success == TRUE;
}

void SetButtonsOwnerDraw(HWND hDlg) {
    HWND hChild = GetWindow(hDlg, GW_CHILD);
    while (hChild) {
        WCHAR className[64];
        GetClassNameW(hChild, className, 64);
        if (wcscmp(className, L"Button") == 0) {
            LONG style = GetWindowLong(hChild, GWL_STYLE);
            if (!(style & BS_OWNERDRAW)) {
                SetWindowLong(hChild, GWL_STYLE, style | BS_OWNERDRAW);
                InvalidateRect(hChild, NULL, TRUE);
            }
        }
        hChild = GetNextWindow(hChild, GW_HWNDNEXT);
    }
}

int GetDefaultIconIndex() {
    if (g_defaultIconIndex != -1) return g_defaultIconIndex;
    WCHAR sysPath[MAX_PATH];
    GetSystemDirectoryW(sysPath, MAX_PATH);
    wcscat_s(sysPath, L"\\explorer.exe");
    SHFILEINFOW sfi = { 0 };
    if (SHGetFileInfoW(sysPath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
        if (sfi.hIcon) {
            g_defaultIconIndex = ImageList_AddIcon(g_hImageList, sfi.hIcon);
            DestroyIcon(sfi.hIcon);
            if (g_defaultIconIndex != -1) return g_defaultIconIndex;
        }
    }
    HICON hBlank = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, 16, 16, LR_SHARED);
    if (hBlank) g_defaultIconIndex = ImageList_AddIcon(g_hImageList, hBlank);
    else g_defaultIconIndex = 0;
    return g_defaultIconIndex;
}

std::pair<int, SIZE_T> GetProcessInstancesInfo(const std::wstring& targetName) {
    int count = 0;
    SIZE_T totalMemKB = 0;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return { 0, 0 };
    PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, targetName.c_str()) == 0) {
                count++;
                HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                if (hProc) {
                    PROCESS_MEMORY_COUNTERS pmc;
                    if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) totalMemKB += pmc.WorkingSetSize / 1024;
                    CloseHandle(hProc);
                }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    return { count, totalMemKB };
}

int GetProcessIconIndex(const std::wstring& exeName) {
    std::wstring key = ToLower(exeName);
    auto it = g_iconCache.find(key);
    if (it != g_iconCache.end()) return it->second;

    int defaultIdx = GetDefaultIconIndex();
    WCHAR fullPath[MAX_PATH] = { 0 };
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnap, &pe)) {
            do {
                if (_wcsicmp(pe.szExeFile, exeName.c_str()) == 0) {
                    HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        GetModuleFileNameExW(hProc, NULL, fullPath, MAX_PATH);
                        CloseHandle(hProc);
                    }
                    break;
                }
            } while (Process32NextW(hSnap, &pe));
        }
        CloseHandle(hSnap);
    }

    if (fullPath[0] == 0) {
        WCHAR sysPath[MAX_PATH];
        GetSystemDirectoryW(sysPath, MAX_PATH);
        swprintf_s(fullPath, L"%s\\%s", sysPath, exeName.c_str());
        if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES) {
            g_iconCache[key] = defaultIdx;
            return defaultIdx;
        }
    }

    SHFILEINFOW sfi = { 0 };
    if (SHGetFileInfoW(fullPath, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) {
        if (sfi.hIcon) {
            int index = ImageList_AddIcon(g_hImageList, sfi.hIcon);
            DestroyIcon(sfi.hIcon);
            if (index != -1) {
                g_iconCache[key] = index;
                return index;
            }
        }
    }
    g_iconCache[key] = defaultIdx;
    return defaultIdx;
}

int CALLBACK CompareProcLeft(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
    HWND hList = (HWND)lParamSort;
    int idx1 = (int)lParam1;
    int idx2 = (int)lParam2;
    WCHAR text1[256] = { 0 };
    WCHAR text2[256] = { 0 };
    if (g_leftSortCol == 1) {
        ListView_GetItemText(hList, idx1, 1, text1, 255);
        ListView_GetItemText(hList, idx2, 1, text2, 255);
        int pid1 = _wtoi(text1);
        int pid2 = _wtoi(text2);
        return g_leftSortAsc ? (pid1 - pid2) : (pid2 - pid1);
    }
    else if (g_leftSortCol == 3) {
        ListView_GetItemText(hList, idx1, 3, text1, 255);
        ListView_GetItemText(hList, idx2, 3, text2, 255);
        int mem1 = _wtoi(text1);
        int mem2 = _wtoi(text2);
        return g_leftSortAsc ? (mem1 - mem2) : (mem2 - mem1);
    }
    else {
        ListView_GetItemText(hList, idx1, 2, text1, 255);
        ListView_GetItemText(hList, idx2, 2, text2, 255);
        int cmp = _wcsicmp(text1, text2);
        return g_leftSortAsc ? cmp : -cmp;
    }
}

int CALLBACK CompareProcRight(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort) {
    HWND hList = (HWND)lParamSort;
    int idx1 = (int)lParam1;
    int idx2 = (int)lParam2;
    WCHAR text1[256] = { 0 };
    WCHAR text2[256] = { 0 };
    if (g_rightSortCol == 3) {
        ListView_GetItemText(hList, idx1, 3, text1, 255);
        ListView_GetItemText(hList, idx2, 3, text2, 255);
        int mem1 = _wtoi(text1);
        int mem2 = _wtoi(text2);
        return g_rightSortAsc ? (mem1 - mem2) : (mem2 - mem1);
    }
    else {
        ListView_GetItemText(hList, idx1, 1, text1, 255);
        ListView_GetItemText(hList, idx2, 1, text2, 255);
        int cmp = _wcsicmp(text1, text2);
        return g_rightSortAsc ? cmp : -cmp;
    }
}

void SortLeftList() {
    if (g_hProcList && ListView_GetItemCount(g_hProcList) > 0) {
        ListView_SortItemsEx(g_hProcList, CompareProcLeft, (LPARAM)g_hProcList);
    }
}

void SortRightList() {
    if (g_hWhiteList && ListView_GetItemCount(g_hWhiteList) > 0) {
        ListView_SortItemsEx(g_hWhiteList, CompareProcRight, (LPARAM)g_hWhiteList);
    }
}

void RefreshProcessList() {
    if (!g_hProcList) return;
    ListView_DeleteAllItems(g_hProcList);
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnap, &pe)) {
        int idx = 0;
        do {
            DWORD pid = pe.th32ProcessID;
            std::wstring name = pe.szExeFile;
            SIZE_T memKB = 0;
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hProc) {
                PROCESS_MEMORY_COUNTERS pmc;
                if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) memKB = pmc.WorkingSetSize / 1024;
                CloseHandle(hProc);
            }
            if (!g_searchFilter.empty()) {
                if (ToLower(name).find(ToLower(g_searchFilter)) == std::wstring::npos) continue;
            }
            WCHAR pidStr[16], memStr[32];
            _itow_s(pid, pidStr, 10);
            _itow_s((int)memKB, memStr, 10);
            int iconIdx = GetProcessIconIndex(name);
            LVITEMW item = { 0 };
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = idx;
            item.iImage = iconIdx;
            static wchar_t empty[] = L"";
            item.pszText = empty;
            item.iSubItem = 0;
            ListView_InsertItem(g_hProcList, &item);
            ListView_SetItemText(g_hProcList, idx, 1, pidStr);
            ListView_SetItemText(g_hProcList, idx, 2, const_cast<LPWSTR>(name.c_str()));
            ListView_SetItemText(g_hProcList, idx, 3, memStr);
            idx++;
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    SortLeftList();
}

void UpdateWhiteListUI() {
    if (!g_hWhiteList) return;
    ListView_DeleteAllItems(g_hWhiteList);
    int idx = 0;
    for (const auto& name : g_whiteSet) {
        auto info = GetProcessInstancesInfo(name);
        WCHAR countStr[16], memStr[32];
        _itow_s(info.first, countStr, 10);
        _itow_s((int)info.second, memStr, 10);
        int iconIdx = GetProcessIconIndex(name);
        LVITEMW item = { 0 };
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = idx;
        item.iImage = iconIdx;
        static wchar_t empty[] = L"";
        item.pszText = empty;
        ListView_InsertItem(g_hWhiteList, &item);
        ListView_SetItemText(g_hWhiteList, idx, 1, const_cast<LPWSTR>(name.c_str()));
        ListView_SetItemText(g_hWhiteList, idx, 2, countStr);
        ListView_SetItemText(g_hWhiteList, idx, 3, memStr);
        idx++;
    }
    SortRightList();
}

void SaveWhiteList() {
    std::wstring combined;
    for (const auto& n : g_whiteSet) {
        if (!combined.empty()) combined += L";";
        combined += n;
    }
    WriteIni(L"Settings", L"WhiteList", combined);
}

void LoadWhiteList() {
    std::wstring data = ReadIni(L"Settings", L"WhiteList");
    g_whiteSet.clear();
    if (data.empty()) return;
    std::wstringstream ss(data);
    std::wstring token;
    while (std::getline(ss, token, L';')) if (!token.empty()) g_whiteSet.insert(ToLower(token));
}

std::wstring GetSelectedProcessName() {
    int sel = ListView_GetNextItem(g_hProcList, -1, LVNI_SELECTED);
    if (sel == -1) return L"";
    WCHAR name[260] = { 0 };
    ListView_GetItemText(g_hProcList, sel, 2, name, 260);
    return name;
}

void AddToWhiteList(const std::wstring& procName) {
    if (procName.empty()) return;
    g_whiteSet.insert(ToLower(procName));
    UpdateWhiteListUI();
    SaveWhiteList();
}

void RemoveSelectedFromWhite() {
    int sel = ListView_GetNextItem(g_hWhiteList, -1, LVNI_SELECTED);
    if (sel == -1) return;
    WCHAR name[260] = { 0 };
    ListView_GetItemText(g_hWhiteList, sel, 1, name, 260);
    g_whiteSet.erase(ToLower(name));
    UpdateWhiteListUI();
    SaveWhiteList();
}

void UpdateFocusWhiteList(const std::wstring& procName) {
    if (procName.empty()) return;
    std::wstring lowerName = ToLower(procName);
    for (auto it = g_focusList.begin(); it != g_focusList.end(); ++it) {
        if (*it == lowerName) {
            g_focusList.erase(it);
            break;
        }
    }
    g_focusList.push_front(lowerName);
    while ((int)g_focusList.size() > g_maxFocusCount) {
        std::wstring removed = g_focusList.back();
        g_focusList.pop_back();
        g_focusSet.erase(removed);
    }
    g_focusSet.clear();
    for (const auto& name : g_focusList) g_focusSet.insert(name);
}

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime) {
    if (!g_focusTrackingEnabled) return;
    if (event == EVENT_SYSTEM_FOREGROUND && hwnd != NULL) {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
            if (hProc) {
                WCHAR exePath[MAX_PATH] = { 0 };
                if (GetModuleFileNameExW(hProc, NULL, exePath, MAX_PATH)) {
                    wchar_t* exeName = wcsrchr(exePath, L'\\');
                    if (exeName) exeName++;
                    else exeName = exePath;
                    UpdateFocusWhiteList(exeName);
                }
                CloseHandle(hProc);
            }
        }
    }
}

void SetFocusTracking(bool enable) {
    if (enable == g_focusTrackingEnabled) return;
    g_focusTrackingEnabled = enable;
    if (enable) {
        g_focusList.clear();
        g_focusSet.clear();
        if (!g_hWinEventHook) {
            g_hWinEventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                NULL, WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
        }
    }
    else {
        g_focusList.clear();
        g_focusSet.clear();
        if (g_hWinEventHook) {
            UnhookWinEvent(g_hWinEventHook);
            g_hWinEventHook = nullptr;
        }
    }
}

bool IsProcessSkipped(const std::wstring& procName) {
    std::wstring lowerName = ToLower(procName);
    // 优先检查隐藏默认白名单（包括程序自身和系统关键进程）
    if (g_defaultWhiteSet.find(lowerName) != g_defaultWhiteSet.end()) return true;
    // 其次检查用户白名单
    if (g_whiteSet.find(lowerName) != g_whiteSet.end()) return true;
    // 最后检查动态聚焦白名单
    if (g_focusTrackingEnabled && g_focusSet.find(lowerName) != g_focusSet.end()) return true;
    return false;
}

void EmptyWorkingSets(bool skipWhiteList) {
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
    if (Process32FirstW(hSnap, &pe)) {
        do {
            if (skipWhiteList && IsProcessSkipped(pe.szExeFile)) continue;
            HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
            if (hProc) { EmptyWorkingSet(hProc); CloseHandle(hProc); }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
}

void CleanMemoryThread() {
    if (!g_optimizationEnabled) return;
    EmptyWorkingSets(true);
    MessageBoxW(g_hSimpleDlg ? g_hSimpleDlg : g_hAdvancedDlg, L"内存优化完成", L"完成", MB_ICONINFORMATION);
}

void GameBoostThread() {
    if (!g_optimizationEnabled) return;
    EmptyWorkingSets(true);
    if (RunEmptyStandbyList()) {
        MessageBoxW(g_hSimpleDlg ? g_hSimpleDlg : g_hAdvancedDlg,
            L"显存优化完成", L"完成", MB_ICONINFORMATION);
    }
    else {
        if (ClearStandbyList()) {
            MessageBoxW(g_hSimpleDlg ? g_hSimpleDlg : g_hAdvancedDlg,
                L"显存优化完成", L"完成", MB_ICONINFORMATION);
        }
        else {
            MessageBoxW(g_hSimpleDlg ? g_hSimpleDlg : g_hAdvancedDlg,
                L"显存优化部分完成（工作集已清理，待机内存清理失败）\n请确保以管理员身份运行。", L"提示", MB_ICONWARNING);
        }
    }
}

int GetRecommendedXmx() {
    MEMORYSTATUSEX mem = { sizeof(mem) };
    GlobalMemoryStatusEx(&mem);
    double totalGB = mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    if (totalGB < 4) return (int)(totalGB * 512);
    if (totalGB < 8) return 2048;
    if (totalGB < 16) return 4096;
    return 8192;
}

void OptimizeJavaProcesses() {
    if (!g_optimizationEnabled) return;
    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{ sizeof(PROCESSENTRY32W) };
    int count = 0;
    if (Process32FirstW(hSnap, &pe)) {
        do {
            std::wstring name = ToLower(pe.szExeFile);
            if ((name.find(L"java") != std::wstring::npos || name.find(L"javaw") != std::wstring::npos) &&
                !IsProcessSkipped(pe.szExeFile)) {
                HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA, FALSE, pe.th32ProcessID);
                if (hProc) { EmptyWorkingSet(hProc); CloseHandle(hProc); count++; }
            }
        } while (Process32NextW(hSnap, &pe));
    }
    CloseHandle(hSnap);
    MessageBoxW(g_hSimpleDlg ? g_hSimpleDlg : g_hAdvancedDlg, (L"已优化 " + std::to_wstring(count) + L" 个 Java 进程").c_str(), L"完成", MB_ICONINFORMATION);
}

// 编辑框子类化窗口过程
LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: {
        HWND hParent = GetParent(hWnd);
        if (hParent && g_hBgAdvanced) {
            RECT rcClient;
            GetClientRect(hWnd, &rcClient);
            POINT pt = { rcClient.left, rcClient.top };
            MapWindowPoints(hWnd, hParent, &pt, 1);
            HDC hdc = (HDC)wParam;
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hBgAdvanced);
            BitBlt(hdc, 0, 0, rcClient.right - rcClient.left, rcClient.bottom - rcClient.top,
                hdcMem, pt.x, pt.y, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
            return TRUE;
        }
        break;
    }
    }
    return CallWindowProc((WNDPROC)GetWindowLongPtr(hWnd, GWLP_USERDATA), hWnd, msg, wParam, lParam);
}

void SubclassEditControl(HWND hEdit, WNDPROC newProc, WNDPROC& oldProc) {
    oldProc = (WNDPROC)SetWindowLongPtr(hEdit, GWLP_WNDPROC, (LONG_PTR)newProc);
    SetWindowLongPtr(hEdit, GWLP_USERDATA, (LONG_PTR)oldProc);
}

// 同步简约窗口的控件状态（从全局变量更新）
void UpdateSimpleDialogControls() {
    if (g_hSimpleDlg) {
        CheckDlgButton(g_hSimpleDlg, IDC_SIMPLE_ENABLE, g_optimizationEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSimpleDlg, IDC_SIMPLE_FOCUS_ENABLE, g_focusTrackingEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hSimpleDlg, IDC_SIMPLE_HIDE_TRAY, g_hideToTray ? BST_CHECKED : BST_UNCHECKED);
    }
}

// 同步高级窗口的控件状态
void UpdateAdvancedDialogControls() {
    if (g_hAdvancedDlg) {
        CheckDlgButton(g_hAdvancedDlg, IDC_ENABLE, g_optimizationEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hAdvancedDlg, IDC_FOCUS_ENABLE, g_focusTrackingEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(g_hAdvancedDlg, IDC_ADVANCED_HIDE_TRAY, g_hideToTray ? BST_CHECKED : BST_UNCHECKED);
    }
}

// 托盘功能（仅简约窗口使用）
void AddTrayIcon(HWND hWnd) {
    if (g_trayIconAdded) return;
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    wcscpy_s(g_nid.szTip, L"Memory & GPU Optimizer");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayIconAdded = true;
}

void RemoveTrayIcon() {
    if (g_trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconAdded = false;
    }
}

void ShowTrayContextMenu(HWND hWnd) {
    if (g_hTrayMenu) DestroyMenu(g_hTrayMenu);
    g_hTrayMenu = CreatePopupMenu();
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_OPEN_WINDOW, L"打开窗口");
    AppendMenuW(g_hTrayMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_DISABLE_OPT, L"关闭优化");
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_ALL_OPT, L"一键三连");
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_MEM_OPT, L"内存优化");
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_VRAM_OPT, L"显存优化");
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_JVM_OPT, L"JVM优化");
    AppendMenuW(g_hTrayMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_hTrayMenu, MF_STRING, ID_TRAY_EXIT, L"关闭程序");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(g_hTrayMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    PostMessage(hWnd, WM_NULL, 0, 0);
}

// 统一退出程序
void ExitProgram() {
    // 关闭优化功能
    g_optimizationEnabled = false;
    WriteIni(L"Settings", L"Enable", L"0");
    UpdateSimpleDialogControls();
    UpdateAdvancedDialogControls();
    // 移除托盘图标
    RemoveTrayIcon();
    // 关闭所有窗口并退出消息循环
    if (g_hSimpleDlg) DestroyWindow(g_hSimpleDlg);
    if (g_hAdvancedDlg) DestroyWindow(g_hAdvancedDlg);
    PostQuitMessage(0);
}

// 高级窗口过程
INT_PTR CALLBACK AdvancedDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG: {
        g_hAdvancedDlg = hDlg;
        g_hProcList = GetDlgItem(hDlg, IDC_PROCESS_LIST);
        g_hWhiteList = GetDlgItem(hDlg, IDC_WHITE_LIST);
        g_iconCache.clear();
        g_defaultIconIndex = -1;
        g_hImageList = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 32, 32);
        ListView_SetImageList(g_hProcList, g_hImageList, LVSIL_SMALL);
        ListView_SetImageList(g_hWhiteList, g_hImageList, LVSIL_SMALL);
        GetDefaultIconIndex();

        LVCOLUMNW col = { 0 };
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 20; wchar_t col0[] = L""; col.pszText = col0; ListView_InsertColumn(g_hProcList, 0, &col);
        col.cx = 55; wchar_t col1[] = L"PID"; col.pszText = col1; ListView_InsertColumn(g_hProcList, 1, &col);
        col.cx = 130; wchar_t col2[] = L"进程名"; col.pszText = col2; ListView_InsertColumn(g_hProcList, 2, &col);
        col.cx = 70; wchar_t col3[] = L"内存(KB)"; col.pszText = col3; ListView_InsertColumn(g_hProcList, 3, &col);
        col.cx = 20; wchar_t rcol0[] = L""; col.pszText = rcol0; ListView_InsertColumn(g_hWhiteList, 0, &col);
        col.cx = 110; wchar_t rcol1[] = L"进程名"; col.pszText = rcol1; ListView_InsertColumn(g_hWhiteList, 1, &col);
        col.cx = 55; wchar_t rcol2[] = L"实例数"; col.pszText = rcol2; ListView_InsertColumn(g_hWhiteList, 2, &col);
        col.cx = 75; wchar_t rcol3[] = L"总内存(KB)"; col.pszText = rcol3; ListView_InsertColumn(g_hWhiteList, 3, &col);

        ListView_SetExtendedListViewStyle(g_hProcList, LVS_EX_FULLROWSELECT);
        ListView_SetExtendedListViewStyle(g_hWhiteList, LVS_EX_FULLROWSELECT);

        g_hBigFont = CreateFont(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessage(g_hProcList, WM_SETFONT, (WPARAM)g_hBigFont, TRUE);
        SendMessage(g_hWhiteList, WM_SETFONT, (WPARAM)g_hBigFont, TRUE);

        LoadWhiteList();
        UpdateWhiteListUI();
        CheckDlgButton(hDlg, IDC_ENABLE, g_optimizationEnabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hDlg, IDC_XMX_EDIT, g_xmx, FALSE);
        CheckDlgButton(hDlg, IDC_FOCUS_ENABLE, g_focusTrackingEnabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hDlg, IDC_FOCUS_COUNT, g_maxFocusCount, FALSE);
        CheckDlgButton(hDlg, IDC_ADVANCED_HIDE_TRAY, g_hideToTray ? BST_CHECKED : BST_UNCHECKED);
        RefreshProcessList();

        SetTimer(hDlg, 1, 5000, NULL);

        RECT rcAdv;
        GetWindowRect(hDlg, &rcAdv);
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenWidth - (rcAdv.right - rcAdv.left)) / 2;
        int y = (screenHeight - (rcAdv.bottom - rcAdv.top)) / 2;
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        SetButtonsOwnerDraw(hDlg);

        HWND hSearchEdit = GetDlgItem(hDlg, IDC_SEARCH_EDIT);
        SubclassEditControl(hSearchEdit, EditSubclassProc, g_oldSearchEditProc);
        HWND hCountEdit = GetDlgItem(hDlg, IDC_FOCUS_COUNT);
        SubclassEditControl(hCountEdit, EditSubclassProc, g_oldCountEditProc);
        break;
    }
    case WM_ERASEBKGND: {
        if (g_hBgAdvanced) {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hDlg, &rc);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hBgAdvanced);
            BITMAP bm;
            GetObject(g_hBgAdvanced, sizeof(bm), &bm);
            StretchBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        // 内存状态栏使用白色背景避免文字残留
        if (GetDlgItem(hDlg, IDC_MEM_INFO) == (HWND)lParam) {
            static HBRUSH hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
            return (INT_PTR)hWhiteBrush;
        }
        if (g_hEmptyBrush)
            return (INT_PTR)g_hEmptyBrush;
        else
            return (INT_PTR)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdcEdit = (HDC)wParam;
        SetBkMode(hdcEdit, OPAQUE);
        SetTextColor(hdcEdit, RGB(0, 0, 0));
        static HBRUSH hWhiteBrush = NULL;
        if (!hWhiteBrush) {
            hWhiteBrush = CreateSolidBrush(RGB(255, 255, 255));
        }
        return (INT_PTR)hWhiteBrush;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;
        if (lpDIS->CtlType == ODT_BUTTON) {
            HDC hdc = lpDIS->hDC;
            RECT rc = lpDIS->rcItem;
            Graphics graphics(hdc);
            SolidBrush brush(Color(153, 255, 255, 255));
            graphics.FillRectangle(&brush, rc.left, rc.top, (INT)(rc.right - rc.left), (INT)(rc.bottom - rc.top));
            wchar_t text[256];
            GetWindowTextW(lpDIS->hwndItem, text, 256);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0));
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        if (HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == IDC_SEARCH_EDIT) {
            WCHAR filter[256];
            GetDlgItemTextW(hDlg, IDC_SEARCH_EDIT, filter, 256);
            g_searchFilter = filter;
            RefreshProcessList();
            return TRUE;
        }
        switch (LOWORD(wParam)) {
        case IDC_ENABLE:
            g_optimizationEnabled = (IsDlgButtonChecked(hDlg, IDC_ENABLE) == BST_CHECKED);
            WriteIni(L"Settings", L"Enable", g_optimizationEnabled ? L"1" : L"0");
            break;
        case IDC_ADVANCED_HIDE_TRAY:
            g_hideToTray = (IsDlgButtonChecked(hDlg, IDC_ADVANCED_HIDE_TRAY) == BST_CHECKED);
            WriteIni(L"Settings", L"HideToTray", g_hideToTray ? L"1" : L"0");
            break;
        case IDC_BTN_REFRESH: RefreshProcessList(); break;
        case IDC_BTN_ADD_WHITE: {
            std::wstring sel = GetSelectedProcessName();
            if (!sel.empty()) AddToWhiteList(sel);
            else MessageBoxW(hDlg, L"请先在左侧选择进程", L"提示", MB_ICONINFORMATION);
            break;
        }
        case IDC_BTN_REMOVE_WHITE: RemoveSelectedFromWhite(); break;
        case IDC_BTN_CLEAR_WHITE: g_whiteSet.clear(); UpdateWhiteListUI(); SaveWhiteList(); break;
        case IDC_RECOMMEND_JVM: {
            int rec = GetRecommendedXmx();
            SetDlgItemInt(hDlg, IDC_XMX_EDIT, rec, FALSE);
            g_xmx = rec;
            WriteIni(L"Settings", L"JvmXmx", std::to_wstring(rec));
            break;
        }
        case IDC_COPY_JVM: {
            BOOL ok;
            int xmx = GetDlgItemInt(hDlg, IDC_XMX_EDIT, &ok, FALSE);
            if (!ok) break;
            std::wstring args = L"-Xmx" + std::to_wstring(xmx) + L"M -Xms" + std::to_wstring(std::min(xmx, 512)) + L"M -XX:+UseG1GC -XX:+ParallelRefProcEnabled";
            if (OpenClipboard(hDlg)) {
                EmptyClipboard();
                HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (args.length() + 1) * sizeof(wchar_t));
                if (hMem) {
                    wchar_t* pMem = (wchar_t*)GlobalLock(hMem);
                    if (pMem) {
                        wcscpy_s(pMem, args.length() + 1, args.c_str());
                        GlobalUnlock(hMem);
                        SetClipboardData(CF_UNICODETEXT, hMem);
                    }
                    else GlobalFree(hMem);
                }
                CloseClipboard();
            }
            SetDlgItemTextW(hDlg, IDC_JVM_ARGS, (L"已复制: " + args).c_str());
            break;
        }
        case IDC_OPT_JAVA: std::thread(OptimizeJavaProcesses).detach(); break;
        case IDC_BTN_OPT_MEM: std::thread(CleanMemoryThread).detach(); break;
        case IDC_BTN_GAME_BOOST: std::thread(GameBoostThread).detach(); break;
        case IDC_BTN_RESET_DEFAULTS: {
            int rec = GetRecommendedXmx();
            int resetXmx = std::max(rec / 2, 512);
            SetDlgItemInt(hDlg, IDC_XMX_EDIT, resetXmx, FALSE);
            g_xmx = resetXmx;
            WriteIni(L"Settings", L"JvmXmx", std::to_wstring(resetXmx));
            g_optimizationEnabled = false;
            CheckDlgButton(hDlg, IDC_ENABLE, BST_UNCHECKED);
            WriteIni(L"Settings", L"Enable", L"0");
            CheckDlgButton(hDlg, IDC_FOCUS_ENABLE, BST_UNCHECKED);
            SetFocusTracking(false);
            SetDlgItemInt(hDlg, IDC_FOCUS_COUNT, 1, FALSE);
            g_maxFocusCount = 1;
            WriteIni(L"Settings", L"FocusTracking", L"0");
            WriteIni(L"Settings", L"FocusCount", L"1");
            MessageBoxW(hDlg, L"已恢复默认设置", L"提示", MB_ICONINFORMATION);
            break;
        }
        case IDC_BTN_HELP: {
            MessageBoxW(hDlg, L"感谢您使用本工具！\n\n - - - > 开发者：Heszer < - - - \n 邮箱：h3532886804@163.com \n\n本工具用于对内存、显存、JVM的优化，\n通过手动或自动清理内存工作集、待机列表，适合游戏或大型软件运行前释放资源。\n不会删除您的任何文件，也不会修改注册表或系统配置。\n\n【注意事项】\n• 清理内存时，后台程序（如浏览器、文档编辑器）可能会短暂响应变慢，这是正常现象。\n  建议使用重要软件前先手动保存，避免意外丢失数据。\n• 部分高级功能需要管理员身份才能生效，若非管理员账户，功能会自动降级或提示。\n• 本工具在 Windows 11 上测试通过，理论支持 Windows 7 及更高版本，\n  但由于杀毒软件、驱动或系统补丁差异，个别功能可能无法使用，还请理解。\n• 本工具不会联网上传任何数据，也不会偷偷记录您的信息。\n  所有配置保存在 %LocalAppData%\\MemoryOptimizer\\Optimizer.ini。\n\n【责任说明】\n使用时请谨记保存重要文件，以免出现内存误删的情况，开发者仅提供技术支持，出现任何问题请自行承担。", L"帮助", MB_ICONINFORMATION);
            break;
        }
        case IDC_FOCUS_ENABLE: {
            bool enable = (IsDlgButtonChecked(hDlg, IDC_FOCUS_ENABLE) == BST_CHECKED);
            SetFocusTracking(enable);
            WriteIni(L"Settings", L"FocusTracking", enable ? L"1" : L"0");
            break;
        }
        case IDC_FOCUS_COUNT: {
            if (HIWORD(wParam) == EN_CHANGE) {
                BOOL ok;
                int newCount = GetDlgItemInt(hDlg, IDC_FOCUS_COUNT, &ok, FALSE);
                if (ok && newCount >= 1) {
                    g_maxFocusCount = newCount;
                    WriteIni(L"Settings", L"FocusCount", std::to_wstring(newCount));
                    if (g_focusTrackingEnabled) {
                        while ((int)g_focusList.size() > g_maxFocusCount) {
                            std::wstring removed = g_focusList.back();
                            g_focusList.pop_back();
                            g_focusSet.erase(removed);
                        }
                    }
                }
                else {
                    SetDlgItemInt(hDlg, IDC_FOCUS_COUNT, g_maxFocusCount, FALSE);
                }
            }
            break;
        }
        case IDC_BTN_BACK_TO_SIMPLE: {
            UpdateSimpleDialogControls();   // 同步简约窗口状态
            DestroyWindow(hDlg);
            if (g_hSimpleDlg) {
                ShowWindow(g_hSimpleDlg, SW_SHOW);
                SetForegroundWindow(g_hSimpleDlg);
            }
            break;
        }
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR* pnmh = (NMHDR*)lParam;
        if (pnmh->code == NM_CUSTOMDRAW && (pnmh->idFrom == IDC_PROCESS_LIST || pnmh->idFrom == IDC_WHITE_LIST)) {
            LPNMLVCUSTOMDRAW lplvcd = (LPNMLVCUSTOMDRAW)lParam;
            if (lplvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                return CDRF_NOTIFYITEMDRAW;
            }
            else if (lplvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                HDC hdc = lplvcd->nmcd.hdc;
                lplvcd->clrText = RGB(0, 0, 0);
                SetBkMode(hdc, TRANSPARENT);
                return CDRF_NEWFONT;
            }
        }
        if (pnmh->code == NM_DBLCLK) {
            if (pnmh->idFrom == IDC_PROCESS_LIST) {
                std::wstring name = GetSelectedProcessName();
                if (!name.empty()) AddToWhiteList(name);
                return TRUE;
            }
            else if (pnmh->idFrom == IDC_WHITE_LIST) {
                RemoveSelectedFromWhite();
                return TRUE;
            }
        }
        if (pnmh->code == HDN_ITEMCLICK) {
            NMHEADER* phdr = (NMHEADER*)lParam;
            int col = phdr->iItem;
            if (pnmh->idFrom == IDC_PROCESS_LIST) {
                if (col == 0) return FALSE;
                if (col == g_leftSortCol) g_leftSortAsc = !g_leftSortAsc;
                else { g_leftSortCol = col; g_leftSortAsc = true; }
                SortLeftList();
                return TRUE;
            }
            else if (pnmh->idFrom == IDC_WHITE_LIST) {
                if (col == 0 || col == 2) return FALSE;
                if (col == g_rightSortCol) g_rightSortAsc = !g_rightSortAsc;
                else { g_rightSortCol = col; g_rightSortAsc = true; }
                SortRightList();
                return TRUE;
            }
        }
        break;
    }
    case WM_TIMER: {
        MEMORYSTATUSEX mem = { sizeof(mem) };
        GlobalMemoryStatusEx(&mem);
        WCHAR buf[256];
        swprintf_s(buf, L"总内存: %.1f GB | 可用: %.1f GB (%d%%)",
            mem.ullTotalPhys / (1024.0 * 1024.0 * 1024.0),
            mem.ullAvailPhys / (1024.0 * 1024.0 * 1024.0), mem.dwMemoryLoad);
        SetDlgItemTextW(hDlg, IDC_MEM_INFO, buf);
        UpdateWhiteListUI();
        break;
    }
    case WM_CLOSE: {
        if (g_hideToTray) {
            // 隐藏高级窗口，确保简约窗口的托盘图标存在
            ShowWindow(hDlg, SW_HIDE);
            if (g_hSimpleDlg && !g_trayIconAdded) {
                AddTrayIcon(g_hSimpleDlg);
            }
        }
        else {
            UpdateSimpleDialogControls();
            DestroyWindow(hDlg);
            if (g_hSimpleDlg) {
                ShowWindow(g_hSimpleDlg, SW_SHOW);
                SetForegroundWindow(g_hSimpleDlg);
            }
        }
        break;
    }
    case WM_DESTROY: {
        if (g_hBigFont) DeleteObject(g_hBigFont);
        if (g_hImageList) ImageList_Destroy(g_hImageList);
        g_hAdvancedDlg = nullptr;
        break;
    }
    }
    return FALSE;
}

// 简约窗口过程
INT_PTR CALLBACK SimpleDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static HBRUSH hBrush = NULL;
    switch (msg) {
    case WM_INITDIALOG: {
        g_hSimpleDlg = hDlg;
        bool elevated = IsElevated();
        if (hBrush) DeleteObject(hBrush);
        hBrush = CreateSolidBrush(elevated ? RGB(192, 255, 192) : RGB(255, 192, 192));
        if (!elevated) {
            MessageBoxW(hDlg, L"程序未以管理员身份运行，部分优化功能可能受限。建议以管理员权限重新启动。", L"权限提示", MB_ICONWARNING);
        }
        g_optimizationEnabled = (ReadIni(L"Settings", L"Enable", L"1") == L"1");
        g_focusTrackingEnabled = (ReadIni(L"Settings", L"FocusTracking", L"0") == L"1");
        g_maxFocusCount = _wtoi(ReadIni(L"Settings", L"FocusCount", L"1").c_str());
        if (g_maxFocusCount < 1) g_maxFocusCount = 1;
        g_hideToTray = (ReadIni(L"Settings", L"HideToTray", L"1") == L"1");
        CheckDlgButton(hDlg, IDC_SIMPLE_ENABLE, g_optimizationEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SIMPLE_FOCUS_ENABLE, g_focusTrackingEnabled ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hDlg, IDC_SIMPLE_HIDE_TRAY, g_hideToTray ? BST_CHECKED : BST_UNCHECKED);
        SetFocusTracking(g_focusTrackingEnabled);

        RECT rcSimple;
        GetWindowRect(hDlg, &rcSimple);
        int screenWidth = GetSystemMetrics(SM_CXSCREEN);
        int screenHeight = GetSystemMetrics(SM_CYSCREEN);
        int x = (screenWidth - (rcSimple.right - rcSimple.left)) / 2;
        int y = (screenHeight - (rcSimple.bottom - rcSimple.top)) / 2;
        SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);

        SetButtonsOwnerDraw(hDlg);
        break;
    }
    case WM_ERASEBKGND: {
        if (g_hBgSimple) {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hDlg, &rc);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hBgSimple);
            BITMAP bm;
            GetObject(g_hBgSimple, sizeof(bm), &bm);
            StretchBlt(hdc, 0, 0, rc.right, rc.bottom, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
            return TRUE;
        }
        else {
            if (hBrush) {
                RECT rc;
                GetClientRect(hDlg, &rc);
                FillRect((HDC)wParam, &rc, hBrush);
                return TRUE;
            }
        }
        return FALSE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdcStatic = (HDC)wParam;
        SetBkMode(hdcStatic, TRANSPARENT);
        if (g_hEmptyBrush)
            return (INT_PTR)g_hEmptyBrush;
        else
            return (INT_PTR)GetStockObject(NULL_BRUSH);
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT lpDIS = (LPDRAWITEMSTRUCT)lParam;
        if (lpDIS->CtlType == ODT_BUTTON) {
            HDC hdc = lpDIS->hDC;
            RECT rc = lpDIS->rcItem;
            Graphics graphics(hdc);
            SolidBrush brush(Color(153, 255, 255, 255));
            graphics.FillRectangle(&brush, rc.left, rc.top, (INT)(rc.right - rc.left), (INT)(rc.bottom - rc.top));
            wchar_t text[256];
            GetWindowTextW(lpDIS->hwndItem, text, 256);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(0, 0, 0));
            DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return TRUE;
        }
        return FALSE;
    }
    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_SIMPLE_JVM_OPT:
            if (g_optimizationEnabled) {
                std::thread(OptimizeJavaProcesses).detach();
            }
            break;
        case IDC_SIMPLE_MEM_OPT:
            if (g_optimizationEnabled) {
                std::thread(CleanMemoryThread).detach();
            }
            break;
        case IDC_SIMPLE_VRAM_OPT:
            if (g_optimizationEnabled) {
                std::thread(GameBoostThread).detach();
            }
            break;
        case IDC_SIMPLE_ALL_OPT:
            // 一键三连：如果未开启优化，则先开启
            if (!g_optimizationEnabled) {
                g_optimizationEnabled = true;
                WriteIni(L"Settings", L"Enable", L"1");
                UpdateSimpleDialogControls();
                UpdateAdvancedDialogControls();
            }
            std::thread([]() {
                CleanMemoryThread();
                GameBoostThread();
                }).detach();
            break;
        case IDC_SIMPLE_RESET: {
            int rec = GetRecommendedXmx();
            int resetXmx = std::max(rec / 2, 512);
            g_xmx = resetXmx;
            WriteIni(L"Settings", L"JvmXmx", std::to_wstring(resetXmx));
            g_optimizationEnabled = false;
            CheckDlgButton(hDlg, IDC_SIMPLE_ENABLE, BST_UNCHECKED);
            WriteIni(L"Settings", L"Enable", L"0");
            CheckDlgButton(hDlg, IDC_SIMPLE_FOCUS_ENABLE, BST_UNCHECKED);
            SetFocusTracking(false);
            g_focusTrackingEnabled = false;
            g_maxFocusCount = 1;
            WriteIni(L"Settings", L"FocusTracking", L"0");
            WriteIni(L"Settings", L"FocusCount", L"1");
            MessageBoxW(hDlg, L"已恢复默认设置", L"提示", MB_ICONINFORMATION);
            break;
        }
        case IDC_SIMPLE_ENABLE: {
            g_optimizationEnabled = (IsDlgButtonChecked(hDlg, IDC_SIMPLE_ENABLE) == BST_CHECKED);
            WriteIni(L"Settings", L"Enable", g_optimizationEnabled ? L"1" : L"0");
            break;
        }
        case IDC_SIMPLE_HIDE_TRAY: {
            g_hideToTray = (IsDlgButtonChecked(hDlg, IDC_SIMPLE_HIDE_TRAY) == BST_CHECKED);
            WriteIni(L"Settings", L"HideToTray", g_hideToTray ? L"1" : L"0");
            break;
        }
        case IDC_SIMPLE_FOCUS_ENABLE: {
            bool enable = (IsDlgButtonChecked(hDlg, IDC_SIMPLE_FOCUS_ENABLE) == BST_CHECKED);
            SetFocusTracking(enable);
            g_focusTrackingEnabled = enable;
            WriteIni(L"Settings", L"FocusTracking", enable ? L"1" : L"0");
            break;
        }
        case IDC_BTN_HELP: {
            MessageBoxW(hDlg,
                L"感谢您使用本工具！\n\n - - - > 开发者：Heszer < - - - \n 邮箱：h3532886804@163.com \n\n本工具用于对内存、显存、JVM的优化，\n通过手动或自动清理内存工作集、待机列表，适合游戏或大型软件运行前释放资源。\n不会删除您的任何文件，也不会修改注册表或系统配置。\n\n【注意事项】\n• 清理内存时，后台程序（如浏览器、文档编辑器）可能会短暂响应变慢，这是正常现象。\n  建议使用重要软件前先手动保存，避免意外丢失数据。\n• 部分高级功能需要管理员身份才能生效，若非管理员账户，功能会自动降级或提示。\n• 本工具在 Windows 11 上测试通过，理论支持 Windows 7 及更高版本，\n  但由于杀毒软件、驱动或系统补丁差异，个别功能可能无法使用，还请理解。\n• 本工具不会联网上传任何数据，也不会偷偷记录您的信息。\n  所有配置保存在 %LocalAppData%\\MemoryOptimizer\\Optimizer.ini。\n\n【责任说明】\n使用时请谨记保存重要文件，以免出现内存误删的情况，开发者仅提供技术支持，出现任何问题请自行承担。", L"帮助", MB_ICONINFORMATION);
            break;
        }
        case IDC_SIMPLE_ADVANCED: {
            if (!g_hAdvancedDlg) {
                g_hAdvancedDlg = CreateDialogW(g_hInst, MAKEINTRESOURCEW(IDD_ADVANCED_DIALOG), NULL, AdvancedDlgProc);
                if (g_hAdvancedDlg) {
                    ShowWindow(g_hAdvancedDlg, SW_SHOW);
                    ShowWindow(hDlg, SW_HIDE);
                }
            }
            else {
                ShowWindow(g_hAdvancedDlg, SW_SHOW);
                ShowWindow(hDlg, SW_HIDE);
                SetForegroundWindow(g_hAdvancedDlg);
            }
            break;
        }
        }
        break;
    }
    case WM_CLOSE: {
        if (g_hideToTray) {
            ShowWindow(hDlg, SW_HIDE);
            AddTrayIcon(hDlg);
        }
        else {
            ExitProgram();
        }
        break;
    }
    case WM_DESTROY: {
        if (hBrush) DeleteObject(hBrush);
        g_hSimpleDlg = nullptr;
        break;
    }
    case WM_TRAYICON: {
        if (lParam == WM_RBUTTONUP) {
            ShowTrayContextMenu(hDlg);
        }
        else if (lParam == WM_LBUTTONUP) {
            ShowWindow(hDlg, SW_SHOW);
            SetForegroundWindow(hDlg);
            // 如果高级窗口存在，同步控件状态
            if (g_hAdvancedDlg) {
                UpdateSimpleDialogControls();
            }
        }
        break;
    }
    }
    return FALSE;
}

// 托盘菜单命令处理
void HandleTrayCommand(WORD cmd) {
    switch (cmd) {
    case ID_TRAY_OPEN_WINDOW:
        if (g_hSimpleDlg) {
            ShowWindow(g_hSimpleDlg, SW_SHOW);
            SetForegroundWindow(g_hSimpleDlg);
        }
        break;
    case ID_TRAY_DISABLE_OPT:
        g_optimizationEnabled = false;
        WriteIni(L"Settings", L"Enable", L"0");
        UpdateSimpleDialogControls();
        UpdateAdvancedDialogControls();
        break;
    case ID_TRAY_ALL_OPT:
        // 一键三连：如果未开启优化，则先开启
        if (!g_optimizationEnabled) {
            g_optimizationEnabled = true;
            WriteIni(L"Settings", L"Enable", L"1");
            UpdateSimpleDialogControls();
            UpdateAdvancedDialogControls();
        }
        std::thread([]() {
            CleanMemoryThread();
            GameBoostThread();
            }).detach();
        break;
    case ID_TRAY_MEM_OPT:
        if (g_optimizationEnabled) {
            std::thread(CleanMemoryThread).detach();
        }
        break;
    case ID_TRAY_VRAM_OPT:
        if (g_optimizationEnabled) {
            std::thread(GameBoostThread).detach();
        }
        break;
    case ID_TRAY_JVM_OPT:
        if (g_optimizationEnabled) {
            std::thread(OptimizeJavaProcesses).detach();
        }
        break;
    case ID_TRAY_EXIT:
        ExitProgram();
        break;
    }
}

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    g_hInst = hInstance;
    InitCommonControls();

    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);

    g_hBgSimple = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_BACKGROUND_SIMPLE));
    g_hBgAdvanced = LoadBitmap(hInstance, MAKEINTRESOURCE(IDB_BACKGROUND_ADVANCED));
    g_hEmptyBrush = (HBRUSH)GetStockObject(NULL_BRUSH);

    if (!g_hBgSimple) MessageBoxW(NULL, L"加载简约窗口背景图失败！", L"错误", MB_ICONERROR);
    if (!g_hBgAdvanced) MessageBoxW(NULL, L"加载高级窗口背景图失败！", L"错误", MB_ICONERROR);

    std::wstring iniPath = GetConfigFilePath();
    bool isFirstRun = (GetFileAttributesW(iniPath.c_str()) == INVALID_FILE_ATTRIBUTES);

    LoadWhiteList();
    g_optimizationEnabled = (ReadIni(L"Settings", L"Enable", L"1") == L"1");
    g_focusTrackingEnabled = (ReadIni(L"Settings", L"FocusTracking", L"0") == L"1");
    g_maxFocusCount = _wtoi(ReadIni(L"Settings", L"FocusCount", L"1").c_str());
    if (g_maxFocusCount < 1) g_maxFocusCount = 1;
    g_xmx = _wtoi(ReadIni(L"Settings", L"JvmXmx", L"2048").c_str());
    g_hideToTray = (ReadIni(L"Settings", L"HideToTray", L"1") == L"1");

    // 初始化隐藏默认白名单：包括程序自身和系统关键进程
    g_defaultWhiteSet.insert(L"explorer.exe");
    g_defaultWhiteSet.insert(L"ctfmon.exe");
    g_defaultWhiteSet.insert(L"taskhostw.exe");
    g_defaultWhiteSet.insert(L"dwm.exe");
    g_defaultWhiteSet.insert(L"SearchIndexer.exe");
    // 添加程序自身
    WCHAR selfPath[MAX_PATH];
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    wchar_t* selfName = wcsrchr(selfPath, L'\\');
    if (selfName) selfName++;
    else selfName = selfPath;
    std::wstring selfExe = ToLower(selfName);
    g_defaultWhiteSet.insert(selfExe);

    // 启动时强制关闭优化（确保没有自动优化）
    if (g_optimizationEnabled) {
        g_optimizationEnabled = false;
        WriteIni(L"Settings", L"Enable", L"0");
    }

    HWND hDlg = CreateDialogW(hInstance, MAKEINTRESOURCEW(IDD_SIMPLE_DIALOG), NULL, SimpleDlgProc);
    if (!hDlg) return 1;
    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_MAIN_ICON));
    SendMessage(hDlg, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    SendMessage(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    ShowWindow(hDlg, nCmdShow);
    UpdateWindow(hDlg);

    if (isFirstRun) {
        MessageBoxW(hDlg,
            L"感谢您使用本工具！\n\n - - - > 开发者：Heszer < - - - \n 邮箱：h3532886804@163.com \n\n本工具用于对内存、显存、JVM的优化，\n通过手动或自动清理内存工作集、待机列表，适合游戏或大型软件运行前释放资源。\n不会删除您的任何文件，也不会修改注册表或系统配置。\n\n【注意事项】\n• 清理内存时，后台程序（如浏览器、文档编辑器）可能会短暂响应变慢，这是正常现象。\n  建议使用重要软件前先手动保存，避免意外丢失数据。\n• 部分高级功能需要管理员身份才能生效，若非管理员账户，功能会自动降级或提示。\n• 本工具在 Windows 11 上测试通过，理论支持 Windows 7 及更高版本，\n  但由于杀毒软件、驱动或系统补丁差异，个别功能可能无法使用，还请理解。\n• 本工具不会联网上传任何数据，也不会偷偷记录您的信息。\n  所有配置保存在 %LocalAppData%\\MemoryOptimizer\\Optimizer.ini。\n\n【责任说明】\n使用时请谨记保存重要文件，以免出现内存误删的情况，开发者仅提供技术支持，出现任何问题请自行承担。",
            L"欢迎使用", MB_ICONINFORMATION);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.hwnd == g_hSimpleDlg && msg.message == WM_COMMAND && HIWORD(msg.lParam) == 0 && msg.lParam == 0) {
            if (msg.wParam >= 3000 && msg.wParam <= 3007) {
                HandleTrayCommand((WORD)msg.wParam);
                continue;
            }
        }
        if (!IsDialogMessageW(g_hSimpleDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (g_hWinEventHook) UnhookWinEvent(g_hWinEventHook);
    if (g_hBgSimple) DeleteObject(g_hBgSimple);
    if (g_hBgAdvanced) DeleteObject(g_hBgAdvanced);
    RemoveTrayIcon();
    GdiplusShutdown(g_gdiplusToken);
    return (int)msg.wParam;
}