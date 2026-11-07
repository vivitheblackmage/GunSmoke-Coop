// Gun.Smoke Co-op patcher: makes the player's own Gun.Smoke (NES: USA, Europe) or the Swedish Krut.Rök into one for two
// players (tools/gunsmoke-coop/variants.mjs; 1 PLAYER or 2 PLAYERS on the title screen, three lives each, the score
// and bought upgrades shared; tools/gunsmoke-coop/coop.asm, docs/HACK.md) with or without friendly
// fire, player 2's colours chosen. Carries only our own code (patch_data.hpp); checks the ROM by SHA-256 and writes a
// new file, leaving the original untouched.
// The window: SDL for the window, input and file dialogs; the picture drawn by Windows GDI at the window's own
// resolution (smooth text in Segoe UI). Its left half is stage 1 with two players drawn from the user's ROM: the native
// port (games/gunsmoke, gunsmoke::Engine with its co-op) runs the ROM's program for a while, and the frame is shown
// with player 2 in the colours chosen and the patch's lives in the corners; the game's logo comes from its title.
//   GunSmoke-Coop-Patcher [rom]                                   the window (a ROM given or dropped is chosen at once)
//   GunSmoke-Coop-Patcher --verify <rom> <expected> [ff]          test: patch and compare, no window (exit 0 when equal)
//   GunSmoke-Coop-Patcher --preview <out.bmp> [rom]               the window as a picture, no window
//   GunSmoke-Coop-Patcher --states <Mesen.exe> <patched rom>      the debug states (Mesen debug states), no window
#define NOMINMAX // std::min / std::max, not windows.h's macros
#include "patch_data.hpp"
#include "compat/nes/ppu.hpp"
#include "engine.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
// The supported ROMs are patch_data.hpp's variants (each by the SHA-256 of the ROM without its 16-byte iNES header).
constexpr std::size_t header_size = 16, prg_size = 0x20000;

