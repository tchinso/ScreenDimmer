#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CFG_NAME "brightness.cfg"
#define MAX_MONITORS 16
#define MAX_RULES 128

#define ID_TOPMOST 1001
#define ID_RAISE 1002
#define ID_AUTO 1003
#define ID_TRAY_SHOW 1004
#define ID_TRAY_EXIT 1005

#define ID_SLIDER_BASE 2000
#define ID_BLACKOUT_BASE 3000
#define ID_VALUE_BASE 4000

#define TIMER_AUTO 1
#define TIMER_TOPMOST 2

#define WM_TRAYICON (WM_APP + 1)

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
} MonitorDimmer;

static HINSTANCE g_inst;
static HWND g_main;
static HWND g_mode_label;
static HWND g_cfg_label;
static HWND g_topmost_check;
static HWND g_sliders[MAX_MONITORS];
static HWND g_value_labels[MAX_MONITORS];
static HWND g_blackout_checks[MAX_MONITORS];

static HFONT g_font;
static HFONT g_title_font;
static HBRUSH g_black_brush;

static MonitorDimmer g_monitors[MAX_MONITORS];
static int g_monitor_count;

static ScheduleRule g_schedule[MAX_RULES];
static int g_schedule_count;
static wchar_t g_cfg_path[MAX_PATH];

static BOOL g_manual_override;
static BOOL g_controls_ready;
static BOOL g_updating_controls;
static BOOL g_tray_added;
static UINT g_taskbar_created_msg;

typedef BOOL (WINAPI *PFN_SET_LAYERED_WINDOW_ATTRIBUTES)(HWND, COLORREF, BYTE, DWORD);
typedef BOOL (WINAPI *PFN_SET_PROCESS_DPI_AWARE)(void);

static PFN_SET_LAYERED_WINDOW_ATTRIBUTES g_set_layered_window_attributes;
static PFN_SET_PROCESS_DPI_AWARE g_set_process_dpi_aware;

static void load_optional_apis(void)
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }

    g_set_layered_window_attributes =
        (PFN_SET_LAYERED_WINDOW_ATTRIBUTES)GetProcAddress(user32, "SetLayeredWindowAttributes");
    g_set_process_dpi_aware =
        (PFN_SET_PROCESS_DPI_AWARE)GetProcAddress(user32, "SetProcessDPIAware");
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

