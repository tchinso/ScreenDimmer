#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "resource.h"

#define CFG_NAME "brightness.cfg"
#define MAX_MONITORS 16
#define MAX_RULES 128

#define ID_TOPMOST 1001
#define ID_RAISE 1002
#define ID_AUTO 1003
#define ID_TRAY_SHOW 1004
#define ID_TRAY_EXIT 1005
#define ID_STARTUP 1006

#define ID_SLIDER_BASE 2000
#define ID_BLACKOUT_BASE 3000
#define ID_VALUE_BASE 4000
#define ID_MONITOR_AUTO_BASE 5000

#define TIMER_AUTO 1
#define TIMER_TOPMOST 2
#define TIMER_DISPLAY_REFRESH 3

#define WM_TRAYICON (WM_APP + 1)
#define WM_TOPMOST_RECHECK (WM_APP + 2)

#define SINGLE_INSTANCE_MUTEX_NAME L"Local\\ScreenDimmer.SingleInstance"
#define SHOW_EXISTING_MESSAGE_NAME L"ScreenDimmer.ShowExisting"
#define STARTUP_RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_VALUE_NAME L"ScreenDimmer"

#ifndef WDA_MONITOR
#define WDA_MONITOR 0x00000001
#endif

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

typedef struct {
    int start_minutes;
    int end_minutes;
    int value;
} ScheduleRule;

typedef struct {
    HMONITOR handle;
    RECT rect;
    HWND overlay;
    int brightness;
    BOOL blackout;
    BOOL automatic;
} MonitorDimmer;

typedef struct {
    HMONITOR handle;
    RECT rect;
    int brightness;
    BOOL blackout;
    BOOL automatic;
} MonitorState;

static HINSTANCE g_inst;
static HWND g_main;
static HWND g_mode_label;
static HWND g_cfg_label;
static HWND g_sliders[MAX_MONITORS];
static HWND g_value_labels[MAX_MONITORS];
static HWND g_blackout_checks[MAX_MONITORS];
static HWND g_auto_checks[MAX_MONITORS];

static HFONT g_font;
static HFONT g_title_font;
static HBRUSH g_black_brush;

static MonitorDimmer g_monitors[MAX_MONITORS];
static int g_monitor_count;

static ScheduleRule g_schedule[MAX_RULES];
static int g_schedule_count;
static BOOL g_using_built_in_schedule;
static wchar_t g_cfg_path[MAX_PATH];

/* Keep the standalone EXE useful until a brightness.cfg is placed beside it. */
static const ScheduleRule g_default_schedule[] = {
    { 19 * 60, 21 * 60, 65 },
    { 21 * 60, 23 * 60, 60 },
    { 23 * 60, 23 * 60 + 59, 55 },
    { 23 * 60 + 59, 1 * 60, 50 },
    { 1 * 60, 2 * 60, 45 },
    { 2 * 60, 6 * 60, 40 },
    { 6 * 60, 17 * 60, 90 },
    { 17 * 60, 19 * 60, 70 }
};

static BOOL g_controls_ready;
static BOOL g_updating_controls;
static int g_displayed_auto_count = -1;
static BOOL g_cfg_label_initialized;
static BOOL g_guard_status_shown;
static BOOL g_tray_added;
static BOOL g_topmost_enabled = TRUE;
static BOOL g_topmost_recheck_pending;
static HWINEVENTHOOK g_topmost_reorder_hook;
static HWINEVENTHOOK g_topmost_show_hook;
static HWINEVENTHOOK g_topmost_foreground_hook;
static UINT g_taskbar_created_msg;
static UINT g_show_existing_msg;
static HICON g_app_icon;
static HANDLE g_single_instance_mutex;

typedef BOOL (WINAPI *PFN_SET_LAYERED_WINDOW_ATTRIBUTES)(HWND, COLORREF, BYTE, DWORD);
typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARE)(void);
typedef BOOL (WINAPI *PFN_SET_WINDOW_DISPLAY_AFFINITY)(HWND, DWORD);

static PFN_SET_LAYERED_WINDOW_ATTRIBUTES g_set_layered_window_attributes;
static PFN_SET_PROCESS_DPI_AWARE g_set_process_dpi_aware;
static PFN_SET_WINDOW_DISPLAY_AFFINITY g_set_window_display_affinity;

static void load_optional_apis(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    union {
        FARPROC proc;
        PFN_SET_LAYERED_WINDOW_ATTRIBUTES fn;
    } layered_window_attributes;
    union {
        FARPROC proc;
        PFN_SET_PROCESS_DPI_AWARE fn;
    } process_dpi_aware;
    union {
        FARPROC proc;
        PFN_SET_WINDOW_DISPLAY_AFFINITY fn;
    } window_display_affinity;

    if (!user32) {
        return;
    }

    layered_window_attributes.proc = GetProcAddress(user32, "SetLayeredWindowAttributes");
    process_dpi_aware.proc = GetProcAddress(user32, "SetProcessDPIAware");
    window_display_affinity.proc = GetProcAddress(user32, "SetWindowDisplayAffinity");

    g_set_layered_window_attributes = layered_window_attributes.fn;
    g_set_process_dpi_aware = process_dpi_aware.fn;
    g_set_window_display_affinity = window_display_affinity.fn;
}

static void make_process_dpi_aware(void)
{
    if (g_set_process_dpi_aware) {
        g_set_process_dpi_aware();
    }
}

static void set_overlay_alpha(HWND hwnd, BYTE alpha)
{
    if (g_set_layered_window_attributes) {
        g_set_layered_window_attributes(hwnd, 0, alpha, LWA_ALPHA);
    }
}

static void hide_overlay_from_capture(HWND hwnd)
{
    if (!g_set_window_display_affinity || !hwnd) {
        return;
    }

    if (!g_set_window_display_affinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        g_set_window_display_affinity(hwnd, WDA_MONITOR);
    }
}

static LPCWSTR utf8_wide(const char *text)
{
    /* Two slots cover calls that pass both a message and a title to MessageBoxW. */
    enum { SLOTS = 2, CHARS = 512 };
    static wchar_t buffers[SLOTS][CHARS];
    static int slot;
    wchar_t *out = buffers[slot++ % SLOTS];

    if (!text) {
        out[0] = L'\0';
        return out;
    }

    out[0] = L'\0';
    if (!MultiByteToWideChar(CP_UTF8, 0, text, -1, out, CHARS)) {
        return L"";
    }
    out[CHARS - 1] = L'\0';
    return out;
}