std::string sha256(const unsigned char* data, std::size_t size) {
    struct Hash { BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
        ~Hash() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); } } context;
    std::array<unsigned char, 32> bytes{};
    if (BCryptOpenAlgorithmProvider(&context.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(context.algorithm, &context.hash, nullptr, 0, nullptr, 0, 0) < 0 ||
        BCryptHashData(context.hash, const_cast<unsigned char*>(data), static_cast<ULONG>(size), 0) < 0 ||
        BCryptFinishHash(context.hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
        throw std::runtime_error("SHA-256 failed");
    std::string text;
    for (const auto b : bytes) { text += "0123456789abcdef"[b >> 4]; text += "0123456789abcdef"[b & 15]; }
    return text;
}

std::vector<unsigned char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("The file cannot be read");
    return {std::istreambuf_iterator<char>(input), {}};
}

// A supported ROM (iNES header, mapper 2, 8 PRG banks) and its variant, or an error for the window.
struct Checked { std::vector<unsigned char> rom; const coop::Variant* variant = nullptr; };
Checked checked(std::vector<unsigned char> rom) {
    if (rom.size() != header_size + prg_size || rom[0] != 'N' || rom[1] != 'E' || rom[2] != 'S' || rom[3] != 0x1A)
        throw std::runtime_error("Not a supported ROM (Gun.Smoke USA or Europe, Krut.Rök)");
    const auto hash = sha256(rom.data() + header_size, rom.size() - header_size);
    for (const auto& v : coop::variants) if (hash == v.sha256) return {std::move(rom), &v};
    throw std::runtime_error("Not a supported ROM (Gun.Smoke USA or Europe, Krut.Rök)");
}

// Player 2's colours: the sprite palette she uses (1-3; the game's four are all in use, so she shares one) and, when
// own is on, its three colours in co-op (NES colour numbers; the others using that palette change too).
struct Options { bool friendly_fire = false; int palette = 2; bool own = false; std::array<int, 3> colours{0x16, 0x36, 0x0F}; };
// The game's sprite palettes in play and what uses each.
constexpr std::array<std::array<int, 3>, 4> game_colours{{{0x22, 0x38, 0x0F}, {0x20, 0x10, 0x0F}, {0x16, 0x36, 0x0F}, {0x1A, 0x37, 0x0F}}};
constexpr std::array<const char*, 4> palette_users{"player 1", "the score and items", "enemies and shots", "the stage's objects"};

std::vector<unsigned char> patch(std::vector<unsigned char> rom, const coop::Variant& variant, const Options& o) {
    for (const auto& run : std::span(variant.runs, variant.count)) std::copy(run.bytes, run.bytes + run.size, rom.begin() + header_size + run.offset);
    if (variant.pal) std::copy(std::begin(coop::pal_header), std::end(coop::pal_header), rom.begin()); // timing PAL in the header
    auto* config = &rom[header_size + coop::config_offset];
    config[0] = o.friendly_fire ? 1 : 0;
    config[1] = static_cast<unsigned char>(o.palette);
    config[2] = o.own ? 1 : 0;
    for (int i = 0; i < 3; ++i) config[3 + i] = static_cast<unsigned char>(o.colours[static_cast<std::size_t>(i)] & 0x3F);
    return rom;
}

std::filesystem::path utf8_path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
std::string path_utf8(const std::filesystem::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

// The debug states (docs/HACK.md, "Debug states"): the user's own Mesen 2 runs our script
// (tools/gunsmoke-coop/mesen-states.lua, coop::mesen_states_lua) in its test runner on the patched ROM and prints a save
// state per slot (slots 1-6: stages 1-6 with 2 PLAYERS, 7: stage 1's boss); they become Mesen's slot files
// <ROM name>_<slot>.mss in its SaveStates folder (next to Mesen.exe when it is portable, else Documents/Mesen2), where
// F1-F7 load them. A slot file already there is kept as <file>.bak. Mesen's scripts need no file access for this.
std::filesystem::path mesen_states_folder(const std::filesystem::path& exe) {
    if (std::filesystem::exists(exe.parent_path() / "settings.json")) return exe.parent_path() / "SaveStates";
    PWSTR documents = nullptr;
    std::filesystem::path folder;
    if (SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents) == S_OK) folder = std::filesystem::path(documents) / "Mesen2" / "SaveStates";
    CoTaskMemFree(documents);
    if (folder.empty()) throw std::runtime_error("No Documents folder for Mesen");
    return folder;
}
// Runs a program hidden and returns what it printed.
std::string run_and_read(std::wstring command) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) throw std::runtime_error("Mesen cannot be started");
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW start{};
    start.cb = sizeof(start); start.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; start.wShowWindow = SW_HIDE;
    start.hStdOutput = write; start.hStdError = write; start.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &process);
    CloseHandle(write);
    if (!started) { CloseHandle(read); throw std::runtime_error("Mesen cannot be started"); }
    std::string out;
    std::vector<char> buffer(1 << 16);
    DWORD n = 0;
    while (ReadFile(read, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) && n) out.append(buffer.data(), n);
    WaitForSingleObject(process.hProcess, 600000);
    CloseHandle(read); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    return out;
}
// Makes the states; the window's status when done.
std::string make_states(const std::filesystem::path& mesen, const std::filesystem::path& rom) {
    const auto script = std::filesystem::temp_directory_path() / "gunsmoke-coop-states.lua";
    {
        std::ofstream file(script, std::ios::binary | std::ios::trunc);
        file << coop::mesen_states_lua;
        if (!file) throw std::runtime_error("The script cannot be written");
    }
    const auto out = run_and_read(L"\"" + mesen.wstring() + L"\" --testrunner \"" + rom.wstring() + L"\" \"" + script.wstring() + L"\"");
    std::error_code ignored;
    std::filesystem::remove(script, ignored);
    std::vector<std::pair<int, std::vector<unsigned char>>> states;
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (line.rfind("GSSTATE ", 0) != 0) continue;
        std::istringstream words(line.substr(8));
        int slot = 0, stage = 0; std::string hex;
        words >> slot >> stage >> hex;
        if (slot < 1 || slot > 7 || hex.empty() || hex.size() % 2) continue;
        const auto digit = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        std::vector<unsigned char> data(hex.size() / 2);
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<unsigned char>(digit(hex[2 * i]) << 4 | digit(hex[2 * i + 1]));
        states.emplace_back(slot, std::move(data));
    }
    if (states.size() != 7 || out.find("GSDONE") == std::string::npos)
        throw std::runtime_error("Mesen made " + std::to_string(states.size()) + " of 7 states (is it Mesen 2?)");
    const auto folder = mesen_states_folder(mesen);
    std::filesystem::create_directories(folder);
    for (const auto& [slot, data] : states) {
        const auto file = folder / (rom.stem().wstring() + L"_" + std::to_wstring(slot) + L".mss");
        if (std::filesystem::exists(file)) {
            auto backup = file; backup += ".bak";
            std::filesystem::remove(backup, ignored);
            std::filesystem::rename(file, backup);
        }
        std::ofstream state(file, std::ios::binary | std::ios::trunc);
        state.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!state) throw std::runtime_error("A state cannot be written");
    }
    return "Mesen states made: F1-F6 load stages 1-6, F7 stage 1's boss";
}

