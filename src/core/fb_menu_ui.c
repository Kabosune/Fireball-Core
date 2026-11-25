/*
 * fb_menu_ui.c — Core-hosted In-Game Overlay Mod Menu (State Machine + Win32 GDI Overlay).
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 */

#include "fb_menu_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_MENU_WND_CLASS L"FireballCoreModMenuOverlay"
#define WM_FB_MENU_SYNC   (WM_APP + 10)
#define WM_FB_MENU_QUIT   (WM_APP + 11)
#define FB_MENU_TIMER_ID  1u

#define FB_MENU_WIDTH     760
#define FB_MENU_HEIGHT    540
#define FB_HEADER_H       46
#define FB_TABBAR_H       36
#define FB_FOOTER_H       54
#define FB_ROW_H          30
#define FB_UI_PAD_X       16

/* Dark slate theme palette (COLORREF = 0x00BBGGRR) */
#define COL_BG            RGB(18, 21, 28)
#define COL_SURFACE       RGB(26, 31, 43)
#define COL_SURFACE_ALT   RGB(22, 26, 36)
#define COL_BORDER        RGB(48, 57, 76)
#define COL_ACCENT        RGB(255, 107, 53)
#define COL_ACCENT_SOFT   RGB(64, 36, 28)
#define COL_SELECT        RGB(40, 51, 74)
#define COL_CHIP          RGB(36, 44, 62)
#define COL_CHIP_ACTIVE   RGB(255, 107, 53)
#define COL_TEXT          RGB(232, 236, 244)
#define COL_MUTED         RGB(150, 162, 184)
#define COL_GREEN         RGB(52, 211, 153)
#define COL_AMBER         RGB(245, 158, 11)

static void fb_menu_set_toast(fb_menu_ui_t* ui, const char* msg) {
    if (!ui) {
        return;
    }
    snprintf(ui->toast, sizeof(ui->toast), "%s", msg ? msg : "");
    ui->toast_sec = msg && msg[0] ? 2.5 : 0.0;
    InterlockedExchange(&ui->dirty, 1);
}

static int fb_env_is_headless(void) {
    wchar_t val[16];
    DWORD n = GetEnvironmentVariableW(L"FIREBALL_HEADLESS", val, (DWORD)(sizeof(val) / sizeof(val[0])));
    if (n > 0 && val[0] != L'\0' && wcscmp(val, L"0") != 0) {
        return 1;
    }
    return 0;
}

void fb_menu_ui_init(
    fb_menu_ui_t*                   ui,
    CRITICAL_SECTION*               lock,
    fb_settings_manager_t*          settings,
    fb_binding_manager_t*           bindings,
    const fb_menu_telemetry_ptrs_t* telemetry)
{
    if (!ui) {
        return;
    }
    memset(ui, 0, sizeof(*ui));
    ui->lock = lock;
    ui->settings = settings;
    ui->bindings = bindings;
    if (telemetry) {
        ui->telemetry = *telemetry;
    }
    ui->width = FB_MENU_WIDTH;
    ui->height = FB_MENU_HEIGHT;
    ui->headless = fb_env_is_headless();
}

void fb_menu_ui_set_host_snapshot(
    fb_menu_ui_t*              ui,
    const fb_host_api_v1_t*    host_api,
    const fb_plugin_manager_t* plugin_mgr)
{
    if (!ui) {
        return;
    }
    if (ui->lock) {
        EnterCriticalSection(ui->lock);
    }
    free(ui->loaded_plugins);
    free(ui->skipped_plugins);
    ui->loaded_plugins = NULL;
    ui->loaded_count = 0u;
    ui->skipped_plugins = NULL;
    ui->skipped_count = 0u;

    if (host_api) {
        snprintf(ui->host_name, sizeof(ui->host_name), "%s", host_api->host_name ? host_api->host_name : "unknown");
        snprintf(ui->host_version, sizeof(ui->host_version), "%s", host_api->host_version ? host_api->host_version : "0.0.0");
        fb_format_caps(host_api->capabilities, ui->host_caps_str, sizeof(ui->host_caps_str));
    }
    if (plugin_mgr && plugin_mgr->plugin_count > 0u) {
        ui->loaded_plugins = (fb_menu_plugin_snapshot_t*)calloc(
            plugin_mgr->plugin_count,
            sizeof(fb_menu_plugin_snapshot_t));
        if (ui->loaded_plugins) {
            ui->loaded_count = plugin_mgr->plugin_count;
            for (size_t i = 0; i < plugin_mgr->plugin_count; ++i) {
                snprintf(ui->loaded_plugins[i].mod_id, sizeof(ui->loaded_plugins[i].mod_id), "%s", plugin_mgr->plugins[i].mod_id);
                snprintf(ui->loaded_plugins[i].plugin_id, sizeof(ui->loaded_plugins[i].plugin_id), "%s", plugin_mgr->plugins[i].plugin_id);
                snprintf(ui->loaded_plugins[i].version, sizeof(ui->loaded_plugins[i].version), "%s", plugin_mgr->plugins[i].version);
                ui->loaded_plugins[i].has_menu_api = plugin_mgr->plugins[i].has_menu_api;
                ui->loaded_plugins[i].has_console_api = plugin_mgr->plugins[i].has_console_api;
            }
        }
    }
    if (plugin_mgr && plugin_mgr->skipped_count > 0u) {
        ui->skipped_plugins = (fb_menu_skipped_snapshot_t*)calloc(
            plugin_mgr->skipped_count,
            sizeof(fb_menu_skipped_snapshot_t));
        if (ui->skipped_plugins) {
            ui->skipped_count = plugin_mgr->skipped_count;
            for (size_t i = 0; i < plugin_mgr->skipped_count; ++i) {
                snprintf(ui->skipped_plugins[i].mod_id, sizeof(ui->skipped_plugins[i].mod_id), "%s", plugin_mgr->skipped[i].mod_id);
                snprintf(ui->skipped_plugins[i].plugin_id, sizeof(ui->skipped_plugins[i].plugin_id), "%s", plugin_mgr->skipped[i].plugin_id);
                snprintf(ui->skipped_plugins[i].version, sizeof(ui->skipped_plugins[i].version), "%s", plugin_mgr->skipped[i].version);
                snprintf(ui->skipped_plugins[i].reason, sizeof(ui->skipped_plugins[i].reason), "%s", plugin_mgr->skipped[i].reason);
            }
        }
    }
    fb_menu_ui_rebuild(ui);
    if (ui->lock) {
        LeaveCriticalSection(ui->lock);
    }
}