static LPCWSTR utf8_wide(const char *text)
{
    enum { SLOTS = 16, CHARS = 512 };
    static wchar_t buffers[SLOTS][CHARS];
    static int slot;
    wchar_t *out = buffers[slot++ % SLOTS];

    if (!text) {
        out[0] = L'\0';
        return out;
    }

    MultiByteToWideChar(CP_UTF8, 0, text, -1, out, CHARS);
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

static void load_schedule(void)
{
    FILE *fp;
    char line[256];

    g_schedule_count = 0;
    resolve_cfg_path();

    fp = _wfopen(g_cfg_path, L"r");
    if (!fp) {
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

    if (!g_controls_ready) {
        return;
    }

    _snprintf(mode, sizeof(mode), "모드: %s", g_manual_override ? "수동(매뉴얼)" : "자동(cfg)");
    mode[sizeof(mode) - 1] = '\0';
    set_text_utf8(g_mode_label, mode);

    _snprintf(cfg, sizeof(cfg), "cfg: %s | 규칙 %d개", CFG_NAME, g_schedule_count);
    cfg[sizeof(cfg) - 1] = '\0';
    set_text_utf8(g_cfg_label, cfg);
}

static void set_guard_status(void)
{
    set_text_utf8(g_mode_label, "모든 모니터를 100% 어둡게 둘 수 없습니다.");
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
    if (index < 0 || index >= g_monitor_count) {
        return;
    }

    if (enabled && would_all_blackout(index, TRUE)) {
        g_updating_controls = TRUE;
        SendMessageW(g_blackout_checks[index], BM_SETCHECK, BST_UNCHECKED, 0);
        EnableWindow(g_sliders[index], TRUE);
        g_updating_controls = FALSE;
        MessageBeep(MB_ICONWARNING);
        set_guard_status();
        return;
    }

    g_monitors[index].blackout = enabled;
    EnableWindow(g_sliders[index], !enabled);
    update_overlay_alpha(index);
    update_value_label(index);

    if (manual) {
        g_manual_override = TRUE;
    }
    refresh_status();
}

static void set_brightness(int index, int value, BOOL manual)
{
    if (index < 0 || index >= g_monitor_count) {
        return;
    }

    g_monitors[index].brightness = clamp_int(value, 5, 100);
    update_overlay_alpha(index);
    update_value_label(index);

    if (manual) {
        g_manual_override = TRUE;
        refresh_status();
    }
}

static void apply_value_to_all(int value)
{
    int i;
    int brightness = clamp_int(value, 5, 100);

    g_updating_controls = TRUE;
    for (i = 0; i < g_monitor_count; i++) {
        g_monitors[i].brightness = brightness;
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, brightness);
        update_overlay_alpha(i);
        update_value_label(i);
    }
    g_updating_controls = FALSE;
}

static void tick_auto(BOOL force)
{
    int value;

    if (g_manual_override && !force) {
        return;
    }
    if (g_schedule_count <= 0) {
        return;
    }

    value = pick_schedule_value();
    if (value >= 0) {
        apply_value_to_all(value);
    }
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

    g_monitor_count++;
    return TRUE;
}

static void create_overlays(void)
{
    g_monitor_count = 0;
    EnumDisplayMonitors(NULL, NULL, enum_monitor_proc, 0);
}

static void create_main_controls(HWND hwnd)
{
    int y;
    int i;

    create_control(hwnd, L"STATIC", "화면 필터 밝기", SS_LEFT,
                   0, 16, 14, 500, 28, g_title_font);
    create_control(hwnd, L"STATIC", "슬라이더는 밝기, 100% 어둡기는 안전을 위해 모든 모니터에 동시에 적용되지 않습니다.",
                   SS_LEFT, 0, 16, 45, 510, 22, g_font);

    g_mode_label = create_control(hwnd, L"STATIC", "", SS_LEFT,
                                  0, 16, 74, 500, 22, g_font);
    g_cfg_label = create_control(hwnd, L"STATIC", "", SS_LEFT,
                                 0, 16, 98, 500, 22, g_font);

    g_topmost_check = create_control(hwnd, L"BUTTON", "필터를 항상 최상위로 설정",
                                     BS_AUTOCHECKBOX, ID_TOPMOST, 16, 126, 260, 24, g_font);
    SendMessageW(g_topmost_check, BM_SETCHECK, BST_CHECKED, 0);

    create_control(hwnd, L"STATIC", "", SS_ETCHEDHORZ,
                   0, 16, 158, 508, 2, g_font);

    y = 176;
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
            270,
            34,
            hwnd,
            (HMENU)(INT_PTR)(ID_SLIDER_BASE + i),
            g_inst,
            NULL);
        SendMessageW(g_sliders[i], TBM_SETRANGE, TRUE, MAKELPARAM(5, 100));
        SendMessageW(g_sliders[i], TBM_SETTICFREQ, 5, 0);
        SendMessageW(g_sliders[i], TBM_SETPAGESIZE, 0, 5);
        SendMessageW(g_sliders[i], TBM_SETPOS, TRUE, 100);

        g_value_labels[i] = create_control(hwnd, L"STATIC", "100%", SS_RIGHT,
                                           ID_VALUE_BASE + i, 380, y + 4, 52, 24, g_font);
        g_blackout_checks[i] = create_control(hwnd, L"BUTTON", "100% 어둡기",
                                              BS_AUTOCHECKBOX, ID_BLACKOUT_BASE + i,
                                              444, y + 2, 96, 24, g_font);
        y += 40;
    }

    create_control(hwnd, L"BUTTON", "필터 다시 앞으로", BS_PUSHBUTTON,
                   ID_RAISE, 16, y + 12, 245, 34, g_font);
    create_control(hwnd, L"BUTTON", "자동(cfg) 모드로", BS_PUSHBUTTON,
                   ID_AUTO, 279, y + 12, 245, 34, g_font);

    g_controls_ready = TRUE;
    refresh_status();
}