// ---------------------------------------------------------------------------------------------- the pictures
// From the user's ROM by the native port: stage 1 a while after both players entered (they stand at the bottom and
// fire up as the gunmen come), and the title's logo. Nothing of the game is in the patcher.
struct Pictures {
    rg::nes::PpuState play{};           // the frame (player 2 recoloured when drawn)
    std::array<bool, 64> hers{};        // her sprites in it
    std::vector<std::uint8_t> logo;     // RGBA of the logo (its rows of the title, black around it trimmed)
    unsigned logo_w = 0, logo_h = 0;
    bool ok = false;
};
// Frames of the play picture after power-on (START at 60).
constexpr unsigned play_frames = 1180, title_frames = 230;
Pictures make_pictures(const std::vector<unsigned char>& rom) {
    Pictures p;
    const std::vector<std::uint8_t> prg(rom.begin() + header_size, rom.begin() + header_size + prg_size);
    try {
        gunsmoke::Engine e(prg);
        e.coop = true;
        e.power_on();
        for (unsigned f = 1; f <= play_frames; ++f) {
            const std::uint8_t start = f >= 60 && f < 66 ? 0x08 : 0;
            // both fire straight up (A+B) in bursts; player 1 a little left at first, player 2 a little right
            const std::uint8_t fire = f > 830 && ((f >> 3) & 1) ? 0x03 : 0;
            const std::uint8_t p1 = f > 830 && f < 850 ? 0x40 : 0, p2 = f > 830 && f < 846 ? 0x80 : 0;
            e.m.pads[0] = static_cast<std::uint8_t>(start | fire | p1); e.m.pads[1] = static_cast<std::uint8_t>(fire | p2);
            if (e.m.ram[0x7B] < 2) e.m.ram[0x7B] = 2; // kept from harm
            e.m.ram[0x49] = 1;
            e.run_tasks();
            e.nmi();
            e.m.apu_writes.clear();
        }
        e.m.latch_scroll();
        p.play = e.m.ppu;
        p.hers = e.her_oam();
        // The patch's lives in the corners (coop.asm hud: the hat, tiles 0 and 1, and the digit $58 + 3) in OAM's last six.
        const std::uint8_t hud[24] = {0xD0, 0x00, 0x00, 0x10, 0xD0, 0x01, 0x00, 0x18, 0xD0, 0x5B, 0x01, 0x22,
                                      0xD0, 0x00, 0x02, 0xD4, 0xD0, 0x01, 0x02, 0xDC, 0xD0, 0x5B, 0x01, 0xE6};
        std::copy(std::begin(hud), std::end(hud), p.play.oam.begin() + 232);
        for (unsigned i = 58; i < 64; ++i) p.hers[i] = (i == 61 || i == 62);
        p.ok = true;
    } catch (...) { p.ok = false; }
    try {
        gunsmoke::Engine t(prg);
        t.power_on();
        for (unsigned i = 0; i < title_frames; ++i) { t.run_tasks(); t.nmi(); }
        t.m.latch_scroll();
        auto logo = t.m.ppu;
        for (unsigned row = 11; row < 30; ++row) std::fill_n(&logo.ciram[row * 32], 32, std::uint8_t{0xF4});
        for (unsigned i = 0; i < 64; ++i) logo.oam[i * 4] = 0xF0;
        const auto full = rg::nes::render(logo);
        unsigned x0 = 256, y0 = 240, x1 = 0, y1 = 0;
        const auto back = std::array<std::uint8_t, 3>{full[0], full[1], full[2]};
        for (unsigned y = 0; y < 96; ++y) for (unsigned x = 0; x < 256; ++x) {
            const auto* q = &full[(y * 256 + x) * 4];
            if (q[0] != back[0] || q[1] != back[1] || q[2] != back[2]) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
        }
        if (x1 > x0 && y1 > y0) {
            p.logo_w = x1 - x0 + 1; p.logo_h = y1 - y0 + 1;
            p.logo.resize(p.logo_w * p.logo_h * 4);
            for (unsigned y = 0; y < p.logo_h; ++y) for (unsigned x = 0; x < p.logo_w; ++x) {
                const auto* q = &full[((y0 + y) * 256 + x0 + x) * 4];
                auto* d = &p.logo[(y * p.logo_w + x) * 4];
                const bool background = q[0] == back[0] && q[1] == back[1] && q[2] == back[2];
                d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = background ? 0 : 255;
            }
        }
    } catch (...) { p.logo.clear(); }
    return p;
}
// The play picture with player 2's colours: her sprites moved from palette 2 to the chosen one, own colours into it.
std::vector<std::uint8_t> play_picture(const Pictures& p, const Options& o) {
    auto ppu = p.play;
    for (unsigned i = 0; i < 64; ++i)
        if (p.hers[i] && (ppu.oam[4 * i + 2] & 3) == 2) ppu.oam[4 * i + 2] = static_cast<std::uint8_t>((ppu.oam[4 * i + 2] & 0xFC) | o.palette);
    if (o.own) for (unsigned k = 0; k < 3; ++k) ppu.palette[0x10 + 4 * static_cast<unsigned>(o.palette) + 1 + k] = static_cast<std::uint8_t>(o.colours[k] & 0x3F);
    return rg::nes::render(ppu);
}