static int clamp_int(int value, int low, int high)
{
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

static int round_to_step(int value, int step)
{
    int rounded = ((value + step / 2) / step) * step;
    return clamp_int(rounded, 5, 100);
}

static void set_control_font(HWND hwnd, HFONT font)
{
    if (hwnd && font) {
        SendMessageW(hwnd, WM_SETFONT, (WPARAM)font, TRUE);
    }
}

static HWND create_control(HWND parent, LPCWSTR cls, const char *text, DWORD style,
                           int id, int x, int y, int w, int h, HFONT font)
{
    HWND hwnd = CreateWindowExW(
        0,
        cls,
        utf8_wide(text),
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        w,
        h,
        parent,
        (HMENU)(INT_PTR)id,
        g_inst,
        NULL);
    set_control_font(hwnd, font);
    return hwnd;
}

static void set_text_utf8(HWND hwnd, const char *text)
{
    SetWindowTextW(hwnd, utf8_wide(text));
}

static void resolve_cfg_path(void)
{
    wchar_t exe_path[MAX_PATH];
    wchar_t *last_slash;

    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    exe_path[MAX_PATH - 1] = L'\0';

    last_slash = wcsrchr(exe_path, L'\\');
    if (last_slash) {
        *(last_slash + 1) = L'\0';
        _snwprintf(g_cfg_path, MAX_PATH, L"%ls%S", exe_path, CFG_NAME);
    } else {
        _snwprintf(g_cfg_path, MAX_PATH, L"%S", CFG_NAME);
    }
    g_cfg_path[MAX_PATH - 1] = L'\0';
}

static wchar_t *current_startup_command(void)
{
    DWORD capacity = MAX_PATH;
    wchar_t *command = NULL;
    DWORD length;

    for (;;) {
        wchar_t *larger = (wchar_t *)realloc(command, (capacity + 3) * sizeof(wchar_t));
        if (!larger) {
            free(command);
            SetLastError(ERROR_OUTOFMEMORY);
            return NULL;
        }
        command = larger;
        length = GetModuleFileNameW(NULL, command + 1, capacity);
        if (length == 0) {
            free(command);
            return NULL;
        }
        if (length < capacity) {
            break;
        }
        if (capacity >= 32768) {
            free(command);
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return NULL;
        }
        capacity *= 2;
        if (capacity > 32768) capacity = 32768;
    }

    command[0] = L'"';
    command[length + 1] = L'"';
    command[length + 2] = L'\0';
    return command;
}

static BOOL is_run_registered(void)
{
    HKEY key;
    DWORD type = 0;
    DWORD bytes = 0;
    wchar_t stack_value[512];
    wchar_t *value = stack_value;
    wchar_t *command;
    BOOL registered = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (RegQueryValueExW(key, STARTUP_VALUE_NAME, NULL, &type, NULL, &bytes) != ERROR_SUCCESS ||
        type != REG_SZ || bytes == 0 || bytes > 32770 * sizeof(wchar_t)) {
        RegCloseKey(key);
        return FALSE;
    }

    if (bytes + sizeof(wchar_t) > sizeof(stack_value)) {
        value = (wchar_t *)malloc(bytes + sizeof(wchar_t));
    }
    command = current_startup_command();
    if (value && command &&
        RegQueryValueExW(key, STARTUP_VALUE_NAME, NULL, &type, (LPBYTE)value, &bytes) == ERROR_SUCCESS &&
        type == REG_SZ) {
        value[bytes / sizeof(wchar_t)] = L'\0';
        registered = _wcsicmp(value, command) == 0;
    }
    if (value != stack_value) free(value);
    free(command);
    RegCloseKey(key);
    return registered;
}

static LSTATUS set_run_registered(BOOL enabled)
{
    HKEY key;
    LSTATUS result;
    wchar_t *command = NULL;

    if (enabled) {
        command = current_startup_command();
        if (!command) {
            return GetLastError() ? (LSTATUS)GetLastError() : ERROR_OUTOFMEMORY;
        }
        result = RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_RUN_KEY, 0, NULL, 0,
                                 KEY_SET_VALUE, NULL, &key, NULL);
        if (result == ERROR_SUCCESS) {
            result = RegSetValueExW(key, STARTUP_VALUE_NAME, 0, REG_SZ,
                                    (const BYTE *)command,
                                    (DWORD)((wcslen(command) + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }
        free(command);
        return result;
    }

    result = RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_RUN_KEY, 0, KEY_SET_VALUE, &key);
    if (result == ERROR_FILE_NOT_FOUND) {
        return ERROR_SUCCESS;
    }
    if (result != ERROR_SUCCESS) {
        return result;
    }
    result = RegDeleteValueW(key, STARTUP_VALUE_NAME);
    RegCloseKey(key);
    return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
}

/* Register both Explorer startup mechanisms. The mutex prevents duplicate instances. */
static LSTATUS startup_link(BOOL write, BOOL enabled)
{
    wchar_t path[MAX_PATH], target[32768], directory[32768];
    wchar_t *command = NULL, *slash;
    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    HRESULT init, hr;
    LSTATUS result = ERROR_SUCCESS;

    hr = SHGetFolderPathW(NULL, CSIDL_STARTUP | CSIDL_FLAG_CREATE, NULL,
                          SHGFP_TYPE_CURRENT, path);
    if (FAILED(hr)) return ERROR_PATH_NOT_FOUND;
    if (wcslen(path) + wcslen(L"\\ScreenDimmer.lnk") >= MAX_PATH)
        return ERROR_FILENAME_EXCED_RANGE;
    wcscat(path, L"\\ScreenDimmer.lnk");
    if (write && !enabled) {
        if (DeleteFileW(path)) return ERROR_SUCCESS;
        result = GetLastError();
        return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
    }
    command = current_startup_command();
    if (!command) return GetLastError();
    command[wcslen(command) - 1] = L'\0';
    wcscpy(directory, command + 1);
    slash = wcsrchr(directory, L'\\');
    if (slash) *slash = L'\0';
    init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
        free(command);
        return ERROR_GEN_FAILURE;
    }
    hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr))
        hr = IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    if (SUCCEEDED(hr) && write) {
        hr = IShellLinkW_SetPath(link, command + 1);
        if (SUCCEEDED(hr)) hr = IShellLinkW_SetWorkingDirectory(link, directory);
        if (SUCCEEDED(hr)) hr = IPersistFile_Save(file, path, TRUE);
    } else if (SUCCEEDED(hr)) {
        hr = IPersistFile_Load(file, path, STGM_READ);
        if (SUCCEEDED(hr)) hr = IShellLinkW_GetPath(link, target, 32768, NULL, SLGP_RAWPATH);
        if (SUCCEEDED(hr) && _wcsicmp(target, command + 1) != 0) hr = E_FAIL;
    }
    if (FAILED(hr)) result = HRESULT_FACILITY(hr) == FACILITY_WIN32
                            ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE;
    if (file) IPersistFile_Release(file);
    if (link) IShellLinkW_Release(link);
    if (SUCCEEDED(init)) CoUninitialize();
    free(command);
    return result;
}

