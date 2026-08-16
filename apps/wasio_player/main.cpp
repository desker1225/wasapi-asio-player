// wasio-player GUI. Single page, light theme. Everything here goes through
// PlayerController; no playback class is touched directly.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Wide-character build: without these, MAKEINTRESOURCE and friends expand to
// their ANSI forms and will not match the ...W entry points used below.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "asio/asio_session.h"
#include "asio/driver_registry.h"
#include "formats/utf8_file.h"
#include "player/player_controller.h"
#include "version.h"
#include "wasapi/wasapi_session.h"

#include <windows.h>

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// Common Controls v6 for the ListView and trackbar.
#pragma comment(linker, "/manifestdependency:\"type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr int IDI_WASIO_PLAYER = 1;

constexpr int kBackendAsio = 101;
constexpr int kBackendWasapi = 102;
constexpr int kDevice = 103;
constexpr int kRefresh = 104;
constexpr int kControlPanel = 105;
constexpr int kPlaylist = 106;
constexpr int kAddFiles = 107;
constexpr int kRemove = 108;
constexpr int kClear = 109;
constexpr int kMoveUp = 110;
constexpr int kMoveDown = 111;
constexpr int kSaveList = 112;
constexpr int kLoadList = 113;
constexpr int kPrev = 114;
constexpr int kPlayPause = 115;
constexpr int kStop = 116;
constexpr int kNext = 117;
constexpr int kSeek = 118;
constexpr int kRepeat = 119;
constexpr int kShuffle = 120;
constexpr int kDsdNative = 121;
constexpr int kDsdDop = 122;
constexpr int kTimeLabel = 123;
constexpr int kStatusLine = 124;

constexpr UINT_PTR kPollTimer = 1;
constexpr int kSeekRange = 1000;
// A drop-down combo's control height also reserves the expanded list.
constexpr int kComboDropHeight = 200;

struct C5Theme {
    COLORREF background = RGB(244, 248, 252);
    COLORREF white = RGB(255, 255, 255);
    COLORREF panel = RGB(235, 242, 249);
    COLORREF border = RGB(207, 220, 234);
    COLORREF text = RGB(37, 52, 69);
    COLORREF muted = RGB(100, 121, 143);
    COLORREF blue = RGB(22, 105, 207);
    COLORREF blue_dark = RGB(13, 84, 174);
    COLORREF green = RGB(25, 145, 88);
    COLORREF red = RGB(193, 58, 58);
};

const C5Theme kTheme;

struct GuiState {
    HWND window = nullptr;
    HWND backend_wasapi = nullptr;
    HWND backend_asio = nullptr;
    HWND device = nullptr;
    HWND refresh = nullptr;
    HWND control_panel = nullptr;
    HWND playlist = nullptr;
    HWND add_files = nullptr;
    HWND remove_track = nullptr;
    HWND clear_list = nullptr;
    HWND move_up = nullptr;
    HWND move_down = nullptr;
    HWND save_list = nullptr;
    HWND load_list = nullptr;
    HWND previous = nullptr;
    HWND play_pause = nullptr;
    HWND stop = nullptr;
    HWND next = nullptr;
    HWND seek = nullptr;
    HWND repeat = nullptr;
    HWND shuffle = nullptr;
    HWND dsd_native = nullptr;
    HWND dsd_dop = nullptr;
    HWND time_label = nullptr;
    HWND status_line = nullptr;

    wasio::PlayerController controller;
    std::vector<wasio::DriverRecord> drivers;
    std::vector<wasio::WasapiEndpointRecord> endpoints;
    // True while the user holds the seek thumb, so the poll timer does not
    // fight the drag by writing the playing position back into the slider.
    bool seeking = false;
    // The DSD mode the user last chose while on ASIO. WASAPI forces DoP, so
    // without this a round trip through WASAPI would silently drop a
    // deliberate Native DSD choice.
    wasio::DsdOutputMode asio_dsd_mode = wasio::default_dsd_mode(wasio::PlaybackBackend::Asio);
    std::string last_error;

    HFONT font = nullptr;
    HFONT font_bold = nullptr;
    HBRUSH background_brush = nullptr;
    HBRUSH white_brush = nullptr;
    HICON icon = nullptr;
};

// ---- small helpers ----

std::wstring widen(const std::string& utf8) { return wasio::widen_utf8(utf8); }
std::string narrow(const std::wstring& wide) { return wasio::narrow_to_utf8(wide); }