static void bring_overlays_to_front(void)
{
    int i;
    BOOL topmost = SendMessageW(g_topmost_check, BM_GETCHECK, 0, 0) == BST_CHECKED;

    for (i = 0; i < g_monitor_count; i++) {
        if (topmost) {
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

static void topmost_watchdog(void)
{
    int i;

    if (!g_controls_ready ||
        SendMessageW(g_topmost_check, BM_GETCHECK, 0, 0) != BST_CHECKED) {
        return;
    }

    for (i = 0; i < g_monitor_count; i++) {
        ensure_overlay_topmost(i);
    }

    if (g_main && IsWindowVisible(g_main)) {
        SetWindowPos(g_main, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
    nid.hIcon = (HICON)LoadIconW(NULL, IDI_APPLICATION);
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

static void hide_to_tray(HWND hwnd)
{
    add_tray_icon(hwnd);
    ShowWindow(hwnd, SW_HIDE);
}

static void show_from_tray(HWND hwnd)
{
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    remove_tray_icon(hwnd);
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
        BOOL enabled = SendMessageW(g_topmost_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        set_all_overlays_topmost(enabled);
        if (enabled) {
            topmost_watchdog();
        }
        return;
    }

    if (id == ID_RAISE) {
        bring_overlays_to_front();
        return;
    }

    if (id == ID_AUTO) {
        g_manual_override = FALSE;
        refresh_status();
        tick_auto(TRUE);
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

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
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
        SetTimer(hwnd, TIMER_TOPMOST, 1200, NULL);
        tick_auto(TRUE);
        set_all_overlays_topmost(TRUE);
        return 0;

    case WM_COMMAND:
        handle_command(hwnd, LOWORD(wparam));
        return 0;

    case WM_HSCROLL:
        handle_scroll((HWND)lparam);
        return 0;

    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED) {
            hide_to_tray(hwnd);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wparam == TIMER_AUTO) {
            tick_auto(FALSE);
        } else if (wparam == TIMER_TOPMOST) {
            topmost_watchdog();
        }
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
        KillTimer(hwnd, TIMER_TOPMOST);
        remove_tray_icon(hwnd);
        destroy_overlays();
        if (g_font) DeleteObject(g_font);
        if (g_title_font) DeleteObject(g_title_font);
        if (g_black_brush) DeleteObject(g_black_brush);
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
    wc.hIcon = (HICON)LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = (HCURSOR)LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ScreenDimmerMain";
    return RegisterClassW(&wc) != 0;
}

static HWND create_main_window(void)
{
    RECT rect;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    DWORD ex_style = WS_EX_TOPMOST;
    int client_w = 548;
    int client_h = 250 + g_monitor_count * 40;

    rect.left = 0;
    rect.top = 0;
    rect.right = client_w;
    rect.bottom = client_h;
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);

    return CreateWindowExW(
        ex_style,
        L"ScreenDimmerMain",
        utf8_wide("화면 필터 밝기"),
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        NULL,
        g_inst,
        NULL);
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
    load_optional_apis();
    make_process_dpi_aware();

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
        return 1;
    }

    create_overlays();
    if (g_monitor_count <= 0) {
        MessageBoxW(NULL, utf8_wide("모니터를 찾지 못했습니다."), utf8_wide("화면 필터 밝기"), MB_ICONERROR);
        return 1;
    }

    hwnd = create_main_window();
    if (!hwnd) {
        destroy_overlays();
        MessageBoxW(NULL, utf8_wide("창을 만들지 못했습니다."), utf8_wide("화면 필터 밝기"), MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