static BOOL startup_allowed(LPCWSTR category, LPCWSTR name)
{
    wchar_t path[160];
    BYTE value[12];
    DWORD bytes = sizeof(value), type;
    LSTATUS result;
    _snwprintf(path, 160, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\%ls", category);
    result = RegGetValueW(HKEY_CURRENT_USER, path, name, RRF_RT_REG_BINARY,
                          &type, value, &bytes);
    /* Explorer records disabled entries with state 3 (or 7). */
    return result == ERROR_FILE_NOT_FOUND ||
           (result == ERROR_SUCCESS && bytes >= 4 && (value[0] == 2 || value[0] == 6));
}

static LSTATUS clear_startup_override(LPCWSTR category, LPCWSTR name)
{
    wchar_t path[160];
    HKEY key;
    LSTATUS result;
    _snwprintf(path, 160, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\%ls", category);
    result = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_SET_VALUE, &key);
    if (result == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;
    if (result != ERROR_SUCCESS) return result;
    result = RegDeleteValueW(key, name);
    RegCloseKey(key);
    return result == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : result;
}

static BOOL is_startup_registered(void)
{
    return is_run_registered() && startup_allowed(L"Run", STARTUP_VALUE_NAME) &&
           startup_link(FALSE, TRUE) == ERROR_SUCCESS &&
           startup_allowed(L"StartupFolder", L"ScreenDimmer.lnk");
}

static LSTATUS set_startup_registered(BOOL enabled)
{
    LSTATUS run_result = set_run_registered(enabled);
    LSTATUS link_result = startup_link(TRUE, enabled);
    LSTATUS approval_result = ERROR_SUCCESS, folder_result = ERROR_SUCCESS;
    if (enabled) {
        /* Only reset a Windows disable override when the user explicitly enables startup. */
        approval_result = clear_startup_override(L"Run", STARTUP_VALUE_NAME);
        folder_result = clear_startup_override(L"StartupFolder", L"ScreenDimmer.lnk");
    }
    if (run_result != ERROR_SUCCESS) return run_result;
    if (link_result != ERROR_SUCCESS) return link_result;
    if (approval_result != ERROR_SUCCESS) return approval_result;
    if (folder_result != ERROR_SUCCESS) return folder_result;
    if (enabled && !is_startup_registered()) return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

static void load_schedule(void)
{
    FILE *fp;
    char line[256];

    g_schedule_count = 0;
    g_using_built_in_schedule = FALSE;
    resolve_cfg_path();

    fp = _wfopen(g_cfg_path, L"r");
    if (!fp) {
        memcpy(g_schedule, g_default_schedule, sizeof(g_default_schedule));
        g_schedule_count = (int)(sizeof(g_default_schedule) / sizeof(g_default_schedule[0]));
        g_using_built_in_schedule = TRUE;
        return;
    }

    while (fgets(line, sizeof(line), fp) && g_schedule_count < MAX_RULES) {
        int h1, m1, h2, m2, value;
        if (sscanf(line, " %d:%d-%d:%d = %d", &h1, &m1, &h2, &m2, &value) == 5) {
            if (h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23 ||
                m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) {
                continue;
            }
            g_schedule[g_schedule_count].start_minutes = h1 * 60 + m1;
            g_schedule[g_schedule_count].end_minutes = h2 * 60 + m2;
            g_schedule[g_schedule_count].value = clamp_int(value, 5, 100);
            g_schedule_count++;
        }
    }

    fclose(fp);
}

static BOOL minutes_in_range(int now, int start, int end)
{
    if (start < end) {
        return now >= start && now <= end;
    }
    return now >= start || now <= end;
}

static int pick_schedule_value(void)
{
    SYSTEMTIME st;
    int now;
    int i;

    GetLocalTime(&st);
    now = st.wHour * 60 + st.wMinute;

    for (i = 0; i < g_schedule_count; i++) {
        if (minutes_in_range(now, g_schedule[i].start_minutes, g_schedule[i].end_minutes)) {
            return g_schedule[i].value;
        }
    }

    return -1;
}

static void update_overlay_alpha(int index)
{
    BYTE alpha;

    if (index < 0 || index >= g_monitor_count || !g_monitors[index].overlay) {
        return;
    }

    alpha = g_monitors[index].blackout
        ? 255
        : (BYTE)(((100 - g_monitors[index].brightness) * 255) / 100);

    set_overlay_alpha(g_monitors[index].overlay, alpha);
}

static void ensure_overlay_topmost(int index)
{
    RECT rc;

    if (index < 0 || index >= g_monitor_count || !g_monitors[index].overlay) {
        return;
    }

    rc = g_monitors[index].rect;
    SetWindowPos(
        g_monitors[index].overlay,
        HWND_TOPMOST,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void set_overlay_topmost(int index, BOOL enabled)
{
    RECT rc;

    if (index < 0 || index >= g_monitor_count || !g_monitors[index].overlay) {
        return;
    }

    rc = g_monitors[index].rect;
    SetWindowPos(
        g_monitors[index].overlay,
        enabled ? HWND_TOPMOST : HWND_NOTOPMOST,
        rc.left,
        rc.top,
        rc.right - rc.left,
        rc.bottom - rc.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void refresh_status(void)
{
    char mode[128];
    char cfg[256];
    int automatic_count = 0;
    int i;

    if (!g_controls_ready) {
        return;
    }

    for (i = 0; i < g_monitor_count; i++) {
        if (g_monitors[i].automatic) {
            automatic_count++;
        }
    }

    if (automatic_count != g_displayed_auto_count || g_guard_status_shown) {
        _snprintf(mode, sizeof(mode), "모드: 자동(cfg) %d개 / 수동 %d개",
                  automatic_count, g_monitor_count - automatic_count);
        mode[sizeof(mode) - 1] = '\0';
        set_text_utf8(g_mode_label, mode);
        g_displayed_auto_count = automatic_count;
        g_guard_status_shown = FALSE;
    }

    if (!g_cfg_label_initialized) {
        _snprintf(cfg, sizeof(cfg), "cfg: %s | 규칙 %d개",
                  g_using_built_in_schedule ? "내장 기본값" : CFG_NAME, g_schedule_count);
        cfg[sizeof(cfg) - 1] = '\0';
        set_text_utf8(g_cfg_label, cfg);
        g_cfg_label_initialized = TRUE;
    }
}

static void set_guard_status(void)
{
    if (!g_guard_status_shown) {
        set_text_utf8(g_mode_label, "모든 모니터를 100% 어둡게 둘 수 없습니다.");
        g_guard_status_shown = TRUE;
    }
}

static void update_value_label(int index)
{
    char text[32];

    if (!g_controls_ready || index < 0 || index >= g_monitor_count) {
        return;
    }

    if (g_monitors[index].blackout) {
        set_text_utf8(g_value_labels[index], "0%");
        return;
    }

    _snprintf(text, sizeof(text), "%d%%", g_monitors[index].brightness);
    text[sizeof(text) - 1] = '\0';
    set_text_utf8(g_value_labels[index], text);
}

static BOOL would_all_blackout(int changed_index, BOOL changed_value)
{
    int i;

    if (g_monitor_count <= 0) {
        return FALSE;
    }

    for (i = 0; i < g_monitor_count; i++) {
        BOOL blackout = (i == changed_index) ? changed_value : g_monitors[i].blackout;
        if (!blackout) {
            return FALSE;
        }
    }

    return TRUE;
}

static void set_blackout(int index, BOOL enabled, BOOL manual)
{
    MonitorDimmer *monitor;
    BOOL mode_changed = FALSE;

    if (index < 0 || index >= g_monitor_count) {
        return;
    }

    monitor = &g_monitors[index];

    if (enabled && would_all_blackout(index, TRUE)) {
        g_updating_controls = TRUE;
        SendMessageW(g_blackout_checks[index], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_sliders[index], TRUE);
        g_updating_controls = FALSE;
        MessageBeep(MB_ICONWARNING);
        set_guard_status();
        return;
    }

    if (monitor->blackout != enabled) {
        monitor->blackout = enabled;
        EnableWindow(g_sliders[index], !enabled);
        update_overlay_alpha(index);
        update_value_label(index);
    }

    if (manual && monitor->automatic) {
        monitor->automatic = FALSE;
        SendMessageW(g_auto_checks[index], BM_SETCHECK, BST_UNCHECKED, 0);
        mode_changed = TRUE;
    }
    if (mode_changed || g_guard_status_shown) {
        refresh_status();
    }
}

static void set_brightness(int index, int value, BOOL manual)
{
    MonitorDimmer *monitor;
    int brightness;

    if (index < 0 || index >= g_monitor_count) {
        return;
    }

    monitor = &g_monitors[index];
    brightness = clamp_int(value, 5, 100);
    if (monitor->brightness != brightness) {
        monitor->brightness = brightness;
        update_overlay_alpha(index);
        update_value_label(index);
    }

    if (manual) {
        if (monitor->automatic) {
            monitor->automatic = FALSE;
            SendMessageW(g_auto_checks[index], BM_SETCHECK, BST_UNCHECKED, 0);
            refresh_status();
        } else if (g_guard_status_shown) {
            refresh_status();
        }
    }
}

static void apply_value_to_automatic(int value)
{
    int i;
    int brightness = clamp_int(value, 5, 100);

    g_updating_controls = TRUE;
    for (i = 0; i < g_monitor_count; i++) {
        if (!g_monitors[i].automatic || g_monitors[i].brightness == brightness) {
            continue;
        }
        g_monitors[i].brightness = brightness;
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, brightness);
        update_overlay_alpha(i);
        update_value_label(i);
    }
    g_updating_controls = FALSE;
}

static void tick_auto(void)
{
    int value;
    int i;

    if (g_schedule_count <= 0) {
        return;
    }

    for (i = 0; i < g_monitor_count; i++) {
        if (g_monitors[i].automatic) {
            break;
        }
    }
    if (i == g_monitor_count) {
        return;
    }

    value = pick_schedule_value();
    if (value >= 0) {
        apply_value_to_automatic(value);
    }
}

static void set_monitor_auto(int index, BOOL enabled)
{
    BOOL was_blackout;
    int old_brightness;

    if (index < 0 || index >= g_monitor_count) {
        return;
    }

    g_monitors[index].automatic = enabled;
    if (enabled) {
        was_blackout = g_monitors[index].blackout;
        old_brightness = g_monitors[index].brightness;
        if (was_blackout) {
            g_monitors[index].blackout = FALSE;
            SendMessageW(g_blackout_checks[index], BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(g_sliders[index], TRUE);
        }
        tick_auto();
        if (was_blackout && old_brightness == g_monitors[index].brightness) {
            update_overlay_alpha(index);
            update_value_label(index);
        }
    }
    refresh_status();
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, g_black_brush);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL CALLBACK enum_monitor_proc(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM data)
{
    MONITORINFO info;
    MonitorDimmer *dimmer;
    HWND overlay;

    (void)hdc;
    (void)rect;
    (void)data;

    if (g_monitor_count >= MAX_MONITORS) {
        return FALSE;
    }

    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    dimmer = &g_monitors[g_monitor_count];
    ZeroMemory(dimmer, sizeof(*dimmer));
    dimmer->handle = monitor;
    dimmer->rect = info.rcMonitor;
    dimmer->brightness = 100;
    dimmer->blackout = FALSE;
    dimmer->automatic = TRUE;

    overlay = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"ScreenDimmerOverlay",
        L"",
        WS_POPUP,
        info.rcMonitor.left,
        info.rcMonitor.top,
        info.rcMonitor.right - info.rcMonitor.left,
        info.rcMonitor.bottom - info.rcMonitor.top,
        NULL,
        NULL,
        g_inst,
        NULL);

    if (!overlay) {
        return TRUE;
    }

    dimmer->overlay = overlay;
    set_overlay_alpha(overlay, 0);
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    hide_overlay_from_capture(overlay);

    g_monitor_count++;
    return TRUE;
}

static void create_overlays(void)
{
    g_monitor_count = 0;
    EnumDisplayMonitors(NULL, NULL, enum_monitor_proc, 0);
}

static BOOL same_rect(const RECT *a, const RECT *b)
{
    return a->left == b->left &&
           a->top == b->top &&
           a->right == b->right &&
           a->bottom == b->bottom;
}

static void snapshot_monitor_states(MonitorState states[MAX_MONITORS], int *count)
{
    int i;

    *count = g_monitor_count;
    for (i = 0; i < g_monitor_count && i < MAX_MONITORS; i++) {
        states[i].handle = g_monitors[i].handle;
        states[i].rect = g_monitors[i].rect;
        states[i].brightness = g_monitors[i].brightness;
        states[i].blackout = g_monitors[i].blackout;
        states[i].automatic = g_monitors[i].automatic;
    }
}

static int find_monitor_state(MonitorState states[MAX_MONITORS], BOOL used[MAX_MONITORS],
                              int count, int preferred_index, HMONITOR handle, const RECT *rect)
{
    int i;

    for (i = 0; i < count; i++) {
        if (!used[i] && states[i].handle == handle) {
            return i;
        }
    }

    for (i = 0; i < count; i++) {
        if (!used[i] && same_rect(&states[i].rect, rect)) {
            return i;
        }
    }

    if (preferred_index >= 0 && preferred_index < count && !used[preferred_index]) {
        return preferred_index;
    }

    for (i = 0; i < count; i++) {
        if (!used[i]) {
            return i;
        }
    }

    return -1;
}

static void restore_monitor_states(MonitorState states[MAX_MONITORS], int count)
{
    BOOL used[MAX_MONITORS];
    BOOL any_visible;
    int i;

    ZeroMemory(used, sizeof(used));
    for (i = 0; i < g_monitor_count; i++) {
        int match = find_monitor_state(states, used, count, i,
                                       g_monitors[i].handle, &g_monitors[i].rect);
        if (match >= 0) {
            used[match] = TRUE;
            g_monitors[i].brightness = states[match].brightness;
            g_monitors[i].blackout = states[match].blackout;
            g_monitors[i].automatic = states[match].automatic;
        }
    }

    any_visible = FALSE;
    for (i = 0; i < g_monitor_count; i++) {
        if (!g_monitors[i].blackout) {
            any_visible = TRUE;
            break;
        }
    }

    if (!any_visible && g_monitor_count > 0) {
        g_monitors[0].blackout = FALSE;
    }

    for (i = 0; i < g_monitor_count; i++) {
        update_overlay_alpha(i);
    }
}

static void create_main_controls(HWND hwnd)
{
    int y;
    int i;

    create_control(hwnd, L"STATIC", "화면 필터 밝기", SS_LEFT,
                   0, 16, 14, 580, 28, g_title_font);
    create_control(hwnd, L"STATIC", "슬라이더는 밝기, 100% 어둡기는 안전을 위해 모든 모니터에 동시에 적용되지 않습니다.",
                   SS_LEFT, 0, 16, 45, 580, 22, g_font);

    g_mode_label = create_control(hwnd, L"STATIC", "", SS_LEFT,
                                  0, 16, 74, 580, 22, g_font);
    g_cfg_label = create_control(hwnd, L"STATIC", "", SS_LEFT,
                                 0, 16, 98, 580, 22, g_font);

    create_control(hwnd, L"STATIC", "", SS_ETCHEDHORZ,
                   0, 16, 128, 580, 2, g_font);

    y = 146;
    for (i = 0; i < g_monitor_count; i++) {
        char name[64];
        _snprintf(name, sizeof(name), "모니터 %d", i + 1);
        name[sizeof(name) - 1] = '\0';

        create_control(hwnd, L"STATIC", name, SS_LEFT,
                       0, 16, y + 4, 78, 24, g_font);

        g_sliders[i] = CreateWindowExW(
            0,
            TRACKBAR_CLASSW,
            L"",
            WS_CHILD | WS_VISIBLE | TBS_HORZ | TBS_AUTOTICKS,
            100,
            y,
            242,
            34,
            hwnd,
            (HMENU)(INT_PTR)(ID_SLIDER_BASE + i),
            g_inst,
            NULL);
        SendMessageW(g_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(5, 100));
        SendMessageW(g_sliders[i], TBM_SETTICFREQ, 5, 0);
        SendMessageW(g_sliders[i], TBM_SETPAGESIZE, 0, 5);
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, g_monitors[i].brightness);

        g_value_labels[i] = create_control(hwnd, L"STATIC", "100%", SS_RIGHT,
                                           ID_VALUE_BASE + i, 348, y + 4, 52, 24, g_font);
        g_auto_checks[i] = create_control(hwnd, L"BUTTON", "자동(cfg)",
                                          BS_AUTOCHECKBOX, ID_MONITOR_AUTO_BASE + i,
                                          410, y + 2, 86, 24, g_font);
        SendMessageW(g_auto_checks[i], BM_SETCHECK,
                     g_monitors[i].automatic ? BST_CHECKED : BST_UNCHECKED, 0);
        g_blackout_checks[i] = create_control(hwnd, L"BUTTON", "100% 어둡기",
                                              BS_AUTOCHECKBOX, ID_BLACKOUT_BASE + i,
                                              504, y + 2, 96, 24, g_font);
        SendMessageW(g_blackout_checks[i], BM_SETCHECK,
                     g_monitors[i].blackout ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(g_sliders[i], !g_monitors[i].blackout);
        y += 40;
    }

    create_control(hwnd, L"BUTTON", "필터 다시 앞으로", BS_PUSHBUTTON,
                   ID_RAISE, 16, y + 12, 282, 34, g_font);
    create_control(hwnd, L"BUTTON", "모든 모니터 자동(cfg)", BS_PUSHBUTTON,
                   ID_AUTO, 314, y + 12, 282, 34, g_font);

    g_controls_ready = TRUE;
    for (i = 0; i < g_monitor_count; i++) {
        update_value_label(i);
    }
    refresh_status();
}

static void bring_overlays_to_front(void)
{
    int i;

    for (i = 0; i < g_monitor_count; i++) {
        if (g_topmost_enabled) {
            ensure_overlay_topmost(i);
        } else {
            set_overlay_topmost(i, FALSE);
        }
        ShowWindow(g_monitors[i].overlay, SW_SHOWNOACTIVATE);
    }

    ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
}

static void set_all_overlays_topmost(BOOL enabled)
{
    int i;
    for (i = 0; i < g_monitor_count; i++) {
        set_overlay_topmost(i, enabled);
    }
}

static BOOL is_own_window(HWND hwnd)
{
    int i;

    if (hwnd == g_main) {
        return TRUE;
    }
    for (i = 0; i < g_monitor_count; i++) {
        if (hwnd == g_monitors[i].overlay) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL has_overlapping_window_above(HWND hwnd, const RECT *bounds)
{
    HWND above;
    RECT window_rect;
    RECT overlap;

    for (above = GetWindow(hwnd, GW_HWNDPREV); above;
         above = GetWindow(above, GW_HWNDPREV)) {
        if (is_own_window(above) || !IsWindowVisible(above) || IsIconic(above)) {
            continue;
        }
        if (GetWindowRect(above, &window_rect) &&
            IntersectRect(&overlap, bounds, &window_rect)) {
            return TRUE;
        }
    }
    return FALSE;
}

static void topmost_watchdog(void)
{
    int i;
    BOOL raised_overlay = FALSE;
    RECT main_rect;

    if (!g_controls_ready || !g_topmost_enabled) {
        return;
    }

    for (i = 0; i < g_monitor_count; i++) {
        HWND overlay = g_monitors[i].overlay;
        if (overlay &&
            (!(GetWindowLongPtrW(overlay, GWL_EXSTYLE) & WS_EX_TOPMOST) ||
             has_overlapping_window_above(overlay, &g_monitors[i].rect))) {
            ensure_overlay_topmost(i);
            raised_overlay = TRUE;
        }
    }

    if (g_main && IsWindowVisible(g_main) &&
        (raised_overlay ||
         !(GetWindowLongPtrW(g_main, GWL_EXSTYLE) & WS_EX_TOPMOST) ||
         (GetWindowRect(g_main, &main_rect) &&
          has_overlapping_window_above(g_main, &main_rect)))) {
        SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

static void CALLBACK topmost_win_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                       LONG object_id, LONG child_id,
                                       DWORD event_thread, DWORD event_time)
{
    (void)hook;
    (void)event_thread;
    (void)event_time;

    if (!g_topmost_enabled || !g_main || !hwnd ||
        object_id != OBJID_WINDOW || child_id != CHILDID_SELF) {
        return;
    }

    /* An ordinary shown/activated window cannot overtake a topmost overlay.
       A z-order change is still checked because its event names the parent. */
    if (event != EVENT_OBJECT_REORDER &&
        !(GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST)) {
        return;
    }
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return;
    }

    /* Out-of-context callbacks arrive on the UI thread. Coalesce busy event bursts. */
    if (!g_topmost_recheck_pending && PostMessageW(g_main, WM_TOPMOST_RECHECK, 0, 0)) {
        g_topmost_recheck_pending = TRUE;
    }
}

static void update_topmost_monitoring(HWND hwnd)
{
    if (g_topmost_enabled) {
        if (!g_topmost_reorder_hook) {
            g_topmost_reorder_hook = SetWinEventHook(
                EVENT_OBJECT_REORDER, EVENT_OBJECT_REORDER, NULL,
                topmost_win_event, 0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        if (!g_topmost_show_hook) {
            g_topmost_show_hook = SetWinEventHook(
                EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL,
                topmost_win_event, 0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        if (!g_topmost_foreground_hook) {
            g_topmost_foreground_hook = SetWinEventHook(
                EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL,
                topmost_win_event, 0, 0,
                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        SetTimer(hwnd, TIMER_TOPMOST, 600, NULL);
        topmost_watchdog();
    } else {
        KillTimer(hwnd, TIMER_TOPMOST);
        if (g_topmost_reorder_hook) {
            UnhookWinEvent(g_topmost_reorder_hook);
            g_topmost_reorder_hook = NULL;
        }
        if (g_topmost_show_hook) {
            UnhookWinEvent(g_topmost_show_hook);
            g_topmost_show_hook = NULL;
        }
        if (g_topmost_foreground_hook) {
            UnhookWinEvent(g_topmost_foreground_hook);
            g_topmost_foreground_hook = NULL;
        }
        g_topmost_recheck_pending = FALSE;
    }
}

static void add_tray_icon(HWND hwnd)
{
    NOTIFYICONDATAW nid;

    if (g_tray_added) {
        return;
    }

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_app_icon ? g_app_icon : (HICON)LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(nid.szTip, utf8_wide("화면 필터 밝기"), sizeof(nid.szTip) / sizeof(nid.szTip[0]));

    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_tray_added = TRUE;
    }
}

static void remove_tray_icon(HWND hwnd)
{
    NOTIFYICONDATAW nid;

    if (!g_tray_added) {
        return;
    }

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = FALSE;
}

static void position_window_on_cursor_monitor(HWND hwnd)
{
    MONITORINFO info;
    HMONITOR monitor;
    POINT pt;
    RECT rect;
    int width;
    int height;
    int work_width;
    int work_height;
    int x;
    int y;

    if (!GetCursorPos(&pt) || !GetWindowRect(hwnd, &rect)) {
        return;
    }

    monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return;
    }

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    work_width = info.rcWork.right - info.rcWork.left;
    work_height = info.rcWork.bottom - info.rcWork.top;

    x = info.rcWork.left + (work_width - width) / 2;
    y = info.rcWork.top + (work_height - height) / 2;

    if (width >= work_width) {
        x = info.rcWork.left;
    } else {
        x = clamp_int(x, info.rcWork.left, info.rcWork.right - width);
    }

    if (height >= work_height) {
        y = info.rcWork.top;
    } else {
        y = clamp_int(y, info.rcWork.top, info.rcWork.bottom - height);
    }

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void show_main_on_cursor_monitor(HWND hwnd)
{
    remove_tray_icon(hwnd);

    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
    }

    position_window_on_cursor_monitor(hwnd);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
}

static void hide_to_tray(HWND hwnd)
{
    add_tray_icon(hwnd);
    ShowWindow(hwnd, SW_HIDE);
}

static void show_from_tray(HWND hwnd)
{
    show_main_on_cursor_monitor(hwnd);
}

static void show_tray_menu(HWND hwnd)
{
    POINT pt;
    HMENU menu;

    GetCursorPos(&pt);
    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SHOW, utf8_wide("열기"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, utf8_wide("종료"));

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
}

static void handle_command(HWND hwnd, int id)
{
    if (id == ID_TOPMOST) {
        BOOL enabled = !g_topmost_enabled;
        g_topmost_enabled = enabled;
        CheckMenuItem(GetMenu(hwnd), ID_TOPMOST,
                      MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
        set_all_overlays_topmost(enabled);
        update_topmost_monitoring(hwnd);
        return;
    }

    if (id == ID_STARTUP) {
        BOOL enabled = !is_startup_registered();
        LSTATUS result = set_startup_registered(enabled);
        if (result == ERROR_SUCCESS) {
            CheckMenuItem(GetMenu(hwnd), ID_STARTUP,
                          MF_BYCOMMAND | (enabled ? MF_CHECKED : MF_UNCHECKED));
        } else {
            wchar_t message[160];
            _snwprintf(message, sizeof(message) / sizeof(message[0]),
                       L"시작 프로그램 설정을 변경하지 못했습니다. (오류 %ld)", (long)result);
            message[(sizeof(message) / sizeof(message[0])) - 1] = L'\0';
            MessageBoxW(hwnd, message, utf8_wide("화면 필터 밝기"), MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (id == ID_RAISE) {
        bring_overlays_to_front();
        return;
    }

    if (id == ID_AUTO) {
        int i;
        for (i = 0; i < g_monitor_count; i++) {
            g_monitors[i].automatic = TRUE;
            SendMessageW(g_auto_checks[i], BM_SETCHECK, BST_CHECKED, 0);
            g_monitors[i].blackout = FALSE;
            SendMessageW(g_blackout_checks[i], BM_SETCHECK, BST_UNCHECKED, 0);
            EnableWindow(g_sliders[i], TRUE);
        }
        tick_auto();
        for (i = 0; i < g_monitor_count; i++) {
            update_overlay_alpha(i);
            update_value_label(i);
        }
        refresh_status();
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        return;
    }

    if (id == ID_TRAY_SHOW) {
        show_from_tray(hwnd);
        return;
    }

    if (id == ID_TRAY_EXIT) {
        DestroyWindow(hwnd);
        return;
    }

    if (id >= ID_MONITOR_AUTO_BASE && id < ID_MONITOR_AUTO_BASE + g_monitor_count) {
        int index = id - ID_MONITOR_AUTO_BASE;
        BOOL enabled;
        if (g_updating_controls) {
            return;
        }
        enabled = SendMessageW(g_auto_checks[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
        set_monitor_auto(index, enabled);
        return;
    }

    if (id >= ID_BLACKOUT_BASE && id < ID_BLACKOUT_BASE + g_monitor_count) {
        int index = id - ID_BLACKOUT_BASE;
        BOOL enabled;
        if (g_updating_controls) {
            return;
        }
        enabled = SendMessageW(g_blackout_checks[index], BM_GETCHECK, 0, 0) == BST_CHECKED;
        set_blackout(index, enabled, TRUE);
        return;
    }
}

static void handle_scroll(HWND control)
{
    int i;

    if (g_updating_controls) {
        return;
    }

    for (i = 0; i < g_monitor_count; i++) {
        if (control == g_sliders[i]) {
            int value = (int)SendMessageW(g_sliders[i], TBM_GETPOS, 0, 0);
            int snapped = round_to_step(value, 5);

            if (snapped != value) {
                g_updating_controls = TRUE;
                SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, snapped);
                g_updating_controls = FALSE;
            }

            set_brightness(i, snapped, TRUE);
            return;
        }
    }
}

static void destroy_child_controls(HWND parent)
{
    HWND child;

    g_controls_ready = FALSE;
    g_updating_controls = TRUE;

    child = GetWindow(parent, GW_CHILD);
    while (child) {
        HWND next = GetWindow(child, GW_HWNDNEXT);
        DestroyWindow(child);
        child = next;
    }

    ZeroMemory(g_sliders, sizeof(g_sliders));
    ZeroMemory(g_value_labels, sizeof(g_value_labels));
    ZeroMemory(g_blackout_checks, sizeof(g_blackout_checks));
    ZeroMemory(g_auto_checks, sizeof(g_auto_checks));
    g_mode_label = NULL;
    g_cfg_label = NULL;
    g_displayed_auto_count = -1;
    g_cfg_label_initialized = FALSE;
    g_guard_status_shown = FALSE;
    g_updating_controls = FALSE;
}

static void resize_main_window(HWND hwnd)
{
    RECT rect;
    DWORD style;
    DWORD ex_style;
    int client_w;
    int client_h;

    style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    ex_style = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    client_w = 612;
    client_h = 220 + g_monitor_count * 40;

    rect.left = 0;
    rect.top = 0;
    rect.right = client_w;
    rect.bottom = client_h;
    AdjustWindowRectEx(&rect, style, TRUE, ex_style);

    SetWindowPos(hwnd, NULL, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void sync_main_controls(void)
{
    int i;

    if (!g_controls_ready) {
        return;
    }
    g_updating_controls = TRUE;
    for (i = 0; i < g_monitor_count; i++) {
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, g_monitors[i].brightness);
        SendMessageW(g_auto_checks[i], BM_SETCHECK,
                     g_monitors[i].automatic ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(g_blackout_checks[i], BM_SETCHECK,
                     g_monitors[i].blackout ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(g_sliders[i], !g_monitors[i].blackout);
        update_value_label(i);
    }
    g_updating_controls = FALSE;
    refresh_status();
}

static void destroy_overlays(void)
{
    int i;
    for (i = 0; i < g_monitor_count; i++) {
        if (g_monitors[i].overlay) {
            DestroyWindow(g_monitors[i].overlay);
            g_monitors[i].overlay = NULL;
        }
    }
}

typedef struct {
    BOOL matched[MAX_MONITORS];
    int count;
} MonitorLayoutCheck;

static BOOL CALLBACK check_monitor_layout(HMONITOR monitor, HDC dc, LPRECT rect, LPARAM data)
{
    MonitorLayoutCheck *check = (MonitorLayoutCheck *)data;
    int i;

    (void)dc;
    if (!rect || check->count >= MAX_MONITORS) {
        return FALSE;
    }
    check->count++;
    for (i = 0; i < g_monitor_count; i++) {
        if (!check->matched[i] && IsWindow(g_monitors[i].overlay) &&
            g_monitors[i].handle == monitor &&
            same_rect(&g_monitors[i].rect, rect)) {
            check->matched[i] = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL monitor_layout_unchanged(void)
{
    MonitorLayoutCheck check;

    ZeroMemory(&check, sizeof(check));
    return EnumDisplayMonitors(NULL, NULL, check_monitor_layout, (LPARAM)&check) &&
           check.count == g_monitor_count;
}

static void refresh_display_layout(HWND hwnd)
{
    MonitorState states[MAX_MONITORS];
    int state_count;

    if (monitor_layout_unchanged()) {
        return;
    }

    snapshot_monitor_states(states, &state_count);

    destroy_overlays();
    create_overlays();
    restore_monitor_states(states, state_count);

    if (state_count == g_monitor_count && g_controls_ready) {
        sync_main_controls();
    } else {
        destroy_child_controls(hwnd);
        resize_main_window(hwnd);
        create_main_controls(hwnd);
    }

    tick_auto();

    set_all_overlays_topmost(g_topmost_enabled);
    if (g_topmost_enabled) {
        topmost_watchdog();
    }
}

static void schedule_display_refresh(HWND hwnd)
{
    SetTimer(hwnd, TIMER_DISPLAY_REFRESH, 500, NULL);
}

static BOOL notify_existing_instance(void)
{
    HWND existing;
    DWORD pid;
    int i;

    for (i = 0; i < 40; i++) {
        existing = FindWindowW(L"ScreenDimmerMain", NULL);
        if (existing) {
            if (!g_show_existing_msg) {
                return FALSE;
            }
            pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            if (pid) {
                AllowSetForegroundWindow(pid);
            }
            PostMessageW(existing, g_show_existing_msg, 0, 0);
            return TRUE;
        }
        Sleep(50);
    }

    return FALSE;
}

static void close_single_instance_mutex(void)
{
    if (g_single_instance_mutex) {
        CloseHandle(g_single_instance_mutex);
        g_single_instance_mutex = NULL;
    }
}

static void release_graphics_resources(void)
{
    UnregisterClassW(L"ScreenDimmerMain", g_inst);
    UnregisterClassW(L"ScreenDimmerOverlay", g_inst);

    if (g_font) {
        DeleteObject(g_font);
        g_font = NULL;
    }
    if (g_title_font) {
        DeleteObject(g_title_font);
        g_title_font = NULL;
    }
    if (g_black_brush) {
        DeleteObject(g_black_brush);
        g_black_brush = NULL;
    }
    if (g_app_icon) {
        DestroyIcon(g_app_icon);
        g_app_icon = NULL;
    }
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (g_show_existing_msg && msg == g_show_existing_msg) {
        show_main_on_cursor_monitor(hwnd);
        return 0;
    }

    if (msg == g_taskbar_created_msg) {
        if (g_tray_added) {
            g_tray_added = FALSE;
            add_tray_icon(hwnd);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        create_main_controls(hwnd);
        SetTimer(hwnd, TIMER_AUTO, 60000, NULL);
        tick_auto();
        set_all_overlays_topmost(g_topmost_enabled);
        update_topmost_monitoring(hwnd);
        return 0;

    case WM_COMMAND:
        handle_command(hwnd, LOWORD(wparam));
        return 0;

    case WM_HSCROLL:
        handle_scroll((HWND)lparam);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED:
        schedule_display_refresh(hwnd);
        return 0;

    case WM_DEVICECHANGE:
        if (wparam == DBT_DEVNODES_CHANGED ||
            wparam == DBT_CONFIGCHANGED ||
            wparam == DBT_DEVICEARRIVAL ||
            wparam == DBT_DEVICEREMOVECOMPLETE) {
            schedule_display_refresh(hwnd);
        }
        return TRUE;

    case WM_SETTINGCHANGE:
        if (wparam == SPI_SETWORKAREA) {
            schedule_display_refresh(hwnd);
        }
        break;

    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            hide_to_tray(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wparam == TIMER_AUTO) {
            tick_auto();
        } else if (wparam == TIMER_TOPMOST) {
            topmost_watchdog();
        } else if (wparam == TIMER_DISPLAY_REFRESH) {
            KillTimer(hwnd, TIMER_DISPLAY_REFRESH);
            refresh_display_layout(hwnd);
        }
        return 0;

    case WM_TOPMOST_RECHECK:
        g_topmost_recheck_pending = FALSE;
        topmost_watchdog();
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lparam) == WM_LBUTTONDBLCLK || LOWORD(lparam) == WM_LBUTTONUP) {
            show_from_tray(hwnd);
        } else if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
            show_tray_menu(hwnd);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_AUTO);
        g_topmost_enabled = FALSE;
        update_topmost_monitoring(hwnd);
        KillTimer(hwnd, TIMER_DISPLAY_REFRESH);
        remove_tray_icon(hwnd);
        destroy_overlays();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static BOOL register_classes(void)
{
    WNDCLASSW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = overlay_proc;
    wc.hInstance = g_inst;
    wc.hCursor = (HCURSOR)LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_black_brush;
    wc.lpszClassName = L"ScreenDimmerOverlay";
    if (!RegisterClassW(&wc)) {
        return FALSE;
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = main_proc;
    wc.hInstance = g_inst;
    wc.hIcon = g_app_icon ? g_app_icon : (HICON)LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = (HCURSOR)LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ScreenDimmerMain";
    return RegisterClassW(&wc) != 0;
}

static HMENU create_main_menu(void)
{
    HMENU menu = CreateMenu();
    HMENU settings = CreatePopupMenu();

    if (!menu || !settings) {
        if (settings) DestroyMenu(settings);
        if (menu) DestroyMenu(menu);
        return NULL;
    }

    if (!AppendMenuW(settings, MF_STRING | (g_topmost_enabled ? MF_CHECKED : 0),
                     ID_TOPMOST, utf8_wide("필터를 항상 최상위로 설정")) ||
        !AppendMenuW(settings, MF_STRING | (is_startup_registered() ? MF_CHECKED : 0),
                     ID_STARTUP, utf8_wide("Windows 시작 시 자동 실행")) ||
        !AppendMenuW(menu, MF_POPUP, (UINT_PTR)settings, utf8_wide("설정"))) {
        DestroyMenu(settings);
        DestroyMenu(menu);
        return NULL;
    }

    return menu;
}

static HWND create_main_window(void)
{
    RECT rect;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD ex_style = WS_EX_TOPMOST;
    HMENU menu = create_main_menu();
    HWND hwnd;
    int client_w = 612;
    int client_h = 220 + g_monitor_count * 40;

    if (!menu) {
        return NULL;
    }

    rect.left = 0;
    rect.top = 0;
    rect.right = client_w;
    rect.bottom = client_h;
    AdjustWindowRectEx(&rect, style, TRUE, ex_style);

    hwnd = CreateWindowExW(
        ex_style,
        L"ScreenDimmerMain",
        utf8_wide("화면 필터 밝기"),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        menu,
        g_inst,
        NULL);
    if (!hwnd) {
        DestroyMenu(menu);
    }
    return hwnd;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show_cmd)
{
    INITCOMMONCONTROLSEX icc;
    HWND hwnd;
    MSG msg;

    (void)prev;
    (void)cmdline;
    (void)show_cmd;

    g_inst = inst;
    g_show_existing_msg = RegisterWindowMessageW(SHOW_EXISTING_MESSAGE_NAME);
    g_single_instance_mutex = CreateMutexW(NULL, TRUE, SINGLE_INSTANCE_MUTEX_NAME);
    if (g_single_instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        notify_existing_instance();
        close_single_instance_mutex();
        return 0;
    } else if (!g_single_instance_mutex && GetLastError() == ERROR_ACCESS_DENIED) {
        notify_existing_instance();
        return 0;
    }

    load_optional_apis();
    make_process_dpi_aware();
    g_app_icon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_SCREENDIMMER),
                                   IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);

    g_black_brush = CreateSolidBrush(RGB(0, 0, 0));
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");
    g_title_font = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Malgun Gothic");

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
    load_schedule();

    if (!register_classes()) {
        MessageBoxW(NULL, utf8_wide("윈도우 클래스를 등록하지 못했습니다."), utf8_wide("화면 필터 밝기"), MB_ICONERROR);
        release_graphics_resources();
        close_single_instance_mutex();
        return 1;
    }

    create_overlays();
    if (g_monitor_count <= 0) {
        MessageBoxW(NULL, utf8_wide("모니터를 찾지 못했습니다."), utf8_wide("화면 필터 밝기"), MB_ICONERROR);
        release_graphics_resources();
        close_single_instance_mutex();
        return 1;
    }

    hwnd = create_main_window();
    if (!hwnd) {
        destroy_overlays();
        MessageBoxW(NULL, utf8_wide("창을 만들지 못했습니다."), utf8_wide("화면 필터 밝기"), MB_ICONERROR);
        release_graphics_resources();
        close_single_instance_mutex();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    release_graphics_resources();
    close_single_instance_mutex();
    return (int)msg.wParam;
}
