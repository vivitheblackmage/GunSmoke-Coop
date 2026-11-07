#include "ppu.hpp"

namespace rg::nes {
namespace {
constexpr std::uint32_t palette_rgb[64] = {
    0x7c7c7c, 0x0000fc, 0x0000bc, 0x4428bc, 0x940084, 0xa80020, 0xa81000, 0x881400, 0x503000, 0x007800, 0x006800, 0x005800, 0x004058, 0x000000, 0x000000, 0x000000,
    0xbcbcbc, 0x0078f8, 0x0058f8, 0x6844fc, 0xd800cc, 0xe40058, 0xf83800, 0xe45c10, 0xac7c00, 0x00b800, 0x00a800, 0x00a844, 0x008888, 0x000000, 0x000000, 0x000000,
    0xf8f8f8, 0x3cbcfc, 0x6888fc, 0x9878f8, 0xf878f8, 0xf85898, 0xf87858, 0xfca044, 0xf8b800, 0xb8f818, 0x58d854, 0x58f898, 0x00e8d8, 0x787878, 0x000000, 0x000000,
    0xfcfcfc, 0xa4e4fc, 0xb8b8f8, 0xd8b8f8, 0xf8b8f8, 0xf8a4c0, 0xf0d0b0, 0xfce0a8, 0xf8d878, 0xd8f878, 0xb8f8b8, 0xb8f8d8, 0x00fcfc, 0xf8d8f8, 0x000000, 0x000000};

// Nametable byte at a PPU address in $2000-$2FFF under the cartridge's mirroring.
std::uint8_t nametable(const PpuState& s, unsigned address) noexcept {
    const unsigned n = (address >> 10) & 3, offset = address & 0x3ff;
    unsigned page = 0;
    switch (s.mirroring) {
        case Mirroring::horizontal: page = n >> 1; break;
        case Mirroring::vertical: page = n & 1; break;
        case Mirroring::single_low: page = 0; break;
        case Mirroring::single_high: page = 1; break;
    }
    return s.ciram[page * 0x400 + offset];
}
unsigned pixel(const PpuState& s, unsigned base, unsigned tile, unsigned x, unsigned y) noexcept {
    const unsigned o = (base + tile * 16 + y) & 0x1fff;
    return ((s.chr[o] >> (7 - x)) & 1) | (((s.chr[(o + 8) & 0x1fff] >> (7 - x)) & 1) << 1);
}
std::uint8_t colour(const PpuState& s, unsigned index) noexcept {
    // $3F10/$3F14/$3F18/$3F1C mirror $3F00/$3F04/$3F08/$3F0C; colour 0 of any palette is the backdrop $3F00.
    if ((index & 3) == 0) index = 0;
    return s.palette[index & 31] & 0x3f;
}
}

void scroll_from_t(std::uint16_t t, std::uint8_t fine_x, std::uint16_t& x, std::uint16_t& y) noexcept {
    x = static_cast<std::uint16_t>(((t & 0x1f) << 3) | (fine_x & 7) | (((t >> 10) & 1) << 8));
    y = static_cast<std::uint16_t>((((t >> 5) & 0x1f) << 3) | ((t >> 12) & 7)) ;
    y = static_cast<std::uint16_t>(y + ((t >> 11) & 1) * 240);
}

std::vector<std::uint8_t> render_indices(const PpuState& s, const std::vector<Split>& splits) {
    std::vector<std::uint8_t> out(256 * 240);
    std::vector<std::uint8_t> opaque(256 * 240, 0);
    std::uint16_t sx = s.scroll_x, sy = s.scroll_y; std::uint8_t ctrl = s.ctrl;
    std::size_t next = 0;
    for (unsigned y = 0; y < 240; ++y) {
        while (next < splits.size() && splits[next].line <= y) { sx = splits[next].scroll_x; sy = splits[next].scroll_y; ctrl = splits[next].ctrl; ++next; }
        const unsigned bg = ctrl & 0x10 ? 0x1000 : 0;
        for (unsigned x = 0; x < 256; ++x) {
            unsigned index = 0;
            if ((s.mask & 0x08) && (x >= 8 || (s.mask & 0x02))) {
                unsigned px = (x + sx) % 512, py = (y + sy) % 480;
                const unsigned nt = (px >= 256 ? 1u : 0u) + (py >= 240 ? 2u : 0u);
                px &= 255; py %= 240;
                const unsigned base = 0x2000 + nt * 0x400;
                const unsigned tile = nametable(s, base + (py >> 3) * 32 + (px >> 3));
                const unsigned attr = nametable(s, base + 0x3c0 + (py >> 5) * 8 + (px >> 5));
                const unsigned shift = ((py >> 4) & 1) * 4 + ((px >> 4) & 1) * 2;
                const unsigned c = pixel(s, bg, tile, px & 7, py & 7);
                if (c) { index = ((attr >> shift) & 3) * 4 + c; opaque[y * 256 + x] = 1; }
            }
            out[y * 256 + x] = colour(s, index);
        }
    }
    if (s.mask & 0x10) {
        const bool tall = s.ctrl & 0x20;
        const unsigned height = tall ? 16 : 8, base8 = s.ctrl & 0x08 ? 0x1000 : 0;
        // Per line the first 8 sprites in OAM order; among overlapping sprites the lower OAM index wins.
        std::vector<std::uint8_t> drawn(256 * 240, 0);
        for (unsigned y = 0; y < 240; ++y) {
            unsigned count = 0;
            for (unsigned i = 0; i < 64 && count < 8; ++i) {
                const unsigned top = s.oam[i * 4] + 1u;
                if (y < top || y >= top + height) continue;
                ++count;
                const unsigned tile_byte = s.oam[i * 4 + 1], attr = s.oam[i * 4 + 2], left = s.oam[i * 4 + 3];
                const unsigned row = attr & 0x80 ? height - 1 - (y - top) : y - top;
                unsigned base = base8, tile = tile_byte;
                if (tall) { base = tile_byte & 1 ? 0x1000 : 0; tile = (tile_byte & 0xfe) + (row >= 8 ? 1 : 0); }
                for (unsigned col = 0; col < 8; ++col) {
                    const unsigned x = left + col;
                    if (x > 255 || drawn[y * 256 + x] || (x < 8 && !(s.mask & 0x04))) continue;
                    const unsigned c = pixel(s, base, tile, attr & 0x40 ? 7 - col : col, row & 7);
                    if (!c) continue;
                    drawn[y * 256 + x] = 1; // a lower-numbered sprite's opaque pixel hides the others, even behind
                    if ((attr & 0x20) && opaque[y * 256 + x]) continue;
                    out[y * 256 + x] = colour(s, 0x10 + (attr & 3) * 4 + c);
                }
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> render(const PpuState& s, const std::vector<Split>& splits) {
    const auto indices = render_indices(s, splits);
    std::vector<std::uint8_t> rgba(256 * 240 * 4);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto c = palette_rgb[indices[i] & 63];
        rgba[i * 4] = static_cast<std::uint8_t>(c >> 16); rgba[i * 4 + 1] = static_cast<std::uint8_t>(c >> 8);
        rgba[i * 4 + 2] = static_cast<std::uint8_t>(c); rgba[i * 4 + 3] = 255;
    }
    return rgba;
}

std::uint32_t rgb(std::uint8_t c) noexcept { return palette_rgb[c & 63]; }
}
