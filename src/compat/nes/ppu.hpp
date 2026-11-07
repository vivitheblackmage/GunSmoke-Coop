#pragma once
#include <array>
#include <cstdint>
#include <vector>

// NES picture from PPU memory, for ports that rebuild the original screens natively (general NES compatibility
// layer, no game code). The background from the nametables at a scroll, then the sprites: 8x8 or 8x16, flips,
// behind-background priority, the first 8 per line in OAM order, the left 8 pixels' masks, a line below their OAM
// Y. Scroll changes during the picture come as a list of splits. Checked against Mesen 2 (docs/ppu-reference.md).
// No grayscale or emphasis bits, no sprite overflow or sprite 0 hit (the port knows its own splits).
namespace rg::nes {
enum class Mirroring : std::uint8_t { horizontal, vertical, single_low, single_high };

struct PpuState {
    std::array<std::uint8_t, 0x2000> chr{};   // pattern tables $0000-$1FFF (CHR RAM or ROM)
    std::array<std::uint8_t, 0x800> ciram{};  // the two nametables of console RAM
    std::array<std::uint8_t, 32> palette{};   // $3F00-$3F1F (6-bit colour numbers)
    std::array<std::uint8_t, 256> oam{};
    std::uint8_t ctrl = 0, mask = 0;          // $2000, $2001
    std::uint16_t scroll_x = 0, scroll_y = 0; // 0-511 / 0-479: fine scroll with the nametable select of $2000
    Mirroring mirroring = Mirroring::horizontal;
};

// From line `line` on, the background uses this scroll and $2000 (a split written during the picture).
struct Split { unsigned line; std::uint16_t scroll_x, scroll_y; std::uint8_t ctrl; };

// 256 x 240 colour numbers (0-63): what the PPU outputs before the palette's RGB.
std::vector<std::uint8_t> render_indices(const PpuState& state, const std::vector<Split>& splits = {});
// 256 x 240 RGBA8 in the platform's NES palette.
std::vector<std::uint8_t> render(const PpuState& state, const std::vector<Split>& splits = {});
// The platform's NES palette (RGB of colour numbers 0-63; a common 2C02 approximation).
std::uint32_t rgb(std::uint8_t colour) noexcept;
// Scroll of a PPU's t register and fine X (as $2005/$2006 leave them): x 0-511, y 0-479.
void scroll_from_t(std::uint16_t t, std::uint8_t fine_x, std::uint16_t& x, std::uint16_t& y) noexcept;
}