static void fb_menu_push_row(
    fb_menu_ui_t*      ui,
    fb_menu_row_kind_t kind,
    size_t             index,
    const char*        text,
    const char*        subtext,
    int                selectable)
{
    if (ui->row_count >= FB_MENU_MAX_ROWS) {
        return;
    }
    fb_menu_row_t* r = &ui->rows[ui->row_count++];
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    r->index = index;
    snprintf(r->text, sizeof(r->text), "%s", text ? text : "");
    snprintf(r->subtext, sizeof(r->subtext), "%s", subtext ? subtext : "");
    r->selectable = selectable;
}

static int fb_menu_tab_exists(const fb_menu_ui_t* ui, const char* mod_id) {
    for (size_t i = 0; i < ui->tab_count; ++i) {
        if (strcmp(ui->tab_mod_ids[i], mod_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void fb_menu_add_tab(fb_menu_ui_t* ui, const char* mod_id, const char* label) {
    if (ui->tab_count >= FB_MENU_MAX_TABS || fb_menu_tab_exists(ui, mod_id)) {
        return;
    }
    snprintf(ui->tab_mod_ids[ui->tab_count], sizeof(ui->tab_mod_ids[0]), "%s", mod_id);
    snprintf(ui->tab_labels[ui->tab_count], sizeof(ui->tab_labels[0]), "%s", (label && label[0]) ? label : mod_id);
    ui->tab_count += 1u;
}

static void fb_menu_ensure_valid_selection(fb_menu_ui_t* ui) {
    if (ui->row_count == 0u) {
        ui->selected_row = 0u;
        return;
    }
    if (ui->selected_row < ui->row_count && ui->rows[ui->selected_row].selectable) {
        return;
    }
    for (size_t i = 0; i < ui->row_count; ++i) {
        if (ui->rows[i].selectable) {
            ui->selected_row = i;
            return;
        }
    }
    ui->selected_row = 0u;
}

void fb_menu_ui_rebuild(fb_menu_ui_t* ui) {
    if (!ui) {
        return;
    }
    /* Rebuild tabs: Tab 0 is always Fireball Core, then each mod that registered settings or actions */
    ui->tab_count = 0u;
    fb_menu_add_tab(ui, "fireball.core", "Fireball Core");

    if (ui->settings) {
        for (size_t i = 0; i < ui->settings->mod_count; ++i) {
            if (strcmp(ui->settings->mods[i].mod_id, "fireball.core") != 0) {
                fb_menu_add_tab(ui, ui->settings->mods[i].mod_id, ui->settings->mods[i].display_name);
            }
        }
    }
    if (ui->bindings) {
        for (size_t i = 0; i < ui->bindings->action_count; ++i) {
            if (strcmp(ui->bindings->actions[i].mod_id, "fireball.core") != 0) {
                fb_menu_add_tab(ui, ui->bindings->actions[i].mod_id, ui->bindings->actions[i].mod_id);
            }
        }
    }

    if (ui->active_tab >= ui->tab_count) {
        ui->active_tab = 0u;
    }

    ui->row_count = 0u;
    const char* cur_mod = ui->tab_mod_ids[ui->active_tab];

    if (ui->active_tab == 0u) {
        /* Fireball Core tab */
        fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Menu & Controls", "", 0);
        if (ui->bindings) {
            for (size_t i = 0; i < ui->bindings->action_count; ++i) {
                if (strcmp(ui->bindings->actions[i].mod_id, "fireball.core") == 0) {
                    fb_menu_push_row(ui, FB_ROW_ACTION, i, ui->bindings->actions[i].label, ui->bindings->actions[i].description, 1);
                }
            }
        }
        fb_menu_push_row(ui, FB_ROW_CORE_SAVE, 0u, "Save User Config Now", "Write fireball_user_config.json atomically to mods folder", 1);
        fb_menu_push_row(ui, FB_ROW_CORE_RESET_ALL, 0u, "Reset All Settings & Bindings", "Restore every mod setting and hotkey/gamepad bind to defaults", 1);

        fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Host & Live Telemetry", "", 0);
        char line[256];
        snprintf(line, sizeof(line), "Host: %s v%s", ui->host_name[0] ? ui->host_name : "unknown", ui->host_version);
        fb_menu_push_row(ui, FB_ROW_INFO, 0u, line, "", 0);
        snprintf(line, sizeof(line), "Caps: %s", ui->host_caps_str[0] ? ui->host_caps_str : "none");
        fb_menu_push_row(ui, FB_ROW_INFO, 0u, line, "", 0);

        long long ds_calls = ui->telemetry.dstorage_calls ? (long long)*ui->telemetry.dstorage_calls : 0ll;
        long long ds_hits = ui->telemetry.dstorage_hits ? (long long)*ui->telemetry.dstorage_hits : 0ll;
        long long fo_hits = ui->telemetry.file_open_hits ? (long long)*ui->telemetry.file_open_hits : 0ll;
        long long frames = ui->telemetry.frames_seen ? (long long)*ui->telemetry.frames_seen : 0ll;
        long long in_ev = ui->telemetry.input_events ? (long long)*ui->telemetry.input_events : 0ll;
        long long in_con = ui->telemetry.input_consumed ? (long long)*ui->telemetry.input_consumed : 0ll;
        snprintf(line, sizeof(line), "DirectStorage: %lld hits / %lld calls   |   Overlays: %lld hits", ds_hits, ds_calls, fo_hits);
        fb_menu_push_row(ui, FB_ROW_INFO, 0u, line, "", 0);
        snprintf(line, sizeof(line), "Frames: %lld   |   Input events: %lld (%lld consumed)", frames, in_ev, in_con);
        fb_menu_push_row(ui, FB_ROW_INFO, 0u, line, "", 0);

        fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Loaded Plugins", "", 0);
        if (ui->loaded_count == 0u) {
            fb_menu_push_row(ui, FB_ROW_INFO, 0u, "(no active plugins loaded)", "", 0);
        } else {
            for (size_t i = 0; i < ui->loaded_count; ++i) {
                snprintf(
                    line,
                    sizeof(line),
                    "%s  (%s v%s)  [%s]",
                    ui->loaded_plugins[i].plugin_id,
                    ui->loaded_plugins[i].mod_id,
                    ui->loaded_plugins[i].version,
                    ui->loaded_plugins[i].has_menu_api ? "Menu API" : "v1-compat");
                fb_menu_push_row(ui, FB_ROW_INFO, 0u, line, "", 0);
            }
        }

        if (ui->skipped_count > 0u) {
            fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Skipped / Standby Plugins", "", 0);
            for (size_t i = 0; i < ui->skipped_count; ++i) {
                snprintf(
                    line,
                    sizeof(line),
                    "%s (%s): %s",
                    ui->skipped_plugins[i].plugin_id,
                    ui->skipped_plugins[i].mod_id,
                    ui->skipped_plugins[i].reason);
                fb_menu_push_row(ui, FB_ROW_INFO, 0u, line, "", 0);
            }
        }
    } else {
        /* Mod tab: Status rows, Settings (grouped by section), then Actions & Bindings */
        int has_status = 0;
        if (ui->settings) {
            for (size_t i = 0; i < ui->settings->setting_count; ++i) {
                const fb_registered_setting_t* s = &ui->settings->settings[i];
                if (strcmp(s->mod_id, cur_mod) == 0 && s->type == (uint32_t)FB_SETTING_STATUS) {
                    if (!has_status) {
                        fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Live Status", "", 0);
                        has_status = 1;
                    }
                    fb_menu_push_row(ui, FB_ROW_SETTING, i, s->label, s->description, 0);
                }
            }

            char last_sec[FB_SET_SECTION_MAX];
            last_sec[0] = '\0';
            int emitted_default_hdr = 0;
            for (size_t i = 0; i < ui->settings->setting_count; ++i) {
                const fb_registered_setting_t* s = &ui->settings->settings[i];
                if (strcmp(s->mod_id, cur_mod) != 0 || s->type == (uint32_t)FB_SETTING_STATUS) {
                    continue;
                }
                if (s->section[0] != '\0') {
                    if (strcmp(s->section, last_sec) != 0) {
                        snprintf(last_sec, sizeof(last_sec), "%s", s->section);
                        fb_menu_push_row(ui, FB_ROW_HEADER, 0u, s->section, "", 0);
                    }
                } else if (!emitted_default_hdr) {
                    emitted_default_hdr = 1;
                    fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Settings", "", 0);
                }
                fb_menu_push_row(ui, FB_ROW_SETTING, i, s->label, s->description, 1);
            }
        }

        if (ui->bindings) {
            int has_actions = 0;
            for (size_t i = 0; i < ui->bindings->action_count; ++i) {
                const fb_registered_action_t* a = &ui->bindings->actions[i];
                if (strcmp(a->mod_id, cur_mod) != 0) {
                    continue;
                }
                if (!has_actions) {
                    fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Actions & Bindings  (Left/Right: Key | Pad | Consume)", "", 0);
                    has_actions = 1;
                }
                fb_menu_push_row(ui, FB_ROW_ACTION, i, a->label, a->description, 1);
            }
        }

        fb_menu_push_row(ui, FB_ROW_HEADER, 0u, "Maintenance", "", 0);
        char rlabel[160];
        snprintf(rlabel, sizeof(rlabel), "Reset '%s' to Defaults", ui->tab_labels[ui->active_tab]);
        fb_menu_push_row(ui, FB_ROW_MOD_RESET, 0u, rlabel, "Restore this mod's settings and key/gamepad bindings to defaults", 1);
    }

    fb_menu_ensure_valid_selection(ui);
    InterlockedExchange(&ui->dirty, 1);
}

/* ---------------------------------------------------------------------------
 * Win32 Overlay Window (focus-safe WS_EX_NOACTIVATE, double-buffered GDI)
 * --------------------------------------------------------------------------- */

typedef struct fb_find_host_wnd {
    DWORD pid;
    HWND  self;
    HWND  best;
    LONG  best_area;
} fb_find_host_wnd_t;

static BOOL CALLBACK fb_enum_host_windows(HWND hwnd, LPARAM lp) {
    fb_find_host_wnd_t* ctx = (fb_find_host_wnd_t*)lp;
    if (!ctx || hwnd == ctx->self || !IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid) {
        return TRUE;
    }
    RECT r;
    if (GetWindowRect(hwnd, &r)) {
        LONG w = r.right - r.left;
        LONG h = r.bottom - r.top;
        LONG area = (w > 200 && h > 200) ? (w * h) : 0;
        if (area > ctx->best_area) {
            ctx->best_area = area;
            ctx->best = hwnd;
        }
    }
    return TRUE;
}

static void fb_menu_position_over_host(fb_menu_ui_t* ui, HWND hwnd) {
    fb_find_host_wnd_t find;
    memset(&find, 0, sizeof(find));
    find.pid = GetCurrentProcessId();
    find.self = hwnd;
    EnumWindows(fb_enum_host_windows, (LPARAM)&find);

    int x = 48;
    int y = 48;
    if (find.best) {
        RECT r;
        if (GetWindowRect(find.best, &r)) {
            int gw = r.right - r.left;
            int gh = r.bottom - r.top;
            x = r.left + (gw > ui->width ? (gw - ui->width) / 2 : 16);
            y = r.top + (gh > ui->height ? (gh - ui->height) / 2 : 36);
        }
    } else {
        int sw = GetSystemMetrics(SM_CXSCREEN);
        int sh = GetSystemMetrics(SM_CYSCREEN);
        x = (sw > ui->width) ? (sw - ui->width) / 2 : 24;
        y = (sh > ui->height) ? (sh - ui->height) / 2 : 24;
    }
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, ui->width, ui->height, SWP_NOACTIVATE);
}

static void fb_draw_rect(HDC dc, int x, int y, int w, int h, COLORREF fill, COLORREF border) {
    RECT r = { x, y, x + w, y + h };
    HBRUSH br = CreateSolidBrush(fill);
    FillRect(dc, &r, br);
    DeleteObject(br);
    if (border != CLR_INVALID) {
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HPEN oldp = (HPEN)SelectObject(dc, pen);
        HBRUSH oldb = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, x, y, x + w, y + h);
        SelectObject(dc, oldb);
        SelectObject(dc, oldp);
        DeleteObject(pen);
    }
}

static void fb_draw_utf8(HDC dc, int x, int y, int w, int h, const char* utf8, COLORREF color, UINT fmt) {
    if (!utf8) {
        return;
    }
    wchar_t wbuf[256];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (n <= 0) {
        return;
    }
    SetTextColor(dc, color);
    RECT r = { x, y, x + w, y + h };
    DrawTextW(dc, wbuf, -1, &r, fmt | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static int fb_menu_tab_width(const fb_menu_ui_t* ui, int total_w) {
    if (!ui || ui->tab_count == 0u) {
        return 148;
    }
    int avail = total_w - FB_UI_PAD_X * 2;
    int tw = avail / (int)ui->tab_count;
    if (tw > 160) {
        tw = 160;
    }
    if (tw < 96) {
        tw = 96;
    }
    return tw;
}

static void fb_menu_paint(fb_menu_ui_t* ui, HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT cr;
    GetClientRect(hwnd, &cr);
    int W = cr.right - cr.left;
    int H = cr.bottom - cr.top;
    if (W <= 0 || H <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HBITMAP old_bmp = (HBITMAP)SelectObject(mem, bmp);

    HFONT f_title = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT f_body  = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT f_bold  = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT f_mono  = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    SetBkMode(mem, TRANSPARENT);

    if (ui->lock) {
        EnterCriticalSection(ui->lock);
    }

    /* Refresh Core tab live telemetry each paint */
    if (ui->active_tab == 0u) {
        fb_menu_ui_rebuild(ui);
    }

    /* Outer frame */
    fb_draw_rect(mem, 0, 0, W, H, COL_BG, COL_BORDER);
    fb_draw_rect(mem, 0, 0, W, 3, COL_ACCENT, CLR_INVALID);

    /* Title bar */
    fb_draw_rect(mem, 1, 3, W - 2, FB_HEADER_H - 3, COL_SURFACE, CLR_INVALID);
    HFONT old_f = (HFONT)SelectObject(mem, f_title);
    fb_draw_utf8(mem, FB_UI_PAD_X, 11, 360, 26, "FIREBALL CORE  //  MOD MENU", COL_TEXT, DT_LEFT | DT_VCENTER);
    SelectObject(mem, f_mono);
    char toggle_hint[128];
    char tk[40] = "F1", tp[40] = "LB+View";
    size_t tidx = 0;
    if (ui->bindings && fb_bindings_find(ui->bindings, "fireball.core", "toggle_menu", &tidx)) {
        fb_format_binding(FB_DEV_KEYBOARD, ui->bindings->actions[tidx].key, tk, sizeof(tk));
        fb_format_binding(FB_DEV_GAMEPAD, ui->bindings->actions[tidx].pad, tp, sizeof(tp));
    }
    snprintf(toggle_hint, sizeof(toggle_hint), "Toggle: %s / %s", tk, tp);
    fb_draw_utf8(mem, W - 310, 12, 294, 24, toggle_hint, COL_MUTED, DT_RIGHT | DT_VCENTER);

    /* Tabs bar */
    int tab_y = FB_HEADER_H;
    fb_draw_rect(mem, 1, tab_y, W - 2, FB_TABBAR_H, COL_SURFACE_ALT, CLR_INVALID);
    int tx = FB_UI_PAD_X;
    int tw = fb_menu_tab_width(ui, W);
    for (size_t t = 0; t < ui->tab_count; ++t) {
        if (tx + tw > W - FB_UI_PAD_X + 6) {
            break;
        }
        int active = (t == ui->active_tab);
        SelectObject(mem, active ? f_bold : f_body);
        fb_draw_rect(mem, tx, tab_y + 4, tw - 6, FB_TABBAR_H - 6, active ? COL_SELECT : COL_SURFACE, active ? COL_ACCENT : COL_BORDER);
        fb_draw_utf8(mem, tx + 6, tab_y + 4, tw - 18, FB_TABBAR_H - 6, ui->tab_labels[t], active ? COL_TEXT : COL_MUTED, DT_CENTER | DT_VCENTER);
        tx += tw;
    }

    /* Scrollable rows area */
    int list_top = FB_HEADER_H + FB_TABBAR_H + 8;
    int list_bottom = H - FB_FOOTER_H - 6;
    int visible_rows = (list_bottom - list_top) / FB_ROW_H;
    if (visible_rows < 1) {
        visible_rows = 1;
    }
    if ((int)ui->selected_row < ui->scroll_offset) {
        ui->scroll_offset = (int)ui->selected_row;
    } else if ((int)ui->selected_row >= ui->scroll_offset + visible_rows) {
        ui->scroll_offset = (int)ui->selected_row - visible_rows + 1;
    }
    if (ui->scroll_offset < 0) {
        ui->scroll_offset = 0;
    }

    for (int vis = 0; vis < visible_rows; ++vis) {
        size_t ridx = (size_t)(ui->scroll_offset + vis);
        if (ridx >= ui->row_count) {
            break;
        }
        const fb_menu_row_t* row = &ui->rows[ridx];
        int ry = list_top + vis * FB_ROW_H;
        int is_sel = (row->selectable && ridx == ui->selected_row);

        if (row->kind == FB_ROW_HEADER) {
            SelectObject(mem, f_bold);
            fb_draw_utf8(mem, FB_UI_PAD_X, ry + 4, W - FB_UI_PAD_X * 2, FB_ROW_H - 4, row->text, COL_ACCENT, DT_LEFT | DT_VCENTER);
            fb_draw_rect(mem, FB_UI_PAD_X, ry + FB_ROW_H - 2, W - FB_UI_PAD_X * 2, 1, COL_BORDER, CLR_INVALID);
            continue;
        }

        if (is_sel) {
            fb_draw_rect(mem, FB_UI_PAD_X - 4, ry + 1, W - (FB_UI_PAD_X - 4) * 2, FB_ROW_H - 2, COL_SELECT, COL_ACCENT);
        } else if (row->kind != FB_ROW_INFO) {
            fb_draw_rect(mem, FB_UI_PAD_X - 4, ry + 1, W - (FB_UI_PAD_X - 4) * 2, FB_ROW_H - 2, COL_SURFACE_ALT, CLR_INVALID);
        }

        if (row->kind == FB_ROW_INFO) {
            SelectObject(mem, f_mono);
            fb_draw_utf8(mem, FB_UI_PAD_X + 6, ry + 2, W - FB_UI_PAD_X * 2 - 12, FB_ROW_H - 4, row->text, COL_MUTED, DT_LEFT | DT_VCENTER);
        } else if (row->kind == FB_ROW_SETTING && ui->settings && row->index < ui->settings->setting_count) {
            const fb_registered_setting_t* s = &ui->settings->settings[row->index];
            SelectObject(mem, is_sel ? f_bold : f_body);
            fb_draw_utf8(mem, FB_UI_PAD_X + 6, ry + 2, 320, FB_ROW_H - 4, s->label, is_sel ? COL_TEXT : COL_TEXT, DT_LEFT | DT_VCENTER);

            char valbuf[160];
            fb_settings_format_value(s, valbuf, sizeof(valbuf));
            SelectObject(mem, f_mono);
            COLORREF vcol = COL_TEXT;
            if (s->type == (uint32_t)FB_SETTING_BOOL) {
                vcol = s->current_val.v.bool_val ? COL_GREEN : COL_MUTED;
            } else if (s->type == (uint32_t)FB_SETTING_BUTTON) {
                vcol = COL_ACCENT;
            } else if (s->type == (uint32_t)FB_SETTING_STATUS) {
                vcol = COL_MUTED;
            }
            char decorated[192];
            if (is_sel && (s->type == (uint32_t)FB_SETTING_INT || s->type == (uint32_t)FB_SETTING_FLOAT ||
                           s->type == (uint32_t)FB_SETTING_ENUM))
            {
                snprintf(decorated, sizeof(decorated), "<  %s  >", valbuf);
            } else {
                snprintf(decorated, sizeof(decorated), "%s", valbuf);
            }
            fb_draw_utf8(mem, 340, ry + 2, W - 340 - FB_UI_PAD_X - 6, FB_ROW_H - 4, decorated, vcol, DT_RIGHT | DT_VCENTER);
        } else if (row->kind == FB_ROW_ACTION && ui->bindings && row->index < ui->bindings->action_count) {
            const fb_registered_action_t* a = &ui->bindings->actions[row->index];
            SelectObject(mem, is_sel ? f_bold : f_body);
            fb_draw_utf8(mem, FB_UI_PAD_X + 6, ry + 2, 230, FB_ROW_H - 4, a->label, COL_TEXT, DT_LEFT | DT_VCENTER);

            char kb[48], pb[48];
            int cap_this = (ui->bindings->capture_active && ui->bindings->capture_action_idx == row->index);
            if (cap_this && ui->bindings->capture_device == FB_DEV_KEYBOARD) {
                snprintf(kb, sizeof(kb), "Press key...");
            } else {
                fb_format_binding(FB_DEV_KEYBOARD, a->key, kb, sizeof(kb));
            }
            if (cap_this && ui->bindings->capture_device == FB_DEV_GAMEPAD) {
                snprintf(pb, sizeof(pb), "Press pad...");
            } else {
                fb_format_binding(FB_DEV_GAMEPAD, a->pad, pb, sizeof(pb));
            }

            size_t k_conf = fb_bindings_find_conflicts(ui->bindings, row->index, FB_DEV_KEYBOARD, a->key, NULL, 0);
            size_t p_conf = fb_bindings_find_conflicts(ui->bindings, row->index, FB_DEV_GAMEPAD, a->pad, NULL, 0);

            int cx0 = 260, cw0 = 150;
            int cx1 = 418, cw1 = 190;
            int cx2 = 616, cw2 = W - cx2 - FB_UI_PAD_X;

            SelectObject(mem, f_mono);
            fb_draw_rect(mem, cx0, ry + 4, cw0, FB_ROW_H - 8,
                         (is_sel && ui->action_subcol == 0) ? COL_ACCENT_SOFT : COL_CHIP,
                         (is_sel && ui->action_subcol == 0) ? COL_CHIP_ACTIVE : (k_conf ? COL_AMBER : COL_BORDER));
            char kchip[64];
            snprintf(kchip, sizeof(kchip), "K: %s%s", kb, k_conf ? " !" : "");
            fb_draw_utf8(mem, cx0 + 4, ry + 4, cw0 - 8, FB_ROW_H - 8, kchip, k_conf ? COL_AMBER : COL_TEXT, DT_CENTER | DT_VCENTER);

            fb_draw_rect(mem, cx1, ry + 4, cw1, FB_ROW_H - 8,
                         (is_sel && ui->action_subcol == 1) ? COL_ACCENT_SOFT : COL_CHIP,
                         (is_sel && ui->action_subcol == 1) ? COL_CHIP_ACTIVE : (p_conf ? COL_AMBER : COL_BORDER));
            char pchip[64];
            snprintf(pchip, sizeof(pchip), "P: %s%s", pb, p_conf ? " !" : "");
            fb_draw_utf8(mem, cx1 + 4, ry + 4, cw1 - 8, FB_ROW_H - 8, pchip, p_conf ? COL_AMBER : COL_TEXT, DT_CENTER | DT_VCENTER);

            fb_draw_rect(mem, cx2, ry + 4, cw2, FB_ROW_H - 8,
                         (is_sel && ui->action_subcol == 2) ? COL_ACCENT_SOFT : COL_CHIP,
                         (is_sel && ui->action_subcol == 2) ? COL_CHIP_ACTIVE : COL_BORDER);
            fb_draw_utf8(mem, cx2 + 4, ry + 4, cw2 - 8, FB_ROW_H - 8, a->consume_input ? "Eat:ON" : "Eat:OFF",
                         a->consume_input ? COL_GREEN : COL_MUTED, DT_CENTER | DT_VCENTER);
        } else {
            /* Action/maintenance button rows */
            SelectObject(mem, is_sel ? f_bold : f_body);
            fb_draw_utf8(mem, FB_UI_PAD_X + 6, ry + 2, W - 160, FB_ROW_H - 4, row->text, COL_TEXT, DT_LEFT | DT_VCENTER);
            SelectObject(mem, f_mono);
            fb_draw_utf8(mem, W - 130, ry + 2, 110, FB_ROW_H - 4, "[Enter / A]", COL_ACCENT, DT_RIGHT | DT_VCENTER);
        }
    }

    /* Footer bar: description, conflict notice, or capture prompt */
    int fy = H - FB_FOOTER_H;
    fb_draw_rect(mem, 1, fy, W - 2, FB_FOOTER_H - 1, COL_SURFACE, CLR_INVALID);
    fb_draw_rect(mem, 1, fy, W - 2, 1, COL_BORDER, CLR_INVALID);

    char foot1[256];
    foot1[0] = '\0';
    COLORREF foot1_col = COL_TEXT;
    if (ui->bindings && ui->bindings->capture_active) {
        snprintf(foot1, sizeof(foot1),
                 "CAPTURE: Press any key or controller button/chord  |  Esc / Hold B = Cancel  |  Backspace / X = Unbind");
        foot1_col = COL_ACCENT;
    } else if (ui->toast_sec > 0.0 && ui->toast[0]) {
        snprintf(foot1, sizeof(foot1), "%s", ui->toast);
        foot1_col = COL_GREEN;
    } else if (ui->selected_row < ui->row_count) {
        const fb_menu_row_t* sel = &ui->rows[ui->selected_row];
        if (sel->kind == FB_ROW_ACTION && ui->bindings && sel->index < ui->bindings->action_count) {
            const fb_registered_action_t* a = &ui->bindings->actions[sel->index];
            size_t conf_idx = 0;
            uint32_t check_dev = (ui->action_subcol == 1) ? FB_DEV_GAMEPAD : FB_DEV_KEYBOARD;
            fb_binding_chord_t ch = (check_dev == FB_DEV_GAMEPAD) ? a->pad : a->key;
            if (fb_bindings_find_conflicts(ui->bindings, sel->index, check_dev, ch, &conf_idx, 1) > 0u) {
                snprintf(foot1, sizeof(foot1), "CONFLICT with '%s' (%s) — %s",
                         ui->bindings->actions[conf_idx].label,
                         ui->bindings->actions[conf_idx].mod_id,
                         sel->subtext);
                foot1_col = COL_AMBER;
            } else {
                snprintf(foot1, sizeof(foot1), "%s", sel->subtext[0] ? sel->subtext : a->action_id);
            }
        } else {
            snprintf(foot1, sizeof(foot1), "%s", sel->subtext);
        }
    }

    SelectObject(mem, f_body);
    fb_draw_utf8(mem, FB_UI_PAD_X, fy + 6, W - FB_UI_PAD_X * 2, 20, foot1, foot1_col, DT_LEFT | DT_VCENTER);
    SelectObject(mem, f_mono);
    fb_draw_utf8(mem, FB_UI_PAD_X, fy + 28, W - FB_UI_PAD_X * 2, 20,
                 "Up/Down: Navigate   Left/Right: Adjust / Column   Enter/A: Activate   Y/Del: Reset   Tab/LB+RB: Tabs   Esc/B: Close",
                 COL_MUTED, DT_LEFT | DT_VCENTER);

    if (ui->lock) {
        LeaveCriticalSection(ui->lock);
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old_f);
    SelectObject(mem, old_bmp);
    DeleteObject(f_title);
    DeleteObject(f_body);
    DeleteObject(f_bold);
    DeleteObject(f_mono);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void fb_menu_activate_selected(fb_menu_ui_t* ui) {
    if (!ui || ui->selected_row >= ui->row_count) {
        return;
    }
    const fb_menu_row_t* row = &ui->rows[ui->selected_row];
    switch (row->kind) {
    case FB_ROW_SETTING:
        if (ui->settings && row->index < ui->settings->setting_count) {
            fb_registered_setting_t* s = &ui->settings->settings[row->index];
            if (s->type == (uint32_t)FB_SETTING_BOOL || s->type == (uint32_t)FB_SETTING_ENUM) {
                if (fb_settings_step(ui->settings, row->index, 1, 1)) {
                    fb_user_config_save(ui->settings, ui->bindings, NULL);
                    fb_menu_set_toast(ui, "Setting updated & saved");
                }
            } else if (s->type == (uint32_t)FB_SETTING_BUTTON && s->on_button) {
                s->on_button(s->setting_id, s->user_ctx);
                fb_menu_set_toast(ui, "Action triggered");
            }
        }
        break;
    case FB_ROW_ACTION:
        if (ui->bindings && row->index < ui->bindings->action_count) {
            if (ui->action_subcol == 2) {
                int next = ui->bindings->actions[row->index].consume_input ? 0 : 1;
                fb_bindings_set_consume(ui->bindings, row->index, next);
                fb_user_config_save(ui->settings, ui->bindings, NULL);
                fb_menu_set_toast(ui, next ? "Input consume: ON" : "Input consume: OFF (pass-through)");
            } else {
                uint32_t dev = (ui->action_subcol == 1) ? FB_DEV_GAMEPAD : FB_DEV_KEYBOARD;
                fb_bindings_start_capture(ui->bindings, row->index, dev);
                InterlockedExchange(&ui->dirty, 1);
            }
        }
        break;
    case FB_ROW_CORE_SAVE:
        if (fb_user_config_save(ui->settings, ui->bindings, NULL) == FB_OK) {
            fb_menu_set_toast(ui, "Saved fireball_user_config.json");
        } else {
            fb_menu_set_toast(ui, "Failed to save fireball_user_config.json");
        }
        break;
    case FB_ROW_CORE_RESET_ALL:
        fb_settings_reset_all(ui->settings, 1);
        fb_bindings_reset_all(ui->bindings);
        fb_user_config_save(ui->settings, ui->bindings, NULL);
        fb_menu_set_toast(ui, "All settings & bindings reset to defaults");
        break;
    case FB_ROW_MOD_RESET:
        if (ui->active_tab < ui->tab_count) {
            const char* mid = ui->tab_mod_ids[ui->active_tab];
            fb_settings_reset_mod(ui->settings, mid, 1);
            fb_bindings_reset_mod(ui->bindings, mid);
            fb_user_config_save(ui->settings, ui->bindings, NULL);
            fb_menu_set_toast(ui, "Mod settings & bindings reset to defaults");
        }
        break;
    default:
        break;
    }
}

static void fb_menu_handle_mouse_click(fb_menu_ui_t* ui, int mx, int my) {
    if (!ui) {
        return;
    }
    if (ui->lock) {
        EnterCriticalSection(ui->lock);
    }
    if (ui->bindings && ui->bindings->capture_active) {
        if (ui->lock) {
            LeaveCriticalSection(ui->lock);
        }
        return;
    }

    /* Tab click */
    if (my >= FB_HEADER_H && my < FB_HEADER_H + FB_TABBAR_H) {
        int tx = FB_UI_PAD_X;
        int tw = fb_menu_tab_width(ui, ui->width);
        for (size_t t = 0; t < ui->tab_count; ++t) {
            if (mx >= tx && mx < tx + tw - 6) {
                ui->active_tab = t;
                ui->selected_row = 0u;
                ui->scroll_offset = 0;
                fb_menu_ui_rebuild(ui);
                break;
            }
            tx += tw;
        }
    } else {
        /* Row click */
        int list_top = FB_HEADER_H + FB_TABBAR_H + 8;
        int list_bottom = ui->height - FB_FOOTER_H - 6;
        if (my >= list_top && my < list_bottom) {
            size_t ridx = (size_t)(ui->scroll_offset + (my - list_top) / FB_ROW_H);
            if (ridx < ui->row_count && ui->rows[ridx].selectable) {
                ui->selected_row = ridx;
                if (ui->rows[ridx].kind == FB_ROW_ACTION) {
                    if (mx >= 616) {
                        ui->action_subcol = 2;
                    } else if (mx >= 418) {
                        ui->action_subcol = 1;
                    } else {
                        ui->action_subcol = 0;
                    }
                }
                fb_menu_activate_selected(ui);
            }
        }
    }
    InterlockedExchange(&ui->dirty, 1);
    if (ui->lock) {
        LeaveCriticalSection(ui->lock);
    }
}

static LRESULT CALLBACK fb_menu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    fb_menu_ui_t* ui = (fb_menu_ui_t*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT:
        if (ui) {
            fb_menu_paint(ui, hwnd);
            InterlockedExchange(&ui->dirty, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        if (ui) {
            int mx = (int)(short)LOWORD(lp);
            int my = (int)(short)HIWORD(lp);
            fb_menu_handle_mouse_click(ui, mx, my);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (ui && ui->is_open) {
            fb_menu_position_over_host(ui, hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_FB_MENU_SYNC:
        if (ui) {
            if (ui->is_open) {
                fb_menu_position_over_host(ui, hwnd);
                ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                ShowWindow(hwnd, SW_HIDE);
            }
        }
        return 0;
    case WM_FB_MENU_QUIT:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, FB_MENU_TIMER_ID);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static DWORD WINAPI fb_menu_ui_thread_main(LPVOID param) {
    fb_menu_ui_t* ui = (fb_menu_ui_t*)param;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = fb_menu_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, MAKEINTRESOURCEW(32512)); /* IDC_ARROW */
    wc.lpszClassName = FB_MENU_WND_CLASS;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        FB_MENU_WND_CLASS,
        L"Fireball Mod Menu",
        WS_POPUP,
        48, 48, ui->width, ui->height,
        NULL, NULL, wc.hInstance, ui);

    ui->hwnd = hwnd;
    InterlockedExchange(&ui->ui_ready, 1);
    if (!hwnd) {
        return 1;
    }

    SetLayeredWindowAttributes(hwnd, 0, 232, LWA_ALPHA);
    SetTimer(hwnd, FB_MENU_TIMER_ID, 150, NULL);
    if (ui->is_open) {
        fb_menu_position_over_host(ui, hwnd);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        InvalidateRect(hwnd, NULL, FALSE);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    UnregisterClassW(FB_MENU_WND_CLASS, wc.hInstance);
    ui->hwnd = NULL;
    return 0;
}

static void fb_menu_sync_window(fb_menu_ui_t* ui) {
    if (!ui || ui->headless) {
        return;
    }
    if (!ui->ui_thread && ui->is_open) {
        ui->ui_thread = CreateThread(NULL, 0, fb_menu_ui_thread_main, ui, 0, &ui->ui_thread_id);
        return;
    }
    if (ui->hwnd) {
        PostMessageW(ui->hwnd, WM_FB_MENU_SYNC, 0, 0);
    }
}

void fb_menu_ui_set_open(fb_menu_ui_t* ui, int open) {
    if (!ui) {
        return;
    }
    ui->is_open = open ? 1 : 0;
    if (!ui->is_open && ui->bindings && ui->bindings->capture_active) {
        fb_bindings_cancel_capture(ui->bindings);
    }
    if (ui->is_open) {
        fb_menu_ui_rebuild(ui);
    }
    InterlockedExchange(&ui->dirty, 1);
    fb_menu_sync_window(ui);
}

void fb_menu_ui_toggle(fb_menu_ui_t* ui) {
    if (!ui) {
        return;
    }
    fb_menu_ui_set_open(ui, ui->is_open ? 0 : 1);
}

int fb_menu_ui_is_open(const fb_menu_ui_t* ui) {
    return ui ? ui->is_open : 0;
}

static void fb_menu_move_selection(fb_menu_ui_t* ui, int dir) {
    if (ui->row_count == 0u) {
        return;
    }
    size_t start = ui->selected_row;
    for (size_t step = 1; step <= ui->row_count; ++step) {
        size_t cand = (dir > 0)
                          ? ((start + step) % ui->row_count)
                          : ((start + ui->row_count - (step % ui->row_count)) % ui->row_count);
        if (ui->rows[cand].selectable) {
            ui->selected_row = cand;
            InterlockedExchange(&ui->dirty, 1);
            return;
        }
    }
}

static void fb_menu_switch_tab(fb_menu_ui_t* ui, int dir) {
    if (ui->tab_count <= 1u) {
        return;
    }
    if (dir > 0) {
        ui->active_tab = (ui->active_tab + 1u) % ui->tab_count;
    } else {
        ui->active_tab = (ui->active_tab + ui->tab_count - 1u) % ui->tab_count;
    }
    ui->selected_row = 0u;
    ui->scroll_offset = 0;
    ui->action_subcol = 0;
    fb_menu_ui_rebuild(ui);
}

static void fb_menu_adjust_horizontal(fb_menu_ui_t* ui, int dir) {
    if (ui->selected_row >= ui->row_count) {
        return;
    }
    const fb_menu_row_t* row = &ui->rows[ui->selected_row];
    if (row->kind == FB_ROW_SETTING && ui->settings && row->index < ui->settings->setting_count) {
        if (fb_settings_step(ui->settings, row->index, dir, 1)) {
            fb_user_config_save(ui->settings, ui->bindings, NULL);
            InterlockedExchange(&ui->dirty, 1);
        }
    } else if (row->kind == FB_ROW_ACTION) {
        ui->action_subcol = (ui->action_subcol + (dir >= 0 ? 1 : 2)) % 3;
        InterlockedExchange(&ui->dirty, 1);
    }
}

static void fb_menu_reset_selected(fb_menu_ui_t* ui) {
    if (ui->selected_row >= ui->row_count) {
        return;
    }
    const fb_menu_row_t* row = &ui->rows[ui->selected_row];
    if (row->kind == FB_ROW_SETTING && ui->settings && row->index < ui->settings->setting_count) {
        fb_settings_reset_one(ui->settings, row->index, 1);
        fb_user_config_save(ui->settings, ui->bindings, NULL);
        fb_menu_set_toast(ui, "Setting reset to default");
    } else if (row->kind == FB_ROW_ACTION && ui->bindings && row->index < ui->bindings->action_count) {
        fb_bindings_reset_action(ui->bindings, row->index);
        fb_user_config_save(ui->settings, ui->bindings, NULL);
        fb_menu_set_toast(ui, "Action bindings reset to defaults");
    }
}

int fb_menu_ui_handle_input(fb_menu_ui_t* ui, const fb_input_event_v1_t* ev) {
    if (!ui || !ev || !ui->is_open) {
        return 0;
    }

    /* 1. Rebind capture mode consumes EVERY input event */
    if (ui->bindings && ui->bindings->capture_active) {
        fb_capture_result_t r = fb_bindings_capture_input(ui->bindings, ev);
        if (r == FB_CAPTURE_BOUND) {
            fb_user_config_save(ui->settings, ui->bindings, NULL);
            fb_menu_set_toast(ui, "Binding updated & saved");
        } else if (r == FB_CAPTURE_UNBOUND) {
            fb_user_config_save(ui->settings, ui->bindings, NULL);
            fb_menu_set_toast(ui, "Binding cleared");
        } else if (r == FB_CAPTURE_CANCELLED) {
            fb_menu_set_toast(ui, "Rebind cancelled");
        }
        if (ui->hwnd) {
            InvalidateRect(ui->hwnd, NULL, FALSE);
        }
        return 1;
    }

    /* 2. Menu toggle hotkey/chord closes the menu cleanly */
    if (ui->bindings && fb_bindings_matches_action_press(ui->bindings, "fireball.core", "toggle_menu", ev)) {
        fb_menu_ui_set_open(ui, 0);
        return 1;
    }

    int is_press_or_rep = (ev->action == (uint32_t)FB_INPUT_PRESS || ev->action == (uint32_t)FB_INPUT_REPEAT);
    int is_press = (ev->action == (uint32_t)FB_INPUT_PRESS);

    if (ev->device_kind == FB_DEV_KEYBOARD) {
        if (ev->code == 0x26u && is_press_or_rep) { /* VK_UP */
            fb_menu_move_selection(ui, -1);
            return 1;
        }
        if (ev->code == 0x28u && is_press_or_rep) { /* VK_DOWN */
            fb_menu_move_selection(ui, 1);
            return 1;
        }
        if (ev->code == 0x25u && is_press_or_rep) { /* VK_LEFT */
            fb_menu_adjust_horizontal(ui, -1);
            return 1;
        }
        if (ev->code == 0x27u && is_press_or_rep) { /* VK_RIGHT */
            fb_menu_adjust_horizontal(ui, 1);
            return 1;
        }
        if ((ev->code == 0x0Du || ev->code == 0x20u) && is_press) { /* VK_RETURN / VK_SPACE */
            fb_menu_activate_selected(ui);
            return 1;
        }
        if (ev->code == 0x09u && is_press) { /* VK_TAB */
            int rev = ((ev->modifiers & FB_MOD_SHIFT) != 0u) ||
                      (ui->bindings && (ui->bindings->held_kbd_mods & FB_MOD_SHIFT) != 0u);
            fb_menu_switch_tab(ui, rev ? -1 : 1);
            return 1;
        }
        if (ev->code == 0x21u && is_press) { /* VK_PRIOR (PgUp) */
            fb_menu_switch_tab(ui, -1);
            return 1;
        }
        if (ev->code == 0x22u && is_press) { /* VK_NEXT (PgDn) */
            fb_menu_switch_tab(ui, 1);
            return 1;
        }
        if (ev->code == 0x2Eu && is_press) { /* VK_DELETE */
            fb_menu_reset_selected(ui);
            return 1;
        }
        if (ev->code == 0x1Bu && is_press) { /* VK_ESCAPE */
            fb_menu_ui_set_open(ui, 0);
            return 1;
        }
        /* Consume navigation keys on release too so they don't leak */
        if (ev->code == 0x25u || ev->code == 0x26u || ev->code == 0x27u || ev->code == 0x28u ||
            ev->code == 0x0Du || ev->code == 0x20u || ev->code == 0x09u || ev->code == 0x1Bu ||
            ev->code == 0x21u || ev->code == 0x22u || ev->code == 0x2Eu)
        {
            return 1;
        }
    } else if (ev->device_kind == FB_DEV_GAMEPAD) {
        if (ev->code == FB_PAD_DPAD_UP && is_press_or_rep) {
            fb_menu_move_selection(ui, -1);
            return 1;
        }
        if (ev->code == FB_PAD_DPAD_DOWN && is_press_or_rep) {
            fb_menu_move_selection(ui, 1);
            return 1;
        }
        if (ev->code == FB_PAD_DPAD_LEFT && is_press_or_rep) {
            fb_menu_adjust_horizontal(ui, -1);
            return 1;
        }
        if (ev->code == FB_PAD_DPAD_RIGHT && is_press_or_rep) {
            fb_menu_adjust_horizontal(ui, 1);
            return 1;
        }
        if (ev->code == FB_PAD_A && is_press) {
            fb_menu_activate_selected(ui);
            return 1;
        }
        if (ev->code == FB_PAD_B && is_press) {
            fb_menu_ui_set_open(ui, 0);
            return 1;
        }
        if (ev->code == FB_PAD_LEFT_SHOULDER && is_press) {
            fb_menu_switch_tab(ui, -1);
            return 1;
        }
        if (ev->code == FB_PAD_RIGHT_SHOULDER && is_press) {
            fb_menu_switch_tab(ui, 1);
            return 1;
        }
        if (ev->code == FB_PAD_Y && is_press) {
            fb_menu_reset_selected(ui);
            return 1;
        }
        if (ev->code == FB_PAD_DPAD_UP || ev->code == FB_PAD_DPAD_DOWN ||
            ev->code == FB_PAD_DPAD_LEFT || ev->code == FB_PAD_DPAD_RIGHT ||
            ev->code == FB_PAD_A || ev->code == FB_PAD_B ||
            ev->code == FB_PAD_Y || ev->code == FB_PAD_LEFT_SHOULDER || ev->code == FB_PAD_RIGHT_SHOULDER)
        {
            return 1;
        }
    }

    return 0;
}

void fb_menu_ui_tick(fb_menu_ui_t* ui, double dt_sec) {
    if (!ui) {
        return;
    }
    if (ui->toast_sec > 0.0 && dt_sec > 0.0 && dt_sec < 1.0) {
        ui->toast_sec -= dt_sec;
        if (ui->toast_sec <= 0.0) {
            ui->toast_sec = 0.0;
            ui->toast[0] = '\0';
            InterlockedExchange(&ui->dirty, 1);
        }
    }
    if (ui->bindings && ui->bindings->capture_active) {
        fb_capture_result_t r = fb_bindings_capture_tick(ui->bindings, dt_sec);
        if (r == FB_CAPTURE_CANCELLED) {
            fb_menu_set_toast(ui, "Rebind cancelled (held B)");
        }
    }
    if (ui->is_open && ui->hwnd && InterlockedCompareExchange(&ui->dirty, 0, 1) == 1) {
        InvalidateRect(ui->hwnd, NULL, FALSE);
    }
}

void fb_menu_ui_shutdown(fb_menu_ui_t* ui) {
    if (!ui) {
        return;
    }
    ui->is_open = 0;
    if (ui->ui_thread) {
        for (int i = 0; i < 100 && !InterlockedCompareExchange(&ui->ui_ready, 0, 0); ++i) {
            Sleep(10);
        }
        if (ui->hwnd) {
            PostMessageW(ui->hwnd, WM_FB_MENU_QUIT, 0, 0);
        }
        WaitForSingleObject(ui->ui_thread, 2000);
        CloseHandle(ui->ui_thread);
        ui->ui_thread = NULL;
        ui->hwnd = NULL;
        InterlockedExchange(&ui->ui_ready, 0);
    }
    free(ui->loaded_plugins);
    free(ui->skipped_plugins);
    ui->loaded_plugins = NULL;
    ui->skipped_plugins = NULL;
    ui->loaded_count = 0u;
    ui->skipped_count = 0u;
}