std::wstring format_clock(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const auto total = static_cast<long long>(seconds + 0.5);
    wchar_t text[32];
    std::swprintf(text, std::size(text), L"%02lld:%02lld", total / 60, total % 60);
    return text;
}

void apply_font(HWND control, HFONT font)
{
    if (control && font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

// A control id travels to CreateWindow as an HMENU; widen through INT_PTR so
// the cast does not warn about growing an int into a pointer.
HMENU control_id(int id)
{
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void draw_card(HDC dc, RECT rect, COLORREF fill, COLORREF border, int radius = 8)
{
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void draw_text(HDC dc, const wchar_t* text, RECT rect, COLORREF color, HFONT font,
               UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    HGDIOBJ old_font = font ? SelectObject(dc, font) : nullptr;
    DrawTextW(dc, text, -1, &rect, format);
    if (old_font) SelectObject(dc, old_font);
}

HWND make_button(HWND parent, int id, const wchar_t* caption)
{
    return CreateWindowExW(0, L"BUTTON", caption,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

HWND make_radio(HWND parent, int id, const wchar_t* caption, bool group_start)
{
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON;
    if (group_start) style |= WS_GROUP;
    return CreateWindowExW(0, L"BUTTON", caption, style, 0, 0, 0, 0, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

// Points the radios and the controller at one mode together, so the checked
// button and what actually plays can never disagree.
void select_dsd_mode(GuiState& state, wasio::DsdOutputMode mode)
{
    const bool native = mode == wasio::DsdOutputMode::Native;
    SendMessageW(state.dsd_native, BM_SETCHECK, native ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(state.dsd_dop, BM_SETCHECK, native ? BST_UNCHECKED : BST_CHECKED, 0);
    state.controller.set_dsd_mode(mode);
}

// ---- device list ----

void refresh_devices(GuiState& state)
{
    SendMessageW(state.device, CB_RESETCONTENT, 0, 0);
    state.drivers.clear();
    state.endpoints.clear();
    if (state.controller.backend() == wasio::PlaybackBackend::Asio) {
        state.drivers = wasio::enumerate_registered_drivers();
        for (const auto& driver : state.drivers) {
            const std::string label =
                driver.description.empty() ? driver.registry_name : driver.description;
            const std::wstring text = widen(label) + (driver.is_x64 ? L"" : L"  (not x64)");
            SendMessageW(state.device, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
    } else {
        state.endpoints = wasio::enumerate_wasapi_endpoints();
        for (const auto& endpoint : state.endpoints) {
            const std::wstring text =
                endpoint.wide_name + (endpoint.is_default ? L"  (default)" : L"");
            SendMessageW(state.device, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
    }
    if (SendMessageW(state.device, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(state.device, CB_SETCURSEL, 0, 0);
    }
    EnableWindow(state.control_panel, state.controller.backend() == wasio::PlaybackBackend::Asio);
}

// Pushes the combo selection into the controller. Empty means "first usable",
// which both backends understand.
void apply_device_selection(GuiState& state)
{
    const auto selected = SendMessageW(state.device, CB_GETCURSEL, 0, 0);
    if (selected == CB_ERR) {
        state.controller.set_device({});
        return;
    }
    const auto index = static_cast<std::size_t>(selected);
    if (state.controller.backend() == wasio::PlaybackBackend::Asio) {
        if (index >= state.drivers.size()) return;
        const auto& driver = state.drivers[index];
        state.controller.set_device(driver.description.empty() ? driver.registry_name
                                                               : driver.description);
    } else {
        if (index >= state.endpoints.size()) return;
        // Select by endpoint id so duplicate friendly names cannot pick wrong.
        state.controller.set_device(narrow(state.endpoints[index].id));
    }
}

// ---- playlist view ----

void rebuild_playlist_view(GuiState& state)
{
    const auto selected = ListView_GetNextItem(state.playlist, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(state.playlist);
    const auto& playlist = state.controller.playlist();
    for (std::size_t i = 0; i < playlist.size(); ++i) {
        const auto& track = playlist.at(i);
        const std::wstring number = std::to_wstring(i + 1);
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        std::wstring number_copy = number;
        item.pszText = number_copy.data();
        ListView_InsertItem(state.playlist, &item);

        std::wstring title = widen(track.display_name);
        ListView_SetItemText(state.playlist, static_cast<int>(i), 1, title.data());

        std::wstring format = track.valid ? widen(wasio::track_kind_name(track.kind))
                                          : std::wstring(L"unavailable");
        if (track.valid && track.kind == wasio::TrackKind::PcmFile) {
            format += track.encoding == wasio::PcmEncoding::Int16
                          ? L" 16"
                          : (track.encoding == wasio::PcmEncoding::Int24 ? L" 24" : L" 32");
        }
        ListView_SetItemText(state.playlist, static_cast<int>(i), 2, format.data());

        std::wstring rate =
            track.valid ? std::to_wstring(static_cast<long long>(track.sample_rate)) : L"-";
        ListView_SetItemText(state.playlist, static_cast<int>(i), 3, rate.data());

        std::wstring channels = track.valid ? std::to_wstring(track.channels) : L"-";
        ListView_SetItemText(state.playlist, static_cast<int>(i), 4, channels.data());

        std::wstring length = track.valid || track.duration_seconds > 0.0
                                  ? format_clock(track.duration_seconds)
                                  : std::wstring(L"-");
        ListView_SetItemText(state.playlist, static_cast<int>(i), 5, length.data());
    }
    if (selected >= 0 && selected < static_cast<int>(playlist.size())) {
        ListView_SetItemState(state.playlist, selected, LVIS_SELECTED | LVIS_FOCUSED,
                              LVIS_SELECTED | LVIS_FOCUSED);
    }
}

int selected_track(const GuiState& state)
{
    return ListView_GetNextItem(state.playlist, -1, LVNI_SELECTED);
}

void select_track(GuiState& state, int index)
{
    if (index < 0) return;
    ListView_SetItemState(state.playlist, index, LVIS_SELECTED | LVIS_FOCUSED,
                          LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(state.playlist, index, FALSE);
}

void add_paths(GuiState& state, const std::vector<std::wstring>& paths)
{
    for (const auto& path : paths) {
        state.controller.playlist().add(wasio::probe_track(narrow(path)));
    }
    rebuild_playlist_view(state);
}

std::vector<std::wstring> ask_for_files(HWND owner, bool playlists)
{
    // Multi-select needs a big buffer: directory then one name per file.
    std::vector<wchar_t> buffer(64 * 1024, L'\0');
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrFilter = playlists ? L"Playlists\0*.m3u8;*.m3u\0All files\0*.*\0"
                                   : L"Audio files\0*.wav;*.dff;*.dsf\0WAV PCM\0*.wav\0"
                                     L"DSDIFF\0*.dff\0DSF\0*.dsf\0All files\0*.*\0";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!playlists) dialog.Flags |= OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&dialog)) return {};

    std::vector<std::wstring> paths;
    const std::wstring first = buffer.data();
    const wchar_t* cursor = buffer.data() + first.size() + 1;
    if (*cursor == L'\0') {
        paths.push_back(first); // single selection: the buffer holds a full path
    } else {
        while (*cursor != L'\0') {
            const std::wstring name = cursor;
            paths.push_back(first + L"\\" + name);
            cursor += name.size() + 1;
        }
    }
    return paths;
}

// ---- transport ----

void update_play_button(GuiState& state, const wasio::PlayerStatus& status)
{
    SetWindowTextW(state.play_pause,
                   status.state == wasio::PlayerState::Playing ? L"Pause" : L"Play");
}

void start_selected(GuiState& state)
{
    auto& playlist = state.controller.playlist();
    if (playlist.empty()) return;
    apply_device_selection(state);
    int index = selected_track(state);
    if (index < 0) {
        const std::size_t entry = playlist.current_index() == wasio::Playlist::kInvalidIndex
                                      ? playlist.first_index()
                                      : playlist.current_index();
        if (entry == wasio::Playlist::kInvalidIndex) return;
        index = static_cast<int>(entry);
    }
    state.controller.play(static_cast<std::size_t>(index));
    select_track(state, index);
}

void update_status(GuiState& state, const wasio::PlayerStatus& status)
{
    std::wstring text;
    if (!status.format_description.empty()) text += widen(status.format_description);
    if (!text.empty()) text += L"  |  ";
    text += L"callbacks " + std::to_wstring(status.callback_count);
    text += L"  |  underruns " + std::to_wstring(status.underrun_count);
    if (!status.error.empty()) text += L"\r\n" + widen(status.error);
    SetWindowTextW(state.status_line, text.c_str());

    const std::wstring clock =
        format_clock(status.position_seconds) + L" / " + format_clock(status.duration_seconds);
    SetWindowTextW(state.time_label, clock.c_str());

    if (!state.seeking) {
        const int position =
            status.duration_seconds > 0.0
                ? static_cast<int>(status.position_seconds / status.duration_seconds * kSeekRange)
                : 0;
        SendMessageW(state.seek, TBM_SETPOS, TRUE, std::clamp(position, 0, kSeekRange));
    }
    if (status.error != state.last_error) {
        state.last_error = status.error;
        InvalidateRect(state.window, nullptr, FALSE);
    }
}

// ---- layout and painting ----

void layout_controls(GuiState& state)
{
    RECT client = {};
    GetClientRect(state.window, &client);
    const int width = std::max(880L, static_cast<long>(client.right));
    const int height = std::max(560L, static_cast<long>(client.bottom));
    const int margin = 22;
    const int inner = width - margin * 2;
    const int gap = 12;

    // ① OUTPUT DEVICE
    const int device_row = 92;
    MoveWindow(state.backend_wasapi, margin, device_row, 92, 30, TRUE);
    MoveWindow(state.backend_asio, margin + 98, device_row, 72, 30, TRUE);
    const int device_x = margin + 184;
    const int device_width = std::max(220, inner - 184 - 290);
    MoveWindow(state.device, device_x, device_row, device_width, kComboDropHeight, TRUE);
    MoveWindow(state.refresh, device_x + device_width + gap, device_row, 84, 30, TRUE);
    MoveWindow(state.control_panel, device_x + device_width + gap + 96, device_row, 170, 30, TRUE);

    // ④ STATUS sits at the bottom; everything above is measured back from it.
    const int status_height = 54;
    const int status_y = height - margin - status_height;
    MoveWindow(state.status_line, margin + 16, status_y + 26, inner - 32, status_height - 34, TRUE);

    // ③ TRANSPORT
    const int transport_height = 96;
    const int transport_y = status_y - 14 - transport_height;
    const int button_y = transport_y + 14;
    MoveWindow(state.previous, margin + 16, button_y, 74, 32, TRUE);
    MoveWindow(state.play_pause, margin + 96, button_y, 92, 32, TRUE);
    MoveWindow(state.stop, margin + 194, button_y, 74, 32, TRUE);
    MoveWindow(state.next, margin + 274, button_y, 74, 32, TRUE);
    const int seek_x = margin + 360;
    const int seek_width = std::max(160, inner - 360 - 130);
    MoveWindow(state.seek, seek_x, button_y + 2, seek_width, 28, TRUE);
    MoveWindow(state.time_label, seek_x + seek_width + gap, button_y + 6, 110, 22, TRUE);

    const int options_y = transport_y + 56;
    MoveWindow(state.repeat, margin + 76, options_y, 96, kComboDropHeight, TRUE);
    MoveWindow(state.shuffle, margin + 194, options_y, 96, 28, TRUE);
    MoveWindow(state.dsd_native, margin + 396, options_y, 116, 28, TRUE);
    MoveWindow(state.dsd_dop, margin + 518, options_y, 70, 28, TRUE);

    // ② PLAYLIST takes whatever vertical space is left. The button row stops
    // 26px above the transport card so the ③ TRANSPORT caption has its own band.
    const int list_y = 156;
    const int buttons_y = transport_y - 26 - 30;
    const int list_height = std::max(90, buttons_y - 10 - list_y);
    MoveWindow(state.playlist, margin, list_y, inner, list_height, TRUE);

    // Not named `small`: rpcndr.h, pulled in by windows.h, defines that as char.
    const int narrow_button = 76;
    int x = margin;
    MoveWindow(state.add_files, x, buttons_y, 96, 30, TRUE); x += 108;
    MoveWindow(state.remove_track, x, buttons_y, narrow_button, 30, TRUE); x += narrow_button + gap;
    MoveWindow(state.clear_list, x, buttons_y, narrow_button, 30, TRUE); x += narrow_button + gap;
    MoveWindow(state.move_up, x, buttons_y, 56, 30, TRUE); x += 56 + gap;
    MoveWindow(state.move_down, x, buttons_y, 66, 30, TRUE); x += 66 + gap;
    MoveWindow(state.save_list, x, buttons_y, 88, 30, TRUE); x += 88 + gap;
    MoveWindow(state.load_list, x, buttons_y, 88, 30, TRUE);

    // Give the title column whatever the fixed columns do not use.
    const int fixed = 46 + 110 + 90 + 56 + 70;
    ListView_SetColumnWidth(state.playlist, 1, std::max(140, inner - fixed - 24));
}

void paint_window(HWND window, GuiState& state, HDC dc)
{
    RECT client = {};
    GetClientRect(window, &client);
    FillRect(dc, &client, state.background_brush);

    RECT header = {0, 0, client.right, 56};
    FillRect(dc, &header, state.white_brush);
    HPEN line_pen = CreatePen(PS_SOLID, 1, kTheme.border);
    HGDIOBJ old_pen = SelectObject(dc, line_pen);
    MoveToEx(dc, 0, 55, nullptr);
    LineTo(dc, client.right, 55);
    SelectObject(dc, old_pen);
    DeleteObject(line_pen);

    if (state.icon) DrawIconEx(dc, 20, 13, state.icon, 24, 24, 0, nullptr, DI_NORMAL);
    RECT title_rect = {54, 9, 230, 34};
    draw_text(dc, WASIO_APP_TITLE, title_rect, kTheme.text, state.font_bold);
    RECT subtitle_rect = {196, 11, 520, 34};
    draw_text(dc, L"x64 - bit-perfect WAV / DFF / DSF", subtitle_rect, kTheme.muted, state.font);

    const bool failed = !state.last_error.empty();
    HBRUSH dot = CreateSolidBrush(failed ? kTheme.red : kTheme.green);
    HGDIOBJ old_brush = SelectObject(dc, dot);
    Ellipse(dc, client.right - 156, 22, client.right - 146, 32);
    SelectObject(dc, old_brush);
    DeleteObject(dot);
    RECT ready_rect = {client.right - 140, 9, client.right - 18, 43};
    draw_text(dc, failed ? L"error" : L"ready", ready_rect, failed ? kTheme.red : kTheme.green,
              state.font);

    const int margin = 22;
    const int width = std::max(880L, static_cast<long>(client.right));
    const int height = std::max(560L, static_cast<long>(client.bottom));
    const int inner = width - margin * 2;

    RECT label = {margin, 70, margin + inner, 88};
    draw_text(dc, L"\x2460 OUTPUT DEVICE", label, kTheme.muted, state.font_bold);

    label.top = 134;
    label.bottom = 152;
    draw_text(dc, L"\x2461 PLAYLIST", label, kTheme.muted, state.font_bold);

    const int status_height = 54;
    const int status_y = height - margin - status_height;
    const int transport_height = 96;
    const int transport_y = status_y - 14 - transport_height;

    RECT transport_card = {margin, transport_y, margin + inner, transport_y + transport_height};
    draw_card(dc, transport_card, kTheme.white, kTheme.border);
    RECT transport_title = {margin + 16, transport_y - 22, margin + inner, transport_y - 4};
    draw_text(dc, L"\x2462 TRANSPORT", transport_title, kTheme.muted, state.font_bold);

    const int options_y = transport_y + 56;
    RECT repeat_label = {margin + 16, options_y + 4, margin + 76, options_y + 26};
    draw_text(dc, L"Repeat", repeat_label, kTheme.muted, state.font);
    RECT dsd_label = {margin + 300, options_y + 4, margin + 396, options_y + 26};
    draw_text(dc, L"DSD output", dsd_label, kTheme.muted, state.font);

    RECT status_card = {margin, status_y, margin + inner, status_y + status_height};
    draw_card(dc, status_card, kTheme.panel, kTheme.border);
    RECT status_title = {margin + 16, status_y + 6, margin + inner - 16, status_y + 24};
    draw_text(dc, L"\x2463 STATUS", status_title, kTheme.muted, state.font_bold);
}

void draw_button(const DRAWITEMSTRUCT& item, const GuiState& state)
{
    const bool primary = item.CtlID == kPlayPause || item.CtlID == kAddFiles;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF fill = primary ? (pressed ? kTheme.blue_dark : kTheme.blue) : kTheme.white;
    COLORREF border = primary ? fill : kTheme.border;
    COLORREF text = primary ? kTheme.white : kTheme.text;
    if (disabled) {
        fill = kTheme.panel;
        border = kTheme.border;
        text = kTheme.muted;
    }
    RECT rect = item.rcItem;
    InflateRect(&rect, -1, -1);
    draw_card(item.hDC, rect, fill, border, 6);
    wchar_t caption[128] = {};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
    draw_text(item.hDC, caption, rect, text, state.font_bold,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ---- commands ----

void on_command(GuiState& state, int id, int code)
{
    auto& playlist = state.controller.playlist();
    switch (id) {
    case kBackendAsio:
    case kBackendWasapi: {
        const auto backend = (id == kBackendAsio ? wasio::PlaybackBackend::Asio
                                                 : wasio::PlaybackBackend::Wasapi);
        state.controller.set_backend(backend);
        const bool wasapi = backend == wasio::PlaybackBackend::Wasapi;
        // WASAPI can only do DoP; ASIO returns to whatever the user last picked
        // there, which starts out as Native DSD.
        const auto mode = wasapi ? wasio::default_dsd_mode(backend) : state.asio_dsd_mode;
        EnableWindow(state.dsd_native, wasapi ? FALSE : TRUE);
        select_dsd_mode(state, mode);
        refresh_devices(state);
        apply_device_selection(state);
        break;
    }
    case kDevice:
        if (code == CBN_SELCHANGE) apply_device_selection(state);
        break;
    case kRefresh:
        refresh_devices(state);
        apply_device_selection(state);
        break;
    case kControlPanel: {
        // Only legal while the device is idle, so stop first.
        state.controller.stop();
        apply_device_selection(state);
        wasio::AsioSession session;
        std::string error;
        std::string device = state.controller.device();
        if (wasio::resolve_device(wasio::PlaybackBackend::Asio, &device) &&
            session.open(device, &error)) {
            session.control_panel();
            session.close();
        } else {
            SetWindowTextW(state.status_line, widen(error.empty() ? "no ASIO driver" : error).c_str());
        }
        break;
    }
    case kAddFiles:
        add_paths(state, ask_for_files(state.window, false));
        break;
    case kRemove: {
        const int index = selected_track(state);
        if (index >= 0 && playlist.remove(static_cast<std::size_t>(index))) {
            rebuild_playlist_view(state);
            select_track(state, std::min(index, static_cast<int>(playlist.size()) - 1));
        }
        break;
    }
    case kClear:
        state.controller.stop();
        playlist.clear();
        rebuild_playlist_view(state);
        break;
    case kMoveUp: {
        const int index = selected_track(state);
        if (index > 0 && playlist.move_up(static_cast<std::size_t>(index))) {
            rebuild_playlist_view(state);
            select_track(state, index - 1);
        }
        break;
    }
    case kMoveDown: {
        const int index = selected_track(state);
        if (index >= 0 && playlist.move_down(static_cast<std::size_t>(index))) {
            rebuild_playlist_view(state);
            select_track(state, index + 1);
        }
        break;
    }
    case kSaveList: {
        std::vector<wchar_t> buffer(MAX_PATH * 4, L'\0');
        OPENFILENAMEW dialog = {};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = state.window;
        dialog.lpstrFile = buffer.data();
        dialog.nMaxFile = static_cast<DWORD>(buffer.size());
        dialog.lpstrFilter = L"Playlists\0*.m3u8\0All files\0*.*\0";
        dialog.lpstrDefExt = L"m3u8";
        dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog)) {
            std::string error;
            if (!playlist.save_m3u8(narrow(buffer.data()), &error)) {
                SetWindowTextW(state.status_line, widen(error).c_str());
            }
        }
        break;
    }
    case kLoadList: {
        const auto files = ask_for_files(state.window, true);
        if (files.empty()) break;
        state.controller.stop();
        std::string error;
        if (!playlist.load_m3u8(narrow(files.front()), &error)) {
            SetWindowTextW(state.status_line, widen(error).c_str());
        }
        rebuild_playlist_view(state);
        break;
    }
    case kPrev:
        state.controller.previous();
        select_track(state, static_cast<int>(playlist.current_index()));
        break;
    case kNext:
        state.controller.next();
        select_track(state, static_cast<int>(playlist.current_index()));
        break;
    case kStop:
        state.controller.stop();
        break;
    case kPlayPause: {
        const auto status = state.controller.status();
        if (status.state == wasio::PlayerState::Playing) {
            state.controller.pause();
        } else if (status.state == wasio::PlayerState::Paused) {
            state.controller.resume();
        } else {
            start_selected(state);
        }
        break;
    }
    case kRepeat:
        if (code == CBN_SELCHANGE) {
            const auto selection = SendMessageW(state.repeat, CB_GETCURSEL, 0, 0);
            const wasio::RepeatMode modes[] = {wasio::RepeatMode::Off, wasio::RepeatMode::One,
                                               wasio::RepeatMode::All};
            if (selection >= 0 && selection < 3) {
                playlist.set_repeat(modes[selection]);
            }
        }
        break;
    case kShuffle:
        playlist.set_shuffle(SendMessageW(state.shuffle, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
    case kDsdNative:
    case kDsdDop: {
        const auto wanted = id == kDsdNative ? wasio::DsdOutputMode::Native
                                             : wasio::DsdOutputMode::DoP;
        if (!wasio::dsd_mode_supported(state.controller.backend(), wanted, nullptr)) {
            select_dsd_mode(state, wasio::default_dsd_mode(state.controller.backend()));
            break;
        }
        if (state.controller.backend() == wasio::PlaybackBackend::Asio) {
            state.asio_dsd_mode = wanted;
        }
        select_dsd_mode(state, wanted);
        break;
    }
    default:
        break;
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* state = reinterpret_cast<GuiState*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<GuiState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->window = window;

        state->font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        state->font_bold = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        state->background_brush = CreateSolidBrush(kTheme.background);
        state->white_brush = CreateSolidBrush(kTheme.white);

        state->backend_wasapi = make_radio(window, kBackendWasapi, L"WASAPI", true);
        state->backend_asio = make_radio(window, kBackendAsio, L"ASIO", false);
        SendMessageW(state->backend_wasapi, BM_SETCHECK, BST_CHECKED, 0);
        state->device = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                            WS_VSCROLL,
                                        0, 0, 0, 0, window,
                                        control_id(kDevice), nullptr, nullptr);
        state->refresh = make_button(window, kRefresh, L"Refresh");
        state->control_panel = make_button(window, kControlPanel, L"ASIO Control Panel");

        state->playlist = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, nullptr,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                              LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                          0, 0, 0, 0, window,
                                          control_id(kPlaylist), nullptr, nullptr);
        ListView_SetExtendedListViewStyle(state->playlist,
                                          LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const struct {
            const wchar_t* title;
            int width;
        } columns[] = {{L"#", 46},   {L"Title", 320}, {L"Format", 110},
                       {L"Rate", 90}, {L"Ch", 56},    {L"Length", 70}};
        for (int i = 0; i < 6; ++i) {
            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<wchar_t*>(columns[i].title);
            column.cx = columns[i].width;
            column.iSubItem = i;
            ListView_InsertColumn(state->playlist, i, &column);
        }

        state->add_files = make_button(window, kAddFiles, L"Add Files");
        state->remove_track = make_button(window, kRemove, L"Remove");
        state->clear_list = make_button(window, kClear, L"Clear");
        state->move_up = make_button(window, kMoveUp, L"Up");
        state->move_down = make_button(window, kMoveDown, L"Down");
        state->save_list = make_button(window, kSaveList, L"Save List");
        state->load_list = make_button(window, kLoadList, L"Load List");

        state->previous = make_button(window, kPrev, L"Prev");
        state->play_pause = make_button(window, kPlayPause, L"Play");
        state->stop = make_button(window, kStop, L"Stop");
        state->next = make_button(window, kNext, L"Next");
        state->seek = CreateWindowExW(0, TRACKBAR_CLASSW, nullptr,
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                      0, 0, 0, 0, window, control_id(kSeek), nullptr,
                                      nullptr);
        SendMessageW(state->seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSeekRange));
        state->time_label = CreateWindowExW(0, L"STATIC", L"00:00 / 00:00",
                                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
                                            control_id(kTimeLabel), nullptr, nullptr);

        state->repeat = CreateWindowExW(0, L"COMBOBOX", nullptr,
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST, 0, 0,
                                        0, 0, window, control_id(kRepeat), nullptr,
                                        nullptr);
        for (const wchar_t* mode : {L"Off", L"One", L"All"}) {
            SendMessageW(state->repeat, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(mode));
        }
        SendMessageW(state->repeat, CB_SETCURSEL, 0, 0);
        state->shuffle = CreateWindowExW(0, L"BUTTON", L"Shuffle",
                                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0,
                                         0, 0, window, control_id(kShuffle), nullptr,
                                         nullptr);
        state->dsd_native = make_radio(window, kDsdNative, L"Native DSD", true);
        state->dsd_dop = make_radio(window, kDsdDop, L"DoP", false);
        // The window opens on WASAPI, which only has DoP.
        select_dsd_mode(*state, wasio::default_dsd_mode(wasio::PlaybackBackend::Wasapi));
        EnableWindow(state->dsd_native, FALSE);

        state->status_line = CreateWindowExW(0, L"STATIC", L"Ready",
                                             WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
                                             control_id(kStatusLine), nullptr,
                                             nullptr);

        for (HWND control : {state->backend_wasapi, state->backend_asio, state->device,
                             state->refresh, state->control_panel, state->playlist,
                             state->add_files, state->remove_track, state->clear_list,
                             state->move_up, state->move_down, state->save_list, state->load_list,
                             state->previous, state->play_pause, state->stop, state->next,
                             state->repeat, state->shuffle, state->dsd_native, state->dsd_dop,
                             state->time_label, state->status_line}) {
            apply_font(control, state->font);
        }

        refresh_devices(*state);
        apply_device_selection(*state);
        layout_controls(*state);
        SetTimer(window, kPollTimer, 100, nullptr);
        return 0;
    }

    case WM_SIZE:
        if (state) layout_controls(*state);
        return 0;

    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize.x = 900;
        info->ptMinTrackSize.y = 600;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        if (state) paint_window(window, *state, dc);
        EndPaint(window, &paint);
        return 0;
    }

    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (state && item->CtlType == ODT_BUTTON) {
            draw_button(*item, *state);
            return TRUE;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kTheme.text);
        // Static text sits on the painted background, so report the matching
        // brush rather than letting the control paint its own grey.
        const HWND control = reinterpret_cast<HWND>(lparam);
        if (control == state->status_line) {
            SetTextColor(dc, state->last_error.empty() ? kTheme.text : kTheme.red);
        }
        return reinterpret_cast<LRESULT>(state->background_brush);
    }

    case WM_HSCROLL: {
        if (!state || reinterpret_cast<HWND>(lparam) != state->seek) break;
        const int code = LOWORD(wparam);
        if (code == TB_THUMBTRACK) {
            state->seeking = true;
        } else if (code == TB_THUMBPOSITION || code == TB_ENDTRACK) {
            const auto position = SendMessageW(state->seek, TBM_GETPOS, 0, 0);
            const auto status = state->controller.status();
            if (status.duration_seconds > 0.0) {
                state->controller.seek(static_cast<double>(position) / kSeekRange *
                                       status.duration_seconds);
            }
            state->seeking = false;
        }
        return 0;
    }

    case WM_NOTIFY: {
        if (!state) break;
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom == kPlaylist && header->code == NM_DBLCLK) {
            const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lparam);
            if (activate->iItem >= 0) {
                apply_device_selection(*state);
                state->controller.play(static_cast<std::size_t>(activate->iItem));
            }
            return 0;
        }
        break;
    }

    case WM_DROPFILES: {
        if (!state) break;
        auto drop = reinterpret_cast<HDROP>(wparam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        for (UINT i = 0; i < count; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring path(length, L'\0');
            DragQueryFileW(drop, i, path.data(), length + 1);
            paths.push_back(path);
        }
        DragFinish(drop);
        add_paths(*state, paths);
        return 0;
    }

    case WM_COMMAND:
        if (state) on_command(*state, LOWORD(wparam), HIWORD(wparam));
        return 0;

    case WM_TIMER: {
        if (!state || wparam != kPollTimer) break;
        const auto status = state->controller.poll();
        update_play_button(*state, status);
        update_status(*state, status);
        // poll() may have moved to the next track by itself.
        const auto current = state->controller.playlist().current_index();
        if (status.state != wasio::PlayerState::Stopped &&
            current != wasio::Playlist::kInvalidIndex &&
            selected_track(*state) != static_cast<int>(current)) {
            select_track(*state, static_cast<int>(current));
        }
        return 0;
    }

    case WM_DESTROY:
        if (state) {
            KillTimer(window, kPollTimer);
            state->controller.stop();
            if (state->font) DeleteObject(state->font);
            if (state->font_bold) DeleteObject(state->font_bold);
            if (state->background_brush) DeleteObject(state->background_brush);
            if (state->white_brush) DeleteObject(state->white_brush);
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show)
{
    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&controls);

    GuiState state;
    state.icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_WASIO_PLAYER));

    WNDCLASSEXW klass = {};
    klass.cbSize = sizeof(klass);
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    klass.lpszClassName = L"WasioPlayerWindow";
    klass.hIcon = state.icon;
    klass.hIconSm = state.icon;
    if (RegisterClassExW(&klass) == 0) return 1;

    HWND window = CreateWindowExW(WS_EX_ACCEPTFILES, klass.lpszClassName, WASIO_APP_TITLE,
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 720,
                                  nullptr, nullptr, instance, &state);
    if (window == nullptr) return 1;

    // Command-line files land in the playlist so "open with" works.
    int argument_count = 0;
    if (LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count)) {
        std::vector<std::wstring> paths;
        for (int i = 1; i < argument_count; ++i) paths.push_back(arguments[i]);
        LocalFree(arguments);
        if (!paths.empty()) add_paths(state, paths);
    }

    ShowWindow(window, show);
    UpdateWindow(window);

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
