#include "machine.hpp"

namespace rg::nes {
Machine::Machine(std::vector<std::uint8_t> prg) : prg_(std::move(prg)) {
    banks_ = prg_.size() >= 0x4000 ? static_cast<unsigned>(prg_.size() / 0x4000) : 1;
}
std::uint8_t Machine::prg_read(std::uint16_t address) const noexcept {
    if (address < 0x8000 || prg_.empty()) return 0;
    const std::size_t offset = address >= 0xc000 ? (banks_ - 1) * 0x4000 + (address - 0xc000) : bank * 0x4000 + (address - 0x8000);
    return offset < prg_.size() ? prg_[offset] : 0;
}
std::uint8_t& Machine::ppu_memory(std::uint16_t address) noexcept {
    address &= 0x3fff;
    if (address < 0x2000) return ppu.chr[address];
    if (address >= 0x3f00) {
        unsigned p = address & 0x1f;
        if ((p & 0x13) == 0x10) p &= 0x0f;
        return ppu.palette[p];
    }
    const unsigned n = (address >> 10) & 3, offset = address & 0x3ff;
    unsigned page = 0;
    switch (ppu.mirroring) {
        case Mirroring::horizontal: page = n >> 1; break;
        case Mirroring::vertical: page = n & 1; break;
        case Mirroring::single_low: page = 0; break;
        case Mirroring::single_high: page = 1; break;
    }
    return ppu.ciram[page * 0x400 + offset];
}
std::uint8_t Machine::read(std::uint16_t address) {
    if (address < 0x2000) return ram[address & 0x7ff];
    if (address < 0x4000) {
        switch (address & 7) {
            case 2: w = false; return 0; // the port does not wait on vblank or sprite 0
            case 4: return ppu.oam[oam_address];
            case 7: {
                const std::uint16_t a = v & 0x3fff;
                std::uint8_t value;
                if (a >= 0x3f00) { value = ppu_memory(a); read_buffer = ppu_memory(static_cast<std::uint16_t>(a - 0x1000)); }
                else { value = read_buffer; read_buffer = ppu_memory(a); }
                v = static_cast<std::uint16_t>((v + (ppu.ctrl & 4 ? 32 : 1)) & 0x7fff);
                return value;
            }
            default: return 0;
        }
    }
    if (address == 0x4016 || address == 0x4017) {
        const unsigned p = address & 1;
        const unsigned bit = strobe_ ? pads[p] & 1 : shift_[p] & 1;
        if (!strobe_) shift_[p] = static_cast<std::uint8_t>((shift_[p] >> 1) | 0x80);
        return static_cast<std::uint8_t>(0x40 | bit);
    }
    return prg_read(address);
}
void Machine::write(std::uint16_t address, std::uint8_t value) {
    if (address < 0x2000) { ram[address & 0x7ff] = value; return; }
    if (address < 0x4000) {
        switch (address & 7) {
            case 0: ppu.ctrl = value; t = static_cast<std::uint16_t>((t & ~0x0c00) | ((value & 3) << 10)); break;
            case 1: ppu.mask = value; break;
            case 3: oam_address = value; break;
            case 4: ppu.oam[oam_address++] = value; break;
            case 5:
                if (!w) { t = static_cast<std::uint16_t>((t & ~0x1f) | (value >> 3)); fine_x = value & 7; w = true; }
                else { t = static_cast<std::uint16_t>((t & ~0x73e0) | ((value & 7) << 12) | ((value & 0xf8) << 2)); w = false; }
                break;
            case 6:
                if (!w) { t = static_cast<std::uint16_t>((t & 0xff) | ((value & 0x3f) << 8)); w = true; }
                else { t = static_cast<std::uint16_t>((t & 0x7f00) | value); v = t; w = false; }
                break;
            case 7: ppu_memory(v) = value; v = static_cast<std::uint16_t>((v + (ppu.ctrl & 4 ? 32 : 1)) & 0x7fff); break;
        }
        return;
    }
    if (address == 0x4014) {
        for (unsigned i = 0; i < 256; ++i) ppu.oam[(oam_address + i) & 0xff] = ram[((value << 8) | i) & 0x7ff];
        return;
    }
    if (address == 0x4016) { strobe_ = value & 1; if (strobe_) shift_ = pads; return; }
    if (address <= 0x4017) { apu_writes.push_back(static_cast<std::uint16_t>((address - 0x4000) << 8 | value)); return; } // the APU: logged
    if (address < 0x8000) return;
    bank = static_cast<unsigned>((value & prg_read(address)) % banks_);
}
}
