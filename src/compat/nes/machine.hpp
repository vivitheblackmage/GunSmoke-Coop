#pragma once
#include "ppu.hpp"
#include <array>
#include <cstdint>
#include <vector>

// The NES as natively ported routines see it (general NES compatibility layer, no game code): 2 KiB RAM, the PPU's
// registers and memories ($2000-$2007 with the t/v/x/w latches and the read buffer, OAM DMA $4014), the pads
// ($4016/$4017) and the cartridge's PRG through UxROM (mapper 2: $8000-$BFFF the selected 16 KiB bank, $C000-$FFFF
// the last; a write to $8000-$FFFF selects, ANDed with the ROM byte there). A port writes the registers as the
// original does and reads its tables through prg_read; rg::nes::render draws `ppu`. Same rules as
// tools/nes/machine.mjs (the oracles' machine). No CPU: the port is the code; the APU's register writes are logged.
namespace rg::nes {
class Machine {
public:
    explicit Machine(std::vector<std::uint8_t> prg = {});
    std::array<std::uint8_t, 0x800> ram{};
    PpuState ppu{};
    std::uint16_t v = 0, t = 0;
    std::uint8_t fine_x = 0, read_buffer = 0, oam_address = 0;
    bool w = false;
    std::array<std::uint8_t, 2> pads{}; // A B Select Start Up Down Left Right, bit 0 first
    unsigned bank = 0;
    // The APU registers written ($4000-$4013, $4015, $4017: register << 8 | value), in order, for a host's APU to
    // play and an oracle to compare; the owner clears it.
    std::vector<std::uint16_t> apu_writes;

    std::uint8_t& operator[](std::uint16_t address) noexcept { return ram[address & 0x7ff]; }
    std::uint8_t read(std::uint16_t address);
    void write(std::uint16_t address, std::uint8_t value);
    // A byte of the cartridge at a CPU address under the current bank.
    std::uint8_t prg_read(std::uint16_t address) const noexcept;
    std::uint16_t prg_word(std::uint16_t address) const noexcept {
        return static_cast<std::uint16_t>(prg_read(address) | prg_read(static_cast<std::uint16_t>(address + 1)) << 8);
    }
    // The picture's scroll from t and fine X into ppu.scroll_x/scroll_y (call before rendering).
    void latch_scroll() noexcept { scroll_from_t(t, fine_x, ppu.scroll_x, ppu.scroll_y); }
    const std::vector<std::uint8_t>& prg() const noexcept { return prg_; }
private:
    std::vector<std::uint8_t> prg_;
    unsigned banks_ = 1;
    std::array<std::uint8_t, 2> shift_{};
    bool strobe_ = false;
    std::uint8_t& ppu_memory(std::uint16_t address) noexcept;
};
}