// ---------------------------------------------------------------------------------------------- drawing
// A 32-bit picture drawn by GDI at `scale` pixels per layout unit (the window's pixel density).
COLORREF hex(std::uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); }
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    std::wstring out(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), static_cast<int>(out.size()));
    return out;
}
struct Canvas {
    int w = 0, h = 0; float s = 1;
    HDC dc = nullptr; HBITMAP bitmap = nullptr; HGDIOBJ old = nullptr; std::uint32_t* bits = nullptr;
    std::map<std::pair<int, int>, HFONT> fonts;
    Canvas(int width, int height, float scale) : w(width), h(height), s(scale) {
        dc = CreateCompatibleDC(nullptr);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (!bitmap) throw std::runtime_error("No picture for the window");
        old = SelectObject(dc, bitmap);
        SetBkMode(dc, TRANSPARENT);
    }
    ~Canvas() { for (auto& [k, f] : fonts) DeleteObject(f); SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); }
    Canvas(const Canvas&) = delete; Canvas& operator=(const Canvas&) = delete;
    int px(float v) const { return static_cast<int>(std::lround(v * s)); }
    void rect(float x, float y, float rw, float rh, std::uint32_t c) {
        RECT r{px(x), px(y), px(x + rw), px(y + rh)};
        HBRUSH b = CreateSolidBrush(hex(c)); FillRect(dc, &r, b); DeleteObject(b);
    }
    void round(float x, float y, float rw, float rh, float radius, std::uint32_t fill, std::optional<std::uint32_t> border = std::nullopt, float width = 1) {
        HBRUSH b = CreateSolidBrush(hex(fill));
        HPEN p = border ? CreatePen(PS_SOLID, std::max(1, px(width)), hex(*border)) : CreatePen(PS_NULL, 0, 0);
        auto ob = SelectObject(dc, b); auto op = SelectObject(dc, p);
        RoundRect(dc, px(x), px(y), px(x + rw) + (border ? 0 : 1), px(y + rh) + (border ? 0 : 1), px(2 * radius), px(2 * radius));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
    }
    HFONT font(float size, int weight) {
        const auto key = std::make_pair(px(size), weight);
        if (auto it = fonts.find(key); it != fonts.end()) return it->second;
        HFONT f = CreateFontW(-key.first, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        return fonts[key] = f;
    }
    // Text in a box (DT_ flags); returns its height in layout units.
    float text(float x, float y, float rw, float rh, const std::string& t, float size, int weight, std::uint32_t c,
               UINT flags = DT_LEFT | DT_TOP | DT_WORDBREAK) {
        const auto w16 = wide(t);
        auto of = SelectObject(dc, font(size, weight));
        SetTextColor(dc, hex(c));
        RECT r{px(x), px(y), px(x + rw), px(y + rh)};
        RECT measure = r;
        DrawTextW(dc, w16.c_str(), -1, &measure, flags | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(dc, w16.c_str(), -1, &r, flags | DT_NOPREFIX);
        SelectObject(dc, of);
        return static_cast<float>(measure.bottom - measure.top) / s;
    }
    float text_width(const std::string& t, float size, int weight) {
        const auto w16 = wide(t);
        auto of = SelectObject(dc, font(size, weight));
        SIZE sz{}; GetTextExtentPoint32W(dc, w16.c_str(), static_cast<int>(w16.size()), &sz);
        SelectObject(dc, of);
        return static_cast<float>(sz.cx) / s;
    }
    // An RGBA picture at a whole number of pixels per pixel (nearest; alpha 0 left out), clipped to rounded corners.
    void picture(float x, float y, const std::vector<std::uint8_t>& rgba, unsigned pw, unsigned ph, int factor, float radius = 0) {
        const int X = px(x), Y = px(y), R = px(radius), W = static_cast<int>(pw) * factor, Hh = static_cast<int>(ph) * factor;
        // outside the rounded corners (radius R pixels)
        const auto cut = [&](int dx, int dy) {
            if (R <= 0) return false;
            const int cx = dx < R ? R - dx : dx >= W - R ? dx - (W - R - 1) : 0, cy = dy < R ? R - dy : dy >= Hh - R ? dy - (Hh - R - 1) : 0;
            return cx > 0 && cy > 0 && cx * cx + cy * cy > R * R;
        };
        for (unsigned j = 0; j < ph; ++j) for (unsigned i = 0; i < pw; ++i) {
            const auto* q = &rgba[(j * pw + i) * 4];
            if (q[3] == 0) continue;
            const std::uint32_t v = static_cast<std::uint32_t>(q[0]) << 16 | static_cast<std::uint32_t>(q[1]) << 8 | q[2];
            for (int dy = 0; dy < factor; ++dy) {
                const int yy = Y + static_cast<int>(j) * factor + dy;
                if (yy < 0 || yy >= h) continue;
                for (int dx = 0; dx < factor; ++dx) {
                    const int xx = X + static_cast<int>(i) * factor + dx;
                    if (xx < 0 || xx >= w) continue;
                    if (cut(static_cast<int>(i) * factor + dx, static_cast<int>(j) * factor + dy)) continue;
                    bits[yy * w + xx] = v;
                }
            }
        }
    }
};

// The look: dark, with the logo's orange as the accent.
namespace colour {
constexpr std::uint32_t window = 0x101216, panel = 0x181B21, raised = 0x20242C, line = 0x2C313B, line_hi = 0x3A404C,
    text = 0xE9ECF1, soft = 0xB3BAC6, muted = 0x7C8494, accent = 0xE8742A, accent_hi = 0xF58C45, ok = 0x46B67B, bad = 0xE25B5B;
}

// ---------------------------------------------------------------------------------------------- the window's state
// The controls in keyboard order; the mouse finds them by the rectangles the last drawing left.
enum Control { choose_button, fire_switch, palette_choice, own_switch, colour1, colour2, colour3, patch_button, states_button, control_count };
struct Hit { Control control; float x, y, w, h; int part = 0; }; // part: a palette chip's number, or -1/+1 of a colour's arrows

struct App {
    std::filesystem::path rom_path, saved_path;
    std::vector<unsigned char> rom;         // checked, empty until a good ROM is chosen
    const coop::Variant* variant = nullptr; // its variant
    Pictures pictures;
    std::vector<std::uint8_t> shown;        // the play picture with the current colours (256 x 240 RGBA)
    std::string status = "Choose your ROM, or drop it on this window";
    bool error = false, busy = false;
    Options options;
    int focus = choose_button, hover = -1;
    std::vector<Hit> hits;

    void choose(const std::filesystem::path& path) {
        rom_path = path; rom.clear(); variant = nullptr; saved_path.clear(); pictures = {}; shown.clear();
        try {
            auto c = checked(read_file(path)); rom = std::move(c.rom); variant = c.variant;
            pictures = make_pictures(rom);
            redraw_picture();
            status = std::string("Ready to patch your ") + variant->name + " ROM"; error = false; focus = patch_button;
        } catch (const std::exception& e) { status = e.what(); error = true; }
    }
    void redraw_picture() { if (pictures.ok) shown = play_picture(pictures, options); }
    bool enabled(int c) const {
        if (c == colour1 || c == colour2 || c == colour3) return options.own;
        if (c == patch_button) return !rom.empty() && !busy;
        if (c == states_button) return !saved_path.empty() && !busy;
        return !busy;
    }
    void move_focus(int step) {
        for (int k = 0; k < control_count; ++k) {
            focus = (focus + step + control_count) % control_count;
            if (enabled(focus)) return;
        }
    }
    // Left/right on a control (or a click on one of its parts).
    void change(int c, int step) {
        switch (c) {
        case fire_switch: options.friendly_fire = !options.friendly_fire; break;
        case own_switch: options.own = !options.own; break;
        case palette_choice: options.palette = (options.palette - 1 + step + 3) % 3 + 1; if (!options.own) options.colours = game_colours[static_cast<std::size_t>(options.palette)]; break;
        case colour1: case colour2: case colour3: {
            auto& v = options.colours[static_cast<std::size_t>(c - colour1)];
            v = (v + step + 64) % 64;
            if ((v & 0x0F) >= 0x0E) v = step > 0 ? (v | 0x0F) + 1 : (v & 0x30) | 0x0D; // the blacks $xE/$xF left out
            v &= 0x3F;
            break;
        }
        default: break;
        }
        redraw_picture();
    }
    void save(const std::filesystem::path& path) {
        try {
            if (rom.empty()) throw std::runtime_error("Choose a ROM first");
            if (std::filesystem::exists(path) && std::filesystem::equivalent(path, rom_path))
                throw std::runtime_error("Choose a new name: your ROM stays as it is");
            const auto patched = patch(rom, *variant, options);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(patched.data()), static_cast<std::streamsize>(patched.size()));
            if (!file) throw std::runtime_error("The new ROM cannot be written");
            saved_path = path;
            status = "Saved as " + path_utf8(path.filename()); error = false; focus = states_button;
        } catch (const std::exception& e) { status = e.what(); error = true; }
    }

    // The window (1000 x 580 layout units).
    static constexpr float W = 1000, H = 580;
    void draw(Canvas& c) {
        hits.clear();
        c.rect(0, 0, W, H, colour::window);
        draw_picture(c);
        draw_panel(c);
    }
    void draw_picture(Canvas& c) {
        const float x = 28, y = 28, pw = 512, ph = 448;
        c.round(x - 1, y - 1, pw + 2, ph + 2, 12, colour::panel, colour::line);
        if (!shown.empty()) {
            // 224 of the 240 lines (8 off the top and the bottom, as a TV), 2 pixels per pixel (more on a dense screen)
            std::vector<std::uint8_t> cut(shown.begin() + 8 * 256 * 4, shown.begin() + 232 * 256 * 4);
            const int factor = std::max(2, static_cast<int>(std::floor(2 * c.s)));
            c.picture(x, y, cut, 256, 224, factor, 11);
            c.text(x, y + ph + 12, pw, 20, "Stage 1 with two players, drawn from your ROM. Player 2 in the colours you choose.", 12.5f, FW_NORMAL, colour::muted);
        } else {
            c.text(x, y + ph / 2 - 40, pw, 30, rom.empty() ? "Drop your Gun.Smoke ROM here" : "No picture for this ROM", 22, FW_SEMIBOLD, colour::soft, DT_CENTER | DT_TOP);
            c.text(x, y + ph / 2 - 4, pw, 60, "USA, Europe or the Swedish Krut.Rök. Your file is only read: the patch is a new file.", 14, FW_NORMAL, colour::muted, DT_CENTER | DT_TOP | DT_WORDBREAK);
        }
    }
    // A button; primary: filled with the accent.
    void button(Canvas& c, Control id, float x, float y, float bw, const std::string& label, bool primary) {
        const bool on = enabled(id), hot = hover == id && on, focused = focus == id && on;
        const auto fill = !on ? colour::panel : primary ? (hot ? colour::accent_hi : colour::accent) : (hot ? colour::line : colour::raised);
        c.round(x, y, bw, 40, 8, fill, focused ? std::optional<std::uint32_t>(primary ? 0xFFD2B0 : colour::accent) : (primary && on ? std::nullopt : std::optional<std::uint32_t>(colour::line)), focused ? 2.f : 1.f);
        c.text(x, y + 10, bw, 22, label, 14.5f, FW_SEMIBOLD, !on ? colour::muted : primary ? 0x1A0D05 : colour::text, DT_CENTER | DT_TOP | DT_SINGLELINE);
        hits.push_back({id, x, y, bw, 40});
    }
    void toggle(Canvas& c, Control id, float x, float y, bool value) {
        const bool focused = focus == id && enabled(id);
        c.round(x, y, 44, 24, 12, value ? colour::accent : colour::line, focused ? std::optional<std::uint32_t>(0xFFD2B0) : std::nullopt, 2);
        c.round(value ? x + 23 : x + 3, y + 3, 18, 18, 9, value ? 0xFFF3EA : colour::soft);
        hits.push_back({id, x - 260, y - 4, 304, 32});
    }
    void swatch(Canvas& c, float x, float y, float size, int nes) {
        const auto v = rg::nes::rgb(static_cast<std::uint8_t>(nes & 0x3F));
        c.round(x, y, size, size, 4, v & 0xFFFFFF, colour::line_hi);
    }
    void draw_panel(Canvas& c) {
        const float x = 568, w = 404;
        float y = 30;
        // the logo from the ROM's title, else the name
        if (!pictures.logo.empty()) {
            const int factor = std::max(1, static_cast<int>(std::floor(c.s * (pictures.logo_w * 2 <= 430 ? 2.f : 1.5f))));
            c.picture(x, y, pictures.logo, pictures.logo_w, pictures.logo_h, factor);
            y += static_cast<float>(pictures.logo_h * static_cast<unsigned>(factor)) / c.s + 14;
        } else {
            c.text(x, y, w, 44, "Gun.Smoke", 34, FW_BOLD, colour::accent);
            y += 50;
        }
        c.text(x, y, w, 30, "Two-player patch", 21, FW_SEMIBOLD, colour::text);
        y += 32;
        // the ROM
        if (variant) {
            c.round(x, y, w, 34, 8, colour::panel, colour::line);
            c.round(x + 12, y + 13, 8, 8, 4, colour::ok);
            const auto name = path_utf8(rom_path.filename());
            c.text(x + 28, y + 8, w - 40, 20, std::string(variant->name) + "  \xC2\xB7  " + name, 13, FW_NORMAL, colour::soft, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            c.round(x, y, w, 34, 8, colour::panel, colour::line);
            c.text(x + 14, y + 8, w - 28, 20, "No ROM chosen yet", 13, FW_NORMAL, colour::muted, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
        y += 48;
        for (const char* line : {"One or two players, chosen on the title screen",
                                 "Three lives each, one shared score",
                                 "A player who is hit is back in a moment, no black screen",
                                 "Out of lives? A, B, B, A, START takes one from your partner"}) {
            c.round(x + 2, y + 7, 5, 5, 2.5f, colour::accent);
            y += c.text(x + 16, y, w - 16, 40, line, 13.5f, FW_NORMAL, colour::soft) + 5;
        }
        y += 14;
        c.text(x, y, w, 16, "OPTIONS", 11, FW_BOLD, colour::muted);
        y += 22;
        c.text(x, y + 2, 260, 22, "Friendly fire", 14.5f, FW_NORMAL, colour::text);
        c.text(x + 110, y + 4, 240, 20, "shots hit the other player", 12.5f, FW_NORMAL, colour::muted);
        toggle(c, fire_switch, x + w - 44, y, options.friendly_fire);
        y += 38;
        // player 2's palette: three chips with the palette's colours
        c.text(x, y, w, 22, "Player 2's colours", 14.5f, FW_NORMAL, colour::text);
        y += 26;
        const bool chips_focused = focus == palette_choice && enabled(palette_choice);
        for (int k = 1; k <= 3; ++k) {
            const float cx = x + static_cast<float>(k - 1) * 100;
            const bool picked = options.palette == k;
            c.round(cx, y, 92, 34, 8, picked ? colour::raised : colour::panel,
                    picked ? std::optional<std::uint32_t>(chips_focused ? colour::accent_hi : colour::accent) : std::optional<std::uint32_t>(colour::line), picked ? 2.f : 1.f);
            const auto& cols = (picked && options.own) ? options.colours : game_colours[static_cast<std::size_t>(k)];
            for (int i = 0; i < 3; ++i) swatch(c, cx + 10 + static_cast<float>(i) * 18, y + 9, 16, cols[static_cast<std::size_t>(i)]);
            c.text(cx + 66, y + 7, 22, 20, std::to_string(k), 14, FW_SEMIBOLD, picked ? colour::text : colour::muted, DT_LEFT | DT_TOP | DT_SINGLELINE);
            hits.push_back({palette_choice, cx, y, 92, 34, k});
        }
        y += 40;
        c.text(x, y, w, 18, std::string("Shared with ") + palette_users[static_cast<std::size_t>(options.palette)] + (options.own ? ": they change too" : ""), 12.5f, FW_NORMAL, colour::muted);
        y += 22;
        c.text(x, y + 2, 260, 22, "Own colours", 14.5f, FW_NORMAL, colour::text);
        toggle(c, own_switch, x + w - 44, y, options.own);
        if (options.own) {
            // the three colours, each with arrows
            for (int i = 0; i < 3; ++i) {
                const Control id = static_cast<Control>(colour1 + i);
                const float cx = x + 110 + static_cast<float>(i) * 76;
                const bool focused = focus == id;
                c.text(cx, y + 2, 16, 22, "\xE2\x80\xB9", 18, FW_NORMAL, focused ? colour::accent_hi : colour::muted, DT_CENTER | DT_TOP | DT_SINGLELINE);
                swatch(c, cx + 18, y + 1, 22, options.colours[static_cast<std::size_t>(i)]);
                if (focused) {
                    c.round(cx + 16, y - 1, 26, 26, 5, colour::accent);
                    swatch(c, cx + 18, y + 1, 22, options.colours[static_cast<std::size_t>(i)]);
                }
                c.text(cx + 42, y + 2, 16, 22, "\xE2\x80\xBA", 18, FW_NORMAL, focused ? colour::accent_hi : colour::muted, DT_CENTER | DT_TOP | DT_SINGLELINE);
                hits.push_back({id, cx, y - 2, 20, 28, -1});
                hits.push_back({id, cx + 40, y - 2, 20, 28, +1});
                hits.push_back({id, cx + 18, y - 2, 22, 28, +1});
            }
        }
        // the buttons and the status
        const float by = H - 96;
        button(c, choose_button, x, by, 120, "Choose ROM", false);
        button(c, patch_button, x + 130, by, 140, "Patch ROM\xE2\x80\xA6", true);
        button(c, states_button, x + 280, by, 124, "Mesen states", false);
        c.text(x, by + 52, w, 34, status, 13, FW_NORMAL, error ? colour::bad : busy ? colour::accent_hi : colour::soft, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    }
    const Hit* hit(float x, float y) const {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)
            if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return &*it;
        return nullptr;
    }
};

// The window as a BMP (the --preview test and the docs).
void save_bmp(const std::filesystem::path& path, const Canvas& c) {
    std::ofstream bmp(path, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) bmp.put(static_cast<char>(v >> (8 * i))); };
    const auto u16 = [&](std::uint16_t v) { bmp.put(static_cast<char>(v)); bmp.put(static_cast<char>(v >> 8)); };
    const auto size = static_cast<std::uint32_t>(c.w * c.h * 4);
    bmp.write("BM", 2); u32(54 + size); u32(0); u32(54); u32(40); u32(static_cast<std::uint32_t>(c.w)); u32(static_cast<std::uint32_t>(-c.h)); u16(1); u16(32); u32(0); u32(size); u32(0); u32(0); u32(0); u32(0);
    bmp.write(reinterpret_cast<const char*>(c.bits), static_cast<std::streamsize>(size));
}

// Results of SDL's file dialogs (they may answer on another thread) and of the debug states' run (its own thread).
struct Pending { std::mutex lock; std::string open, save, mesen; bool making = false, made = false, made_error = false; std::string made_status; };
void SDLCALL opened(void* user, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto& p = *static_cast<Pending*>(user); std::lock_guard g(p.lock); p.open = files[0];
}
void SDLCALL saved(void* user, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto& p = *static_cast<Pending*>(user); std::lock_guard g(p.lock); p.save = files[0];
}
void SDLCALL chose_mesen(void* user, const char* const* files, int) {
    if (!files || !files[0]) return;
    auto& p = *static_cast<Pending*>(user); std::lock_guard g(p.lock); p.mesen = files[0];
}

int run_window(int argc, char** argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
    SDL_Window* window = SDL_CreateWindow("Gun.Smoke Co-op Patcher", static_cast<int>(App::W), static_cast<int>(App::H), SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) throw std::runtime_error(SDL_GetError());
    App app; Pending pending;
    if (argc > 1) app.choose(utf8_path(argv[1]));
    const SDL_DialogFileFilter filters[] = {{"NES ROM", "nes"}, {"All files", "*"}};
    const SDL_DialogFileFilter programs[] = {{"Mesen", "exe"}};
    SDL_Texture* texture = nullptr;
    int tw = 0, th = 0;
    std::unique_ptr<Canvas> canvas;
    const auto activate = [&](int control, int part) {
        if (!app.enabled(control)) return;
        app.focus = control;
        switch (control) {
        case choose_button: SDL_ShowOpenFileDialog(opened, &pending, window, filters, 2, nullptr, false); break;
        case patch_button: {
            const auto suggested = app.rom_path.parent_path() / (app.rom_path.stem().wstring() + L" (Co-op).nes");
            const auto location = path_utf8(suggested);
            SDL_ShowSaveFileDialog(saved, &pending, window, filters, 1, location.c_str());
            break;
        }
        case states_button: SDL_ShowOpenFileDialog(chose_mesen, &pending, window, programs, 1, nullptr, false); break;
        case palette_choice:
            if (part >= 1 && part <= 3) { app.options.palette = part; if (!app.options.own) app.options.colours = game_colours[static_cast<std::size_t>(part)]; app.redraw_picture(); }
            else app.change(control, 1);
            break;
        case colour1: case colour2: case colour3: app.change(control, part ? part : 1); break;
        default: app.change(control, 1); break;
        }
    };
    bool running = true;
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) app.choose(utf8_path(e.drop.data));
            else if (e.type == SDL_EVENT_MOUSE_MOTION) { const auto* h = app.hit(e.motion.x, e.motion.y); app.hover = h ? h->control : -1; }
            else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                if (const auto* h = app.hit(e.button.x, e.button.y)) { const Hit copy = *h; activate(copy.control, copy.part); }
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                const auto k = e.key.key;
                if (k == SDLK_ESCAPE) running = false;
                else if (k == SDLK_DOWN || (k == SDLK_TAB && !(e.key.mod & SDL_KMOD_SHIFT))) app.move_focus(1);
                else if (k == SDLK_UP || k == SDLK_TAB) app.move_focus(-1);
                else if (k == SDLK_LEFT || k == SDLK_RIGHT) { if (app.enabled(app.focus) && app.focus != choose_button && app.focus != patch_button && app.focus != states_button) app.change(app.focus, k == SDLK_LEFT ? -1 : 1); }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) activate(app.focus, 0);
            }
        }
        {
            std::lock_guard g(pending.lock);
            if (!pending.open.empty()) { app.choose(utf8_path(pending.open)); pending.open.clear(); }
            if (!pending.save.empty()) {
                auto path = utf8_path(pending.save);
                if (path.extension().empty()) path += ".nes";
                app.save(path); pending.save.clear();
            }
            if (!pending.mesen.empty() && !pending.making) {
                pending.making = true; app.busy = true;
                app.status = "Making the states in Mesen (a minute or two)..."; app.error = false;
                std::thread([&pending, mesen = utf8_path(pending.mesen), rom = app.saved_path] {
                    std::string status; bool failed = false;
                    try { status = make_states(mesen, rom); } catch (const std::exception& ex) { status = ex.what(); failed = true; }
                    std::lock_guard g2(pending.lock);
                    pending.made_status = status; pending.made_error = failed; pending.made = true; pending.making = false;
                }).detach();
                pending.mesen.clear();
            }
            if (pending.made) { app.status = pending.made_status; app.error = pending.made_error; app.busy = false; pending.made = false; }
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        if (!canvas || pw != tw || ph != th) {
            if (texture) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);
            if (!texture) throw std::runtime_error(SDL_GetError());
            canvas = std::make_unique<Canvas>(pw, ph, static_cast<float>(pw) / App::W);
            tw = pw; th = ph;
        }
        app.draw(*canvas);
        GdiFlush();
        SDL_UpdateTexture(texture, nullptr, canvas->bits, pw * 4);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }
    canvas.reset();
    if (texture) SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
}

int main(int argc, char** argv) {
    try {
        if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--verify") { // test: the same ROM as tools/gunsmoke-coop/build.mjs
            Options o;
            if (argc >= 5) o.friendly_fire = argv[4][0] == '1';
            auto c = checked(read_file(utf8_path(argv[2])));
            return patch(c.rom, *c.variant, o) == read_file(utf8_path(argv[3])) ? 0 : 2;
        }
        if (argc == 4 && std::string(argv[1]) == "--states") { // the debug states without the window
            std::printf("%s\n", make_states(utf8_path(argv[2]), utf8_path(argv[3])).c_str());
            return 0;
        }
        if ((argc == 3 || argc == 4) && std::string(argv[1]) == "--preview") { // the window as a picture [with a ROM]
            App app;
            if (argc == 4) app.choose(utf8_path(argv[3]));
            Canvas canvas(static_cast<int>(App::W), static_cast<int>(App::H), 1.f);
            app.draw(canvas);
            GdiFlush();
            save_bmp(utf8_path(argv[2]), canvas);
            return 0;
        }
        return run_window(argc, argv);
    } catch (const std::exception& e) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Gun.Smoke Co-op Patcher", e.what(), nullptr);
        return 1;
    }
}
