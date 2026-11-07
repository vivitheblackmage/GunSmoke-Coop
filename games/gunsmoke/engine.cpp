#include "engine.hpp"
#include <cstdio>

namespace gunsmoke {
namespace {
std::string hex4(unsigned v) { char b[8]; std::snprintf(b, sizeof b, "$%04X", v & 0xffff); return b; }
}
Unported::Unported(unsigned address) : std::runtime_error("unported original code at " + hex4(address)) {}

Engine::Engine(std::vector<std::uint8_t> prg) : m(std::move(prg)) { m.ppu.mirroring = rg::nes::Mirroring::horizontal; }

// ---- the main loop and the NMI ----

// $E651: the reset (after the vblank waits).
void Engine::power_on() {
    m.write(0x2000, 0x10);
    m.write(0x2001, 0x00);
    r(0xFE) = 0x00;
    for (unsigned x = 0xf0; x > 0; --x) r(static_cast<std::uint16_t>(x)) = 0;   // $01-$F0
    for (unsigned x = 8; x > 0; --x) r(static_cast<std::uint16_t>(0xdf + x)) = 0x88; // $E0-$E7: the sound queue empty
    r(0xE8) = 0; r(0xE9) = 0;
    r(0x24) = 0xE6; r(0x23) = 0xAD;
    start_task(0, 0xE6AD);
    r(0xFF) = 0x90;
}

// $C0B0/$C087: a = frames; the task's slot waits (state 1, count $11+4k) and the scan restarts.
void Engine::Wait::await_suspend(std::coroutine_handle<> h) noexcept {
    e.tasks_[e.current_].resume = h;
    e.r(0x23) = frames;
    const std::uint16_t x = static_cast<std::uint16_t>(e.current_ * 4);
    e.r(0x11 + x) = frames;
    e.r(0x10 + x) = 1;
}

// $C000-$C06E until no task is ready (the original then scans until the NMI).
void Engine::run_tasks(unsigned first_slot, unsigned stop_mask) {
    for (;;) {
        int found = -1;
        // The scan interrupted by the NMI goes on from first_slot to slot 3, then ($C01A) restarts at slot 0.
        for (unsigned k = first_slot; k < 4 && found < 0; ++k) if (r(static_cast<std::uint16_t>(0x10 + 4 * k)) >= 4) found = static_cast<int>(k);
        first_slot = 0;
        if (found < 0) {
            r(0x20) = 0;
            for (unsigned k = 0; k < 4; ++k) if (r(static_cast<std::uint16_t>(0x10 + 4 * k)) >= 4) { found = static_cast<int>(k); break; }
        }
        if (found < 0 || (stop_mask >> found & 1)) return;
        const auto k = static_cast<unsigned>(found);
        const std::uint16_t x = static_cast<std::uint16_t>(4 * k);
        current_ = k; r(0x21) = static_cast<std::uint8_t>(k);
        const std::uint8_t state = r(0x10 + x);
        r(0x10 + x) = 2;
        auto& task = tasks_[k];
        if (state == 8) { // start at $12/$13+4k (through $23/$24)
            r(0x23) = r(0x12 + x); r(0x24) = r(0x13 + x);
            task.owned.clear();
            task.owned.push_back(start(static_cast<std::uint16_t>(r(0x23) | r(0x24) << 8)));
            task.resume = task.owned.back().handle();
        } else if (k == 0) { // $C040: task 0 resumes after the sound engine and the pads
            bank_write(6);
            for (;;) {
                std::uint8_t xi = r(0xE9);
                const std::uint8_t a = r(static_cast<std::uint16_t>(0xE0 + xi));
                if (a == 0x88) break;
                r(static_cast<std::uint16_t>(0xE0 + xi)) = 0x88;
                xi = static_cast<std::uint8_t>((xi + 1) & 7);
                r(0xE9) = xi;
                sound_play(a);
            }
            sound_tick();
            bank_write(r(0x40));
            try { read_pads(); }
            catch (const RestartTitle& restart) { restart_title(k, restart.fade); continue; }
        }
        try {
            task.resume.resume();
            auto top = task.owned.back().handle();
            if (top.done() && top.promise().error) std::rethrow_exception(top.promise().error);
        } catch (const RestartTitle& restart) {
            restart_title(k, restart.fade);
        }
        auto top = task.owned.back().handle();
        if (top.done()) {
            if (top.promise().error) std::rethrow_exception(top.promise().error);
            throw std::runtime_error("task " + std::to_string(k) + " ended without leaving its slot");
        }
    }
}

// $E4C6.
void Engine::nmi(int busy) {
    std::uint8_t a = static_cast<std::uint8_t>((r(0xFF) & 0x7D) | r(0xF0));
    r(0xFF) = a; m.write(0x2000, a);
    m.write(0x2001, 0); m.write(0x2003, 0); m.write(0x4014, 0x02); her_drawn_ = her_ram_;
    if (std::uint8_t x = r(0x38)) { // $E4EA: a row from $0300 (32 bytes, 64 when $38 = $FF)
        r(0x38) = 0; m.read(0x2002);
        m.write(0x2006, r(0x300)); std::uint8_t y = 0x20; m.write(0x2006, r(0x301));
        x = static_cast<std::uint8_t>(x + 1); if (x == 0) y = 0x40;
        for (x = 0; y; --y, ++x) m.write(0x2007, r(static_cast<std::uint16_t>(0x302 + x)));
    }
    if (r(0x39)) { // $E514
        r(0x39) = 0; m.read(0x2002);
        m.write(0x2006, r(0x342)); m.write(0x2006, r(0x343));
        std::uint8_t y = r(0x344);
        for (std::uint8_t x = 0; ; ++x) { m.write(0x2007, r(static_cast<std::uint16_t>(0x345 + x))); if (--y == 0) break; }
    }
    if (r(0x3A)) { // $E538
        r(0x3A) = 0; m.read(0x2002);
        m.write(0x2006, r(0x36A)); m.write(0x2006, r(0x36B));
        std::uint8_t y = r(0x36C);
        for (std::uint8_t x = 0; ; ++x) { m.write(0x2007, r(static_cast<std::uint16_t>(0x36D + x))); if (--y == 0) break; }
    }
    if (r(0x27)) { vram_script(); r(0x27) = 0; } // $E558
    if (r(0x3B)) { // $E567: the palette from $03E0
        r(0x3B) = 0; m.read(0x2002);
        m.write(0x2006, 0x3F); m.write(0x2006, 0x00);
        for (unsigned x = 0; x < 0x20; ++x) m.write(0x2007, r(static_cast<std::uint16_t>(0x3E0 + x)));
        m.write(0x2006, 0x3F); m.write(0x2006, 0); m.write(0x2006, 0); m.write(0x2006, 0);
    }
    if (r(0x3C)) { // $E594: a tile between $06B0 + ($2D & $30) and CHR $2A/$2B (the stage's events)
        r(0x00) = static_cast<std::uint8_t>((r(0x2D) & 0x30) + 0xB0); r(0x01) = 0x06;
        r(0x04) = r(0x2B); r(0x02) = 1; r(0x03) = r(0x2A);
        r(0x3C) = 0;
        upload_tiles();
    }
    if (r(0x3D)) { // $E5B7: two window rows of 24 tiles from $01C0 at $59/$58 + 4 and the row below
        r(0x3D) = 0; m.read(0x2002);
        m.write(0x2006, r(0x59));
        const unsigned s = r(0x58) + 4u;
        m.write(0x2006, static_cast<std::uint8_t>(s));
        r(0xEE) = static_cast<std::uint8_t>((s & 0xFF) + 0x20 + (s >> 8));
        for (unsigned x = 0; x < 0x18; ++x) m.write(0x2007, r(static_cast<std::uint16_t>(0x1C0 + x)));
        m.write(0x2006, r(0x59)); m.write(0x2006, r(0xEE));
        for (unsigned x = 0x18; x < 0x30; ++x) m.write(0x2007, r(static_cast<std::uint16_t>(0x1C0 + x)));
    }
    if (r(0x3E) && r(0xAA) == 0) { // $E5E7: a tile a frame from ($28) to CHR $2A/$2B (the boss's graphics)
        --r(0x3E);
        r(0x01) = r(0x29); r(0x00) = r(0x28);
        unsigned s = r(0x28) + 0x10u;
        r(0x28) = static_cast<std::uint8_t>(s); r(0x29) = static_cast<std::uint8_t>(r(0x29) + (s >> 8));
        r(0x04) = r(0x2B);
        const std::uint8_t hi = r(0x2A);
        s = r(0x2B) + 0x10u;
        r(0x2B) = static_cast<std::uint8_t>(s); r(0x2A) = static_cast<std::uint8_t>(r(0x2A) + (s >> 8));
        r(0x02) = 1; r(0x03) = hi;
        upload_tiles();
    }
    m.read(0x2002); // $E616
    m.write(0x2005, r(0xFD)); m.write(0x2005, r(0xFC));
    ++r(0x22);
    std::uint8_t mask = r(0x25);
    for (int x = 0x0C; x >= 0; x -= 4) { // the waiting tasks' counts (a set bit of $25 holds a task)
        const bool hold = mask & 0x80; mask = static_cast<std::uint8_t>(mask << 1);
        if (hold || x == busy * 4) continue; // a task the NMI interrupted (a lag frame) is running, not waiting
        auto& state = r(static_cast<std::uint16_t>(0x10 + x));
        if (state == 1 && --r(static_cast<std::uint16_t>(0x11 + x)) == 0) state = 4;
    }
    m.write(0x2001, r(0xFE));
    nmi_on();
}

// ---- bank 7 ----

void Engine::start_task(std::uint8_t task, std::uint16_t entry) { // $C06F (entry in $23/$24)
    const std::uint16_t x = static_cast<std::uint16_t>(task * 4);
    r(0x23) = static_cast<std::uint8_t>(entry); r(0x24) = static_cast<std::uint8_t>(entry >> 8);
    r(0x12 + x) = r(0x23); r(0x13 + x) = r(0x24); r(0x10 + x) = 8;
}
void Engine::bank_write(std::uint8_t bank) { m.write(static_cast<std::uint16_t>(0xFFD9 + bank), bank); } // $C363
void Engine::select_bank(std::uint8_t bank) { r(0x40) = bank; bank_write(bank); }                        // $C361
void Engine::sound_command(std::uint8_t a, std::uint8_t x_register) { // $C36A: into the queue $E0-$E7 (dropped when full); X kept in $F3
    r(0xF3) = x_register;
    if (r(0x68) && a < 0x15) return; // $C36C: the demonstration plays only the effects
    const std::uint8_t x = r(0xE8);
    r(0x4D) = a;
    if (r(static_cast<std::uint16_t>(0xE0 + x)) != 0x88) return;
    r(static_cast<std::uint16_t>(0xE0 + x)) = r(0x4D);
    r(0xE8) = static_cast<std::uint8_t>((x + 1) & 7);
}
void Engine::read_pads() { // $C15A: $F5 = new presses, $F7 = held (A B Select Start Up Down Left Right, bit 7 first)
    if (coop) coop_other_pad();
    if (coop && ram_owner_) std::swap(m.pads[0], m.pads[1]);
    m.write(0x4016, 1); m.write(0x4016, 0);
    if (coop && ram_owner_) std::swap(m.pads[0], m.pads[1]);
    for (int i = 0; i < 8; ++i) {
        const std::uint8_t a = m.read(0x4016);
        r(0xF5) = static_cast<std::uint8_t>((r(0xF5) << 1) | (a & 1));
        r(0x31) = static_cast<std::uint8_t>((r(0x31) << 1) | ((a >> 1) & 1));
    }
    const std::uint8_t held = static_cast<std::uint8_t>(r(0x31) | r(0xF5));
    r(0xF5) = static_cast<std::uint8_t>((held ^ r(0xF7)) & held);
    r(0xF7) = held;
    if (r(0x68)) {
        if (r(0xF5) & 0x10) { select_bank(1); throw RestartTitle{false}; } // START ends the demonstration
        if (r(0x69)) { // $C19C: the recorded pads ($0780 pairs) play
            r(0xF5) &= 0xDF;
            std::uint8_t x = r(0x6A);
            r(0xF7) = r(static_cast<std::uint16_t>(0x780 + x));
            if (r(0x6B) == 0) r(0x6B) = r(static_cast<std::uint16_t>(0x781 + x));
            if (--r(0x6B) != 0) return;
            x = static_cast<std::uint8_t>(x + 2);
            r(0x6A) = x;
            if (!(x & 0x80)) return;
            throw RestartTitle{true}; // the record's end
        }
        return;
    }
    if (r(0x41)) return;
    if (r(0x69) == 0) return;
    // $C1DB: stage 1's pads recorded for the demonstration at $0780 (pairs: pad, frames up to $FF) while $69.
    std::uint8_t x = r(0x6A);
    const std::uint8_t a = r(0xF7);
    if (r(0x781) != 0) {
        if (a == r(static_cast<std::uint16_t>(0x780 + x))) {
            const std::uint8_t n = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x781 + x)) + 1);
            if (n != 0) { r(static_cast<std::uint16_t>(0x781 + x)) = n; return; }
        }
        x = static_cast<std::uint8_t>(x + 2);
        r(0x6A) = x;
        if ((x & 0x80) && --r(0x69) == 0) return; // the record is full
    }
    r(static_cast<std::uint16_t>(0x780 + x)) = a;
    r(static_cast<std::uint16_t>(0x781 + x)) = 1;
}
void Engine::nmi_off() { r(0xFF) = r(0xFF) & 0x7F; m.write(0x2000, r(0xFF)); }                       // $C0BB
void Engine::nmi_on() { r(0xFF) = r(0xFF) | 0x80; m.write(0x2000, r(0xFF)); }                        // $C0C5
void Engine::rendering_off() { r(0xFE) = r(0xFE) & 0xE7; m.write(0x2001, r(0xFE)); }                 // $C0CF
void Engine::rendering_on() { r(0xFE) = r(0xFE) | 0x18; m.write(0x2001, r(0xFE)); }                  // $C0D9

// $C0DF: the VRAM script at ($26): address high, low, length (bit 7 down a column, bit 6 one byte repeated, bits 0-5
// the count, 0 = 64), the bytes; a byte with bit 7 ends it. A write to $3Fxx resets the PPU address after it.
void Engine::vram_script() {
    m.read(0x2002);
    const std::uint16_t p = static_cast<std::uint16_t>(r(0x26) | r(0x27) << 8);
    const auto rd = [&](std::uint8_t y) { return m.read(static_cast<std::uint16_t>(p + y)); };
    std::uint8_t y = 0, a = rd(y);
    for (;;) {
        const std::uint8_t high = a;
        m.write(0x2006, a); ++y; m.write(0x2006, rd(y)); ++y;
        a = rd(y);
        const bool down = a & 0x80; a = static_cast<std::uint8_t>(a << 1);
        std::uint8_t ctrl = static_cast<std::uint8_t>(r(0xFF) | 4); if (!down) ctrl &= 0xFB;
        m.write(0x2000, ctrl); r(0xFF) = ctrl;
        const bool repeat = a & 0x80; a = static_cast<std::uint8_t>(a << 1);
        const bool zero = a == 0;
        if (repeat) { a |= 2; ++y; }
        std::uint8_t carry = zero ? 1 : 0;
        a = static_cast<std::uint8_t>((a >> 1) | (carry << 7)); // ror (bit 0 out is 0)
        const bool same = a & 1; a = static_cast<std::uint8_t>(a >> 1);
        std::uint8_t x = a;
        do { if (!same) ++y; m.write(0x2007, rd(y)); } while (--x);
        if (high == 0x3F) { m.write(0x2006, 0x3F); m.write(0x2006, 0); m.write(0x2006, 0); m.write(0x2006, 0); }
        ++y; a = rd(y);
        if (a & 0x80) return;
    }
}
// $C134 in $C266 for a JSR at call_site: $05/$06 = call_site + 2 (the return address), $00/$01 = the 2 bytes after.
void Engine::inline_pointer(std::uint16_t call_site) {
    const std::uint16_t ret = static_cast<std::uint16_t>(call_site + 2);
    r(0x05) = static_cast<std::uint8_t>(ret); r(0x06) = static_cast<std::uint8_t>(ret >> 8);
    r(0x00) = m.read(static_cast<std::uint16_t>(ret + 1)); r(0x01) = m.read(static_cast<std::uint16_t>(ret + 2));
}
// $C266: x tiles of 16 bytes from the inline pointer (the current bank) to VRAM page y, low address a & $F0.
void Engine::copy_tiles(std::uint8_t a, std::uint8_t x, std::uint8_t y, std::uint16_t call_site) {
    r(0x04) = a; r(0x02) = x; r(0x03) = y;
    inline_pointer(call_site);
    upload_tiles();
}
// $C26F: VRAM address $03 / $04 & $F0; $04 bit 1: read the tiles from VRAM into ($00) (after the PPU's dummy read)
// instead of writing them; the first tile from offset ($04 & $0F) >> 2.
void Engine::upload_tiles() {
    m.read(0x2002);
    m.write(0x2006, r(0x03)); m.write(0x2006, static_cast<std::uint8_t>(r(0x04) & 0xF0));
    const std::uint8_t low = r(0x04) & 0x0F;
    const bool reading = low & 2;
    if (reading) m.read(0x2007);
    std::uint8_t index = static_cast<std::uint8_t>(low >> 2);
    do {
        const std::uint16_t source = static_cast<std::uint16_t>(r(0x00) | r(0x01) << 8);
        for (unsigned i = 0; i < 16; ++i) {
            const auto at = static_cast<std::uint16_t>(source + index + i);
            if (reading) m.write(at, m.read(0x2007)); else m.write(0x2007, m.read(at));
        }
        index = 0;
        const unsigned sum = r(0x00) + 0x10u;
        r(0x00) = static_cast<std::uint8_t>(sum); r(0x01) = static_cast<std::uint8_t>(r(0x01) + (sum >> 8));
    } while (--r(0x02));
}
// $C209: nametable page a ($20-$2F) filled with x, its attributes ($23C0 + ...) with y.
void Engine::fill_nametable(std::uint8_t a, std::uint8_t x, std::uint8_t y) {
    r(0x00) = a; r(0x01) = x; r(0x02) = y;
    m.read(0x2002);
    m.write(0x2006, r(0x00)); m.write(0x2006, 0);
    const unsigned pages = r(0x00) >= 0x20 ? 4 : r(0x02);
    for (unsigned i = 0; i < pages * 256; ++i) m.write(0x2007, r(0x01));
    if (r(0x00) >= 0x20) {
        m.write(0x2006, static_cast<std::uint8_t>(r(0x00) + 3)); m.write(0x2006, 0xC0);
        for (unsigned i = 0; i < 0x40; ++i) m.write(0x2007, r(0x02));
    }
}
// $C24E: pages y, y-1, ... (x - y + 1... until x wraps) filled with a: pages y down to x.
void Engine::fill_pages(std::uint8_t a, std::uint8_t x, std::uint8_t y) {
    r(0x01) = y;
    std::uint8_t count = static_cast<std::uint8_t>(x - y - 1);
    r(0x00) = 0;
    do {
        const std::uint16_t page = static_cast<std::uint16_t>(r(0x01) << 8);
        for (unsigned i = 0; i < 256; ++i) r(static_cast<std::uint16_t>(page + i)) = a;
        --r(0x01);
    } while (++count);
}
void Engine::hide_sprites() { for (unsigned x = 0; x < 256; x += 4) r(static_cast<std::uint16_t>(0x200 + x)) = 0xF8; her_ram_.fill(false); } // $C353

// $C41F: the title's tiles (banks 2, 3, 0) and nametable $2000 cleared to tile $F4, attributes $FF; bank 1.
void Engine::title_graphics() {
    nmi_off(); rendering_off();
    select_bank(2); copy_tiles(0x00, 0xC0, 0x00, 0xC42F);
    select_bank(3); copy_tiles(0x00, 0xC0, 0x10, 0xC43F);
    select_bank(0); copy_tiles(0x00, 0x40, 0x1C, 0xC44D);
    fill_nametable(0x20, 0xF4, 0xFF);
    select_bank(1);
}
// $C4E4: palette set $A3 ($C692/$C6AB pointers: first byte the start in $03E0, then colours until bit 7) and $3B.
std::uint8_t Engine::load_palette() {
    const std::uint8_t set = r(0xA3);
    r(0xF3) = m.read(static_cast<std::uint16_t>(0xC692 + set)); r(0xF4) = m.read(static_cast<std::uint16_t>(0xC6AB + set));
    const std::uint16_t p = static_cast<std::uint16_t>(r(0xF3) | r(0xF4) << 8);
    std::uint8_t y = 0, x = m.read(p);
    for (;;) {
        ++y;
        const std::uint8_t a = m.read(static_cast<std::uint16_t>(p + y));
        if (a & 0x80) break;
        r(static_cast<std::uint16_t>(0x3E0 + x)) = a;
        if (++x == 0) break;
    }
    r(0x3B) = y;
    palette_y_ = y;
    return x;
}
// $C945: the objects at $0400 (bit 7 active), every other frame in the other order.
void Engine::objects() {
    r(0xA1) = coop ? 0x30 : 0x18; // co-op: player 2's sprites at $0218-$022F
    ++r(0xA0);
    if (r(0xA0) & 1) {
        for (std::uint8_t x = 0x1F;; ) { r(0xA2) = x; if (r(static_cast<std::uint16_t>(0x400 + x)) & 0x80) draw_object(x);
            --r(0xA2); x = r(0xA2); if (x & 0x80) break; }
    } else {
        r(0xA2) = 0;
        for (std::uint8_t x = 0;; ) { if (r(static_cast<std::uint16_t>(0x400 + x)) & 0x80) draw_object(x);
            ++r(0xA2); x = r(0xA2); if (x == 0x20) break; }
    }
}
// $C978: object x into OAM from $0200 + $A1: its animation ($0420: $DB8D/$DC53 tables, frame $0580, timer $05A0),
// the frame's sprites ($D56F/$D66F: count, offsets entry, tile/attribute pairs; offsets $D911/$D94E, mirrored with
// $0400 bit 6 = +$10) at Y $05C0 / X $05E0. A sprite past an edge (the offset's sign against the add's carry) gets
// Y $F8 and its OAM slot is used again. Bit 5: falling by $62 a frame (gone at $FC).
void Engine::draw_object(std::uint8_t x) {
    const auto at = [x](unsigned base) { return static_cast<std::uint16_t>(base + x); };
    if (r(at(0x400)) & 0x20) {
        draw_y_ = r(at(0x5C0));
        const std::uint8_t y = static_cast<std::uint8_t>(r(at(0x5C0)) + r(0x62));
        r(at(0x5C0)) = y;
        if (y >= 0xFC) { r(at(0x400)) = static_cast<std::uint8_t>(r(at(0x400)) >> 1); return; }
    }
    const std::uint8_t kind = r(at(0x440));
    r(0x36) = m.read(static_cast<std::uint16_t>(0xDF7B + kind)); r(0x37) = m.read(static_cast<std::uint16_t>(0xDF86 + kind));
    if (r(at(0x400)) & 0x10) {
        if (!touch_player(x)) return;
        if (coop && other_.active && !coop_busy_ && (r(at(0x400)) & 0x90) == 0x90) { // co-op: player 2 too
            swap_players();
            const bool kept = touch_player(x);
            swap_players();
            if (!kept) return;
        }
    }
    if ((r(at(0x400)) & 0x08) && r(at(0x5C0)) >= 0x0C && !shot_hits(x)) return;
    const std::uint8_t animation = r(at(0x420));
    r(0x31) = m.read(static_cast<std::uint16_t>(0xDB8D + animation)); r(0x32) = m.read(static_cast<std::uint16_t>(0xDC53 + animation));
    const auto ptr = [this](std::uint8_t zp) { return static_cast<std::uint16_t>(r(zp) | r(static_cast<std::uint16_t>(zp + 1)) << 8); };
    std::uint8_t a = r(at(0x5A0)); ++r(at(0x5A0));
    if (a == m.read(static_cast<std::uint16_t>(ptr(0x31) + 1))) {
        r(at(0x5A0)) = 0;
        a = r(at(0x580)); ++r(at(0x580));
        if (a == m.read(ptr(0x31))) r(at(0x580)) = 0;
    }
    std::uint8_t y = static_cast<std::uint8_t>(r(at(0x580)) + 2);
    r(0x36) = r(at(0x5C0)); r(0x37) = r(at(0x5E0));
    const std::uint8_t frame = m.read(static_cast<std::uint16_t>(ptr(0x31) + y));
    if (frame == 0) { r(at(0x400)) = 0; draw_y_ = y; return; }
    r(0xF4) = r(at(0x400)) & 0x40;
    draw_frame(frame);
}
// $CA0C (from $C9FD with $F4 = 0, $CA04 with the object's mirror bit).
void Engine::draw_frame(std::uint8_t frame) {
    const auto ptr = [this](std::uint8_t zp) { return static_cast<std::uint16_t>(r(zp) | r(static_cast<std::uint16_t>(zp + 1)) << 8); };
    std::uint8_t y;
    r(0x33) = m.read(static_cast<std::uint16_t>(0xD56F + frame)); r(0x34) = m.read(static_cast<std::uint16_t>(0xD66F + frame));
    r(0x35) = m.read(ptr(0x33));
    std::uint8_t e = m.read(static_cast<std::uint16_t>(ptr(0x33) + 1));
    if (r(0xF4) & 0x40) e = static_cast<std::uint8_t>(e + 0x10);
    const std::uint8_t low = m.read(static_cast<std::uint16_t>(0xD911 + e));
    r(0x31) = static_cast<std::uint8_t>(low - 2);
    r(0x32) = static_cast<std::uint8_t>(m.read(static_cast<std::uint16_t>(0xD94E + e)) - (low < 2 ? 1 : 0));
    std::uint8_t o = r(0xA1);
    if (o == 0) { draw_y_ = e; return; }
    y = 1;
    // A sprite is off the screen when an offset with bit 7 (negative) leaves no carry, or a positive one leaves one.
    const auto off = [](std::uint8_t offset, bool carry) { return (offset & 0x80) ? !carry : carry; };
    for (;;) {
        ++y;
        r(static_cast<std::uint16_t>(0x201 + o)) = m.read(static_cast<std::uint16_t>(ptr(0x33) + y));
        std::uint8_t offset = m.read(static_cast<std::uint16_t>(ptr(0x31) + y));
        unsigned sum = r(0x36) + offset;
        r(static_cast<std::uint16_t>(0x200 + o)) = static_cast<std::uint8_t>(sum);
        bool hidden = false;
        if (off(offset, sum > 0xFF)) { ++y; hidden = true; }
        else {
            ++y;
            r(static_cast<std::uint16_t>(0x202 + o)) = static_cast<std::uint8_t>(m.read(static_cast<std::uint16_t>(ptr(0x33) + y)) ^ r(0xF4));
            offset = m.read(static_cast<std::uint16_t>(ptr(0x31) + y));
            sum = r(0x37) + offset;
            r(static_cast<std::uint16_t>(0x203 + o)) = static_cast<std::uint8_t>(sum);
            hidden = off(offset, sum > 0xFF);
        }
        if (hidden) r(static_cast<std::uint16_t>(0x200 + o)) = 0xF8;
        else {
            o = static_cast<std::uint8_t>(o + 4);
            r(0xA1) = o;
            if (o == 0) { draw_y_ = y; return; }
        }
        if (static_cast<std::uint8_t>(--r(0x35)) & 0x80) { draw_y_ = y; return; }
    }
}
// $C796: object x from the list $DE83[a] (a count, then bytes into $0400 + x + $20 n); $0580/$05A0 + x cleared.
void Engine::set_object(std::uint8_t a, std::uint8_t& x, std::uint8_t y_register) {
    r(0x31) = x; r(0x32) = y_register;
    const std::uint8_t y2 = static_cast<std::uint8_t>(a << 1);
    r(0x34) = m.read(static_cast<std::uint16_t>(0xDE83 + y2)); r(0x35) = m.read(static_cast<std::uint16_t>(0xDE84 + y2));
    r(static_cast<std::uint16_t>(0x580 + x)) = 0; r(static_cast<std::uint16_t>(0x5A0 + x)) = 0;
    const std::uint16_t p = static_cast<std::uint16_t>(r(0x34) | r(0x35) << 8);
    std::uint8_t y = 0;
    r(0x33) = m.read(p);
    std::uint8_t xi = x;
    do { ++y; r(static_cast<std::uint16_t>(0x400 + xi)) = m.read(static_cast<std::uint16_t>(p + y)); xi = static_cast<std::uint8_t>(xi + 0x20); } while (--r(0x33));
    x = r(0x31);
}
// $E711.
void Engine::game_init(std::uint8_t y) {
    for (int x = 0x0D; x >= 0; --x) r(static_cast<std::uint16_t>(0x6F0 + x)) = m.read(static_cast<std::uint16_t>(0xE749 + x));
    std::uint8_t x = 0x17;
    set_object(0x30, x, y);
    r(static_cast<std::uint16_t>(0x5C0 + x)) = 0x10; r(static_cast<std::uint16_t>(0x5E0 + x)) = 0x68;
    r(0x7A) = 3;
    if (r(0xAB) == 0) {
        r(0x78) = 0; r(0x79) = 0; r(0x88) = 0;
        for (int i = 0x0F; i >= 0; --i) r(static_cast<std::uint16_t>(0x90 + i)) = 0;
    }
    r(0xAB) = 0;
    if (coop) coop_init();
}

// $E8A4: the poster's tiles (bank 2 $9000 and $92B0, the stage's bank $A800), bank 0.
void Engine::stage_tiles() {
    nmi_off(); rendering_off();
    select_bank(2); copy_tiles(0x00, 0x40, 0x14, 0xE8B5); copy_tiles(0x00, 0x04, 0x08, 0xE8C0);
    select_bank(r(0x41)); copy_tiles(0x00, 0x70, 0x0D, 0xE8D0);
    select_bank(0);
}
// $E75D-$E7B8: the stage's tiles (banks 0, $E881[$41], $E887[$41], 3 ($E908), $41).
void Engine::stage_graphics() {
    select_bank(0); copy_tiles(0x00, 0x80, 0x00, 0xE765);
    if (r(0x41) == 2) copy_tiles(0x00, 0x1D, 0x08, 0xE780); else copy_tiles(0x00, 0x1D, 0x08, 0xE776);
    select_bank(m.read(static_cast<std::uint16_t>(0xE881 + r(0x41)))); copy_tiles(0xD0, 0x20, 0x09, 0xE793);
    select_bank(m.read(static_cast<std::uint16_t>(0xE887 + r(0x41)))); copy_tiles(0xD0, 0x1B, 0x0B, 0xE7A6);
    select_bank(3); copy_tiles(0x80, 0x28, 0x0D, 0xE913); select_bank(r(0x41)); // $E908
    copy_tiles(0x00, 0xC0, 0x10, 0xE7B4);
}
// $0:BA80: the poster: sprites $D0-$FE in a grid of 4 rows from (40, 68) at $0200 + $A0, nametables $2000 (attributes 0)
// and $2800 ($AA), the script $BB7D, scroll ($C0, $EF), palette set $41 + $0C held ($A4).
void Engine::poster_setup() {
    r(0xFF) = 0xB0; nmi_off(); rendering_off(); hide_sprites();
    r(0xF0) = 0;
    fill_nametable(0x20, 0xF4, 0x00); fill_nametable(0x28, 0xF4, 0xAA);
    std::uint8_t y = 0xD0;
    r(0x08) = 0x40; r(0x09) = 0x68;
    for (unsigned x = 0xA0; x < 0x100; x += 4) {
        r(static_cast<std::uint16_t>(0x200 + x)) = r(0x08); r(static_cast<std::uint16_t>(0x201 + x)) = y;
        r(static_cast<std::uint16_t>(0x202 + x)) = 0; r(static_cast<std::uint16_t>(0x203 + x)) = r(0x09);
        y = static_cast<std::uint8_t>(y + 2);
        std::uint8_t a = static_cast<std::uint8_t>(r(0x08) + 0x10);
        const bool wrap = a == 0x80; if (wrap) a = 0x40;
        r(0x08) = a;
        if (wrap) r(0x09) = static_cast<std::uint8_t>(r(0x09) + 8);
    }
    nmi_on();
    r(0x26) = 0x7D; r(0x27) = 0xBB; r(0xFC) = 0xEF; r(0xFD) = 0xC0;
    // $BAF3-$BB02 (one frame, rendering on, the palette set) follow in wanted_poster.
}

// ---- routines that wait ----

Routine Engine::wait_frames(std::uint8_t& x) { do { co_await wait(); } while (--x); } // $C0B4

// $C49F: palette set $A3 darkened by $0F = a, changed by x each step ($C496: in from $30 by -$10; $C49C: out from $10
// by +$10 to $50), y frames each (sprites hidden meanwhile unless $A4).
Routine Engine::fade(std::uint8_t a, std::uint8_t x, std::uint8_t y) {
    r(0x0F) = a; r(0x0B) = x; r(0x0E) = y;
    for (;;) {
        fade_x_ = load_palette();
        for (int i = 0x1F; i >= 0; --i) {
            std::uint8_t c = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x3E0 + i)) - r(0x0F));
            if (c & 0x80) c = 0x0F;
            r(static_cast<std::uint16_t>(0x3E0 + i)) = c;
        }
        std::uint8_t left = r(0x0E);
        do {
            if (r(0xA4) == 0) { hide_sprites(); objects(); if (her_turn_) her_sprites(); fade_x_ = r(0xA2); }
            co_await wait();
        } while (--left);
        r(0x0F) = static_cast<std::uint8_t>(r(0x0F) + r(0x0B));
        if (r(0x0F) == 0x50 || (r(0x0F) & 0x80)) break;
    }
    r(0xA4) = 0;
}

Routine Engine::start(std::uint16_t entry) {
    switch (entry) {
        case 0xE6AD: return task0();
        case 0xFF00: return random_task();
        case 0xA000: if (r(0x41) == 0) return boss_1(); if (r(0x41) == 1) return boss_2(); if (r(0x41) == 2) return boss_3(); if (r(0x41) == 3) return boss_4(); if (r(0x41) == 4) return boss_5(); if (r(0x41) == 5) return boss_6(); throw Unported(0x10000u * r(0x41) + entry);
        case 0xA1B4: if (r(0x41) == 5) return boss_6_shots(); throw Unported(0x10000u * r(0x41) + entry);
        case 0xA24B: if (r(0x41) == 4) return boss_5_shots(); throw Unported(0x10000u * r(0x41) + entry);
        case 0xA23C: if (r(0x41) == 3) return boss_4_shots(); throw Unported(0x10000u * r(0x41) + entry);
        case 0xA1EE: if (r(0x41) == 2) return boss_3_shots(); throw Unported(0x10000u * r(0x41) + entry);
        case 0xA17F: if (r(0x41) == 1) return boss_2_shots(); throw Unported(0x10000u * r(0x41) + entry);
        case 0xA1E5: if (r(0x41) == 0) return boss_1_shots(); throw Unported(0x10000u * r(0x41) + entry);
        default: throw Unported(entry);
    }
}

// $E6AD: task 0 from power-on.
Routine Engine::task0() {
    m.write(0x2000, r(0xFF));
    sound_command(0xFE, 0x30); sound_command(0xFF, 0x30); // X = $30 from the task's stack setup
    co_await wait();
    if (r(0x102) == 0x35 && r(0x103) == 0x53) { co_await title_and_game(); co_return; } // $E703: a soft reset keeps the high score
    select_bank(0);
    for (unsigned x = 0; x < 0x80; ++x) r(static_cast<std::uint16_t>(0x780 + x)) = m.read(static_cast<std::uint16_t>(0xAF80 + x));
    game_init(0); // Y = 0 from $C361
    r(0x6A2) = 6;
    for (int x = 5; x >= 0; --x) r(static_cast<std::uint16_t>(0x6A3 + x)) = 0xD0;
    r(0x6A9) = 0xFF;
    r(0x102) = 0x35; r(0x103) = 0x53;
    co_await wait();
    co_await title_and_game();
}
Routine Engine::title_and_game() {
    select_bank(1);
    co_await title(); // returns here through $C49C (its stack reset at $B01F leaves this return address)
    game_init(0xFF);  // Y = $FF after the fade
    co_await stage_load();
}
// $FAEE: after the player was hit in a boss fight: the boss again (its variables cleared, task 1 from $A000).
void Engine::boss_again() {
    for (unsigned k = 0; k < 0x10; ++k) r(static_cast<std::uint16_t>(0xB0 + k)) = 0;
    r(0x4B) = 1;
    r(0x23) = 0x00; r(0x24) = 0xA0;
    start_task(1, 0xA000);
}

// $C1BC / $1:B000: back to the title (its graphics on, $B01C) from the demonstration; then the game as from the title.
void Engine::restart_title(unsigned k, bool fade) {
    auto& task = tasks_[k];
    task.owned.clear();
    task.owned.push_back(title_again(fade));
    task.resume = task.owned.back().handle();
    current_ = k; r(0x21) = static_cast<std::uint8_t>(k);
    task.resume.resume();
}
Routine Engine::title_again(bool fade) {
    if (fade) {
        r(0x62) = 0; r(0xA4) = 0x0A;
        co_await this->fade(0x10, 0x10, 0x0A);
        select_bank(1);
    }
    co_await title(true);
    game_init(0xFF); // Y = $FF after the fade
    co_await stage_load();
}
// $1:B26B: the attract mode: the story's first page ($B435), the second with the four characters ($78-$7B) walking
// (their animation every 240 frames, $B3F9) under the story typed ($B787/$B79D, like the ending's $B917), then the
// demonstration: stage 1 without the poster ($E75D) with the recorded pads ($68).
Routine Engine::attract() {
    ++r(0x68);
    co_await fade(0x10, 0x10, 0x0A);
    nmi_off(); rendering_off();
    fill_nametable(0x20, 0xF4, 0xFF);
    r(0x26) = 0x35; r(0x27) = 0xB4;
    r(0xFE) = 0x1E;
    nmi_on();
    co_await wait();
    co_await fade(0x30, 0xF0, 0x0A);
    std::uint8_t n = 0x3C; co_await wait_frames(n);
    co_await fade(0x10, 0x10, 0x0A);
    nmi_off(); rendering_off(); // $C460
    select_bank(3); copy_tiles(0x00, 0x40, 0x10, 0xC471);
    select_bank(1);
    nmi_on();
    r(0x0A) = 0;
    for (int x = 6; x >= 0; x -= 2) {
        r(0x26) = m.read(static_cast<std::uint16_t>(0xB5C2 + x)); r(0x27) = m.read(static_cast<std::uint16_t>(0xB5C3 + x));
        co_await wait();
    }
    for (std::uint8_t k = 0; k < 4; ++k) { std::uint8_t s = k; set_object(static_cast<std::uint8_t>(0x78 + k), s, 0); } // Y = 0 after $C266
    r(0x5C0) = r(0x5C2) = r(0x5C3) = 0x60; r(0x5E0) = 0x20;
    r(0x5C1) = 0x29; r(0x5E1) = 0xB0; r(0x5E2) = r(0x5E3) = 0x80;
    r(0xA3) = 0x13; r(0xFE) = 0x1E;
    co_await fade(0x30, 0xF0, 0x0A);
    r(0x07) = 0; r(0x08) = 0;
    for (;;) {
        r(0x36C) = 1;
        attract_address(r(0x07));
        for (;;) {
            const std::uint8_t x = r(0x07);
            r(0xF3) = m.read(static_cast<std::uint16_t>(0xB787 + x)); r(0xF4) = m.read(static_cast<std::uint16_t>(0xB79D + x));
            std::uint8_t a;
            do {
                ++r(0x36B);
                const std::uint8_t y = r(0x08)++;
                a = m.read(static_cast<std::uint16_t>((r(0xF3) | r(0xF4) << 8) + y));
            } while (a == 0xF4);
            if (a == 0xFF) break;
            r(0x36D) = a; r(0x3A) = a;
            for (r(0x09) = 6; r(0x09); --r(0x09)) { attract_frame(); co_await wait(); }
        }
        for (r(0x09) = 0x0A; r(0x09); --r(0x09)) { attract_frame(); co_await wait(); }
        const std::uint8_t line = r(0x07)++;
        if (line == 0x14) break;
        r(0x08) = 0;
        if (line < 2) continue;
        co_await attract_line(static_cast<std::uint8_t>(line - 1));
        co_await attract_line(static_cast<std::uint8_t>(r(0x07) - 1));
        co_await attract_line(0x15);
    }
    for (r(0x09) = 0x3C; r(0x09); --r(0x09)) { attract_frame(); co_await wait(); }
    r(0xA4) = 0x0A;
    co_await fade(0x10, 0x10, 0x0A);
    nmi_off(); rendering_off();
    for (int x = 3; x >= 0; --x) r(static_cast<std::uint16_t>(0x400 + x)) = 0;
    r(0x417) = 0x80; r(0x5D7) = 0x10; r(0x5F7) = 0x68;
    sound_command(0xFF, 0xFF);
    co_await stage_load(false); // $E4C3 -> $E75D
}
// $1:B3F9: a frame's characters: every other frame $0A counts; at 240 their poses ($0422/$0423, Y $B430) and the
// third one's Y step; drawn in a fixed order ($A0 = $FF).
void Engine::attract_frame() {
    if (r(0x22) & 1) {
        if (++r(0x0A) == 0xF0) {
            r(0x0A) = 0;
            ++r(0x422); ++r(0x423);
            const std::uint8_t y = m.read(static_cast<std::uint16_t>(0xB430 + static_cast<std::uint8_t>(r(0x422) - 0xBB)));
            r(0x5C2) = y; r(0x5C3) = y;
            ++r(0x5C1);
        }
    }
    hide_sprites();
    r(0xA0) = 0xFF;
    objects();
}
// $1:B363: line x written whole at the window's next row ($09).
Routine Engine::attract_line(std::uint8_t x) {
    r(0xF3) = m.read(static_cast<std::uint16_t>(0xB787 + x)); r(0xF4) = m.read(static_cast<std::uint16_t>(0xB79D + x));
    attract_address(r(0x09)++);
    ++r(0x36B);
    r(0x36C) = 0x19;
    std::uint8_t y = 0, a = 0;
    for (unsigned i = 0; i < 0x19; ++i) {
        r(static_cast<std::uint16_t>(0x36D + i)) = 0xF4;
        a = m.read(static_cast<std::uint16_t>((r(0xF3) | r(0xF4) << 8) + y));
        if (a != 0xFF) { r(static_cast<std::uint16_t>(0x36D + i)) = a; ++y; }
    }
    r(0x3A) = a;
    attract_frame();
    co_await wait();
}
void Engine::attract_address(std::uint8_t x) {
    if (x >= 2) x = 2;
    r(0x36B) = m.read(static_cast<std::uint16_t>(0xB7B3 + x));
    r(0x36A) = m.read(static_cast<std::uint16_t>(0xB7B6 + x));
}
// $1:B003: the title screen until START (or the attract mode).
Routine Engine::title(bool again) {
    if (!again) {
        r(0x4E) = 0; r(0xFE) = 0;
        co_await wait();
        start_task(3, 0xFF00);
        co_await wait();
    }
    title_graphics();
    for (const std::uint16_t a : std::initializer_list<std::uint16_t>{0x68, 0x69, 0xF0, 0xFC, 0xFD, 0x78, 0x79}) r(a) = 0;
    hide_sprites();
    fill_pages(0x00, 0x03, 0x05);
    nmi_on();
    co_await wait();
    r(0x26) = 0x50; r(0x27) = 0xB1; // the title's VRAM script $B150
    co_await wait();
    r(0x6A0) = 0x22; r(0x6A1) = 0x73; r(0x26) = 0xA0; r(0x27) = 0x06; // the high score line at $06A0
    r(0xFE) = 0x1E;
    sound_command(0x00, 0x00); // X = 0 after $C24E
    r(0xA3) = 0x12;
    co_await fade(0x30, 0xF0, 0x0A);
    std::uint8_t x = 0;
    r(0x07) = 0; r(0x08) = 0;
    for (;;) {
        if (++x == 0) { co_await attract(); co_return; } // $B0E0: the attract mode after 255 frames
        co_await wait();
        const std::uint8_t a = r(0xF5);
        if (a) {
            std::uint8_t y = r(0x08);
            bool reset = true;
            if (a == m.read(static_cast<std::uint16_t>(0xB244 + y))) {
                ++r(0x08);
                if (y != 0x0A) reset = false;
                else if (++r(0x4F) != 0) reset = false;
            }
            if (reset) { r(0x08) = 0; }
            y = r(0x07);
            if (a == m.read(static_cast<std::uint16_t>(0xB22F + y))) { ++r(0x07); if (y == 0x14) ++r(0x4C); }
            else r(0x07) = 0;
        }
        if (a == 0x10) break; // START
    }
    // $B0B2: the music stops, the start sound, PUSH START blinks five times (scripts $B25D / $B24F, 10 frames each).
    sound_command(0xFF, x); sound_command(0x15, x);
    for (std::uint8_t y = 5; y; --y) {
        r(0x26) = 0x5D; r(0x27) = 0xB2;
        std::uint8_t frames = 0x0A; co_await wait_frames(frames);
        r(0x26) = 0x4F; r(0x27) = 0xB2;
        frames = 0x0A; co_await wait_frames(frames);
    }
    co_await fade(0x10, 0x10, 0x0A); // $C49C with Y = $0A
}

// $C2C4: a text typed one character a frame at a time: the record at $01/$02 (address, count, characters; $F4 a space
// that is skipped), each shown character through $036A ($3A = its index) with the sound and `delay` frames. x is the
// caller's X register (kept in $F3 by the sound command; 0 after a wait).
Routine Engine::type_text(std::uint8_t delay, std::uint8_t sound, std::uint16_t text, std::uint8_t& x) {
    r(0x04) = delay; r(0x03) = sound;
    r(0x01) = static_cast<std::uint8_t>(text); r(0x02) = static_cast<std::uint8_t>(text >> 8);
    std::uint8_t y = 0;
    r(0x36A) = m.read(text); ++y; r(0x36B) = m.read(static_cast<std::uint16_t>(text + y)); r(0x36C) = y; ++y;
    r(0x06) = m.read(static_cast<std::uint16_t>(text + y));
    do {
        ++y;
        const std::uint8_t c = m.read(static_cast<std::uint16_t>(text + y));
        r(0x36D) = c;
        if (c != 0xF4) {
            r(0x3A) = y;
            sound_command(r(0x03), x);
            x = r(0x04);
            co_await wait_frames(x);
        }
        ++r(0x36B);
    } while (--r(0x06));
}
// $0:BA80 to its end: the poster set up, a frame, rendering on, its palette set held.
Routine Engine::poster() {
    poster_setup();
    co_await wait(); // $BAF3
    rendering_on();
    r(0xA3) = static_cast<std::uint8_t>(r(0x41) + 0x0C); r(0xA4) = r(0xA3);
}
// $0:B896: the stage's wanted poster.
Routine Engine::wanted_poster() {
    fill_nametable(0x20, 0xF4, 0xFF);
    const std::uint8_t stage = r(0x41);
    r(0x26) = m.read(static_cast<std::uint16_t>(0xBB71 + stage)); r(0x27) = m.read(static_cast<std::uint16_t>(0xBB77 + stage));
    nmi_on(); rendering_on();
    r(0xA3) = 0x12;
    co_await fade(0x30, 0xF0, 0x0A);
    std::uint8_t x = 0x1E; co_await wait_frames(x);
    co_await fade(0x10, 0x10, 0x0A);
    co_await poster();
    co_await fade(0x30, 0xF0, 0x05);
    x = 0x28; co_await wait_frames(x);
    do { // the poster scrolls in, its sprites with it
        co_await wait();
        for (unsigned i = 0; i < 0x100; i += 4) --r(static_cast<std::uint16_t>(0x203 + i));
        ++r(0xFD);
    } while (r(0xFD) != 0xF8);
    x = 0x16; // the name, the weapon and the reward ($BD2A/$BD3C, $BD30/$BD42, $BD36/$BD48), 12 frames a character
    for (const std::uint16_t low : {std::uint16_t{0xBD2A}, std::uint16_t{0xBD30}, std::uint16_t{0xBD36}}) {
        const std::uint16_t text = static_cast<std::uint16_t>(m.read(static_cast<std::uint16_t>(low + r(0x41))) |
            m.read(static_cast<std::uint16_t>(low + 0x12 + r(0x41))) << 8);
        co_await type_text(0x0C, 0x16, text, x);
        x = 0x16;
    }
    x = 0x3C; co_await wait_frames(x);
    r(0xA4) = 0x05;
    co_await fade(0x10, 0x10, 0x05);
    nmi_off(); rendering_off();
}
// $E757: the stage's poster, then its graphics.
Routine Engine::stage_load(bool poster) {
    if (poster) {
        stage_tiles();
        co_await wanted_poster();
    }
    stage_graphics();
    nmi_on();
    co_await wait();
    // $E7BF: the map's start ($5A/$5B = $8440; its end $5E/$5F from $E875), VRAM row $2B40, scroll 0.
    r(0xFF) = 0x90;
    for (const int a : {0xF0, 0xFC, 0xFD, 0x5C, 0x42, 0x2E, 0x2C, 0x49, 0x2F}) r(static_cast<std::uint16_t>(a)) = 0;
    r(0x5D) = 0x10; r(0x58) = 0x40; r(0x59) = 0x2B; r(0x5A) = 0x40; r(0x5B) = 0x84;
    const std::uint8_t s = static_cast<std::uint8_t>(r(0x41) << 1);
    r(0x5E) = m.read(static_cast<std::uint16_t>(0xE875 + s)); r(0x5F) = m.read(static_cast<std::uint16_t>(0xE876 + s));
    co_await wait();
    rendering_off();
    save_rows();
    co_await draw_stage();
    restore_rows();
    r(0xFE) = 0x1E;
    co_await enter_player();
    if (coop) coop_place_other();
    r(0x43) = 0x00; r(0x44) = 0x8C; r(0x63) = 0;
    const std::uint8_t music = static_cast<std::uint8_t>(r(0x41) + 4);
    r(0xEA) = music; sound_command(music, r(0xA2)); // $C368 (X: $A2 from the fade's last object scan)
    co_await wait();
    r(0x69) = 1;
    for (const int a : {0x6A, 0x6B, 0x22, 0xA0, 0x437}) r(static_cast<std::uint16_t>(a)) = 0;
    if (r(0x68) == 0) r(0x781) = 0;
    r(0xAC) = 0x88; r(0xAD) = 0; r(0xAE) = 0; r(0xAF) = 0; r(0x00) = 0;
    if (r(0x4F)) { // $E84D: the title's code: gun 3 with 300 shots
        r(0x94) = r(0x4F); r(0x4F) = 0;
        r(0x95) = 0xD3; r(0x96) = 0xD0; r(0x97) = 0xD0;
    }
    co_await play();
}

// $E91D: a frame of play. Every third frame (while $63 is 0) the stage scrolls a line ($62 = 1: the player is pushed
// down with it); otherwise the stage's events ($EACC). Then the player, the shots, the enemies (bank 6's handlers
// $B000 for objects $10-$16 of kind $40 on) and the objects into OAM; the end of the frame checks $76 (the player hit,
// ...) and the pause.
Routine Engine::play() {
    for (;;) {
        hide_sprites();
        r(0x62) = 0;
        if (r(0x63) == 0 && ++r(0x60) == 3) { r(0x60) = 0; r(0x62) = 1; scroll_step(); }
        else stage_events();
        player();
        fire();
        player_sprites();
        if (coop && other_.active) coop_other_frame();
        shots();
        enemy_shots();
        select_bank(6);
        for (std::uint8_t x = 0x10; x != 0x17; ++x) {
            if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80) || r(static_cast<std::uint16_t>(0x420 + x)) < 0x40) continue;
            const std::uint8_t y = static_cast<std::uint8_t>((r(static_cast<std::uint16_t>(0x420 + x)) - 0x40) << 1);
            r(0xF3) = m.read(static_cast<std::uint16_t>(0xB000 + y)); r(0xF4) = m.read(static_cast<std::uint16_t>(0xB001 + y)); // jmp ($F3)
            enemy(static_cast<std::uint16_t>(r(0xF3) | r(0xF4) << 8), x);
        }
        select_bank(r(0x41));
        objects();
        if (coop && other_.active && (other_.block[0x76 - 0x70] & 7)) { // co-op: player 2's hit, horse or bomb with her block in
            swap_players(); coop_busy_ = true; her_turn_ = true;
            if (r(0x76) & 1) co_await player_hit();
            else if (r(0x76) & 2) co_await horse_mount();
            else { r(0x76) = 0; r(0x88) = 0; if (r(0x79)) ++r(0x88); sound_command(0x29, 0); clear_enemies(draw_y_); }
            coop_busy_ = false; her_turn_ = false; swap_players();
            if (coop_out_) { coop_out_ = false; other_.active = false; }
            continue;
        }
        if (r(0x76) & 1) {
            if (coop) coop_busy_ = true;
            co_await player_hit();
            coop_busy_ = false;
            if (coop_out_) { coop_out_ = false; swap_players(); ram_owner_ ^= 1; other_.active = false; } // player 2 plays on alone
            continue;
        }
        if (r(0x76) & 2) co_await horse_mount(); // $F291
        if (r(0x76) & 4) { // $F29A: gun 3 hit: a white flash, the gun gone, a bomb ($CB8D)
            r(0x3F0) = 0x20; r(0x3B) = 0x20;
            std::uint8_t n = 0x0A; co_await wait_frames(n);
            r(0x3F0) = 0x0F; r(0x3B) = 0x0F;
            r(0x9C) = 0; r(0x76) = 0; r(0x88) = 0;
            if (r(0x79)) ++r(0x88);
            sound_command(0x29, 0);
            clear_enemies(draw_y_);
        }
        if (r(0x4B) == 2) { // $F2C4: the boss beaten
            r(0x4B) = 0;
            co_await fade(0x10, 0x10, 2);
            stage_tiles();
            co_await stage_won();
            co_return;
        }
        co_await wait();
        if (r(0xF5) & 0x10) { // $F2EC: START pauses (the other tasks held, $30 for the sound) until START again
            const std::uint8_t x = r(0xA2); // X from the object scan
            r(0x25) = 0xE0;
            sound_command(0x20, x);
            co_await wait();
            ++r(0x30);
            do co_await wait(); while (!(r(0xF5) & 0x10));
            r(0x30) = 0; r(0x25) = 0;
            sound_command(0x20, x);
            continue;
        }
        if (r(0xA5) || (r(0xF5) & 0x20)) co_await open_window(); // $F2E6: a shop or Select
    }
}
// $E85E: the player at the first free place from X $80 on line $BC, the play variables, the stage's palette fades in.
Routine Engine::enter_player() {
    r(0x74) = 0x80; r(0x71) = 0xBC;
    place_player(0xBC);
    clear_play();
    r(0xA3) = r(0x41);
    co_await fade(0x30, 0xF0, 0x0A);
}
// $F31C: the player hit: tasks 1 and 2 stopped, the music off and the death sound, the player's death object
// ($35) for 72 frames while the shots and objects go on, a fade out; the stage's objects cleared except the
// scenery kinds, the power-ups lose a level ($CC67), a life less ($7A; none left: the game over $F3F4), the lives
// screen (a sprite "1"-style row $F51C, the lives digit, frames $4A and 0) faded in and out, then the player again
// and the stage music.
Routine Engine::player_hit() {
    r(0x14) = 0; r(0x18) = 0; // $C07F: tasks 1 and 2
    r(0x4A) = 0; r(0x69) = 0;
    sound_command(0xFF, 8); sound_command(0x2A, 8);
    co_await wait();
    r(0x62) = 0; r(0x7D) = 0;
    std::uint8_t x = 0;
    set_object(0x35, x, r(0x32));
    r(0x5C0) = r(0x71); r(0x5E0) = r(0x74);
    for (std::uint8_t n = 0x48; n; --n) {
        hide_sprites();
        shots(); enemy_shots();
        objects();
        if (her_turn_) her_sprites();
        co_await wait();
    }
    co_await fade(0x10, 0x10, 0x14);
    if (r(0x68)) { select_bank(1); throw RestartTitle{false}; } // $F36A: the demonstration's player hit
    r(0x437) = 0;
    clear_objects();
    hide_sprites();
    if (const std::uint8_t g = m.read(static_cast<std::uint16_t>(0xF1D7 + r(0x88))); g != 4) r(static_cast<std::uint16_t>(0x90 + 4 * g)) = 0; // $F3A4: the gun lost
    power_down();
    r(0xA1) = 4; r(0xA3) = 0x14;
    if (--r(0x7A) == 0) {
        if (coop && other_.active) { // co-op: this player is out, the other plays on
            coop_out_ = true;
            if (r(0x4B) == 1) boss_again(); // its tasks were stopped ($F31C)
            r(0xFE) = 0x1E;
            r(0xA3) = r(0x41);
            co_await fade(0x30, 0xF0, 0x0A);
            sound_command(r(0xEA), 0);
            co_return;
        }
        co_await game_over(); co_return;
    }
    inline_pointer(0xF3BA); // $C30C from $F3BA: its data $F51C (also $05/$06)
    sprite_text(0xF51C);
    r(0x205) = static_cast<std::uint8_t>(0x58 + r(0x7A));
    r(0x36) = 0x74; r(0x37) = 0x74; r(0xF4) = 0; draw_frame(0x4A);
    r(0x36) = 0x10; r(0x37) = 0x68; r(0xF4) = 0; draw_frame(0);
    if (her_turn_) her_sprites();
    r(0xFE) = 0x14;
    r(0xA4) = 5;
    co_await fade(0x30, 0xF0, 5);
    co_await wait(0x3C);
    r(0xA4) = 5; // $F4D0
    co_await fade(0x10, 0x10, 5);
    r(0xFE) = 0x1E;
    if (r(0x7A) == 0) throw Unported(0xF4F6);
    co_await enter_player();
    if (r(0x4B) == 1) { boss_again(); co_return; } // $F4F0
    sound_command(r(0xEA), static_cast<std::uint8_t>(r(0x4B) - 1));
    co_await wait();
}
// $F3F4: the game over screen: cleared, the texts by VRAM scripts $F522 and $F55F (GAME OVER, SCORE, HI SCORE,
// CONTINUE / RETRY), the player's frame 0, the high score ($F434) shown through the script at $06A0; the cursor
// colour $03E8 blinks with the frame counter $22; Select toggles $07 (the arrow at CONTINUE / RETRY: $F55F / $F568),
// Start ends it ($F4C4).
Routine Engine::game_over() {
    nmi_off(); rendering_off();
    fill_nametable(0x20, 0xF4, 0xFF);
    nmi_on();
    sound_command(0x12, 0xF4); // X = $01 after $C209
    r(0x26) = 0x22; r(0x27) = 0xF5;
    co_await wait();
    r(0x26) = 0x5F; r(0x27) = 0xF5;
    co_await wait();
    r(0x36) = 0x78; r(0x37) = 0x88; r(0xF4) = 0; draw_frame(0);
    high_score();
    r(0x6A0) = 0x22; r(0x6A1) = 0x31;
    r(0x26) = 0xA0; r(0x27) = 0x06;
    rendering_on();
    r(0xF0) = 0; r(0xFC) = 0;
    r(0xA4) = 0x0A;
    co_await fade(0x30, 0xF0, 0x0A);
    r(0x07) = 0;
    for (;;) {
        load_palette();
        const std::uint8_t x = (r(0x22) & 0x18) >> 3;
        r(static_cast<std::uint16_t>(0x3E8 + x)) = 0x20;
        if (r(0xF5) & 0x10) { // $F4C4: CONTINUE (the stage again, the guns kept: $AB) or RETRY (the title)
            sound_command(0xFF, x); sound_command(0x15, x);
            r(0xA4) = 0x0A;
            co_await fade(0x10, 0x10, 0x0A);
            r(0xFE) = 0x1E;
            r(0x4B) = 0;
            if (r(0x07)) { r(0x41) = 0; co_await title_and_game(); co_return; }
            for (int k = 0x1F; k >= 0; --k) if (r(static_cast<std::uint16_t>(0x420 + k))) r(static_cast<std::uint16_t>(0x400 + k)) = 0;
            r(0xAB) = 0xFF;
            game_init(0xFF); // Y = $FF after the fade
            co_await stage_load();
            co_return;
        }
        if (r(0xF5) & 0x20) {
            r(0x07) ^= 0xFF;
            sound_command(0x2B, x);
            r(0x26) = r(0x07) ? 0x68 : 0x5F; r(0x27) = 0xF5;
        }
        co_await wait();
    }
}
// $F434: the score $06F2-$06FA (every other byte, tiles $58-$61) above the high score $06A3-$06A8 (tiles $D0-$D9):
// copied as the new high score.
void Engine::high_score() {
    for (unsigned x = 2, y = 1; y != 7; x += 2, ++y) {
        const std::uint8_t a = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x6F0 + x)) + 0x78);
        const std::uint8_t h = r(static_cast<std::uint16_t>(0x6A2 + y));
        if (a == h) continue;
        if (a < h) return;
        for (unsigned i = 0; i < 6; ++i) r(static_cast<std::uint16_t>(0x6A3 + i)) = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x6F2 + 2 * i)) + 0x78);
        return;
    }
}
// $C38B: the boss's entrance: a second, then until no enemy ($10-$16, kind $50 on) is left; the boss's 40 tiles
// streamed from $A400 to CHR $0D80 by the NMI ($3E), its strength $B2 ($C40B) and music ($C405), 90 frames, its
// palette set $41 + 6, 240 frames; the strength bar's row $0300 ($C411).
Routine Engine::boss_intro() {
    std::uint8_t n = 0x3C; co_await wait_frames(n);
    for (std::uint8_t x = 0x10; x != 0x17;) {
        if (r(static_cast<std::uint16_t>(0x420 + x)) >= 0x50 && (r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) { co_await wait(); x = 0x10; continue; }
        ++x;
    }
    r(0x28) = 0x00; r(0x29) = 0xA4; r(0x2B) = 0x80; r(0x2A) = 0x0D; r(0x3E) = 0x28;
    const std::uint8_t stage = r(0x41);
    r(0xB2) = m.read(static_cast<std::uint16_t>(0xC40B + stage));
    const std::uint8_t music = m.read(static_cast<std::uint16_t>(0xC405 + stage));
    r(0xEA) = music; sound_command(music, stage);
    n = 0x5A; co_await wait_frames(n);
    r(0xA3) = r(0x4A) = static_cast<std::uint8_t>(stage + 6);
    if (stage == 5 && r(0xBA)) r(0xA3) = 0x18;
    load_palette();
    co_await wait(0xF0);
    for (int x = 0x0D; x >= 0; --x) r(static_cast<std::uint16_t>(0x300 + x)) = m.read(static_cast<std::uint16_t>(0xC411 + x));
    r(0x437) = 0x0A;
    r(0x300) = r(0xB2);
}
// $0:A000: stage 1's boss (object $0E): enters from the top at a random X ($A0D8) to line $40, then moves in random
// directions ($AE += $AF) kept within X $40-$CF / Y $28-$7F, firing ($A11B); hit ($0420 >= $8D) $E1F9.
Routine Engine::boss_1() {
    nmi_on();
    co_await boss_intro();
    start_task(2, 0xA1E5);
    std::uint8_t x = 1;
    for (int y = 3; y >= 0; --y) {
        do ++x; while (r(static_cast<std::uint16_t>(0x400 + x)) & 0x80);
        r(static_cast<std::uint16_t>(0xB6 + y)) = x;
    }
    x = 0x0E;
    std::uint8_t y = 0xFF;
    set_object(0x55, x, y);
    r(0x5CE) = 0;
    r(0x5EE) = m.read(static_cast<std::uint16_t>(0xA0D8 + (r(0xAC) & 3)));
    r(0xB5) = 1;
    r(0x40E) = 0x9C;
    do { co_await wait(); y = boss_1_move(x, y); } while (r(0x5CE) != 0x40);
    r(0x40E) = 0x98;
    for (;;) {
        if (r(0x42E) >= 0x8D) {
            r(0x07) = 0x55; r(0x08) = 0x8E;
            if (boss_hit(y)) { co_await boss_end(); co_return; }
            if (r(0x42E) == 0x88) { r(0x40E) = 0x90; r(0x42E) = 0x8C; ++r(0xB0); r(0xBA) = 0xC4; r(0xBC) = 0; }
        } else {
            if (r(0xB4) == 0) {
                r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAF));
                r(0x4AE) = (r(0xAE) & 0x1C) | 0x40;
                r(0xB4) = static_cast<std::uint8_t>(0x18 * ((r(0xAE) & 3) + 1));
                y = 0xFF;
            }
            y = boss_1_move(x, y);
            const std::uint8_t by = r(0x5CE), bx = r(0x5EE);
            if (by < 0x28 || by >= 0x80 || bx < 0x40 || bx >= 0xD0) { r(0x4AE) = static_cast<std::uint8_t>(r(0x4AE) + 0x10); y = move(x); }
            y = boss_1_fire(x, y);
        }
        co_await wait();
    }
}
// $0:A0DD: while not firing ($BC): a step every frame of the walk ($B5: 4 or 12 frames moving, then standing; the
// animation $88/$8C by $B0), during $B0 only on odd frames and straight.
std::uint8_t Engine::boss_1_move(std::uint8_t x, std::uint8_t y) {
    if (r(0xBC)) return y;
    --r(0xB4); --r(0xB5);
    if ((r(0xB5) & 0x7F) == 0) {
        if (!(r(0xB5) & 0x80)) r(0xB5) = r(0xB0) ? 0x8C : 0x88;
        else r(0xB5) = r(0xB0) ? 0x0C : 0x04;
    }
    if (r(0xB5) & 0x80) return y;
    if (r(0xB0)) {
        if (!(r(static_cast<std::uint16_t>(0xA0 + x)) & 1)) return y; // $A0,x: zero page $AE for the boss
        r(static_cast<std::uint16_t>(0x4A0 + x)) &= 0x1F;
    }
    return move(x);
}
// $0:A11B: every $6C frames ($BA) a burst: at $0C aimed (only toward the player's side: directions $0B-$15, during
// $B0 $0E-$11), then four shots ($B6-$B9, object $70) at $0C, $18, $24, $30 from the gun's place ($A147/$A14C by the
// pose $042E).
std::uint8_t Engine::boss_1_fire(std::uint8_t x, std::uint8_t y_in) {
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    if (r(0xB0) && (r(0xA0) & 1) && ++r(0xB0) == 0) goto reset;
    if (++r(0xBA) == 0x6C) {
        r(0xBA) = 0;
        if (r(0xB0) == 0) return y_in;
        goto reset;
    }
    if (r(0xBA) == 0x0C) {
        const std::uint8_t a = aim(x);
        if (r(0xB0)) {
            if (a < 0x0E || a >= 0x12) { r(0xBA) = 0xC4; return r(0xB0); }
            r(0xBB) = a | 0x80;
        } else {
            if (a < 0x0B || a >= 0x16) { r(0xBA) = 0xC4; return 0; }
            r(0xBE) = a;
            r(0x42E) = rd(0xA1CF + a);
            r(0x58E) = 0;
            r(0xBB) = r(0xBE) | 0x80;
        }
        r(0xBC) = 1;
    }
    {
        std::uint8_t a = r(0xBA);
        int y = 4;
        for (;;) {
            if (--y < 0) return 0xFF;
            a = static_cast<std::uint8_t>(a - 0x0C);
            if (a == 0) break;
        }
        std::uint8_t s = r(static_cast<std::uint16_t>(0xB6 + y));
        set_object(0x70, s, static_cast<std::uint8_t>(y));
        const std::uint8_t pose = r(0x42E);
        r(static_cast<std::uint16_t>(0x5C0 + s)) = static_cast<std::uint8_t>(r(0x5CE) + rd(0xA147 + pose));
        r(static_cast<std::uint16_t>(0x5E0 + s)) = static_cast<std::uint8_t>(r(0x5EE) + rd(0xA14C + pose));
        r(static_cast<std::uint16_t>(0x4A0 + s)) = r(0xBB);
        r(static_cast<std::uint16_t>(0x4C0 + s)) = 0x21;
        sound_command(0x16, s);
        if (r(0xBA) == 0x30) {
            r(0xB4) = 0; r(0xBC) = 0;
            if (r(0x42E) != 0x8C) r(0x42E) = 0x88;
        }
        return pose;
    }
reset:
    r(0xB0) = 0;
    std::uint8_t s = x;
    set_object(0x55, s, y_in);
    return y_in;
}
// $1:A000: stage 2's boss (object $0E, poses $90-$96): enters to line $40, walks at random ($A169) within X
// $20-$DF / Y $28-$8F; every $56 frames ($BA from $A9) two homing shots ($5C/$5D from $A177-$A17C, task 2);
// hit ($042E >= $95) $E1F9.
Routine Engine::boss_2() {
    nmi_on();
    co_await boss_intro();
    start_task(2, 0xA17F);
    std::uint8_t x = 0x0E, y = palette_y_;
    set_object(0x58, x, y);
    r(0x5CE) = 0;
    r(0x5EE) = m.read(static_cast<std::uint16_t>(0xA0E3 + (r(0xAC) & 3)));
    r(0xB5) = 1;
    do { co_await wait(); y = boss_2_move(x, y); } while (r(0x5CE) != 0x40);
    for (;;) {
        const std::uint8_t pose = r(0x42E);
        if (pose >= 0x95) {
            r(0x07) = 0x58; r(0x08) = 0x96;
            if (boss_hit(y)) { co_await boss_end(); co_return; }
            if (r(0x42E) == 0x90) {
                r(0x58E) = 0;
                std::uint8_t p = 0x93, d = 0x88;
                if (r(0x5EE) & 0x80) { p = 0x94; d = 0x98; }
                r(0x42E) = p; r(0x4AE) = d; y = d;
                r(0xBA) = 0xA9;
            }
            co_await wait();
            continue;
        }
        if (pose != 0x90) { // $A1F9
            if (pose == 0x92 && r(0x58E) == 2) { r(0x42E) = 0x90; r(0x40E) = 0x98; }
            y = move(x);
        } else {
            if (r(0xB4) == 0) {
                r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAF));
                r(0x4AE) = m.read(static_cast<std::uint16_t>(0xA169 + (r(0xAE) & 0x0F)));
                r(0xB4) = static_cast<std::uint8_t>(0x18 * ((r(0xAE) & 3) + 1));
                y = 0xFF;
            }
            y = boss_2_move(x, y);
        }
        const std::uint8_t by = r(0x5CE), bx = r(0x5EE);
        if (by < 0x28 || by >= 0x90 || bx < 0x20 || bx >= 0xE0) {
            r(0x4AE) = static_cast<std::uint8_t>(r(0x4AE) + 0x10);
            y = move(x);
            if (r(0x42E) >= 0x93) { set_object(0x5A, x, y); r(0x4AE) = 0xC0; }
        }
        y = boss_2_fire(x, y);
        co_await wait();
    }
}
// $1:A0E8: a step while walking ($B5: 8 frames moving, 4 standing).
std::uint8_t Engine::boss_2_move(std::uint8_t x, std::uint8_t y) {
    --r(0xB4); --r(0xB5);
    if ((r(0xB5) & 0x7F) == 0) r(0xB5) = (r(0xB5) & 0x80) ? 0x04 : 0x88;
    if (r(0xB5) & 0x80) return y;
    return move(x);
}
// $1:A105: at $BA = $E6 the boss stands ($59); at 0 it fires: the pose by its side, two shots (objects $5C/$5D in
// slots 2/3 at $A177/$A179 from it, aimed at ($A17B, $B0)).
std::uint8_t Engine::boss_2_fire(std::uint8_t x, std::uint8_t y) {
    if (++r(0xBA) != 0) {
        if (r(0xBA) != 0xE6) return y;
        r(0x4AE) = 0xC0;
        set_object(0x59, x, y);
        return y;
    }
    std::uint8_t d = 0x84; y = 0x93;
    if (r(0x5EE) & 0x80) { d = 0x9C; y = 0x94; }
    r(0x42E) = y; r(0x58E) = 0; r(0x4AE) = d;
    for (std::uint8_t k = 2; k != 4; ++k) {
        std::uint8_t s = k;
        set_object(static_cast<std::uint8_t>(k + 0x5A), s, y);
        r(static_cast<std::uint16_t>(0x5C0 + k)) = static_cast<std::uint8_t>(r(0x5CE) + m.read(static_cast<std::uint16_t>(0xA177 + k)));
        r(static_cast<std::uint16_t>(0x5E0 + k)) = static_cast<std::uint8_t>(r(0x5EE) + m.read(static_cast<std::uint16_t>(0xA179 + k)));
        r(static_cast<std::uint16_t>(0x4E0 + k)) = aim_at(k, 0xB0, m.read(static_cast<std::uint16_t>(0xA17B + k)));
        y = aim_y_;
        r(static_cast<std::uint16_t>(0x540 + k)) = 0;
    }
    sound_command(0x1D, x);
    return y;
}
// $1:A17F: the two homing shots (slots 2/3): solid after 30 frames ($0540); turning toward their target $04E0 on
// even frames; past line $B0 they home on the player's place then ($BA/$BC + x), up to step 4 ($04C0).
Routine Engine::boss_2_shots() {
    nmi_on();
    for (;;) {
        for (std::uint8_t x = 2; x != 4; ++x) {
            const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
            if (!(o(0x400) & 0x80)) continue;
            if (++o(0x540) >= 0x1E) o(0x400) = 0x98;
            const std::uint8_t a = o(0x4C0);
            if (a != 0) {
                if (a & 1) {
                    o(0x4E0) = aim_at(x, r(static_cast<std::uint16_t>(0xBA + x)), r(static_cast<std::uint16_t>(0xBC + x)));
                    ++o(0x4C0);
                }
                if ((o(0x4A0) & 0x1F) == o(0x4E0)) { if (++o(0x4C0) >= 5) --o(0x4C0); }
            }
            if (!(r(0xA0) & 1)) { r(0x37) = o(0x4E0); turn(x); }
            move(x);
            if (o(0x4C0) == 0 && o(0x5C0) >= 0xB0) {
                ++o(0x4C0);
                r(static_cast<std::uint16_t>(0xBA + x)) = r(0x71); r(static_cast<std::uint16_t>(0xBC + x)) = r(0x74);
            }
        }
        co_await wait();
    }
}
// $2:A000: stage 3's boss (poses $9A-$A0): enters to line $40, walks at random ($A1C2) within X $20-$DF / Y
// $30-$8F (else it turns to (128, 96) and arcs); every 48 frames by chance ($AD += $AE) it stops to fire a fan of
// three ($9B: $8C/$8E/$90 by the aim) or arcs ($A1D2/$A1E2, $B7 frames) and fires a fan of five at the turn.
Routine Engine::boss_3() {
    nmi_on();
    co_await boss_intro();
    start_task(2, 0xA1EE);
    std::uint8_t x = 0x0E, y = palette_y_;
    set_object(0x5E, x, y);
    r(0x5CE) = 0;
    r(0x5EE) = m.read(static_cast<std::uint16_t>(0xA1BE + (r(0xAC) & 3)));
    r(0xB5) = 1;
    do { co_await wait(); y = boss_2_move(x, y); } while (r(0x5CE) != 0x40);
    for (;;) {
        const std::uint8_t pose = r(0x42E);
        bool tick = false;
        if (pose >= 0x9F) {
            r(0x07) = 0x5E; r(0x08) = 0xA0;
            if (boss_hit(y)) { co_await boss_end(); co_return; }
        } else if (pose == 0x9B) {
            const std::uint8_t phase = r(0x58E);
            if (phase == 1) {
                if (r(0x5AE) == 0) {
                    const std::uint8_t a = aim(x); y = aim_y_;
                    if (a >= 8 && a < 0x19) {
                        y = 0x8C;
                        if (a >= 0x0F) y = a >= 0x12 ? 0x90 : 0x8E;
                        r(0xBF) = 0x24;
                        y = boss_3_fan(4, y);
                    }
                }
            } else if (phase == 2) { r(0x40E) = 0x98; r(0x42E) = 0x9A; }
        } else if (pose >= 0x9C) {
            if (pose != 0x9E) {
                if (r(0x58E) == 2) { r(0x42E) = pose == 0x9C ? 0x9E : 0x9A; r(0x58E) = 0; }
            } else if (--r(0xB7) & 0x80) {
                set_object(0x60, x, y);
                tick = true;
            } else {
                const std::uint8_t k = static_cast<std::uint8_t>(r(0xB7) >> 1);
                const std::uint8_t a = m.read(static_cast<std::uint16_t>((r(0xB8) >= 0x12 ? 0xA1E2 : 0xA1D2) + k));
                move_by(x, a);
                y = move(x);
                if (r(0xB7) == r(0xB8)) {
                    const std::uint8_t d = aim(x); y = aim_y_;
                    if (d >= 0x0C && d < 0x15) { // $A211
                        r(0xBF) = 0x2D;
                        y = 0x8C;
                        if (r(0x4AE) < 0x49 || r(0x4AE) >= 0x58) y = boss_3_fan(6, y);
                    }
                }
            }
        } else {
            if (r(0xB4) == 0) {
                r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAC));
                r(0x4AE) = m.read(static_cast<std::uint16_t>(0xA1C2 + (r(0xAE) & 0x0F)));
                r(0xB4) = static_cast<std::uint8_t>(0x18 * ((r(0xAE) & 3) + 1));
                y = 0xFF;
            }
            y = boss_2_move(x, y);
            const std::uint8_t by = r(0x5CE), bx = r(0x5EE);
            if (by < 0x30 || by >= 0x90 || bx < 0x20 || bx >= 0xE0) {
                set_object(0x5F, x, y);
                r(0x4AE) = aim_at(x, 0x60, 0x80) | 0x40; y = aim_y_;
                r(0xB7) = 0x20; r(0xB8) = 0x18;
            }
            tick = true;
        }
        if (tick) y = boss_3_tick(x, y);
        co_await wait();
    }
}
// $2:A175: every 48 frames ($B6) by chance ($AD += $AE: 0 nothing, 1-8 the fan stand $9B, else the arc $5F up or
// down by the line, $B7 = 24, $B8 = $12).
std::uint8_t Engine::boss_3_tick(std::uint8_t x, std::uint8_t y) {
    if (++r(0xB6) != 0x30) return y;
    r(0xB6) = 0;
    r(0xAD) = static_cast<std::uint8_t>(r(0xAD) + r(0xAE));
    const std::uint8_t a = r(0xAD) & 0x0F;
    if (a < 1) return y;
    if (a < 9) { r(0x42E) = 0x9B; r(0x58E) = 0; r(0x5AE) = 0; return y; }
    set_object(0x5F, x, y);
    y = r(0x5CE) >= 0x58 ? 0x40 : 0xC0;
    r(0x4AE) = y;
    r(0xB7) = 0x18; r(0xB8) = 0x12;
    return y;
}
std::uint8_t Engine::boss_3_fan(std::uint8_t x, std::uint8_t y) {
    do {
        std::uint8_t s = x;
        set_object(0x62, s, y);
        r(static_cast<std::uint16_t>(0x5C0 + x)) = r(0x5CE); r(static_cast<std::uint16_t>(0x5E0 + x)) = r(0x5EE);
        r(static_cast<std::uint16_t>(0x4A0 + x)) = y;
        y = static_cast<std::uint8_t>(y + 2);
        r(static_cast<std::uint16_t>(0x4C0 + x)) = r(0xBF);
    } while (--x != 1);
    sound_command(0x1F, x);
    return y;
}
// $2:A1EE: the fans' shots (slots 2-6) moved, gone after $04C0 frames.
Routine Engine::boss_3_shots() {
    nmi_on();
    for (;;) {
        for (std::uint8_t x = 2; x != 7; ++x) {
            if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) continue;
            move(x);
            if (--r(static_cast<std::uint16_t>(0x4C0 + x)) == 0) r(static_cast<std::uint16_t>(0x400 + x)) >>= 1;
        }
        co_await wait();
    }
}
// $3:A000: stage 4's boss (poses $A8-$AD): appears at one of four places ($A1DC/$A1E0 by the player's line and
// $AC), throws dynamite ($A9: object $6C in slot 2, aimed $0C-$14) and moves in arcs ($BD) toward random points or
// the player, firing kind 3 on the way; hit ($AC on) $E1F9, then it reappears ($AB, $B1).
Routine Engine::boss_4() {
    nmi_on();
    co_await boss_intro();
    start_task(2, 0xA23C);
    const auto dist = [](std::uint8_t a, std::uint8_t b) { return a >= b ? static_cast<std::uint8_t>(a - b) : static_cast<std::uint8_t>(b - a); };
    std::uint8_t x = 0x0E, y = palette_y_;
    for (;;) {
        // $A016: (re)appear
        x = 0x0E;
        set_object(static_cast<std::uint8_t>(0x66 + r(0xB1)), x, y);
        r(0xB1) = 0;
        std::uint8_t a = r(0xAC) & 3;
        if (r(0x71) < 0xB0) { a &= 1; if (r(0x71) < 0x68) a |= 2; }
        y = a;
        r(0x5CE) = m.read(static_cast<std::uint16_t>(0xA1DC + y)); r(0x5EE) = m.read(static_cast<std::uint16_t>(0xA1E0 + y));
        sound_command(0x22, x);
        bool again = false;
        while (!again) {
            co_await wait();
            const std::uint8_t pose = r(0x42E);
            y = r(0x58E);
            bool plan = false; // $A0F6
            if (pose >= 0xAC) {
                r(0x07) = 0x68; r(0x08) = 0xAD;
                if (boss_hit(y)) { co_await boss_end(); co_return; }
                r(0xB1) = 1;
            } else if (pose == 0xAA) { if (y == 5) plan = true; }
            else if (pose == 0xAB) { if (y == 0x0A) again = true; }
            else if (pose == 0xA9) {
                if (y == 1) {
                    if (r(0x5AE) == 0) {
                        const std::uint8_t d = aim(x); y = aim_y_;
                        if (d >= 0x0C && d < 0x15) {
                            r(0x4A2) = d | 0x40;
                            std::uint8_t s = 2;
                            set_object(0x6C, s, y);
                            r(0x5C2) = r(0x5CE); r(0x5E2) = r(0x5EE); r(0x4C2) = 0x28;
                        }
                    }
                } else if (y == 2) plan = true;
            } else if (pose != 0xA8) plan = true;
            else if (--r(0xB7) & 0x80) plan = true;
            else { // $A0C1: along the arc ($BD/$BE)
                const std::uint16_t arc = static_cast<std::uint16_t>(r(0xBD) | r(0xBE) << 8);
                move_by(x, m.read(static_cast<std::uint16_t>(arc + (r(0xB7) >> 1))));
                y = move(x);
                if (r(0xB9) == 2 && (r(0x5AE) == 0x0C || r(0x5AE) == 0x24)) {
                    r(0xA8) = 3; r(0xF4) = 0x40;
                    const std::uint8_t d = aim(x) | 0x40; y = aim_y_;
                    enemy_shot(x, d);
                    sound_command(0x1E, x);
                }
            }
            if (!plan) continue;
            if (r(0xB9) == 2) r(0xB9) = 0;
            if (r(0xB9) == 0) {
                const std::uint8_t by = r(0x5CE), bx = r(0x5EE);
                if (by < 0x38 || by >= 0xA0 || bx < 0x50 || bx >= 0xD0) { set_object(0x68, x, y); sound_command(0x22, x); continue; }
            }
            y = boss_4_plan(x, y);
            (void)dist;
        }
    }
}
// $3:A131: the next move: (with $B8, by chance, when no dynamite flies) a throw ($6A); else the walk pose ($69) and
// either a short random arc ($A1E8, $AE & $E0 set) or an arc to the player's place ($B9 = 1, 24 lines above it) /
// a random corner ($B9 = 2): the arc table ($A224/$A22C/$A234) by the larger distance / 16.
std::uint8_t Engine::boss_4_plan(std::uint8_t x, std::uint8_t y) {
    const auto dist = [](std::uint8_t a, std::uint8_t b) { return a >= b ? static_cast<std::uint8_t>(a - b) : static_cast<std::uint8_t>(b - a); };
    if (r(0xB9) == 0) {
        if (r(0xB8) && !(r(0x402) & 0x80)) {
            r(0xAD) = static_cast<std::uint8_t>(r(0xAD) + r(0xAC));
            if ((r(0xAD) & 0x0F) >= 6) { r(0xB8) = 0; set_object(0x6A, x, y); return y; }
        }
        set_object(0x69, x, y);
        r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAC));
        if (r(0xAE) & 0xE0) {
            r(0x4AE) = (r(0xAE) & 0x1C) | 0x40;
            r(0xB7) = 0x10; r(0xB8) = 0x10;
            r(0xBD) = 0xE8; r(0xBE) = 0xA1;
            return y;
        }
        ++r(0xB9);
        r(0xBB) = static_cast<std::uint8_t>(r(0x71) - 0x18); r(0xBC) = r(0x74);
        if (r(0xBC) == 0) goto corner;
    } else {
    corner: // $A18B
        ++r(0xB9);
        r(0x5AE) = 0;
        y = r(0xAE) & 3;
        r(0xBB) = m.read(static_cast<std::uint16_t>(0xA1DC + y)); r(0xBC) = m.read(static_cast<std::uint16_t>(0xA1E0 + y));
    }
    r(0xBF) = dist(r(0xBB), r(static_cast<std::uint16_t>(0x5C0 + x)));
    std::uint8_t a = dist(r(0xBC), r(static_cast<std::uint16_t>(0x5E0 + x)));
    if (a < r(0xBF)) a = r(0xBF);
    a = static_cast<std::uint8_t>((a & 0xF0) >> 4);
    if (a >= 8) a = 7;
    y = a;
    r(0xB7) = m.read(static_cast<std::uint16_t>(0xA224 + y));
    r(0xBD) = m.read(static_cast<std::uint16_t>(0xA22C + y)); r(0xBE) = m.read(static_cast<std::uint16_t>(0xA234 + y));
    r(0x4AE) = aim_at(x, r(0xBB), r(0xBC)) | 0x40;
    return aim_y_;
}
// $3:A23C: the dynamite (slot 2) and its pieces (4-7) moved; the dynamite's end: the blast ($6D, sound $1B) and four
// pieces ($70, directions $A28E + x, 40 frames).
Routine Engine::boss_4_shots() {
    nmi_on();
    for (;;) {
        std::uint8_t y = 0;
        for (std::uint8_t x = 2; x != 8; ++x) {
            if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) continue;
            y = move(x);
            if (--r(static_cast<std::uint16_t>(0x4C0 + x)) != 0) continue;
            r(static_cast<std::uint16_t>(0x400 + x)) >>= 1;
            if (x != 2) continue;
            std::uint8_t s = 2;
            set_object(0x6D, s, y);
            sound_command(0x1B, 2);
            for (s = 4; s != 8; ++s) {
                std::uint8_t t = s;
                set_object(0x70, t, y);
                r(static_cast<std::uint16_t>(0x5C0 + s)) = r(0x5C2); r(static_cast<std::uint16_t>(0x5E0 + s)) = r(0x5E2);
                r(static_cast<std::uint16_t>(0x4A0 + s)) = m.read(static_cast<std::uint16_t>(0xA28E + s));
                r(static_cast<std::uint16_t>(0x4C0 + s)) = 0x28;
            }
            break;
        }
        co_await wait();
    }
}
// $4:A000: stage 5's boss (poses $80-$84): enters to line $40, walks at random ($A1EB) within X $30-$CF / Y
// $28-$8F; every $4C frames ($B6) by chance ($AC += $AD) it throws a bomb ($A138: object $52 in slot 2, aimed $0E-$12;
// task 2 bursts it into five pieces at the player's line) or arcs: sideways ($A1FB) when low, else to the player
// ($A1A4, then to (128, 92) and another throw); hit ($83 on) $E1F9.
Routine Engine::boss_5() {
    nmi_on();
    co_await boss_intro();
    start_task(2, 0xA24B);
    std::uint8_t x = 0x0E, y = palette_y_;
    set_object(0x50, x, y);
    r(0x5CE) = 0;
    r(0x5EE) = m.read(static_cast<std::uint16_t>(0xA1E7 + (r(0xAC) & 3)));
    r(0xB5) = 1;
    do { co_await wait(); y = boss_2_move(x, y); } while (r(0x5CE) != 0x40);
    for (;;) {
        if (r(0x42E) >= 0x83) {
            r(0x07) = 0x50; r(0x08) = 0x84;
            if (boss_hit(y)) { co_await boss_end(); co_return; }
        } else {
            r(0x42E) = 0x80;
            if (r(0xB7)) { --r(0xB7); r(0x42E) = 0x82; }
            else if (r(0xB8)) {
                r(0x40E) = 0x9C; r(0x42E) = 0x81;
                if (--r(0xB8) == 0) r(0xB7) = 0x0C;
                const std::uint16_t arc = static_cast<std::uint16_t>(r(0xBD) | r(0xBE) << 8);
                move_by(x, m.read(static_cast<std::uint16_t>(arc + (r(0xB8) >> 1))));
                y = move(x);
            } else if (r(0xB9)) {
                r(0xB6) = 0;
                if (--r(0xB9) == 0) { ++r(0xBC); y = boss_5_throw(x, y); }
                else y = boss_5_arc(x, 0x5C, 0x80); // $A1DF
            } else {
                if (r(0xB4) == 0) {
                    r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAF));
                    r(0x4AE) = m.read(static_cast<std::uint16_t>(0xA1EB + (r(0xAE) & 0x0F)));
                    r(0xB4) = static_cast<std::uint8_t>(0x18 * ((r(0xAE) & 3) + 1));
                    y = 0xFF;
                }
                y = boss_2_move(x, y);
                const std::uint8_t by = r(0x5CE), bx = r(0x5EE);
                if (by < 0x28 || by >= 0x90 || bx < 0x30 || bx >= 0xD0) {
                    if (r(0xBC)) y = boss_5_arc(x, 0x5C, 0x80);
                    else { r(0x4AE) = static_cast<std::uint8_t>(r(0x4AE) + 0x10); y = move(x); }
                } else {
                    r(0xBC) = 0;
                    y = boss_5_tick(x, y);
                }
            }
        }
        co_await wait();
    }
}
std::uint8_t Engine::boss_5_tick(std::uint8_t x, std::uint8_t y) {
    if (++r(0xB6) != 0x4C) return y;
    r(0xB6) = 0;
    r(0xAC) = static_cast<std::uint8_t>(r(0xAC) + r(0xAD));
    const std::uint8_t a = r(0xAC) & 0x0F;
    if (a >= 8) return boss_5_throw(x, y);
    r(0xB7) = 0x18;
    if (a < 2 || r(0x5CE) < 0x48) { r(0xB9) = 2; return boss_5_arc(x, r(0x71), r(0x74)); }
    r(0xB8) = 0x10; r(0xB9) = 1;
    r(0xBD) = 0xFB; r(0xBE) = 0xA1;
    y = (r(0x5EE) & 0x80) ? 0x5C : 0x44;
    r(0x4AE) = y;
    return y;
}
std::uint8_t Engine::boss_5_throw(std::uint8_t x, std::uint8_t y) {
    r(0x40E) = 0x98;
    const std::uint8_t a = aim(x); y = aim_y_;
    if (a < 0x0E || a >= 0x13) return y;
    std::uint8_t s = 2;
    set_object(0x52, s, y);
    r(0x5C2) = static_cast<std::uint8_t>(r(0x5CE) + 6); r(0x5E2) = static_cast<std::uint8_t>(r(0x5EE) + 0xF8);
    r(0x4A2) = r(0x37) | 0x80;
    sound_command(0x1C, 2);
    return y;
}
// $4:A1A4: an arc toward (tx, ty): the direction with speed bit 7, the arc table ($A233/$A23B/$A243) by the larger
// distance / 16.
std::uint8_t Engine::boss_5_arc(std::uint8_t x, std::uint8_t ty, std::uint8_t tx) {
    const auto dist = [](std::uint8_t a, std::uint8_t b) { return a >= b ? static_cast<std::uint8_t>(a - b) : static_cast<std::uint8_t>(b - a); };
    r(0x07) = ty; r(0x08) = tx;
    r(0x4AE) = aim_at(x, ty, tx) | 0x80;
    r(0xBF) = dist(r(0x07), r(static_cast<std::uint16_t>(0x5C0 + x)));
    std::uint8_t a = dist(r(0x08), r(static_cast<std::uint16_t>(0x5E0 + x)));
    if (a < r(0xBF)) a = r(0xBF);
    a = static_cast<std::uint8_t>((a & 0xF0) >> 4);
    if (a >= 8) a = 7;
    r(0xB8) = m.read(static_cast<std::uint16_t>(0xA233 + a));
    r(0xBD) = m.read(static_cast<std::uint16_t>(0xA23B + a)); r(0xBE) = m.read(static_cast<std::uint16_t>(0xA243 + a));
    return a;
}
// $4:A24B: the bomb and its pieces (slots 2-7): moved; the bomb ($86) bursts at the player's line or when its time
// ends (each object becomes its $0480): five pieces ($53, slots 3-7 at $A2BF/$A2C4 from it, one every 4 frames).
Routine Engine::boss_5_shots() {
    nmi_on();
    std::uint8_t y = 0;
    for (;;) {
        if (r(0xBA) && --r(0xBA) == 0) {
            std::uint8_t s = static_cast<std::uint8_t>(r(0xBB) + 3);
            set_object(0x53, s, y);
            y = r(0xBB);
            r(static_cast<std::uint16_t>(0x5C0 + s)) = static_cast<std::uint8_t>(r(0x5C2) + m.read(static_cast<std::uint16_t>(0xA2BF + y)));
            r(static_cast<std::uint16_t>(0x5E0 + s)) = static_cast<std::uint8_t>(r(0x5E2) + m.read(static_cast<std::uint16_t>(0xA2C4 + y)));
            if (!(--r(0xBB) & 0x80)) r(0xBA) = 4;
        }
        for (std::uint8_t x = 2; x != 8; ++x) {
            if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) continue;
            y = move(x);
            const bool bomb = r(static_cast<std::uint16_t>(0x420 + x)) == 0x86;
            if (--r(static_cast<std::uint16_t>(0x4C0 + x)) != 0 && !(bomb && r(0x5C2) >= r(0x71))) continue;
            if (bomb) { r(0xBA) = 4; r(0xBB) = 4; }
            std::uint8_t s = x;
            set_object(r(static_cast<std::uint16_t>(0x480 + x)), s, y);
        }
        co_await wait();
    }
}
// $5:A000: stage 6's boss (poses $A3-$A6; its second round: $BA, $046E = $0C): enters to line $40, walks at random
// ($A194) within X $20-$DF / Y $28-$61 (else a short dash, $A1A4, back toward (128, 64)), firing aimed shots at the
// end of each step ($A13D); hit ($A5 on) $E1F9 and 120 frames ($B1) unhit.
Routine Engine::boss_6() {
    nmi_on();
    for (int k = 9; k >= 0; --k) r(static_cast<std::uint16_t>(0xB0 + k)) = 0;
    co_await boss_intro();
    start_task(2, 0xA1B4);
    std::uint8_t x = 0x0E, y = palette_y_;
    set_object(0x63, x, y);
    const auto second_round = [&] { if (r(0xBA)) r(0x46E) = 0x0C; }; // $A1D7
    second_round();
    r(0x5CE) = 0;
    r(0x5EE) = m.read(static_cast<std::uint16_t>(0xA190 + (r(0xAC) & 3)));
    r(0xB5) = 1;
    do { co_await wait(); y = boss_6_move(x, y); } while (r(0x5CE) != 0x40);
    const auto outside = [&] { const std::uint8_t by = r(0x5CE), bx = r(0x5EE); return by < 0x28 || by >= 0x62 || bx < 0x20 || bx >= 0xE0; };
    for (;;) {
        const std::uint8_t pose = r(0x42E);
        if (pose >= 0xA5) {
            r(0x07) = 0x63; r(0x08) = 0xA6;
            if (boss_hit(y)) { co_await boss_end(); co_return; }
            second_round();
            r(0xB1) = 0x78;
        } else if (pose == 0xA4) {
            if (--r(0xB7) & 0x80) {
                if (r(0xB0) == 0 || outside()) {
                    r(0x4AE) = aim_at(x, 0x40, 0x80) | 0x40; y = aim_y_;
                    r(0xB7) = 0x10; r(0xB0) = 0x10;
                } else { r(0xB0) = 0; r(0x40E) = 0x98; r(0x42E) = 0xA3; }
            } else {
                move_by(x, m.read(static_cast<std::uint16_t>(0xA1A4 + r(0xB7))));
                y = move(x);
            }
        } else {
            if (r(0xB1) && --r(0xB1) == 0) r(0x40E) = 0x98;
            if (r(0xB4) == 0) {
                r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAF));
                r(0x4AE) = m.read(static_cast<std::uint16_t>(0xA194 + (r(0xAE) & 0x0F)));
                r(0xB4) = static_cast<std::uint8_t>(0x18 * ((r(0xAE) & 3) + 1));
                y = 0xFF;
            }
            y = boss_6_move(x, y);
            if (outside()) {
                set_object(0x64, x, y);
                r(0x4AE) = (r(0x4AE) + 0x10) & 0xDF;
                r(0xB7) = 0x10;
            }
        }
        co_await wait();
    }
}
// $5:A120: a step while walking ($B5); when it stops ($B5 = $84) by chance ($AC += $AD) an aimed shot ($70, kind
// $40 frames) from a free slot 2-7.
std::uint8_t Engine::boss_6_move(std::uint8_t x, std::uint8_t y) {
    --r(0xB4); --r(0xB5);
    if ((r(0xB5) & 0x7F) == 0) r(0xB5) = (r(0xB5) & 0x80) ? 0x04 : 0x88;
    if (!(r(0xB5) & 0x80)) return move(x);
    if (r(0xB5) != 0x84) return y;
    r(0xAC) = static_cast<std::uint8_t>(r(0xAC) + r(0xAD));
    if ((r(0xAC) & 3) == 0) return y;
    const std::uint8_t a = aim(x); y = aim_y_;
    if (a < 0x0C || a >= 0x15) return y;
    std::uint8_t s = 2;
    while (r(static_cast<std::uint16_t>(0x400 + s)) & 0x80) if (++s == 8) return y;
    std::uint8_t t = s;
    set_object(0x70, t, y);
    r(static_cast<std::uint16_t>(0x5C0 + s)) = static_cast<std::uint8_t>(r(0x5CE) + 6);
    r(static_cast<std::uint16_t>(0x5E0 + s)) = static_cast<std::uint8_t>(r(0x5EE) + 0xF8);
    r(static_cast<std::uint16_t>(0x4A0 + s)) = r(0x37) | 0x40;
    r(static_cast<std::uint16_t>(0x4C0 + s)) = 0x40;
    sound_command(0x16, s);
    return y;
}
// $5:A1B4: the shots (slots 2-7) moved, gone after $04C0 frames.
Routine Engine::boss_6_shots() {
    nmi_on();
    for (;;) {
        for (std::uint8_t x = 2; x != 8; ++x) {
            if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) continue;
            move(x);
            if (--r(static_cast<std::uint16_t>(0x4C0 + x)) == 0) r(static_cast<std::uint16_t>(0x400 + x)) >>= 1;
        }
        co_await wait();
    }
}
// $0:A1E5: the boss's four shots ($B6-$B9): moved, gone after $04C0 frames.
Routine Engine::boss_1_shots() {
    nmi_on();
    for (;;) {
        for (r(0xBE) = 3; !(r(0xBE) & 0x80); --r(0xBE)) {
            const std::uint8_t x = r(static_cast<std::uint16_t>(0xB6 + r(0xBE)));
            if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) continue;
            if (--r(static_cast<std::uint16_t>(0x4C0 + x)) == 0) r(static_cast<std::uint16_t>(0x400 + x)) >>= 1;
            move(x);
        }
        co_await wait();
    }
}
// $E1F9: the boss hit (object $0E, $07 its object list, $08 its hit pose): while it has strength ($B2) it flashes
// ($A9) and loses a point when the flash ends ($05AE >= 9: its bar $0300 shorter); at 0 the death pose and a bounce
// ($E28D) for 9 frames ($B3), then beaten.
bool Engine::boss_hit(std::uint8_t& y_register) {
    const std::uint8_t x = 0x0E;
    if (r(0xB2)) {
        if (r(0xA9) == 0) { ++r(0xA9); sound_command(0x2D, x); return false; }
        if (r(0x5AE) < 9) return false;
        --r(0xB2); --r(0x300); --r(0xA9);
        std::uint8_t s = x;
        set_object(r(0x07), s, y_register);
        return false;
    }
    r(0x302) = 0x57;
    const std::uint8_t y = r(0xB3);
    y_register = y;
    if (y == 0) sound_command(0x29, x);
    if (y == 9) return true;
    r(0x42E) = r(0x08);
    ++r(0xB3);
    if (r(0x41) != 3) r(0x5CE) = static_cast<std::uint8_t>(r(0x5CE) + m.read(static_cast<std::uint16_t>(0xE28D + y)) + (r(0x41) >= 3 ? 1 : 0));
    return false;
}
// $E244: the beaten boss's last pose, the enemies cleared, the poster and the bar gone, a second; then the stage is
// won ($4B = 2), task 2 stopped and task 1 ends ($C07F, $C0A0).
Routine Engine::boss_end() {
    if (r(0x41) == 3) r(0x5CE) = static_cast<std::uint8_t>(r(0x5CE) + 0x10);
    r(0x40E) = 0x80;
    r(0x42E) = static_cast<std::uint8_t>(r(0x08) + 1);
    clear_enemies(static_cast<std::uint8_t>(r(0x08) + 1));
    r(0x4A) = 0; r(0x49) = 0; r(0x7B) = 0xFF;
    co_await wait(0x3C);
    if (r(0x41) == 5 && r(0xBA) == 0) { // $E279: stage 6's boss comes back once, stronger
        ++r(0xBA);
        co_await wait(0x3C);
        co_await boss_6();
        co_return;
    }
    r(0x4B) = 2;
    r(0x18) = 0;                                            // $C07F with A = 2: task 2 (the shots) stopped
    r(static_cast<std::uint16_t>(0x10 + 4 * current_)) = 0; // $C0A0: this task ends
    co_await Stop{}; // never resumed
}
// $CDAB: every enemy ($10-$16, kind $50 on) scored and turned into its death object; the enemy shots gone.
void Engine::clear_enemies(std::uint8_t y) {
    for (std::uint8_t x = 0x10; x != 0x17; ++x) {
        if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80) || r(static_cast<std::uint16_t>(0x420 + x)) < 0x50) continue;
        y = score(x, y);
        die(r(static_cast<std::uint16_t>(0x480 + x)), x, y);
    }
    for (unsigned k = 0x18; k < 0x20; ++k) r(static_cast<std::uint16_t>(0x400 + k)) = 0;
}
// $0:B932: the poster again, faded in; the jingle; 3 seconds; six shots into it (frame $11 at $B9E8/$BA00 from one
// of four sets by $AC, the gunshot sound, waits $BA18); "BOUNTY" typed ($BA30) and the stage's amount ($BA74/$BA7A);
// a second; 35 blinks of the whole picture; faded out; the objects off but the player; the bounty's score ($E2E9[$41
// + $0A]); the next stage ($E757; after stage 6 the ending $E8DB).
Routine Engine::stage_won() {
    co_await poster();
    co_await fade(0x30, 0xF0, 2);
    sound_command(1, fade_x_); // X as the fade left it
    std::uint8_t n = 0xB4; co_await wait_frames(n);
    r(0xA3) = 0x16;
    load_palette();
    r(0xA1) = 0x10;
    r(0x07) = 5;
    r(0xF3) = static_cast<std::uint8_t>((r(0xAC) & 3) << 1);
    r(0x08) = static_cast<std::uint8_t>(r(0xF3) * 3);
    do {
        const std::uint8_t x = r(0x08);
        r(0x36) = m.read(static_cast<std::uint16_t>(0xB9E8 + x)); r(0x37) = m.read(static_cast<std::uint16_t>(0xBA00 + x));
        r(0xF4) = 0; draw_frame(0x11);
        sound_command(0x21, r(0xA1));
        n = m.read(static_cast<std::uint16_t>(0xBA18 + r(0x08)));
        co_await wait_frames(n);
        ++r(0x08);
    } while (!(--r(0x07) & 0x80));
    std::uint8_t x = 0x16;
    r(0x04) = 0x0C; r(0x03) = x;
    inline_pointer(0xB988); r(0x02) = r(0x01); r(0x01) = r(0x00); // $C2CA: the text after the JSR at $B988
    x = 0x2C; // X = the stack pointer after $C134 (task 0 here; read off the original)
    co_await type_text(0x0C, 0x16, 0xBA30, x);
    const std::uint8_t stage = r(0x41);
    const std::uint16_t amount = static_cast<std::uint16_t>(m.read(static_cast<std::uint16_t>(0xBA74 + stage)) | m.read(static_cast<std::uint16_t>(0xBA7A + stage)) << 8);
    x = 0x16;
    co_await type_text(0x12, 0x16, amount, x);
    n = 0x3C; co_await wait_frames(n);
    for (std::uint8_t y = 0x23; y; --y) {
        r(0xFE) = 0x1E; co_await wait();
        r(0xFE) = 0; co_await wait();
    }
    co_await fade(0x10, 0x10, 1);
    r(0xFC) = 0; r(0xFD) = 0; r(0x77) = 0;
    for (unsigned k = 0x1F; k; --k) r(static_cast<std::uint16_t>(0x400 + k)) >>= 1;
    r(0x417) = 0x80;
    add_score(static_cast<std::uint8_t>(stage + 0x0A), 0);
    if (++r(0x41) == 6) { co_await ending(); co_return; }
    co_await stage_load();
}
// $C2B8: the typewriter from ($01): a count, then the text, at the address already in $036A/$036B.
Routine Engine::type_here(std::uint8_t delay, std::uint8_t sound, std::uint8_t& x) {
    r(0x04) = delay; r(0x03) = sound; r(0x36C) = 1;
    const std::uint16_t text = static_cast<std::uint16_t>(r(0x01) | r(0x02) << 8);
    std::uint8_t y = 0;
    r(0x06) = m.read(text);
    do {
        ++y;
        const std::uint8_t c = m.read(static_cast<std::uint16_t>(text + y));
        r(0x36D) = c;
        if (c != 0xF4) {
            r(0x3A) = y;
            sound_command(r(0x03), x);
            x = r(0x04);
            co_await wait_frames(x);
        }
        ++r(0x36B);
    } while (--r(0x06));
}
// $F571: the window over the play field (a shop: $A5, from $CC51; or the status with Select): the play paused
// ($AA, the other tasks held: $25 = $E0), the status sprites ($FA15), the row buffer saved, the map rows four half
// rows back ($F73E); then 9 frames of two window rows each ($F788: borders $F7A0/$F7D0, a middle $F7B8 filled by
// $F7E8 (status) or $F8D7 (shop)) uploaded by the NMI ($3D) with their attributes ($39); the status ($0:B800) or the
// shop ($0:BD4E); then the rows drawn again ($F6C5), the music back.
Routine Engine::open_window() {
    ++r(0xAA);
    status_sprites(true);
    r(0x25) = 0xE0;
    sound_command(0x2C, r(0xA1));
    co_await wait();
    save_rows();
    r(0xA6) = 0; r(0x09) = 0; r(0x07) = 8;
    for (int i = 0; i < 4; ++i) row_back();
    row_attributes();
    for (int y = 5; y >= 0; --y) r(static_cast<std::uint16_t>(0x36A + y)) = r(static_cast<std::uint16_t>(0x346 + y));
    co_await wait();
    for (;;) {
        row_back();
        for (r(0x08) = 2; r(0x08); --r(0x08)) {
            const std::uint8_t a = r(0xA6);
            if (a == 0) window_row(0xF5B7);           // $F7A0: the top border
            else if (a == 0x11) window_row(0xF5C3);   // $F7D0: the bottom border
            else {
                window_row(0xF5CB);                   // $F7B8: a middle row
                if (r(0xA5) == 0) status_row(); else shop_row();
            }
            ++r(0xA6);
        }
        row_attributes();
        std::uint8_t a;
        const auto blank = [&] { for (int y = 5; y >= 0; --y) r(static_cast<std::uint16_t>(0x345 + y)) = 0xFF; };
        bool attributes = true;
        if (r(0x58) == 0x80 && ((r(0x59) & 3) ^ 3) == 0) blank();
        else if (r(0x07) == 8 && r(0x0F) != 0) {
            for (int y = 5; y >= 0; --y) r(static_cast<std::uint16_t>(0x345 + y)) = r(static_cast<std::uint16_t>(0x36A + y)) | 0xF0;
        } else if (r(0x0F) != 0) attributes = false;
        else if (r(0x07) != 0) blank();
        else {
            row_back(); row_attributes(); next_row();
            for (unsigned y = 0; y < 6; ++y) r(static_cast<std::uint16_t>(0x345 + y)) = r(static_cast<std::uint16_t>(0x346 + y)) | 0x0F;
        }
        if (attributes) { a = 6; r(0x344) = 6; ++r(0x343); r(0x39) = a; }
        else a = r(0x0F);
        r(0x3D) = a;
        co_await wait();
        if (--r(0x07) & 0x80) break;
    }
    select_bank(0);
    if (r(0xA5)) co_await shop(); else co_await status();
    select_bank(r(0x41));
    sound_command(0x2C, r(0x41)); // X = the bank after $C363
    status_sprites(true);
    restore_rows();
    save_rows();
    co_await draw_rows(0x0C, r(0x5C) ? 0x38 : 0x40, true);
    restore_rows();
    r(0x25) = 0; r(0xA5) = 0;
    sound_command(r(0xEA), 0xFF); // X = $FF after $F6A1
    co_await wait();
    --r(0xAA);
}
// $FA15 (hide: from $FA15, else $FA18): the lives sprite at (16, 104), frame 1 or $9D with $0437.
void Engine::status_sprites(bool hide) {
    if (hide) hide_sprites();
    r(0xA1) = 4;
    r(0x36) = 0x10; r(0x37) = 0x68;
    r(0xF4) = 0; draw_frame(r(0x437) ? 0x9D : 1);
}
// $F73E: the map half a row back (the inverse of $E98F).
void Engine::row_back() {
    r(0x5C) ^= 1;
    if (r(0x5C) != 0) {
        bool borrow = r(0x5A) < 8;
        r(0x5A) = static_cast<std::uint8_t>(r(0x5A) - 8); r(0x5B) = static_cast<std::uint8_t>(r(0x5B) - (borrow ? 1 : 0));
        if (r(0x5B) == 0x83) {
            borrow = r(0x5E) < 8;
            r(0x5A) = static_cast<std::uint8_t>(r(0x5E) - 8); r(0x5B) = static_cast<std::uint8_t>(r(0x5F) - (borrow ? 1 : 0));
        }
    }
    const unsigned s = r(0x58) + 0x40u;
    r(0x58) = static_cast<std::uint8_t>(s);
    std::uint8_t a = static_cast<std::uint8_t>(r(0x59) + (s >> 8));
    if ((a == 0x23 || a == 0x2B) && r(0x58) == 0xC0) { a = static_cast<std::uint8_t>((a - 3) ^ 8); r(0x58) = 0; }
    r(0x59) = a;
}
// $F788 (its data after the JSR at call_site): a window row of 24 tiles into $01C0 (even rows) or $01D8.
void Engine::window_row(std::uint16_t call_site) {
    inline_pointer(call_site);
    const std::uint16_t p = static_cast<std::uint16_t>(r(0x00) | r(0x01) << 8);
    std::uint8_t x = (r(0xA6) & 1) ? 0x2F : 0x17;
    for (int y = 0x17; y >= 0; --y, --x) r(static_cast<std::uint16_t>(0x1C0 + x)) = m.read(static_cast<std::uint16_t>(p + y));
}
// $F7E8: the status window's rows: the four guns ($90 + 4k owned: the name $F83C, the shots $91-$93 without leading
// zeros), the text $F87C, a line, the boots / rifle / lives marks ($78, $79, $7A - 1) and the poster ($49).
void Engine::status_row() {
    const std::uint8_t a = r(0xA6);
    if (a == 2 || a == 4 || a == 6 || a == 8) {
        std::uint8_t x = r(0x09);
        if (r(static_cast<std::uint16_t>(0x90 + x))) {
            std::uint8_t y = static_cast<std::uint8_t>(r(0x09) * 3);
            for (unsigned i = 0; i < 0x0C; ++i) r(static_cast<std::uint16_t>(0x1C5 + i)) = m.read(static_cast<std::uint16_t>(0xF83C + y++));
            x = r(0x09);
            std::uint8_t lead = 0;
            if (r(static_cast<std::uint16_t>(0x91 + x)) != 0xD0) { lead = 1; r(0x1D2) = r(static_cast<std::uint16_t>(0x91 + x)); }
            const std::uint8_t d = r(static_cast<std::uint16_t>(0x92 + x));
            if (lead || d != 0xD0) r(0x1D3) = d;
            r(0x1D4) = r(static_cast<std::uint16_t>(0x93 + x));
        }
        r(0x09) = static_cast<std::uint8_t>(r(0x09) + 4);
        return;
    }
    if (a == 0x0A) { for (int x = 0x0F; x >= 0; --x) r(static_cast<std::uint16_t>(0x1C5 + x)) = m.read(static_cast<std::uint16_t>(0xF87C + x)); return; }
    if (a == 0x0D) { for (int x = 0x13; x >= 0; --x) r(static_cast<std::uint16_t>(0x1DA + x)) = 0xF9; return; }
    if (a != 0x0F) return;
    std::uint8_t x = 6;
    for (std::uint8_t y = r(0x78); y; --y) r(static_cast<std::uint16_t>(0x1D8 + x--)) = 0xFB;
    x = 0x0C;
    for (std::uint8_t y = r(0x79); y; --y) r(static_cast<std::uint16_t>(0x1D8 + x--)) = 0xFC;
    std::uint8_t y = static_cast<std::uint8_t>(r(0x7A) - 1);
    if (y) { x = 0x12; do r(static_cast<std::uint16_t>(0x1D8 + x--)) = 0xFD; while (--y); }
    if (r(0x49)) r(0x1EC) = 0xCE;
}
// $F8D7: the shop window's rows: the shopkeeper (frame $61 + kind at row 3; the window's VRAM rows kept in $0C/$0D
// and $0A/$0B), the goods' names ($F995) or, for the poster shop ($A5 = $41), its texts ($F9D9) with the price
// marks (frames $26/$1C/$27) and the stage's price ($FA03).
void Engine::shop_row() {
    const std::uint8_t a = r(0xA6);
    if (a == 3) {
        r(0x0C) = r(0x58); r(0x0D) = r(0x59);
        r(0x36) = static_cast<std::uint8_t>(0x50 + r(0x61)); r(0x37) = 0x50;
        r(0xF4) = 0; draw_frame(static_cast<std::uint8_t>((r(0xA7) & 0x7F) + 0x61));
        return;
    }
    if (a == 4) { r(0x0A) = r(0x58); r(0x0B) = r(0x59); return; }
    if (r(0xA5) != 0x41) {
        if (a == 7 || a == 9 || a == 0x0B || a == 0x0D) {
            std::uint8_t x = r(0x09);
            for (unsigned y = 0; y < 0x11; ++y) r(static_cast<std::uint16_t>(0x1DD + y)) = m.read(static_cast<std::uint16_t>(0xF995 + x++));
            r(0x09) = x;
        }
        return;
    }
    r(0xF2) = 0;
    if (a == 0x0A) ++r(0xF2);
    else if (a != 7 && a != 0x0D) return;
    r(0x36) = static_cast<std::uint8_t>(a * 8 + 0x38 + r(0x61)); r(0x37) = 0x50;
    std::uint8_t frame = 0x26;
    if (r(0x09)) frame = r(0x09) == 0x0E ? 0x1C : 0x27;
    r(0xF4) = 0; draw_frame(frame);
    std::uint8_t x = r(0x09);
    for (unsigned y = 0; y < 0x0E; ++y, ++x) {
        const std::uint8_t c = m.read(static_cast<std::uint16_t>(0xF9D9 + x));
        r(static_cast<std::uint16_t>(0x1E0 + y)) = c;
        if (--r(0xF2) == 0) { ++r(0xF2); r(static_cast<std::uint16_t>(0x1C8 + y)) = c; }
    }
    r(0x09) = x;
    if (r(0x09) == 0x2A) {
        x = static_cast<std::uint8_t>(r(0x41) * 3);
        for (unsigned y = 0; y < 3; ++y) r(static_cast<std::uint16_t>(0x1E9 + y)) = m.read(static_cast<std::uint16_t>(0xFA03 + x++));
    }
}
// $0:B800: the status: a gun chosen with Up/Down ($07 over the owned ones with shots, 4 = none), Select ends it
// (the gun $88 from $F1DD; 0 becomes 1 with the rifle).
Routine Engine::status() {
    sound_command(2, 0);
    r(0x07) = m.read(static_cast<std::uint16_t>(0xF1D7 + r(0x88)));
    std::uint8_t x = 0;
    do { status_step(x); co_await wait(); } while (r(0xF5) != 0x20);
    r(0x88) = m.read(static_cast<std::uint16_t>(0xF1DD + r(0x07)));
    if (r(0x88) == 0 && r(0x79)) ++r(0x88);
}
// $0:B828 (x: the original's X, kept for the sound's $F3).
void Engine::status_step(std::uint8_t& x) {
    cursor(true);
    std::uint8_t a = r(0xF5) & 0x0C;
    if (a) a = static_cast<std::uint8_t>(((a ^ 0x0C) >> 1) - 3);
    r(0xF3) = a;
    r(0xF4) = r(0x07);
    for (;;) {
        a = static_cast<std::uint8_t>(r(0x07) + r(0xF3));
        if (a & 0x80) a = 4;
        if (a == 5) a = 0;
        r(0x07) = a;
        if (a == 4) break;
        x = static_cast<std::uint8_t>(a << 2);
        if (r(static_cast<std::uint16_t>(0x90 + x)) == 0) continue;
        if (static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x91 + x)) + r(static_cast<std::uint16_t>(0x92 + x)) + r(static_cast<std::uint16_t>(0x93 + x))) == 0x70) continue;
        break;
    }
    if (r(0x07) != r(0xF4)) sound_command(0x2B, x);
}
// $0:B871 / $0:B875 (wide: the status's 16-line steps): the arrow sprite $02E0 at line $3F + $61 + $07 * 4 (* 16),
// blinking.
void Engine::cursor(bool wide) {
    unsigned v = r(0x07);
    v = (v << (wide ? 4 : 2)) & 0xFF;
    const unsigned s = v + 0x3F;
    r(0x2E0) = static_cast<std::uint8_t>((s & 0xFF) + r(0x61) + (s >> 8));
    r(0x2E1) = 0x79;
    r(0x2E2) = (r(0x22) & 4) >> 2;
    r(0x2E3) = 0x38;
}
// $0:BD4E: a shop ($A5: 1 the gun shop, else goods; bit 6: three items): the greeting and the goods typed
// ($BF1A by $0E, at the window's rows $0C/$0D + $2D and $0A/$0B + $0D); Up/Down choose ($08), A buys when the score
// allows (its digits against the price $BEC7), Select leaves. Paid in steps of $E2E9[9].
Routine Engine::shop() {
    std::uint8_t x = 0;
    sound_command(3, x);
    r(0x08) = 0;
    std::uint8_t y = 4; x = 0;
    if (r(0xA5) & 0x40) { y = 3; x = 4; }
    r(0x09) = y; r(0x0E) = x;
    r(0x36B) = static_cast<std::uint8_t>(r(0x0C) + 0x2D); r(0x36A) = r(0x0D);
    r(0x01) = m.read(static_cast<std::uint16_t>(0xBF1A + x)); r(0x02) = m.read(static_cast<std::uint16_t>(0xBF1B + x));
    x = 0xFE; co_await type_here(8, 0xFE, x);
    std::uint8_t k = r(0x0E) | 2;
    r(0x36B) = static_cast<std::uint8_t>(r(0x0A) + 0x0D); r(0x36A) = r(0x0B);
    r(0x01) = m.read(static_cast<std::uint16_t>(0xBF1A + k)); r(0x02) = m.read(static_cast<std::uint16_t>(0xBF1B + k));
    x = 0xFE; co_await type_here(8, 0xFE, x);
    for (;;) {
        const std::uint8_t a = r(0xF5);
        if (a == 0x80) {
            x = buy(x);
        } else {
            if (a == 0x20) co_return;
            if (a == 0x08) { --r(0x08); sound_command(0x2B, x); }
            else if (a == 0x04) { ++r(0x08); sound_command(0x2B, x); }
        }
        std::uint8_t c = r(0x08); // $BDCC
        if (c & 0x80) { c = static_cast<std::uint8_t>(r(0x09) - 1); r(0x08) = c; }
        if (c == r(0x09)) r(0x08) = 0;
        x = r(0x08);
        r(0x07) = m.read(static_cast<std::uint16_t>((r(0xA5) == 1 ? 0xBEB6 : 0xBEBA) + x));
        cursor(false);
        co_await wait();
    }
}
// $0:BDF7: buying item $08 (returns the original's X).
std::uint8_t Engine::buy(std::uint8_t x) {
    std::uint8_t a = r(0x08);
    if (r(0xA5) != 1) { a |= 4; if (r(0x08) == 2) a = static_cast<std::uint8_t>(a + r(0x41)); }
    r(0x0E) = a;
    x = static_cast<std::uint8_t>(a * 3);
    if (r(0x6F2) == 0x58) { // below 10000: the price's digits against the score's
        for (unsigned y = 0; y < 6; y += 2, ++x) {
            const std::uint8_t d = r(static_cast<std::uint16_t>(0x6F4 + y)), p = m.read(static_cast<std::uint16_t>(0xBEC7 + x));
            if (d < p) return x;
            if (d != p) break;
        }
    }
    std::uint8_t count;
    if (r(0xA5) == 1) {
        const std::uint8_t y = r(0x08);
        x = static_cast<std::uint8_t>(y << 2);
        if (r(static_cast<std::uint16_t>(0x90 + x))) return x;
        for (unsigned i = 0; i < 4; ++i) r(static_cast<std::uint16_t>(0x90 + x + i)) = 0xD0;
        count = m.read(static_cast<std::uint16_t>(0xBEBD + y));
        pay(count, x);
        return r(0xA1);
    }
    x = static_cast<std::uint8_t>(r(0xA5) - 1);
    switch (r(0x08)) {
        case 1: // the shield
            if (r(0x76) == 2) return x;
            r(0x76) = 2;
            count = 0x28;
            break;
        case 2: // the wanted poster
            if (r(0x49)) return x;
            ++r(0x49);
            count = m.read(static_cast<std::uint16_t>(0xBEC1 + r(0x41)));
            break;
        default: { // shots for every gun not full
            int k = 0x0C;
            for (; k >= 0; k -= 4) {
                if (r(static_cast<std::uint16_t>(0x90 + k)) == 0) continue;
                if (r(static_cast<std::uint16_t>(0x91 + k)) != m.read(static_cast<std::uint16_t>(0xCC42 + k)) ||
                    r(static_cast<std::uint16_t>(0x92 + k)) != m.read(static_cast<std::uint16_t>(0xCC43 + k)) ||
                    r(static_cast<std::uint16_t>(0x93 + k)) != m.read(static_cast<std::uint16_t>(0xCC44 + k))) break;
            }
            if (k < 0) return static_cast<std::uint8_t>(k);
            x = static_cast<std::uint8_t>(k);
            for (r(0x0F) = 4; r(0x0F); --r(0x0F)) counters(x);
            count = 3;
            break;
        }
    }
    pay(count, x);
    return r(0xA1);
}
// $0:BE46: the price paid: the sound, count times the amount $E2E9[9] (a negative score step), the lives sprite.
void Engine::pay(std::uint8_t count, std::uint8_t x) {
    r(0x0A) = count;
    sound_command(0x20, x);
    do add_score(9, x); while (--r(0x0A));
    status_sprites(false); // $BE56
}
// $E8DB / $1:B7C0: the ending. The tiles (banks 4 and 5); page 1 on $2800 (script $BA0A, the sprites $BAF9, palette
// $15 faded in over 30 frames each step, the jingle, the text $BD71 typed); page 2 on $2000 ($BB4A, sprites $BC38,
// the text $BE0B) rising 56 lines; the credits scrolled up a line every 3 frames (scripts $BFA8/$BFC4 per 16
// lines, 35 of them); then Start for the next, harder round ($4E; after the second: "$BFEE" and the end).
Routine Engine::ending() {
    nmi_off(); rendering_off();
    select_bank(4); copy_tiles(0x00, 0x80, 0x00, 0xE8EB);
    select_bank(5); copy_tiles(0x00, 0xA0, 0x10, 0xE8FB);
    select_bank(1);
    fill_nametable(0x28, 0xF4, 0x55);
    nmi_on();
    co_await wait();
    r(0x26) = 0x0A; r(0x27) = 0xBA;
    r(0xFE) = 0x1E;
    r(0xFF) = 0x90; m.write(0x2000, 0x90);
    r(0xFD) = 0xF8;
    co_await wait();
    r(0xA1) = 0;
    inline_pointer(0xB7ED); sprite_text(0xBAF9);
    std::uint8_t n = 0x3C; co_await wait_frames(n);
    r(0xA3) = 0x15; r(0xA4) = 0x1E;
    co_await fade(0x30, 0xF0, 0x1E);
    sound_command(0x11, fade_x_);
    r(0x0A) = 0;
    co_await ending_text();
    n = 0x3C; co_await wait_frames(n);
    r(0xA4) = 0x0A;
    co_await fade(0x10, 0x10, 0x0A);
    hide_sprites();
    nmi_off(); rendering_off(); // $C47B
    select_bank(5); copy_tiles(0x00, 0x60, 0x10, 0xC48C);
    select_bank(1);
    fill_nametable(0x20, 0xF4, 0x55);
    nmi_on();
    co_await wait();
    r(0x26) = 0x4A; r(0x27) = 0xBB;
    r(0xFE) = 0x1E; r(0xFD) = 0x08;
    co_await wait();
    r(0xA1) = 0;
    inline_pointer(0xB846); sprite_text(0xBC38);
    r(0xA4) = 0x0A;
    co_await fade(0x30, 0xF0, 0x0A);
    ++r(0x0A);
    co_await ending_text();
    for (r(0x07) = 0x38; r(0x07); --r(0x07)) {
        --r(0xFD);
        for (unsigned x = 0; x < 0x100; x += 4) ++r(static_cast<std::uint16_t>(0x203 + x));
        co_await wait();
    }
    n = 0x3C; co_await wait_frames(n);
    r(0x07) = 0; r(0x08) = 0;
    r(0xA3) = 0x17;
    load_palette();
    for (;;) {
        if (++r(0x08) == 3) {
            r(0x08) = 0;
            ++r(0xFC);
            if ((r(0xFC) & 0x0F) == 0) ++r(0x07);
            if (r(0xFC) == 0xF0) { r(0xF0) ^= 2; r(0xFC) = 0; }
            for (unsigned x = 0; x < 0x100; x += 4) if (r(static_cast<std::uint16_t>(0x200 + x)) != 0xF8) --r(static_cast<std::uint16_t>(0x200 + x));
        }
        const std::uint8_t x = r(0x07);
        if (x == 0x23) break;
        if (x < 0x1C) { r(0x26) = m.read(static_cast<std::uint16_t>(0xBFA8 + x)); r(0x27) = m.read(static_cast<std::uint16_t>(0xBFC4 + x)); }
        co_await wait();
    }
    n = 0x3C; co_await wait_frames(n);
    if (r(0x4E) == 2) {
        std::uint8_t x = 0x16;
        r(0x04) = 0x14; r(0x03) = x;
        inline_pointer(0xB909); r(0x02) = r(0x01); r(0x01) = r(0x00);
        co_await type_text(0x14, 0x16, static_cast<std::uint16_t>(r(0x01) | r(0x02) << 8), x);
        for (;;) co_await wait();
    }
    r(0x26) = 0xE0; r(0x27) = 0xBF;
    do co_await wait(); while (!(r(0xF5) & 0x10));
    r(0xA4) = 0x0A;
    co_await fade(0x10, 0x10, 0x0A);
    r(0xF0) = 0; r(0xFC) = 0; r(0xFD) = 0; r(0x41) = 0;
    ++r(0x4E);
    co_await stage_load(); // $E4C0
}
// $1:B917: the page's text ($0A: page) typed a character every 6 frames from the line pointers ($B9F0) at the
// addresses $B9C0; after each line 10 frames, and from the third line on the lines above written again whole
// (the text rises) - 10 lines on page 1, 12 on page 2.
Routine Engine::ending_text() {
    r(0x07) = 0; r(0x08) = 0;
    for (;;) {
        r(0x36C) = 1;
        ending_address(r(0x07));
        for (;;) {
            ending_pointer(r(0x07));
            std::uint8_t a;
            do {
                ++r(0x36B);
                const std::uint8_t y = r(0x08)++;
                a = m.read(static_cast<std::uint16_t>((r(0xF3) | r(0xF4) << 8) + y));
            } while (a == 0xF4);
            if (a == 0xFF) break;
            r(0x36D) = a; r(0x3A) = a;
            std::uint8_t n = 6; co_await wait_frames(n);
        }
        for (r(0x09) = 0x0A; r(0x09); --r(0x09)) co_await wait();
        const std::uint8_t line = r(0x07)++;
        if (line == (r(0x0A) ? 0x0C : 0x0A)) co_return;
        r(0x08) = 0;
        if (line < 2) continue;
        co_await ending_line(static_cast<std::uint8_t>(line - 1));
        co_await ending_line(static_cast<std::uint8_t>(r(0x07) - 1));
        co_await ending_line(r(0x0A) ? 0x0D : 0x0B);
    }
}
Routine Engine::ending_line(std::uint8_t x) {
    ending_pointer(x);
    const std::uint8_t row = r(0x09)++;
    ending_address(row);
    ++r(0x36B);
    r(0x36C) = r(0x0A) ? 0x0D : 0x19;
    std::uint8_t y = 0, a = 0;
    for (unsigned i = 0; i < 0x19; ++i) {
        r(static_cast<std::uint16_t>(0x36D + i)) = 0xF4;
        a = m.read(static_cast<std::uint16_t>((r(0xF3) | r(0xF4) << 8) + y));
        if (a != 0xFF) { r(static_cast<std::uint16_t>(0x36D + i)) = a; ++y; }
    }
    r(0x3A) = a;
    co_await wait();
}
void Engine::ending_address(std::uint8_t x) {
    if (x >= 2) x = 2;
    const bool page2 = r(0x0A) != 0;
    r(0x36B) = m.read(static_cast<std::uint16_t>((page2 ? 0xB9EA : 0xB9E4) + x));
    r(0x36A) = m.read(static_cast<std::uint16_t>((page2 ? 0xB9ED : 0xB9E7) + x));
}
void Engine::ending_pointer(std::uint8_t x) {
    const bool page2 = r(0x0A) != 0;
    r(0xF3) = m.read(static_cast<std::uint16_t>((page2 ? 0xBE0B : 0xBD71) + x));
    r(0xF4) = m.read(static_cast<std::uint16_t>((page2 ? 0xBE19 : 0xBD7D) + x));
}
// $FBD9: the horse: the player standing ($36), the horse ($3D, slot 1) running in from below to the player's side
// (left, or right when the left is blocked: $77 = 1), the player mounted ($37 + $77); then riding ($77 = 3), 60 frames
// of blinking, the music back ($FE).
Routine Engine::horse_mount() {
    r(0x76) = 0; r(0x62) = 0; r(0x77) = 0;
    std::uint8_t s = 0;
    set_object(0x36, s, draw_y_); // Y after the frame's object scan
    const std::uint8_t y = static_cast<std::uint8_t>(r(0x71) - 7);
    r(0x5C0) = y;
    r(0x5E0) = r(0x74);
    std::uint8_t x = static_cast<std::uint8_t>(r(0x74) - 0x18);
    if (collision(x, y)) { ++r(0x77); x = static_cast<std::uint8_t>(x + 0x30); }
    r(0x5E1) = x;
    r(0x5C1) = 0xF8;
    hide_sprites(); objects();
    std::uint8_t n = 0x14; co_await wait_frames(n);
    s = 1;
    set_object(0x3D, s, draw_y_);
    sound_command(0x24, 1);
    do {
        hide_sprites();
        --r(0x5C1);
        objects();
        co_await wait();
        if (static_cast<std::uint8_t>(r(0x71) + 0x12) == r(0x5C1)) {
            s = 0;
            set_object(static_cast<std::uint8_t>(0x37 + r(0x77)), s, draw_y_);
            r(0x5C0) = static_cast<std::uint8_t>(r(0x5C0) - 4);
            std::uint8_t a = static_cast<std::uint8_t>(r(0x5E0) - 0x0C);
            if (r(0x77)) a = static_cast<std::uint8_t>(a + 0x18);
            r(0x5E0) = a;
        }
    } while (static_cast<std::uint8_t>(r(0x71) + 8) != r(0x5C1));
    r(0x421) = 0x0E;
    r(0x74) = r(0x5E1);
    r(0x77) = 3; r(0x7B) = 0x3C;
    sound_command(0xFE, r(0xA2)); // X from the last object scan
}
// ---- co-op (an enhancement) ----

// While player 2's hit runs (her block in): the player's and the death's sprites (tiles below $36) drawn in palette 0
// (her death, her figure on the lives screen) in her palette 2.
void Engine::her_sprites() {
    for (unsigned o = 4; o < 0x100; o += 4)
        if (r(static_cast<std::uint16_t>(0x201 + o)) < 0x36 && (r(static_cast<std::uint16_t>(0x202 + o)) & 3) == 0) {
            r(static_cast<std::uint16_t>(0x202 + o)) = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x202 + o)) | 2);
            her_ram_[o / 4] = true;
        }
}

// The RAM's player and the saved one exchanged.
void Engine::swap_players() {
    for (unsigned i = 0; i < other_.block.size(); ++i) std::swap(r(static_cast<std::uint16_t>(0x70 + i)), other_.block[i]);
    for (unsigned i = 0; i < other_.guns.size(); ++i) std::swap(r(static_cast<std::uint16_t>(0x90 + i)), other_.guns[i]);
    std::swap(r(0xF5), other_.pad_new); std::swap(r(0xF7), other_.pad_held);
    for (unsigned i = 0; i < other_.oam.size(); ++i) std::swap(r(static_cast<std::uint16_t>(0x200 + i)), other_.oam[i]);
}
// At the game's start: player 2 as player 1 starts (lives, no guns), both in.
void Engine::coop_init() {
    for (unsigned i = 0; i < other_.block.size(); ++i) other_.block[i] = r(static_cast<std::uint16_t>(0x70 + i));
    for (unsigned i = 0; i < other_.guns.size(); ++i) other_.guns[i] = r(static_cast<std::uint16_t>(0x90 + i));
    other_.pad_new = 0; other_.pad_held = 0; other_.oam.fill(0xF8);
    other_.active = true; ram_owner_ = 0; coop_busy_ = false; coop_out_ = false;
}
// At a stage's start: the other player 32 pixels beside the RAM's, its move and shots cleared.
void Engine::coop_place_other() {
    if (!other_.active) return;
    auto& b = other_.block;
    const auto at = [&](unsigned a) -> std::uint8_t& { return b[a - 0x70]; };
    for (unsigned a = 0x70; a <= 0x76; ++a) at(a) = r(static_cast<std::uint16_t>(a));
    at(0x74) = r(0x74) < 0xC0 ? static_cast<std::uint8_t>(r(0x74) + 0x20) : static_cast<std::uint8_t>(r(0x74) - 0x20);
    at(0x75) = at(0x74);
    for (const unsigned a : {0x80u, 0x81u, 0x82u, 0x83u, 0x84u, 0x85u, 0x86u, 0x87u}) at(a) = 0;
    at(0x76) = 0; at(0x7B) = 0x3C;
}
// The other player's frame: the same move, shots and sprites as player 1 ($EB82, $ED89, $EF13) with her block in;
// her sprites to $0218-$022F in sprite palette 2 (the enemies' and shots': red clothes; the ROM hack's default).
void Engine::coop_other_frame() {
    swap_players();
    player(); fire(); player_sprites();
    swap_players();
    for (unsigned i = 0; i < other_.oam.size(); i += 4) {
        r(static_cast<std::uint16_t>(0x218 + i)) = other_.oam[i];
        r(static_cast<std::uint16_t>(0x219 + i)) = other_.oam[i + 1];
        r(static_cast<std::uint16_t>(0x21A + i)) = static_cast<std::uint8_t>((other_.oam[i + 2] & 0xFC) | ((other_.oam[i + 2] + 2) & 3));
        r(static_cast<std::uint16_t>(0x21B + i)) = other_.oam[i + 3];
        her_ram_[(0x18 + i) / 4] = true;
    }
}
// The other player's pad ($C15A's bits: A in bit 7): its new presses and held buttons.
void Engine::coop_other_pad() {
    const std::uint8_t pad = m.pads[ram_owner_ ^ 1];
    std::uint8_t held = 0;
    for (int i = 0; i < 8; ++i) if (pad & (1 << i)) held |= static_cast<std::uint8_t>(0x80 >> i);
    other_.pad_new = static_cast<std::uint8_t>((held ^ other_.pad_held) & held);
    other_.pad_held = held;
}
// The enemies' target: the nearer of the two players (the other one when she is nearer and in).
void Engine::coop_target(std::uint8_t x, std::uint8_t& ty, std::uint8_t& tx) {
    if (!other_.active) return;
    const auto dist = [](std::uint8_t p, std::uint8_t q) { return p >= q ? p - q : q - p; };
    const std::uint8_t by = r(static_cast<std::uint16_t>(0x5C0 + x)), bx = r(static_cast<std::uint16_t>(0x5E0 + x));
    const std::uint8_t oy = other_.block[0x71 - 0x70], ox = other_.block[0x74 - 0x70];
    if (dist(oy, by) + dist(ox, bx) < dist(ty, by) + dist(tx, bx)) { ty = oy; tx = ox; }
}
std::vector<std::uint8_t> Engine::second_state() const {
    std::vector<std::uint8_t> s(other_.block.begin(), other_.block.end());
    s.insert(s.end(), other_.guns.begin(), other_.guns.end());
    s.push_back(other_.pad_new); s.push_back(other_.pad_held);
    s.push_back(other_.active ? 1 : 0); s.push_back(static_cast<std::uint8_t>(ram_owner_));
    return s;
}
// $F379: objects of the kinds 1-6, $0B-$1D and $2F on removed.
void Engine::clear_objects() {
    for (int x = 0x1F; x >= 0; --x) {
        const std::uint8_t a = r(static_cast<std::uint16_t>(0x420 + x));
        if (a == 0) continue;
        if ((a >= 7 && a < 0x0B) || (a >= 0x1E && a < 0x2F)) continue;
        r(static_cast<std::uint16_t>(0x400 + x)) = 0;
    }
}
// $C30C (its data after the JSR, here the address): groups of count, Y, attribute, X, then tiles (0: a gap), 0 ends.
void Engine::sprite_text(std::uint16_t p) {
    r(0x00) = static_cast<std::uint8_t>(p); r(0x01) = static_cast<std::uint8_t>(p >> 8);
    std::uint8_t y = 0;
    std::uint8_t a = m.read(p);
    for (;;) {
        r(0x02) = a;
        r(0x03) = m.read(static_cast<std::uint16_t>(p + ++y));
        r(0x05) = m.read(static_cast<std::uint16_t>(p + ++y));
        r(0x06) = m.read(static_cast<std::uint16_t>(p + ++y));
        do {
            const std::uint8_t x = r(0xA1);
            r(static_cast<std::uint16_t>(0x200 + x)) = r(0x03);
            r(static_cast<std::uint16_t>(0x202 + x)) = r(0x05);
            for (;;) {
                r(static_cast<std::uint16_t>(0x203 + x)) = r(0x06);
                r(0x06) = static_cast<std::uint8_t>(r(0x06) + 8);
                a = m.read(static_cast<std::uint16_t>(p + ++y));
                if (a) break;
            }
            r(static_cast<std::uint16_t>(0x201 + x)) = a;
            r(0xA1) = static_cast<std::uint8_t>(r(0xA1) + 4);
        } while (--r(0x02));
        a = m.read(static_cast<std::uint16_t>(p + ++y));
        if (a == 0) return;
    }
}
// $E93A: the scroll Y $FC up a line (through the other nametable at -1: $F0 bit 1), then by the line $5D (0-31: a
// metatile row is 32 lines): on odd lines 1-7 two columns of the next row's tiles into the buffer, on 9 its
// attributes, on 11 and 13 the uploads ($38 = $FF / 1), on 15 the enemies of the row and the next row.
void Engine::scroll_step() {
    std::uint8_t x = static_cast<std::uint8_t>(r(0xFC) - 1);
    if (x == 0xFF) { r(0xF0) ^= 2; x = 0xEF; }
    r(0xFC) = x;
    if (r(0x5D) & 1) {
        const std::uint8_t a = r(0x5D) & 0x0F;
        if (a < 9) {
            r(0x300) = r(0x59); r(0x301) = r(0x58);
            r(0x07) = a;
            row_tiles(a);
            row_tiles(static_cast<std::uint8_t>(r(0x07) - 1));
        } else if (a == 9) { row_attributes(); r(0x39) = 1; }
        else if (a == 0x0F) { spawn(); next_row(); }
        else if (a == 0x0B) r(0x38) = 0xFF;
        else if (a == 0x0D) r(0x38) = 1;
    }
    r(0x5D) = (r(0x5D) + 1) & 0x1F;
    r(0x61) = r(0x5D) & 0x0F;
}
// $EACC: the stage's events at $83F0 (16 rows): one at the row $42 starts ($EAE1); one running ($2C) goes on.
void Engine::stage_events() {
    for (int y = 0x0F; y >= 0; --y) {
        const std::uint8_t a = m.read(static_cast<std::uint16_t>(0x83F0 + y));
        if (a == r(0x2E)) continue;
        if (a == r(0x42)) { // $EAE1: the event of this row starts: CHR address $2A/$2B and its count $2C from $83E0 + (y & $0C)
            r(0x2E) = a; r(0x2D) = 0;
            const std::uint8_t k = static_cast<std::uint8_t>(y & 0x0C);
            r(0x2B) = m.read(static_cast<std::uint16_t>(0x83E0 + k));
            const std::uint8_t b = m.read(static_cast<std::uint16_t>(0x83E1 + k));
            r(0x2A) = b & 0x1F;
            r(0x2C) = static_cast<std::uint8_t>((b & 0xE0) + k);
            if (r(0x2C)) { r(0x2B) = static_cast<std::uint8_t>(((r(0x2B) & 0xCF) | 2) + r(0x2D)); r(0x3C) = r(0x2B); return; }
            stage_event_step();
            return;
        }
    }
    if (r(0x2C)) stage_event_step();
}
// $EB06: an event's steps, a tile each frame ($3C for the NMI): 4 tiles ($2D: $00-$30) read from CHR into $06B0,
// mirrored (bits reversed) and written back; then the next tile ($2B += $40) until the end $83E2/$83E3.
void Engine::stage_event_step() {
    r(0x2D) = (r(0x2D) + 0x10) & 0x70;
    std::uint8_t a = r(0x2D);
    if (a == 0) {
        r(0x2B) &= 0xC0;
        if (!(r(0x2C) & 0x80)) {
            const std::uint8_t y = r(0x2C) & 0x1F;
            if (m.read(static_cast<std::uint16_t>(0x83E2 + y)) == r(0x2B) && (m.read(static_cast<std::uint16_t>(0x83E3 + y)) & 0x1F) == r(0x2A)) {
                r(0x2C) = 0; return;
            }
            const unsigned s = r(0x2B) + 0x40u;
            r(0x2B) = static_cast<std::uint8_t>(s); r(0x2A) = static_cast<std::uint8_t>(r(0x2A) + (s >> 8));
        }
    }
    if (a >= 0x40) { // $EB54: mirror the tile read into $06B0 + ($2D & $30), then write it back
        std::uint8_t y = r(0x2D) & 0x30;
        r(0x3C) = (y + 0x20) & 0x30;
        do {
            std::uint8_t v = r(static_cast<std::uint16_t>(0x6B0 + y));
            for (int i = 0; i < 8; ++i) { r(0xF3) = static_cast<std::uint8_t>((r(0xF3) << 1) | (v & 1)); v >>= 1; }
            r(static_cast<std::uint16_t>(0x6B0 + y)) = r(0xF3);
            ++y;
        } while (y & 0x0F);
        r(0x2B) = static_cast<std::uint8_t>((r(0x2B) & 0xCD) + r(0x3C));
        ++r(0x3C);
        return;
    }
    r(0x2B) = static_cast<std::uint8_t>(((r(0x2B) & 0xCF) | 2) + r(0x2D)); // $EB46: read the next tile
    r(0x3C) = r(0x2B);
}
// $EB82: the player's move by the pad ($F7 low nibble through $ED4F; speed set $ED5F by the frame's parity and the
// boots/horse: tables $E057/$E0BA for X, $DF91/$DFF4 for Y, 8.8), kept within X $10-$F0 and Y $30-$D8; the
// collision bits of the new place in $82 bits 7-6 (bit 6: blocked, then the move is taken back per axis); pushed
// below $D8 by the scroll: placed again at $D7 ($C733) and 60 frames ($7B) of blinking. Then its sprites' places.
void Engine::player() {
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    std::uint8_t a = 0, x;
    bool scripted = false;
    if (r(0x82) & 0x20) { // $EB89: the scripted walk ($ED65: Y steps, $88 ends it)
        const std::uint8_t y82 = r(0x82);
        x = y82 & 0x1F;
        ++r(0x82);
        a = rd(0xED65 + x);
        if (a != 0x88) {
            r(0x75) = r(0x74); r(0x72) = r(0x71);
            a = static_cast<std::uint8_t>(r(0x71) + a);
            scripted = a != 0;
        }
        if (!scripted) r(0x82) = y82 & 0xD0;
    }
    if (!scripted) {
        x = 4;
        if ((r(0x77) | r(0x7C) | r(0x7D)) == 0) {
            x = 0;
            if ((r(0x82) & 0x30) != 0x10 && r(0x78)) x = 2;
        }
        r(0xF3) = x;
        x = static_cast<std::uint8_t>((r(0xA0) & 1) | r(0xF3));
        x = rd(0xED4F + (r(0xF7) & 0x0F)) | rd(0xED5F + x);
        unsigned sum = r(0x73) + rd(0xE057 + x);
        r(0x73) = static_cast<std::uint8_t>(sum);
        r(0x75) = r(0x74);
        a = static_cast<std::uint8_t>(r(0x74) + rd(0xE0BA + x) + (sum >> 8));
        if (a < 0x11) a = 0x10;
        else if (a >= 0xF0) a = 0xF0;
        r(0x74) = a;
        sum = r(0x70) + rd(0xDF91 + x);
        r(0x70) = static_cast<std::uint8_t>(sum);
        r(0x72) = r(0x71);
        a = static_cast<std::uint8_t>(r(0x71) + rd(0xDFF4 + x) + (sum >> 8));
    }
    if (a < 0x31) a = 0x30;
    else if (a >= 0xD8) a = 0xD8;
    r(0x71) = a;
    r(0x72) = static_cast<std::uint8_t>(r(0x72) + r(0x62));
    const std::uint8_t c = collision(r(0x74), r(0x71));
    r(0x82) = (r(0x82) & 0x3F) | c;
    std::uint8_t px = 0;
    if (c == 0) {
        if ((r(0x82) & 0x30) == 0x10) r(0x82) = 0x20;
        goto EC6C;
    }
    if (c & 0x40) {
        if (collision(static_cast<std::uint8_t>(r(0x74) + rd(0xED85 + (r(0xF7) & 3))), r(0x72)) & 0x40) {
            r(0x74) = r(0x75);
            if (r(0x74)) goto ECC7;
        } else goto ECD8;
    }
    // $EC62
    if ((r(0x82) & 0x30) == 0) r(0x82) = 0x30;
EC6C:
    if (r(0xF7) & 3) {
        r(0x0D) = r(0xF7) & 3;
        px = static_cast<std::uint8_t>(r(0x74) + rd(0xED85 + r(0x0D)));
        if (collision(px, r(0x71)) & 0x40) {
            if (collision(px, r(0x72)) & 0x40) { r(0x74) = px; goto ECC7; }
            goto ECE7;
        }
        if (collision(static_cast<std::uint8_t>(r(0x74) + rd(0xED85 + (r(0x0D) ^ 3))), r(0x71)) & 0x40) goto ECD8;
        goto ECF8;
    }
    if (collision(static_cast<std::uint8_t>(r(0x74) + 0xF9), r(0x71)) & 0x40) goto ECE7;
    if (collision(static_cast<std::uint8_t>(r(0x74) + 6), r(0x71)) & 0x40) goto ECE7;
    goto ECF8;
ECC7:
    if (!(collision(r(0x75), r(0x71)) & 0x40)) { r(0x74) = r(0x75); if (r(0x74)) goto ECF8; }
ECD8:
    if (collision(r(0x74), r(0x72)) & 0x40) r(0x74) = r(0x75);
ECE7:
    r(0x71) = r(0x72);
    if (r(0x72) >= 0xD9) { place_player(0xD7); r(0x7B) = 0x3C; }
ECF8:
    r(0x207) = r(0x20F) = r(0x217) = r(0x74);
    r(0x203) = r(0x20B) = r(0x213) = static_cast<std::uint8_t>(r(0x74) - 8);
    if ((r(0x82) & 0x30) == 0x10 && r(0x77) == 0) r(0x217) = r(0x213) = static_cast<std::uint8_t>(r(0x74) - 4);
    r(0xF3) = (r(0x80) & 4) >> 1 >> 1;
    a = static_cast<std::uint8_t>(r(0x71) - r(0xF3) - 0x16);
    r(0x200) = r(0x204) = a;
    unsigned s = a + 8u;
    r(0x208) = r(0x20C) = static_cast<std::uint8_t>(s);
    s = (s & 0xFF) + 8u + (s >> 8);
    r(0x210) = r(0x214) = static_cast<std::uint8_t>(s);
}
// $ED89: the walk animation ($80: 0-12 by 4, every 6 frames moving, 12 standing), the shot pause $84 (then the
// shot's direction $83 back to 0), the fire buttons ($F7 bits 7-6: a press waits up to 3 frames for the other to make
// a pair; $87 holds until released): the shot pattern $83 (B: left $FF, A: right 1, both: 0) from $F0F4/$F0FA
// [$88: the gun] put into free shot slots $08-$0D (count, how many, sound, $0440/$0460/$0480/$0540 and per shot
// kind, Y/X offsets, speeds).
void Engine::fire() {
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    const std::uint8_t x81 = ++r(0x81);
    if ((r(0xF7) & 0x0F) == 0 ? x81 >= 0x0C : x81 >= 6) { r(0x81) = 0; r(0x80) = (r(0x80) + 4) & 0x0F; }
    if (r(0x84) && --r(0x84) == 0) r(0x83) = 0;
    r(0xF3) = r(0xF7) & 0xC0;
    if (r(0x87)) { if (r(0xF3) == 0) r(0x87) = 0; return; }
    if (r(0x86) == 0) {
        if (r(0xF3)) { r(0x85) = r(0xF3); r(0x86) = 3; }
        return;
    }
    r(0x85) |= r(0xF3);
    if (--r(0x86) != 0) {
        if (r(0xF3)) return;
        r(0x86) = 0; r(0x87) = 0;
    }
    r(0x84) = 0x0E;
    if (r(0x88) != 2) r(0x87) = 0x0E;
    std::uint8_t x = 0;
    if (r(0x4C)) { // $EDFF: the title's code: Left/Right aim the shots
        const std::uint8_t d = r(0xF7) & 3;
        if (d && (d ^ 3)) x = static_cast<std::uint8_t>(((d ^ 3) << 1) - 3);
    } else if (r(0x85) != 0xC0) {
        // rol x4 through the carry (clear after the compare): $40 -> 2, $80 -> 4; less 3.
        unsigned v = r(0x85);
        for (int i = 0; i < 4; ++i) v = ((v << 1) | (v >> 8)) & 0x1FF;
        x = static_cast<std::uint8_t>((v & 0xFF) - 3);
    }
    r(0x83) = x;
    std::uint8_t gun = r(0x88);
    if (gun == 3) gun = r(0x79) ? 1 : 0; // $EE22: gun 3 fires the rifle's or the revolver's pattern
    r(0x31) = rd(0xF0F4 + gun); r(0x32) = rd(0xF0FA + gun);
    const std::uint16_t p = static_cast<std::uint16_t>(r(0x31) | r(0x32) << 8);
    r(0x08) = rd(p);
    r(0x09) = r(0x0A) = rd(p + 1u);
    for (x = 8;; ++x) {
        if (!(r(static_cast<std::uint16_t>(0x400 + x)) & 0x80)) {
            r(static_cast<std::uint16_t>(0x0A + r(0x09))) = x;
            if (--r(0x09) == 0) break;
        }
        if (--r(0x08) == 0) return; // not enough free slots
    }
    sound_command(rd(p + 2u), x);
    r(0xF3) = rd(p + 3u); r(0xF1) = rd(p + 4u); r(0xF2) = rd(p + 5u); r(0xF4) = rd(p + 6u);
    std::uint8_t y = rd(p + static_cast<std::uint8_t>(r(0x83) + 8));
    do {
        x = r(static_cast<std::uint16_t>(0x0A + r(0x0A)));
        const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
        o(0x400) = 0x80; o(0x580) = 0; o(0x5A0) = 0;
        o(0x420) = rd(p + ++y);
        o(0x5C0) = static_cast<std::uint8_t>(r(0x71) + rd(p + ++y));
        o(0x5E0) = static_cast<std::uint8_t>(r(0x74) + rd(p + ++y));
        o(0x440) = r(0xF3); o(0x460) = r(0xF1); o(0x480) = r(0xF2); o(0x540) = r(0xF4);
        o(0x4A0) = rd(p + ++y);
        o(0x4C0) = rd(p + ++y);
        o(0x4E0) = r(0x88);
    } while (--r(0x0A));
    if (r(0x88) < 2 || r(0x88) == 3) return;
    // $EED9: a shot of guns 2, 4, 5 from their counter $91-$93 + 4 F1D7[gun] (digits $D0-$D9); empty: the gun back
    const std::uint8_t k = static_cast<std::uint8_t>(m.read(static_cast<std::uint16_t>(0xF1D7 + r(0x88))) << 2);
    const auto d = [&](unsigned i) -> std::uint8_t& { return r(static_cast<std::uint16_t>(0x91 + k + i)); };
    if (--d(2) == 0xCF) { d(2) = 0xD9; if (--d(1) == 0xCF) { d(1) = 0xD9; --d(0); } }
    if (static_cast<std::uint8_t>(d(2) + d(1) + d(0)) == 0x70) { r(0x88) = 0; if (r(0x79)) ++r(0x88); }
}
// $EF13: the player's 6 sprites: tiles and attributes from $F014/$F024 by the shot direction $83, the walk frame
// $80 and the pose (standing $84 = 0: $C0 + frame), the gun sprites ($0201/$0205) by $83; then the blink $7B.
void Engine::player_sprites() {
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    std::uint8_t x = static_cast<std::uint8_t>(((r(0x83) + 1) << 5) + r(0x80));
    if ((r(0x82) & 0x30) != 0x10) {
        std::uint8_t a = static_cast<std::uint8_t>(x + 0x60);
        if (r(0x84) == 0) {
            a = static_cast<std::uint8_t>((r(0x77) ? 0x80 : 0xC0) + r(0x80)); // on the horse: its poses
        }
        x = a;
    }
    if (r(0x83)) {
        const unsigned s = r(0x83) + 1u;
        std::uint8_t a = static_cast<std::uint8_t>(s);
        const bool c = a & 1; // lsr's carry
        a = static_cast<std::uint8_t>((a >> 1) ^ 1);
        a = static_cast<std::uint8_t>(a + 4 + (c ? 1 : 0));
        r(0x205) = a;
        r(0x201) = static_cast<std::uint8_t>(a + r(0x83));
    } else { r(0x201) = 0; r(0x205) = 1; }
    r(0x209) = rd(0xF014 + x); r(0x20D) = rd(0xF015 + x);
    std::uint8_t a = 0x32, y = 0x32; // on the horse: blank legs
    if (!r(0x77)) { a = rd(0xF016 + x); y = rd(0xF017 + x); if (y == 0) a = y = 0x32; }
    r(0x211) = a; r(0x215) = y;
    a = r(0x83) == 1 ? 0x40 : 0;
    r(0x202) = r(0x206) = a;
    r(0x20A) = rd(0xF024 + x); r(0x20E) = rd(0xF025 + x);
    if (!r(0x77)) { r(0x212) = rd(0xF026 + x); r(0x216) = rd(0xF027 + x); }
    else { // $EFA5: the horse (object 1) under the player: its pose bit 0 by the walk, its place
        if ((r(0x82) & 0x30) == 0x10) r(0x421) |= 1;
        else { r(0x421) &= 0xFE; if (r(0x421) == 0) r(0x421) |= 1; }
        r(0x5C1) = static_cast<std::uint8_t>(r(0x71) + 8); r(0x5E1) = r(0x74);
        r(0x212) = 0; r(0x216) = 0x40;
    }
    if (r(0x7B)) --r(0x7B);
    if (r(0x7C)) { // $EFE0: invincible ($7C frames): the sprites' palettes cycle, faster at the end
        if (!(r(0xA0) & 1) && --r(0x7C) == 0) r(0x7B) = 0x3C;
        std::uint8_t c = r(0xA0);
        if (r(0x7C) < 0x1E) c = static_cast<std::uint8_t>(c >> 2);
        if (c & 1) return; // the last lsr's carry
        for (const unsigned s : {0x202u, 0x206u, 0x20Au, 0x20Eu, 0x212u, 0x216u}) ++r(static_cast<std::uint16_t>(s));
        sound_command(0x2B, r(0x7C)); // X = $7C
    }
}
// $F1E2: shots $08-$0D: their life $0440 counts down, Y up by $04A0 (gone past the top), X by $04C0 (gone outside
// $08-$FB); except the gun 4's, a shot ends ($0440 = 1) on a wall (collision $C0).
void Engine::shots() {
    for (r(0x08) = 8, r(0x09) = 5; !(r(0x09) & 0x80); ++r(0x08), --r(0x09)) {
        const std::uint8_t x = r(0x08);
        const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
        if (!(o(0x400) & 0x80)) continue;
        bool gone = --o(0x440) == 0;
        if (!gone) {
            const std::uint8_t y = o(0x5C0);
            if (o(0x580) && o(0x420) == 0x3C) { // $F1FD: an exploding shot stays
                if (o(0x4E0) != 4 && collision(o(0x5E0), y) == 0xC0) o(0x440) = 1;
            } else if (y < o(0x4A0)) gone = true;
            else {
                o(0x5C0) = static_cast<std::uint8_t>(y - o(0x4A0));
                const std::uint8_t nx = static_cast<std::uint8_t>(o(0x5E0) + o(0x4C0));
                if (nx < 8 || nx >= 0xFC) gone = true;
                else {
                    o(0x5E0) = nx;
                    if (o(0x4E0) != 4 && collision(nx, o(0x5C0)) == 0xC0) o(0x440) = 1;
                }
            }
        }
        if (gone) o(0x400) >>= 1;
    }
}
// $E375: the enemies' shots $18-$1F: their life $04C0, a wall ends them (except kind 1; a rifle shot, kind 2, explodes at its end), kinds 0-1
// fly by their direction with the speed of the difficulty ($4E + $45 + $2F, at most 4) and the frame's parity
// ($E440), kinds 2 on by their own ($E11D). Then the barrels.
void Engine::enemy_shots() {
    for (std::uint8_t x = 0x18; x < 0x20; ++x) {
        const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
        if (!(o(0x400) & 0x80)) continue;
        if (--o(0x4C0) == 0) {
            if (o(0x560) != 2) { o(0x400) >>= 1; continue; }
            sound_command(0x1B, x); // $E3DA: the rifle shot explodes
            std::uint8_t s = x; set_object(0x3C, s, r(0x32));
            move(x);
            continue;
        }
        const std::uint8_t kind = o(0x560);
        if (kind != 1) {
            if (kind == 6) { // $E38C: a curving shot: its direction $04E0 ($50: straight) bent by $E44A[life / 8]
                std::uint8_t b = m.read(static_cast<std::uint16_t>(0xE44A + (o(0x4C0) >> 3)));
                const std::uint8_t d = o(0x4E0);
                if (d != 0x50) {
                    if (d < 0x50) b = static_cast<std::uint8_t>(-b);
                    o(0x4A0) = (static_cast<std::uint8_t>(b + d) & 0x1F) | 0x40;
                }
            }
            if (kind == 2 && o(0x4C0) == 0xB4) { o(0x400) = 0xB0; o(0x4A0) = 0xC0; } // $E3BD: the rifle shot lands
            if (wall_ahead(x) & 0x40) { o(0x400) >>= 1; continue; }
        }
        if (o(0x560) < 2) {
            const unsigned s1 = r(0x4E) + r(0x45);
            std::uint8_t a = static_cast<std::uint8_t>((s1 & 0xFF) + r(0x2F) + (s1 >> 8));
            if (a >= 5) a = 4;
            const std::uint8_t y = static_cast<std::uint8_t>((a << 1) | (r(0xA0) & 1));
            move_by(x, m.read(static_cast<std::uint16_t>(0xE440 + y)) | o(0x4A0));
        } else move(x);
    }
    // $E3F3: a barrel (2-7, kind 9) broken open after 10 frames becomes its content $0480 ($29 only while one of
    // $90/$94/$98/$9C, else nothing).
    for (std::uint8_t x = 2; x < 8; ++x) {
        if (r(static_cast<std::uint16_t>(0x420 + x)) != 9 || r(static_cast<std::uint16_t>(0x5A0 + x)) < 0x0A) continue;
        std::uint8_t a = r(static_cast<std::uint16_t>(0x480 + x));
        if (a == 0x29 && (r(0x90) | r(0x94) | r(0x98) | r(0x9C)) == 0) a = 0;
        std::uint8_t s = x;
        set_object(a, s, r(0x32));
    }
}
// $FA30: the enemy list at ($43) (3 bytes each: the map row $42 = ($5A/$5B - $8400) / 8, the half and place, the
// kind) for the row the scroll reached: each into a free slot ($10-$16, or $02-$07 for bit 5) with its object
// ($C796 $31, or $39 from kind $2C), place ($FB09/$FB71), $0540 a random number from $AC-$AE, $0560 the kind's bits 7-6.
void Engine::spawn() {
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    const unsigned row = ((r(0x5B) - 0x84u) & 0xFF) << 8 | r(0x5A);
    r(0x42) = static_cast<std::uint8_t>(row >> 3);
    for (;;) {
        const std::uint16_t p = static_cast<std::uint16_t>(r(0x43) | r(0x44) << 8);
        if (rd(p) != r(0x42)) return;
        const std::uint8_t b = rd(p + 1u);
        if (b == 0) { // $FAE6: the stage's end: with the wanted poster ($49) the scroll stops and task 1 runs the boss ($A000)
            if (r(0x49)) {
                r(0x63) = 1;
                for (unsigned k = 0; k < 0x10; ++k) r(static_cast<std::uint16_t>(0xB0 + k)) = 0;
                r(0x4B) = 1;
                r(0x23) = 0x00; r(0x24) = 0xA0;
                start_task(1, 0xA000);
            }
            const unsigned q = r(0x43) + 3u;
            r(0x43) = static_cast<std::uint8_t>(q); r(0x44) = static_cast<std::uint8_t>(r(0x44) + (q >> 8));
            continue;
        }
        if (b == 0xFF) { r(0x43) = 0; r(0x44) = 0x8C; ++r(0x2F); return; }
        if ((b >> 7) != r(0x5C)) return; // $FA6A: the other half row: later
        spawn_entry();
    }
}
// $FA78: the enemy at ($43) (place byte, kind byte) into a free slot; ($43) on by 3.
void Engine::spawn_entry() {
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    const auto advance = [&] {
        const unsigned q = r(0x43) + 3u;
        r(0x43) = static_cast<std::uint8_t>(q); r(0x44) = static_cast<std::uint8_t>(r(0x44) + (q >> 8));
    };
    const std::uint16_t p = static_cast<std::uint16_t>(r(0x43) | r(0x44) << 8);
    const std::uint8_t b = rd(p + 1u);
    {
        const std::uint8_t kind = rd(p + 2u);
        std::uint8_t x = 0x10, n = 7;
        if (kind & 0x20) { x = 2; n = 6; }
        while ((r(static_cast<std::uint16_t>(0x400 + x)) & 0x80) && --n) ++x;
        if (r(static_cast<std::uint16_t>(0x400 + x)) & 0x80) { advance(); return; } // $FA92: no free slot: left out
        std::uint8_t a = kind & 0x3F;
        if (a < 0x20) {
            unsigned s = r(0xAC) + r(0xAD); // the carry is clear (the compare above)
            const unsigned d = (s & 0xFF) - r(0xAE) - ((s >> 8) ? 0 : 1);
            r(0xAC) = static_cast<std::uint8_t>(d);
            r(static_cast<std::uint16_t>(0x540 + x)) = r(0xAC);
            r(static_cast<std::uint16_t>(0x560 + x)) = kind & 0xC0;
            a = kind & 0x1F;
        }
        if (a >= 0x20 || a == 0) {
            r(static_cast<std::uint16_t>(0x480 + x)) = a;
            a = a >= 0x2C ? 0x39 : 0x31;
        }
        set_object(a, x, 2);
        const std::uint8_t place = b & 0x7F;
        r(static_cast<std::uint16_t>(0x5C0 + x)) = rd(0xFB09 + place);
        r(static_cast<std::uint16_t>(0x5E0 + x)) = rd(0xFB71 + place);
        advance();
    }
}

// Bank 6's object handlers ($6:B000 + 2 (kind - $40)), called with the object's slot in X.
void Engine::enemy(std::uint16_t handler, std::uint8_t x) {
    switch (handler) {
        case 0xB284: gunman(x); return;
        case 0xB501: rifleman(x); return;
        case 0xB080: sniper(x); return;
        case 0xB40B: jumper(x); return;
        case 0xB46E: swooper(x); return;
        case 0xB0E5: bandit(x); return;
        case 0xB5BF: zigzag(x); return;
        case 0xB671: crawler(x); return;
        case 0xB775: kneeler(x); return;
        case 0xB82F: lurker(x); return;
        case 0xB849: riser(x); return;
        case 0xB858: popper(x); return;
        case 0xB8F4: strider(x); return;
        case 0xBA51: thrower(x); return;
        case 0xBB29: dynamiter(x); return;
        case 0xB4E4: swooper_wait(x); return;
        case 0xE17B: falling(x); return;
        case 0xE1AD: dropped_item(x); return;
        default: throw Unported(handler);
    }
}
// $6:B284: a gunman: facing the player (pose $B451/$B459 by the direction) until its move starts ($05A0), firing when
// on screen every $40 frames ($0540 += 3 to $C0) if the player is within 3 steps of its facing; moving by the steps
// of $04C0 (0: enter, 1: walk $04E0 frames, 2: turn near the player, 3: wander, else walk until a wall); kinds
// without the bits $18 walk down past walls ($B3B4).
void Engine::gunman(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    std::uint8_t y = static_cast<std::uint8_t>(aim(x) >> 2);
    if (o(0x5A0) == 0) {
        o(0x400) = (o(0x400) & 0xBF) | rd(0xB459 + y);
        o(0x420) = rd(0xB451 + y);
        if (r(0x41) == 2) { ++o(0x420); o(0x480) = 0x44; }
    }
    if (o(0x5C0) >= 0x10 && o(0x5C0) < 0xE0) {
        o(0x540) = static_cast<std::uint8_t>(o(0x540) + 3);
        if (o(0x540) >= 0xC0) {
            o(0x540) = 0;
            y = 7;
            std::uint8_t a;
            if ((o(0x400) & 0x18) == 0 && (y = 1, o(0x420) < 0x59)) a = 0x10;
            else a = o(0x4A0) & 0x1F;
            r(0x36) = a; r(0xA8) = y; r(0xF4) = 0x40;
            maybe_fire(x);
        }
    }
    if ((o(0x400) & 0x18) == 0) { // $B3B4
        r(0x07) = o(0x5E0);
        std::uint8_t a = o(0x5E0);
        if (o(0x4A0) == 8 && static_cast<std::uint8_t>(a + 0x10) != 0) a = static_cast<std::uint8_t>(a + 0x10);
        else a = static_cast<std::uint8_t>(o(0x4A0) == 8 ? 0xF0 : a - 0x10); // $B3C5 (also after an add that wrapped to 0)
        o(0x5E0) = a;
        const std::uint8_t w = wall_ahead(x);
        o(0x5E0) = r(0x07);
        if (w != 0) { move(x); return; }
        // $B3DA: a free way down: near the player (within $65 lines) it becomes the kind $5E ($5D, mirrored when facing 8)
        if (player_dy(x) >= 0x65) return;
        o(0x400) = 0x80; o(0x420) = 0x5E; o(0x580) = 0; o(0x4E0) = 0x33;
        if (r(0x41) == 2) return;
        --o(0x420);
        if (o(0x4A0) == 8) o(0x400) = 0xC0;
        return;
    }
    const auto step_on = [&] { ++o(0x4C0); };
    const auto walk = [&] { // $B35D
        move(x);
        if (wall_ahead(x) == 0) return;
        o(0x4C0) = 2;
        move_by(x, (o(0x4A0) + 0x10) & 0xDF);
    };
    switch (o(0x4C0) & 0x7F) {
        case 0: { // $B30E
            std::uint8_t a = o(0x4A0);
            if (a == 0) { a = 0xF8; o(0x5C0) = a; }
            if (a == 8 && (o(0x5E0) & 0x80)) o(0x4A0) = 0x18;
            o(0x4E0) = 0x30;
            step_on();
            return;
        }
        case 1: // $B32F
            if (--o(0x4E0) == 0) { step_on(); return; }
            move(x);
            return;
        case 2: // $B337
            if (!far_from_player(x)) {
                std::uint8_t a = o(0x4A0), s;
                if (a < 0x10) { s = 0x83; a = static_cast<std::uint8_t>(a - 8); }
                else { s = 3; a = static_cast<std::uint8_t>(a + 8); }
                o(0x4A0) = a & 0x1F; o(0x4C0) = s;
                return;
            }
            turn(x);
            walk();
            return;
        case 3: { // $B375
            if (++o(0x4E0) < 5) { walk(); return; }
            o(0x4E0) = 0;
            if (!far_from_player(x)) { walk(); return; }
            if (!(o(0x4C0) & 0x80)) --o(0x4A0); // then $B395 adds it back: only the other turn changes it
            ++o(0x4A0);
            o(0x4A0) &= 0x1F;
            const std::uint8_t a = o(0x4A0);
            if (a == 0) {
                if ((o(0x560) & 0x3F) == 0) { walk(); return; }
                step_on(); return;
            }
            if (a == 0x10) ++o(0x560);
            walk();
            return;
        }
        default: walk(); return;
    }
}
// $6:B501 (kinds $67/$68): walks in (turned back by walls below line $1E); within $40 lines of the player and below
// line $20 it starts: then by turns a random direction ($AD += $AC; $B5B6 frames) or, at random, a stand of 90
// frames ($67) with a shot of kind 2 ($D4 frames) toward the player (directions $0A-$16 only).
void Engine::rifleman(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    move(x);
    if (o(0x5C0) >= 0x1E && wall_ahead(x) != 0) {
        o(0x4A0) = (o(0x4A0) + 0x10) & 0x1F;
        move_by(x, o(0x4A0));
        o(0x5C0) = static_cast<std::uint8_t>(o(0x5C0) + r(0x62));
        o(0x4E0) = 8;
    }
    if (o(0x4C0) == 0) {
        if (player_dy(x) >= 0x40) return;
        if (o(0x5C0) >= 0x20) ++o(0x4C0);
        return;
    }
    if (o(0x4E0) != 0) { --o(0x4E0); return; }
    o(0x420) = 0x68;
    const std::uint8_t y = o(0x5C0);
    if (y < 0x30 || y >= 0xC0) { o(0x4A0) = y < 0x30 ? 0 : 0x10; --o(0x4E0); return; }
    r(0xAD) = static_cast<std::uint8_t>(r(0xAD) + r(0xAC));
    if ((r(0xAD) & 0x80) && !(o(0x4C0) & 0x80)) {
        o(0x4C0) = 0x80; o(0x4E0) = 0x5A; o(0x4A0) = 0xC0; o(0x420) = 0x67;
        r(0xA8) = 2; r(0xF4) = 0xD4;
        std::uint8_t a = aim(x) & 0x1E;
        if (a < 0x0A) a = 0x0A;
        if (a >= 0x17) a = 0x16;
        enemy_shot(x, a);
        return;
    }
    const std::uint8_t a = r(0xAD) & 0x1C;
    o(0x4A0) = a;
    o(0x4E0) = m.read(static_cast<std::uint16_t>(0xB5B6 + (a >> 2)));
    o(0x4C0) = 1;
}
// $6:B080 (kinds $50-$55, a window): its pose's direction $36 ($B0DF) turned one pose toward the player's (between
// poses), on screen firing every 90 frames ($0540) when facing the player.
void Engine::sniper(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    if (r(0x41) == 2) o(0x480) = 0x44;
    r(0x36) = m.read(static_cast<std::uint16_t>(0xB0DF + static_cast<std::uint8_t>(o(0x420) - 0x50)));
    aim(x);
    if (o(0x5A0) == 0) {
        const std::uint8_t lo = r(0x36) & 0x0F;
        if (lo != 4) { // the carry of the compare with 4 goes into the subtraction
            const std::uint8_t a = static_cast<std::uint8_t>(r(0x36) - 1 - (lo > 4 ? 0 : 1));
            if (a >= r(0x37)) { --o(0x420); return; }
        }
        if (lo != 0x0C) {
            const std::uint8_t a = static_cast<std::uint8_t>(r(0x36) + 2 + (lo > 0x0C ? 1 : 0));
            if (a < r(0x37)) { ++o(0x420); return; }
        }
    }
    if (o(0x5C0) >= 0xE0) return;
    if (--o(0x540) != 0) return;
    o(0x540) = 0x5A;
    r(0xA8) = 1;
    maybe_fire(x);
}
// $CB3D: an item taken (the sound $2B, its score, gone): $20 boots (speed $78 up to 4, then 2 points... $E2E9[2]),
// $21 the rifle ($79 up to 4; the gun $88 at least 1), $22 ($CB8D), $23 nothing, $24 the shots cleared and $76 = 2,
// $25 a life (at most 5; then $E2E9[7]), $26 the shield ($7C = 180 frames), $27 the power-down ($CC67; with the
// score ...5B60 and Left held: $7D), $28 $49 = 1, $29 on ($CBE2); $2F only the score.
void Engine::item(std::uint8_t x, std::uint8_t kind) {
    sound_command(0x2B, x);
    const std::uint8_t item_y = score(x, r(0x32));
    r(static_cast<std::uint16_t>(0x400 + x)) = 0;
    if (kind == 0x2F) return;
    switch (kind - 0x20) {
        case 0:
            if (r(0x6F6) == 0x5D && r(0x6F8) == 0x5B) ++r(0x7D);
            if (r(0x78) == 4) { add_score(2, x); return; }
            ++r(0x78); r(0x45) = 2;
            return;
        case 1:
            if (r(0x79) == 4) { add_score(2, x); return; }
            ++r(0x79);
            if (r(0x88) == 0) ++r(0x88);
            return;
        case 2: // $CB8D: the bomb
            sound_command(0x29, x);
            clear_enemies(item_y);
            return;
        case 3: return;
        case 4:
            r(0x76) = 2;
            for (unsigned k = 0x18; k < 0x20; ++k) r(static_cast<std::uint16_t>(0x400 + k)) = 0; // $CDD4
            return;
        case 5:
            if (r(0x7A) == 5) { add_score(7, x); return; }
            ++r(0x7A);
            return;
        case 6: r(0x7C) = 0xB4; return;
        case 7:
            if (r(0x6F6) == 0x5B && r(0x6F8) == 0x60 && r(0xF7) == 4) ++r(0x7D);
            power_down();
            return;
        case 8: r(0x49) = 1; return;
        default: counters(x); return;
    }
}
// $CBE2: the four guns' shots $91-$93 + 4k (digit tiles $D0-$D9) up by $CC3D[k], at most $CC42 + 4k (X kept in $32).
void Engine::counters(std::uint8_t x) {
    r(0x32) = x;
    for (r(0xF3) = 3; !(r(0xF3) & 0x80); --r(0xF3)) {
        const std::uint8_t k = r(0xF3);
        std::uint8_t n = m.read(static_cast<std::uint16_t>(0xCC3D + k));
        const auto d = [&](unsigned i) -> std::uint8_t& { return r(static_cast<std::uint16_t>(0x91 + 4 * k + i)); };
        const auto cap = [&](unsigned i) { return m.read(static_cast<std::uint16_t>(0xCC42 + 4 * k + i)); };
        do {
            if (++d(2) >= 0xDA) { d(2) = static_cast<std::uint8_t>(d(2) - 0x0A); if (++d(1) >= 0xDA) { d(1) = static_cast<std::uint8_t>(d(1) - 0x0A); ++d(0); } }
        } while (--n);
        bool over = d(0) > cap(0);
        if (d(0) == cap(0)) over = d(1) > cap(1) || (d(1) == cap(1) && d(2) >= cap(2));
        if (over) for (unsigned i = 0; i < 3; ++i) d(i) = cap(i);
    }
}
// $CC67: the boots and the rifle a level down (the speed bonus $45 off at 0; the gun 1 back to 0).
void Engine::power_down() {
    if (r(0x78) && --r(0x78) == 0) r(0x45) = 0;
    if (r(0x79) && --r(0x79) == 0 && r(0x88) == 1) r(0x88) = 0;
}
// $6:B40B (kinds $5D/$5E/$61/$62): a gunman jumping down from a roof: the jump's steps from $B461 by $04E0 counting
// down (mirrored when facing $18); at the end a walking gunman again (kind - 6, step 2).
void Engine::jumper(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    if (o(0x4E0) & 0x80) {
        o(0x400) |= 0x38;
        o(0x420) = static_cast<std::uint8_t>(o(0x420) - 6);
        o(0x4A0) = 0x10; o(0x4C0) = 2;
        move(x);
        return;
    }
    const std::uint8_t t = o(0x4E0)--;
    std::uint8_t a = m.read(static_cast<std::uint16_t>(0xB461 + (t >> 2)));
    if (o(0x4A0) == 0x18) a = mirror(a);
    move_by(x, a);
}
// $E17B: a hit enemy thrown back: $0560 frames along $E197[$0560], then the kind $E161[kind] ($0400 = $A0).
void Engine::falling(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    if (--o(0x560) == 0) {
        o(0x400) = 0xA0;
        o(0x420) = m.read(static_cast<std::uint16_t>(0xE161 + o(0x420)));
    }
    move_by(x, m.read(static_cast<std::uint16_t>(0xE197 + o(0x560))));
}
// $E1AD: an item thrown out of a dead enemy: $0560 frames along $E1D4[$0560 / 2] (mirrored when $0540), then its
// object ($C796 $E192[kind]).
void Engine::dropped_item(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    if (o(0x560) == 0) return;
    if (--o(0x560) == 0) {
        const std::uint8_t kind = o(0x420);
        std::uint8_t s = x;
        set_object(m.read(static_cast<std::uint16_t>(0xE192 + kind)), s, kind);
    }
    std::uint8_t a = m.read(static_cast<std::uint16_t>(0xE1D4 + (o(0x560) >> 1)));
    if (o(0x540)) a = mirror(a);
    move_by(x, a);
}
// $6:B46E (kind $63): aimed at the player when it starts (mirrored, $04C0 bit 7, from the right half), then each frame
// a step that way plus one along the curve $04E0, which turns a step every 4 frames ($0540 counts 64 frames: then
// the next kind, $64).
void Engine::swooper(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    if ((o(0x4C0) & 0x7F) == 0) {
        if (!(o(0x5E0) & 0x80)) { o(0x4C0) = 0x80; o(0x400) = 0xC0; }
        o(0x4A0) = aim(x);
        swooper_start(x);
        return;
    }
    move(x);
    move_by(x, o(0x4E0));
    --o(0x540);
    if ((o(0x540) & 3) == 0) {
        if (o(0x4C0) & 0x80) ++o(0x4E0);
        else {
            o(0x4E0) = static_cast<std::uint8_t>(o(0x4E0) + 0x1F);
            if (o(0x4E0) == 0) ++o(0x4E0);
        }
    }
    o(0x4E0) &= 0xDF;
    if (o(0x540) == 0) { ++o(0x420); o(0x5A0) = 0; }
}
// $B48C: the next step: the curve from direction $40, 64 frames ($04E0 = 0 at the second).
void Engine::swooper_start(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    ++o(0x4C0);
    o(0x4E0) = 0x40; o(0x540) = 0x40;
    if ((o(0x4C0) & 0x7F) == 2) o(0x4E0) = 0;
}
// $6:B4E4 (kind $64): 30 frames still (drawn without collisions: $0400 | $B8), then kind $63 again.
void Engine::swooper_wait(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    o(0x400) |= 0xB8;
    if (o(0x5A0) < 0x1E) return;
    o(0x400) &= 0xC0;
    --o(0x420);
    swooper_start(x);
}
// $6:B0E5 (kind $56): a plan chosen at the start ($0540: the side in bit 7, the plan 0-4 by the player's distance
// and height), then by the steps of $04C0: walk ($B153) until near/far from the player or line $40, a curve
// ($B1D3 / $B23B, mirrored by the side), a pause, a fan of three shots ($B24B).
void Engine::bandit(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto next = [&] { ++o(0x4C0); o(0x4E0) = 0; }; // $B188
    switch (o(0x4C0)) {
        case 0: { // $B102
            std::uint8_t y;
            if (o(0x4A0) == 0) {
                o(0x4A0) = (o(0x5E0) & 0x80) ? 0x18 : 0x08;
                y = player_dx(x) >= 0x40 ? 0 : 1;
            } else {
                y = 2;
                if (r(0x71) < 0x88) { y = 3; if (r(0x71) < 0x42) y = 4; }
            }
            r(0xF3) = y;
            o(0x540) = (o(0x5E0) & 0x80) | y;
            ++o(0x4C0);
            return;
        }
        case 1: { // $B13D
            const std::uint8_t plan = o(0x540) & 0x7F;
            if (plan == 0) { if (player_dx(x) >= 0x40) bandit_walk(x); else next(); return; }
            if (plan == 1) { if (player_dx(x) < 0x40) bandit_walk(x); else next(); return; }
            if (plan != 4 && o(0x5C0) >= 0x40) { next(); return; }
            bandit_walk(x);
            return;
        }
        case 2: { // $B19A
            std::uint8_t y = static_cast<std::uint8_t>(o(0x4E0)++ >> 2);
            if (y == 8) {
                if ((o(0x540) & 0x7F) == 3) ++o(0x4C0);
                next();
                return;
            }
            if ((o(0x540) & 0x7F) >= 2) y |= 8;
            std::uint8_t a = m.read(static_cast<std::uint16_t>(0xB1D3 + y));
            if (o(0x540) & 0x80) a = mirror(a);
            o(0x4A0) = a;
            bandit_walk(x);
            return;
        }
        case 3: { // $B1E3
            const std::uint8_t a = ++o(0x4E0);
            if (a == 0x14) next();
            else if (a == 0x0A) bandit_fan(x);
            return;
        }
        case 4: { // $B1F4
            r(0xF3) = o(0x4E0)++;
            std::uint8_t y = static_cast<std::uint8_t>(r(0xF3) >> 2);
            if (y >= 8) {
                if ((o(0x540) & 0x7F) < 2 || y == 0x0C) { ++o(0x4C0); return; }
                if (r(0xF3) == 0x28) bandit_fan(x);
                return;
            }
            if ((o(0x540) & 0x7F) >= 2) y |= 8;
            std::uint8_t a = m.read(static_cast<std::uint16_t>(0xB23B + y));
            if (o(0x540) & 0x80) a = mirror(a);
            o(0x4A0) = a;
            bandit_walk(x);
            return;
        }
        default: bandit_walk(x); return;
    }
}
// $B153: a step; on screen (Y from $10, X $10-$EF) a wall turns it back (with the scroll's push).
void Engine::bandit_walk(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    move(x);
    if (o(0x5C0) < 0x10 || o(0x5E0) < 0x10 || o(0x5E0) >= 0xF0) return;
    if (wall_ahead(x) == 0) return;
    o(0x5C0) = static_cast<std::uint8_t>(o(0x5C0) + r(0x62));
    move_by(x, (o(0x4A0) + 0x10) & 0xDF);
}
// $B24B: with three free shot slots, three shots of kind 0 toward the player in a fan ($13/$11/$0F by the aim, less 1
// and 2).
void Engine::bandit_fan(std::uint8_t x) {
    r(0xF3) = 3;
    std::uint8_t y = 0x18;
    for (;; ++y) {
        if (y == 0x20) return;
        if (r(static_cast<std::uint16_t>(0x400 + y)) & 0x80) continue;
        if (--r(0xF3) == 0) break;
    }
    const std::uint8_t a = aim(x);
    r(0xA8) = 0;
    std::uint8_t d = 0x13;
    if (a < 0x12) { d = 0x11; if (a < 0x0F) d = 0x0F; }
    enemy_shot(x, d);
    enemy_shot(x, static_cast<std::uint8_t>(r(0x37) - 1));
    enemy_shot(x, static_cast<std::uint8_t>(r(0x37) - 1));
}
// $6:B5BF (kinds $6C/$6D): a path from $B65F (steps of turn target, frames, start direction; mirrored from the right
// half), one of two by $AC; turning one step a frame toward each target; at the path's end or a wall the object $47.
void Engine::zigzag(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    const auto index = [&] { r(0xF3) = o(0x4C0) & 0x0F; return static_cast<std::uint8_t>(((r(0xF3) & 7) << 1) + r(0xF3)); };
    std::uint8_t y;
    if (o(0x4A0) == 0xFF) {
        r(0xF3) = o(0x5E0) & 0x80;
        r(0xAC) = static_cast<std::uint8_t>(r(0xAC) + r(0xAE));
        o(0x4C0) = (r(0xAC) & 8) | r(0xF3);
    } else {
        move(x);
        if (--o(0x4E0) != 0) return;
        y = index();
        std::uint8_t a = rd(0xB65F + y);
        if (o(0x4C0) & 0x80) a = mirror(a);
        if (a != o(0x4A0)) {
            o(0x4E0) = rd(0xB65F + ++y);
            if (o(0x4C0) & 0x80) --o(0x4A0); else ++o(0x4A0);
            return;
        }
        o(0x420) ^= 1;
        ++o(0x4C0);
        if (wall_ahead(x) == 0) { std::uint8_t s = x; set_object(0x47, s, collision_y_); return; }
    }
    y = index(); // $B5DB
    if (rd(0xB65F + y) == 0xFF) { std::uint8_t s = x; set_object(0x47, s, y); return; }
    o(0x4E0) = rd(0xB65F + ++y);
    std::uint8_t a = rd(0xB65F + ++y);
    if (o(0x4C0) & 0x80) a = mirror(a);
    o(0x4A0) = a;
}
// $6:B671 (kinds $6E-$70): waits $0560 frames, walks down to line $20, then near the player (within $40 lines) by
// chance ($AD += $AE) walks toward it ($40 speed) or sideways ($84/$9C) for 16 frames; a wall makes it duck ($6F,
// then $70: a hop along $B764 with a shot of kind 3 at its top).
void Engine::crawler(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto settle = [&] { o(0x400) = 0x98; o(0x420) = 0x6E; o(0x560) = (o(0x560) & 0xC0) | 0x14; }; // $B74F
    const auto sideways = [&] { o(0x4E0) = 0x10; }; // $B6D9
    const auto choose = [&] { // $B6A6
        r(0xAD) = static_cast<std::uint8_t>(r(0xAD) + r(0xAE));
        const std::uint8_t limit = o(0x4C0) == 1 ? 0xC0 : 0x40;
        const std::uint8_t a = limit >= r(0xAD) ? 0x80 : 0x40;
        o(0x4C0) = a;
        if (a == 0x40) { o(0x4A0) = aim(x) | 0x40; return; }
        o(0x4A0) = (o(0x5E0) & 0x80) ? 0x9C : 0x84;
        sideways();
    };
    if (o(0x560) & 0x3F) { --o(0x560); return; }
    move(x);
    if (!(o(0x400) & 0x80)) return;
    const std::uint8_t step = o(0x4C0);
    if (step & 0x80) { // $B6F9
        o(0x420) = 0x70; o(0x580) = 0;
        move_by(x, m.read(static_cast<std::uint16_t>(0xB764 + o(0x4E0))));
        if (!(o(0x400) & 0x80)) return;
        const std::uint8_t t = o(0x4E0);
        if (t == 0) { // $B73E
            if (wall_ahead(x) == 0) { choose(); return; }
            o(0x4A0) = mirror(o(0x4A0));
            sideways();
            return;
        }
        if (t == 0x10) { r(0xA8) = 3; r(0xF4) = 0x40; enemy_shot(x, aim(x) | 0x40); }
        if (--o(0x4E0) == 0) { settle(); return; }
        o(0x400) |= static_cast<std::uint8_t>((o(0x4A0) & 0x10) << 2);
        return;
    }
    if (step == 0) {
        if (o(0x5C0) != 0x20) return;
        ++o(0x4C0);
        settle();
        return;
    }
    if (step & 0x40) { // $B6DF
        o(0x400) = 0x98; o(0x420) = 0x6F;
        if (wall_ahead(x) == 0) return;
        o(0x4C0) = 0x80;
        settle();
        sideways();
        return;
    }
    if (player_dy(x) >= 0x40) return;
    settle();
    choose();
}
// $6:B775 (kinds $71/$72): walks in; facing down ($10) below line $30 within $60 lines of the player (or after 80
// frames) it stops to aim (pose by the direction: $0540 0-2) and fires kind 0 at frames $10-$50 ($B81F); after 90
// frames it turns away.
void Engine::kneeler(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto next = [&] { ++o(0x4C0); o(0x4E0) = 0; o(0x580) = 0; o(0x5A0) = 0; }; // $B7C9
    ++o(0x4E0);
    switch (o(0x4C0)) {
        case 0:
            if (o(0x4A0) == 8 && (o(0x5E0) & 0x80)) o(0x4A0) = 0x18;
            next();
            return;
        case 1: {
            if (o(0x4A0) != 0x10) { if (o(0x4E0) != 0x50) { move(x); return; } }
            else if (o(0x5C0) < 0x30 || player_dy(x) >= 0x60) { move(x); return; }
            o(0x400) = 0xB8; // $B7AE
            const std::uint8_t a = aim(x);
            std::uint8_t y = 0;
            if (a < 0x12) { y = 1; if (a < 0x0E) y = 2; }
            o(0x540) = y;
            ++o(0x420);
            next();
            return;
        }
        case 2: {
            const std::uint8_t a = o(0x4E0);
            if (a == 0x5A) {
                o(0x400) = 0x98;
                o(0x4A0) = (o(0x4A0) + 0x10) & 0xDF;
                --o(0x420);
                next();
                return;
            }
            if (a & 0x0F) return;
            r(0xA8) = 0;
            r(0xF3) = static_cast<std::uint8_t>(o(0x4E0) >> 4);
            const std::uint8_t y = static_cast<std::uint8_t>(o(0x540) * 5 + r(0xF3));
            enemy_shot(x, m.read(static_cast<std::uint16_t>(0xB81F + y)));
            return;
        }
        default: move(x); return;
    }
}
// $B840.
void Engine::still(std::uint8_t x) { r(static_cast<std::uint16_t>(0x580 + x)) = 0; r(static_cast<std::uint16_t>(0x5A0 + x)) = 0; }
// $6:B82F (kind $73): hidden until the player is within $60 lines and $40 columns.
void Engine::lurker(std::uint8_t x) {
    if (player_dy(x) >= 0x60 || player_dx(x) >= 0x40) return;
    ++r(static_cast<std::uint16_t>(0x420 + x));
    still(x);
}
// $6:B849 (kind $74): at its animation's frame 2 it rises ($B872).
void Engine::riser(std::uint8_t x) {
    if (r(static_cast<std::uint16_t>(0x580 + x)) != 2) return;
    r(static_cast<std::uint16_t>(0x400 + x)) = 0xB8;
    popper_aim(x);
}
// $6:B858 (kinds $75/$76): popping up by chance ($AE += $AD) when above the player, aiming (pose $0580 by the
// direction, $09-$17) and firing at frames $1E and $0A of $28; otherwise it hides ($3B) or walks for 48 frames
// turning toward the player.
void Engine::popper(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    if (o(0x4E0) != 0) {
        const std::uint8_t t = o(0x4E0);
        if (--o(0x4E0) == 0) { o(0x420) = 0x75; still(x); return; }
        if (o(0x4C0) == 0) {
            if (t == 0x0A || t == 0x1E) { r(0xA8) = 0; r(0x36) = o(0x540); aim(x); maybe_fire(x); }
            return;
        }
        aim(x); turn(x); // $B8DD
        move(x);
        const std::uint8_t w = wall_ahead(x);
        if (w == 0x80) return;
        move_by(x, (o(0x4A0) + 0x10 + (w >= 0x80 ? 1 : 0)) & 0xDF);
        return;
    }
    r(0xAE) = static_cast<std::uint8_t>(r(0xAE) + r(0xAD));
    const std::uint8_t a = r(0xAE) & 0x0F;
    if (a == 0) { o(0x400) = 0xA0; o(0x420) = 0x3B; still(x); return; } // $B8AB
    if (a & 1) { o(0x4C0) = 1; o(0x4E0) = 0x30; return; }
    if (o(0x5C0) >= r(0x71)) { o(0x400) = 0xA0; o(0x420) = 0x3B; still(x); return; }
    popper_aim(x);
}
// $B872.
void Engine::popper_aim(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    o(0x420) = 0x76;
    const std::uint8_t a = aim(x);
    if (a >= 0x18 || a < 0x09) { o(0x4C0) = 1; o(0x4E0) = 0x30; return; }
    std::uint8_t y = 0;
    if (a < 0x13) { y = 1; if (a < 0x0E) y = 2; }
    o(0x540) = a; o(0x580) = y;
    o(0x4C0) = 0; o(0x4E0) = 0x28;
}
// $6:B8F4 (kinds $65/$66): walks down (turned back by walls) to line $28, then sideways ($48/$58) and in arcs
// ($B9F8 / $BA09, mirrored) across, crouching ($66) to fire kind 4 when the player is straight below ($0F-$11).
void Engine::strider(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto walk = [&] { // $B92C
        move(x);
        if (wall_ahead(x) == 0) return;
        o(0x5C0) = static_cast<std::uint8_t>(o(0x5C0) + r(0x62));
        move_by(x, (o(0x4A0) + 0x10) & 0xDF);
        if (o(0x540) == 0) o(0x540) = 0x22;
    };
    if (o(0x4E0)) { --o(0x4E0); return; }
    switch (o(0x4C0) & 0x1F) {
        case 0:
            o(0x4C0) = o(0x5E0) & 0x80;
            o(0x540) = 0;
            ++o(0x4C0);
            return;
        case 1:
            if (o(0x5C0) < 0x28) { walk(); return; }
            o(0x4E0) = 0x14;
            o(0x4A0) = (o(0x4C0) & 0x80) ? 0x58 : 0x48;
            ++o(0x4C0);
            return;
        case 2: {
            if (o(0x5C0) < 0x28) { o(0x4C0) = 4; o(0x4E0) = 0x14; return; }
            if (o(0x540) == 0) {
                if (o(0x5E0) < 0x28 || o(0x5E0) >= 0xD8) { o(0x540) = 0x22; o(0x4C0) &= 0xDF; return; }
                if (o(0x580) == 0 && !(o(0x4C0) & 0x20)) {
                    const std::uint8_t a = aim(x);
                    if (a < 0x12 && a >= 0x0F) { o(0x540) = a; o(0x4C0) |= 0x23; }
                }
                walk();
                return;
            }
            const std::uint8_t y = static_cast<std::uint8_t>(--o(0x540) >> 1);
            std::uint8_t a;
            if (o(0x4C0) & 0x40) a = m.read(static_cast<std::uint16_t>(0xBA09 + y));
            else { a = m.read(static_cast<std::uint16_t>(0xB9F8 + y)); if (a == 0) a = m.read(static_cast<std::uint16_t>(0xBA09 + y)); }
            if (o(0x4C0) & 0x80) a = mirror(a);
            o(0x4A0) = a;
            if (o(0x540) == 0) {
                a = o(0x4C0) ^ 0x80;
                if (o(0x5C0) >= 0x79) a |= 0x40;
                o(0x4C0) = a;
            }
            walk();
            return;
        }
        case 3: // $BA1A
            o(0x400) = 0xB8; o(0x420) = 0x66;
            if (o(0x580) != 2) return;
            r(0xA8) = 4; r(0xF4) = 0x40;
            enemy_shot(x, o(0x540) | 0x40);
            o(0x400) = 0x98; o(0x420) = 0x65;
            --o(0x4C0);
            o(0x540) = 0; o(0x580) = 0;
            return;
        default: move_by(x, 0x40); return;
    }
}
// $6:BA51 (kinds $69-$6B): a knife thrower: turning on the spot ($04E0: poses $69/$6A), then dashes along $BAD6 /
// $BAFC in random directions ($AC -= $AD), throwing kind 5 when facing the player.
void Engine::thrower(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto rd = [&](unsigned a) { return m.read(static_cast<std::uint16_t>(a)); };
    if (const std::uint8_t y = o(0x4E0)) {
        --o(0x4E0);
        o(0x420) = (y < 0x1C && y >= 0x0C) ? 0x69 : 0x6A;
        return;
    }
    bool carry = false;
    const auto dash = [&](std::uint8_t a, bool random) { // $BABD / $BAC7
        if (random) {
            r(0xAC) = static_cast<std::uint8_t>(r(0xAC) - r(0xAD) - (carry ? 0 : 1));
            a = (r(0xAC) & 0x18) | 0x44;
        }
        o(0x4A0) = a; o(0x4E0) = 0x28; o(0x540) = 0x20;
        ++o(0x4C0);
    };
    const auto flip = [&] { // $BAA9
        o(0x4C0) ^= 0x80;
        if (!(o(0x4C0) & 0x80)) { dash(0, true); return; }
        const unsigned s = o(0x4A0) + 0x10u;
        carry = s > 0xFF;
        const std::uint8_t a = static_cast<std::uint8_t>(s) & 0xDF;
        if (a != 0) dash(a, false); else dash(0, true);
    };
    switch (o(0x4C0) & 0x7F) {
        case 0:
            if (o(0x4A0) & 8) {
                if (o(0x5E0) & 0x80) o(0x4A0) = 0x58;
                o(0x540) = 0x28;
            } else o(0x540) = 0x18;
            ++o(0x4C0);
            return;
        case 1:
            if (--o(0x540) == 0) { dash(0, true); return; }
            move_by(x, rd(0xBAD6 + (o(0x540) >> 3)));
            move(x);
            return;
        default: {
            o(0x420) = 0x6B;
            if (--o(0x540) == 0) { flip(); return; }
            move_by(x, rd(0xBAFC + (o(0x540) >> 2)));
            move(x);
            if (o(0x540) != 0x18) return;
            if (o(0x4A0) >= 0x48 && o(0x4A0) < 0x59) return;
            const std::uint8_t a = aim(x);
            if (a >= 0x18 || a < 0x0A) return;
            r(0xA8) = 5; r(0xF4) = 0xFF;
            enemy_shot(x, (a & 0xFE) | 0x40);
            return;
        }
    }
}
// $6:BB29 (kinds $77/$78): enters, walks toward the player, waits within $60 of it, then by chance ($AD += $AC)
// fires kind 6 (crouched, $78) when facing it or runs (toward / sideways / away) for 24 frames.
void Engine::dynamiter(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto walk = [&] { // $BB5A
        move(x);
        if (wall_ahead(x) == 0) return;
        o(0x5C0) = static_cast<std::uint8_t>(o(0x5C0) + r(0x62));
        move_by(x, (o(0x4A0) + 0x10) & 0xDF);
    };
    const auto run = [&](std::uint8_t a) { o(0x4A0) = a; o(0x4C0) = 5; o(0x540) = 0x18; }; // $BC22
    if (o(0x4E0)) { o(0x400) = 0xB8; --o(0x4E0); return; }
    o(0x400) = 0x98;
    switch (o(0x4C0)) {
        case 0:
            if ((o(0x4A0) & 8) && (o(0x5E0) & 0x80)) o(0x4A0) = 0x18;
            o(0x540) = 0x20;
            ++o(0x4C0);
            return;
        case 1:
            if (--o(0x540) != 0) { walk(); return; }
            o(0x4A0) = aim(x) | 0x40;
            o(0x4E0) = 0x28;
            ++o(0x4C0);
            return;
        case 2:
            if (player_dy(x) >= 0x60 || player_dx(x) >= 0x60) { walk(); return; }
            o(0x4E0) = 0x14;
            ++o(0x4C0);
            return;
        case 3: {
            o(0x400) = 0xB8;
            if (o(0x580) != 0) return;
            if (o(0x5C0) >= 0xD0) { run(0x50); return; }
            r(0xAD) = static_cast<std::uint8_t>(r(0xAD) + r(0xAC));
            if (!(r(0xAD) & 4)) {
                const std::uint8_t a = aim(x);
                if (a >= 0x17 || a < 0x0A) { run(0x40); return; }
                ++o(0x4C0); ++o(0x420);
                r(0xA8) = 6; r(0xF4) = 0x40;
                enemy_shot(x, a | 0x40);
                return;
            }
            const bool far = player_dy(x) >= 0x40 || player_dx(x) >= 0x40; // $BBF7
            const bool toward = (r(0xAD) & 3) ? far : !far;
            if (toward) { run(aim(x) | 0x40); return; }
            run(o(0x5E0) < r(0x74) ? 0x5C : 0x44);
            return;
        }
        case 4: // $BC30
            o(0x400) = 0xB8;
            if (o(0x580) == 3) { --o(0x420); --o(0x4C0); }
            return;
        default:
            if (--o(0x540) == 0) o(0x4C0) = 3;
            walk();
            return;
    }
}
std::uint8_t Engine::mirror(std::uint8_t a) {
    r(0xF3) = a;
    r(0xF4) = static_cast<std::uint8_t>(0x20 - (a & 0x1F));
    return (a & 0xE0) | r(0xF4);
}
bool Engine::far_from_player(std::uint8_t x) { return player_dy(x) >= 0x38 || player_dx(x) >= 0x38; }
std::uint8_t Engine::player_dy(std::uint8_t x) {
    std::uint8_t ty = r(0x71), tx = r(0x74);
    if (coop && !coop_busy_) coop_target(x, ty, tx);
    const std::uint8_t b = r(static_cast<std::uint16_t>(0x5C0 + x));
    return ty >= b ? static_cast<std::uint8_t>(ty - b) : static_cast<std::uint8_t>(b - ty);
}
std::uint8_t Engine::player_dx(std::uint8_t x) {
    std::uint8_t ty = r(0x71), tx = r(0x74);
    if (coop && !coop_busy_) coop_target(x, ty, tx);
    const std::uint8_t b = r(static_cast<std::uint16_t>(0x5E0 + x));
    return tx >= b ? static_cast<std::uint8_t>(tx - b) : static_cast<std::uint8_t>(b - tx);
}
// $C7E6: the octant by the signs (y: 0/4 down/up, +2 left) and which distance is larger, the step within it from
// the smaller distance against the larger / 8 in odd multiples; the direction from the table $C86B.
std::uint8_t Engine::aim(std::uint8_t x) {
    std::uint8_t ty = r(0x71), tx = r(0x74);
    if (coop && !coop_busy_) coop_target(x, ty, tx);
    return aim_at(x, ty, tx);
}
std::uint8_t Engine::aim_at(std::uint8_t x, std::uint8_t ty, std::uint8_t tx) {
    r(0x35) = tx;
    std::uint8_t y = 0;
    const std::uint8_t by = r(static_cast<std::uint16_t>(0x5C0 + x)), bx = r(static_cast<std::uint16_t>(0x5E0 + x));
    std::uint8_t a = static_cast<std::uint8_t>(ty - by);
    if (ty < by) { a = static_cast<std::uint8_t>(-a); y = 4; }
    r(0x31) = a;
    a = static_cast<std::uint8_t>(r(0x35) - bx);
    if (r(0x35) < bx) { a = static_cast<std::uint8_t>(-a); y = static_cast<std::uint8_t>(y + 2); }
    r(0x32) = a;
    if (a < r(0x31)) {
        r(0x33) = a; r(0x32) = r(0x31); r(0x31) = r(0x33);
        if (y == 0 || y == 4) ++y;
    } else if (y == 2 || y == 6) ++y;
    r(0x33) = 0;
    r(0x34) = (r(0x32) >> 3) & 0x1F;
    a = r(0x34);
    for (int k = 0; k < 4 && a < r(0x31); ++k) { ++r(0x33); a = static_cast<std::uint8_t>(a + 2 * r(0x34)); }
    aim_y_ = static_cast<std::uint8_t>((y << 3) | r(0x33));
    r(0x37) = m.read(static_cast<std::uint16_t>(0xC86B + aim_y_));
    return r(0x37);
}
// $C8AE: fire ($E452) when the facing $36 is within 3 of the direction to the player $37.
void Engine::maybe_fire(std::uint8_t x) {
    const std::uint8_t d = r(0x36) >= r(0x37) ? static_cast<std::uint8_t>(r(0x36) - r(0x37)) : static_cast<std::uint8_t>(r(0x37) - r(0x36));
    if (d < 3) enemy_shot(x, r(0x37) | 0x40);
}
void Engine::turn(std::uint8_t x) {
    std::uint8_t& d = r(static_cast<std::uint16_t>(0x4A0 + x));
    r(0xF3) = d & 0x1F;
    const std::uint8_t a = static_cast<std::uint8_t>((((r(0xF3) + 0x10) & 0x1F) - r(0x37)) & 0x1F);
    if (a == 0x10) return;
    if (a < 0x10) ++r(0xF3); else --r(0xF3);
    r(0xF3) &= 0x1F;
    d = (d & 0xC0) | r(0xF3);
}
// $C8F8: the collision bits a step ahead in its direction ($C925 offsets), read in the stage's bank.
std::uint8_t Engine::wall_ahead(std::uint8_t x) {
    bank_write(r(0x41));
    r(0xF3) = x;
    const std::uint8_t y = r(static_cast<std::uint16_t>(0x4A0 + x)) & 0x1E;
    r(0xF4) = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x5E0 + x)) + m.read(static_cast<std::uint16_t>(0xC926 + y)));
    const std::uint8_t py = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x5C0 + x)) + m.read(static_cast<std::uint16_t>(0xC925 + y)));
    const std::uint8_t c = collision(r(0xF4), py);
    bank_write(r(0x40));
    return c;
}
std::uint8_t Engine::move(std::uint8_t x) {
    std::uint8_t& d = r(static_cast<std::uint16_t>(0x4A0 + x));
    d &= 0xDF;
    return move_by(x, d);
}
// $E125: a step in direction a & $1F at speed (a & $C0) >> 1 (tables $DF91/$DFF4 for Y, $E057/$E0BA for X, 8.8);
// gone ($0400 >>= 1) when it wraps past an edge.
std::uint8_t Engine::move_by(std::uint8_t x, std::uint8_t a) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    const auto rd = [&](unsigned p) { return m.read(static_cast<std::uint16_t>(p)); };
    r(0xF3) = a;
    r(0xF4) = (a & 0xC0) >> 1;
    const std::uint8_t y = (a & 0x1F) | r(0xF4);
    unsigned s = o(0x500) + rd(0xDF91 + y);
    o(0x500) = static_cast<std::uint8_t>(s);
    s = o(0x5C0) + rd(0xDFF4 + y) + (s >> 8);
    o(0x5C0) = static_cast<std::uint8_t>(s);
    if (((s >> 8) != 0) == !(rd(0xDFF4 + y) & 0x80)) { o(0x400) >>= 1; return y; }
    s = o(0x520) + rd(0xE057 + y);
    o(0x520) = static_cast<std::uint8_t>(s);
    s = o(0x5E0) + rd(0xE0BA + y) + (s >> 8);
    o(0x5E0) = static_cast<std::uint8_t>(s);
    if (((s >> 8) != 0) == !(rd(0xE0BA + y) & 0x80)) o(0x400) >>= 1;
    return y;
}
// $E452: an enemy shot of kind $A8 (object $70 + kind) from x's place into a free slot $18-$1F, direction a
// ($04A0, $04E0); kinds 0-1 live by the difficulty ($E4AB), others $F4 frames.
void Engine::enemy_shot(std::uint8_t x, std::uint8_t a) {
    r(0x37) = a;
    std::uint8_t y = 0x18;
    while (r(static_cast<std::uint16_t>(0x400 + y)) & 0x80) if (++y == 0x20) return;
    r(static_cast<std::uint16_t>(0x5C0 + y)) = r(static_cast<std::uint16_t>(0x5C0 + x));
    r(static_cast<std::uint16_t>(0x5E0 + y)) = r(static_cast<std::uint16_t>(0x5E0 + x));
    r(0xF3) = x;
    std::uint8_t s = y;
    r(static_cast<std::uint16_t>(0x560 + s)) = r(0xA8);
    set_object(static_cast<std::uint8_t>(r(0xA8) + 0x70), s, y);
    std::uint8_t d;
    if (r(0xA8) >= 2) { r(static_cast<std::uint16_t>(0x4C0 + s)) = r(0xF4); d = r(0x37); }
    else {
        const unsigned s1 = r(0x4E) + r(0x45);
        std::uint8_t k = static_cast<std::uint8_t>((s1 & 0xFF) + r(0x2F) + (s1 >> 8));
        if (k >= 5) k = 4; // $E495
        r(static_cast<std::uint16_t>(0x4C0 + s)) = m.read(static_cast<std::uint16_t>(0xE4AB + k));
        d = r(0x37) & 0x1F;
    }
    r(static_cast<std::uint16_t>(0x4A0 + s)) = d;
    r(static_cast<std::uint16_t>(0x4E0 + s)) = d;
}
// $F68B / $F6A1: the row buffer $0300-$034C and $50-$5C saved to $0393 and $0104 around the drawing, and back.
void Engine::save_rows() {
    for (int x = 0x4C; x >= 0; --x) r(static_cast<std::uint16_t>(0x393 + x)) = r(static_cast<std::uint16_t>(0x300 + x));
    for (int x = 0x0C; x >= 0; --x) r(static_cast<std::uint16_t>(0x104 + x)) = r(static_cast<std::uint16_t>(0x50 + x));
}
void Engine::restore_rows() {
    for (int x = 0x4C; x >= 0; --x) r(static_cast<std::uint16_t>(0x300 + x)) = r(static_cast<std::uint16_t>(0x393 + x));
    for (int x = 0x0C; x >= 0; --x) r(static_cast<std::uint16_t>(0x50 + x)) = r(static_cast<std::uint16_t>(0x104 + x));
}
// $E9F7: column c of the map row at ($5A): its metatile ($8000 + 5 m: 4 tile codes, the attribute byte; pointer in
// $F3) half $5C ^ 1 as 2 x 2 CHR tiles per code (code * 4 + 0-3) into the buffer's two tile rows ($0302 / $0322 + 4c).
void Engine::row_tiles(std::uint8_t c) {
    const std::uint16_t row = static_cast<std::uint16_t>(r(0x5A) | r(0x5B) << 8);
    const std::uint8_t mt = m.read(static_cast<std::uint16_t>(row + c));
    const unsigned address = 0x8000u + mt * 5u;
    r(0xF3) = static_cast<std::uint8_t>(address); r(0xF4) = static_cast<std::uint8_t>(address >> 8);
    const std::uint8_t x = static_cast<std::uint8_t>(c << 2);
    std::uint8_t y = static_cast<std::uint8_t>((r(0x5C) ^ 1) << 1);
    for (unsigned half = 0; half < 2; ++half, ++y) {
        const std::uint8_t t = static_cast<std::uint8_t>((m.read(static_cast<std::uint16_t>(address + y)) & 0x3F) << 2);
        const unsigned o = x + half * 2;
        r(static_cast<std::uint16_t>(0x302 + o)) = t; r(static_cast<std::uint16_t>(0x322 + o)) = static_cast<std::uint8_t>(t + 1);
        r(static_cast<std::uint16_t>(0x303 + o)) = static_cast<std::uint8_t>(t + 2); r(static_cast<std::uint16_t>(0x323 + o)) = static_cast<std::uint8_t>(t + 3);
    }
}
// $EA57: the row's 8 attribute bytes into $0345 (address $59|3, low ((($59 & 7) + $0C) << 4) + 8 for the lower half),
// each the metatile's nibble ($5C: low, else high) merged with $50 + x (the other half: $0F = $58 & $40).
void Engine::row_attributes() {
    r(0x0F) = r(0x58) & 0x40;
    unsigned v = (r(0x59) & 7) + 0x0Cu;
    bool carry = false;
    for (int i = 0; i < 4; ++i) { carry = v & 0x80; v = (v << 1) & 0xFF; }
    if (r(0x58) & 0x80) v = (v + 8 + (carry ? 1 : 0)) & 0xFF;
    r(0x343) = static_cast<std::uint8_t>(v); r(0x342) = r(0x59) | 3; r(0x344) = 8;
    const std::uint16_t row = static_cast<std::uint16_t>(r(0x5A) | r(0x5B) << 8);
    for (int x = 7; x >= 0; --x) {
        const std::uint8_t mt = m.read(static_cast<std::uint16_t>(row + x));
        const unsigned address = 0x8000u + mt * 5u;
        r(0xF3) = static_cast<std::uint8_t>(address); r(0xF4) = static_cast<std::uint8_t>(address >> 8);
        std::uint8_t a = m.read(static_cast<std::uint16_t>(address + 4));
        a = r(0x5C) ? static_cast<std::uint8_t>(a & 0x0F) : static_cast<std::uint8_t>(a >> 4);
        if (r(0x0F) == 0) a |= r(static_cast<std::uint16_t>(0x50 + x));
        else { a = static_cast<std::uint8_t>(a << 4); r(static_cast<std::uint16_t>(0x50 + x)) = a; }
        r(static_cast<std::uint16_t>(0x345 + x)) = a;
    }
}
// $E98F: the next half row up: the map row advances by 8 every other half (wrapping at $5E/$5F to $8400), the VRAM
// row $58/$59 up by $40 (from the top of one nametable to the bottom of the other).
void Engine::next_row() {
    r(0x5C) ^= 1;
    if (r(0x5C) == 0) {
        const unsigned sum = r(0x5A) + 8u;
        r(0x5A) = static_cast<std::uint8_t>(sum); r(0x5B) = static_cast<std::uint8_t>(r(0x5B) + (sum >> 8));
        if (r(0x5B) == r(0x5F) && r(0x5A) == r(0x5E)) { r(0x5A) = 0; r(0x5B) = 0x84; }
    }
    const bool borrow = r(0x58) < 0x40;
    r(0x58) = static_cast<std::uint8_t>(r(0x58) - 0x40); r(0x59) = static_cast<std::uint8_t>(r(0x59) - (borrow ? 1 : 0));
    if (r(0x59) & 4) { r(0x58) = 0x80; r(0x59) = static_cast<std::uint8_t>(((r(0x59) & 0x0F) | 0x20) ^ 4); }
}
// $C6C4.
std::uint8_t Engine::collision(std::uint8_t x, std::uint8_t y) {
    r(0x35) = 0;
    r(0x33) = static_cast<std::uint8_t>(y - r(0x5D));
    unsigned a = ((r(0x33) & 0xE0) >> 2) + 8;
    if (r(0x5C) == 0) a += 8;
    r(0x31) = static_cast<std::uint8_t>(a);
    bool borrow = r(0x5A) < r(0x31);
    r(0x31) = static_cast<std::uint8_t>(r(0x5A) - r(0x31));
    r(0x32) = static_cast<std::uint8_t>(r(0x5B) - (borrow ? 1 : 0));
    if (r(0x32) < 0x84) { // before the map's start: from its end
        r(0x31) = static_cast<std::uint8_t>(-r(0x31));
        borrow = r(0x5E) < r(0x31);
        r(0x31) = static_cast<std::uint8_t>(r(0x5E) - r(0x31));
        r(0x32) = static_cast<std::uint8_t>(r(0x5F) - (borrow ? 1 : 0));
    }
    const std::uint16_t row = static_cast<std::uint16_t>(r(0x31) | r(0x32) << 8);
    const std::uint8_t mt = m.read(static_cast<std::uint16_t>(row + (x >> 5)));
    const unsigned address = 0x8000u + mt * 5u;
    r(0x34) = static_cast<std::uint8_t>(address); r(0x35) = static_cast<std::uint8_t>(address >> 8);
    const std::uint8_t q = static_cast<std::uint8_t>(((r(0x33) & 0x10) ? 2 : 0) + ((x & 0x10) ? 1 : 0));
    collision_y_ = q;
    return m.read(static_cast<std::uint16_t>(address + q)) & 0xC0;
}
// $C733: the player's X $74 (to 8 within 16) moved to the nearest free place on line y (bit 6 clear): left and right
// in steps of 16, the fewer steps wins (right on a tie).
void Engine::place_player(std::uint8_t y) {
    r(0x0D) = y;
    std::uint8_t x = static_cast<std::uint8_t>((r(0x74) & 0xF8) | 8);
    r(0x74) = x;
    if (x != 0xF8 && !(collision(x, r(0x0D)) & 0x40)) return;
    r(0x0A) = 0; r(0x0B) = 0;
    for (;;) { // left
        if (x < 0x10) { r(0x0A) = 0xFF; break; }
        x = static_cast<std::uint8_t>(x - 0x10); ++r(0x0A);
        if (!(collision(x, r(0x0D)) & 0x40)) { r(0x0C) = x; break; }
    }
    x = r(0x74);
    for (;;) { // right
        if (x >= 0xE0) { r(0x0B) = 0xFF; break; }
        x = static_cast<std::uint8_t>(x + 0x10); ++r(0x0B);
        if (!(collision(x, r(0x0D)) & 0x40)) break;
    }
    if (r(0x0A) < r(0x0B)) x = r(0x0C);
    r(0x74) = x; r(0x71) = r(0x0D);
}
// $E88D.
void Engine::clear_play() {
    for (const int a : {0x83, 0x84, 0x86, 0x87, 0x76, 0x82, 0x88}) r(static_cast<std::uint16_t>(a)) = 0;
    if (r(0x79)) ++r(0x88); // the rifle kept: gun 1
}
// $F6B7: the stage's first screen, a pair of tile rows a frame for 16 frames (the NMI uploads $0300 with $38 = $FF and
// the attributes $0342 with $39 when a row ends an attribute row), from VRAM row $2B80 up.
Routine Engine::draw_stage() { co_await draw_rows(0x10, 0x40, false); }
// $F6B7 ($58 = $80, $0B = 0) / $F6C5 (redraw: $5C toggled, $0B = 2): count pairs of rows from back rows behind.
Routine Engine::draw_rows(std::uint8_t count, std::uint8_t back, bool redraw) {
    if (redraw) { r(0x5C) ^= 1; r(0x0B) = 2; }
    else { r(0x58) = 0x80; r(0x0B) = 0; }
    r(0x0D) = count; r(0x0F) = back;
    r(0x59) ^= 8;
    bool borrow = r(0x5A) < r(0x0F);
    r(0x5A) = static_cast<std::uint8_t>(r(0x5A) - r(0x0F)); r(0x0F) = r(0x5A);
    r(0x5B) = static_cast<std::uint8_t>(r(0x5B) - (borrow ? 1 : 0));
    if (r(0x5B) < 0x84) {
        const std::uint8_t x = static_cast<std::uint8_t>(r(0x5F) - 1);
        const unsigned sum = r(0x5E) + r(0x0F);
        r(0x5A) = static_cast<std::uint8_t>(sum); r(0x5B) = static_cast<std::uint8_t>(x + (sum >> 8));
    }
    do {
        r(0x300) = r(0x59); r(0x301) = r(0x58);
        for (r(0x07) = 7; !(r(0x07) & 0x80); --r(0x07)) row_tiles(r(0x07));
        if (--r(0x0B) & 0x80) r(0x38) = 0xFF;
        row_attributes();
        if ((r(0x0B) & 0x80) && r(0x0F) == 0) r(0x39) = 1;
        co_await wait();
        next_row();
    } while (--r(0x0D));
}

// $FF00: task 3, the random numbers $AC-$AF stepped every frame.
Routine Engine::random_task() {
    m.write(0x2000, r(0xFF));
    for (;;) {
        r(0x00) = r(0xAC) & 2;
        std::uint8_t carry = ((r(0xAD) & 2) ^ r(0x00)) ? 1 : 0;
        for (std::uint16_t x = 0xAC; x < 0xB0; ++x) { const std::uint8_t out = r(x) & 1; r(x) = static_cast<std::uint8_t>((r(x) >> 1) | (carry << 7)); carry = out; }
        ++r(0xAC);
        // $FF28: during a boss ($4A) at frames $40 and $60 of every 128 (3 times in 4) an enemy from the random list
        // $83BF + ($AD & $1E) ($43 kept).
        const std::uint8_t t = r(0x22) & 0x7F;
        if (r(0x4A) && (t == 0x40 || t == 0x60) && (r(0xAC) & 3)) {
            r(0x07) = r(0x43); r(0x08) = r(0x44);
            r(0x43) = static_cast<std::uint8_t>((r(0xAD) & 0x1E) + 0xBF); r(0x44) = 0x83;
            spawn_entry();
            r(0x43) = r(0x07); r(0x44) = r(0x08);
        }
        co_await wait();
    }
}
// $CA83: object x (half sizes $36/$37) against the player: below kind $10 it blocks (the move taken back), $20-$2F
// is an item ($CB3D), from $30 it hits the player ($76 = 1, 4 with the gun 3) unless blinking ($7B); kinds
// $30-$3D vanish on touching.
bool Engine::touch_player(std::uint8_t x) {
    if (r(0x76) & 1) return true;
    const auto dist = [](std::uint8_t p, std::uint8_t q) { return p >= q ? static_cast<std::uint8_t>(p - q) : static_cast<std::uint8_t>(q - p); };
    std::uint8_t a = dist(r(0x71), r(static_cast<std::uint16_t>(0x5C0 + x))); // this player (not the enemies' target)
    if (a >= r(0x36)) {
        a = static_cast<std::uint8_t>(a - r(0x36));
        if (a >= 4) { if (r(0x77) == 0 || static_cast<std::uint8_t>(a - 4) >= 8) return true; } // on the horse 8 lines more
    }
    if (dist(r(0x74), r(static_cast<std::uint16_t>(0x5E0 + x))) >= r(0x37)) return true;
    a = r(static_cast<std::uint16_t>(0x420 + x));
    if (a < 0x10) { // $CB2A
        r(0x74) = r(0x75); r(0x71) = r(0x72);
        if (r(0x72) >= 0xD9) { r(0x71) = 0xD8; r(0x72) = 0xD8; }
        return true;
    }
    if (a < 0x20) { // $CC51: the horse (kind in $A7, $A5 = its $0560 + 1), 60 frames of blinking, the enemy shots gone
        r(0xA7) = a;
        r(0xA5) = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x560 + x)) + 1);
        r(static_cast<std::uint16_t>(0x400 + x)) = 0;
        r(0x7B) = 0x3C;
        for (unsigned k = 0x18; k < 0x20; ++k) r(static_cast<std::uint16_t>(0x400 + k)) = 0;
        return false;
    }
    if (a < 0x30) { item(x, a); return true; }
    if (a < 0x3E) r(static_cast<std::uint16_t>(0x400 + x)) = 0;
    if (r(0x7B)) return true;
    if (r(0x7C)) { // $CAD0: invincible: the enemy dies
        const std::uint8_t y = score(x, r(static_cast<std::uint16_t>(0x440 + x)));
        die(r(static_cast<std::uint16_t>(0x480 + x)), x, y);
        sound_command(0x28, x);
        return true;
    }
    if (r(0x77)) { // $CAEF: the horse takes the hit: a step down (3 -> 2: hurt, 1: gone)
        if (r(0x77) == 2) { r(0x421) = static_cast<std::uint8_t>(r(0x421) + 0xA6); r(0x5A1) = 0; }
        if (--r(0x77) == 0) {
            r(0x401) = 0xA0; r(0x421) = 0xB6; r(0x581) = 0; r(0x5A1) = 0;
            r(0x5E1) = static_cast<std::uint8_t>(r(0x5E1) - 0x10);
        }
        r(0x7B) = 0x3C;
        sound_command(0x26, x);
        return true;
    }
    r(0x76) = r(0x88) == 3 ? 4 : 1;
    return true;
}
// $CC83: the player's shots $08-$0D against object x: a hit (boxes $36/$37 + the shot's $0460/$0480) takes the
// shot's power $0540 from the object's strength $0460 (and the object's from the shot's: the shot ends at 0); at 0
// the object dies ($E297 the score, $CD4E its death object $0480 / $33). False: the object is gone (no drawing).
bool Engine::shot_hits(std::uint8_t x) {
    const auto o = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + x)); };
    for (std::uint8_t y = 8; y != 0x0E; ++y) {
        const auto s = [&](unsigned base) -> std::uint8_t& { return r(static_cast<std::uint16_t>(base + y)); };
        if (!(s(0x400) & 0x80)) continue;
        std::uint8_t a = s(0x5C0) >= o(0x5C0) ? static_cast<std::uint8_t>(s(0x5C0) - o(0x5C0)) : static_cast<std::uint8_t>(o(0x5C0) - s(0x5C0));
        if (a >= r(0x36) && static_cast<std::uint8_t>(a - r(0x36)) >= s(0x460)) continue;
        a = s(0x5E0) >= o(0x5E0) ? static_cast<std::uint8_t>(s(0x5E0) - o(0x5E0)) : static_cast<std::uint8_t>(o(0x5E0) - s(0x5E0));
        if (a >= r(0x37) && static_cast<std::uint8_t>(a - r(0x37)) >= s(0x480)) continue;
        if (s(0x420) == 0x3F) continue;
        r(0xF3) = s(0x540);
        if (s(0x540) <= o(0x460)) {
            s(0x540) = static_cast<std::uint8_t>(s(0x540) - o(0x460));
            s(0x400) = 0;
            if (s(0x4E0) == 4) { // $CCE7: gun 4's shot explodes ($F3 keeps X: the damage below takes the slot number)
                r(0xF3) = x;
                std::uint8_t t = y; set_object(0x32, t, y);
            }
        } else s(0x540) = static_cast<std::uint8_t>(s(0x540) - o(0x460));
        if (o(0x400) & 4) continue;
        o(0x460) = o(0x460) >= r(0xF3) ? static_cast<std::uint8_t>(o(0x460) - r(0xF3)) : 0;
        if (o(0x420) < 9) sound_command(0x27, x);
        if (o(0x460) != 0) continue;
        if (o(0x420) == 7) {
            r(0x32) = y; score(x); y = r(0x32);
            die(0x33, x, y);
            continue;
        }
        sound_command(0x28, x);
        r(0x32) = y; score(x); y = r(0x32);
        if (o(0x480) == 0) { o(0x400) >>= 1; return false; }
        die(o(0x480), x, y);
    }
    return true;
}
// $CD4E: object x becomes its death object a; one with $0560 bits 7-6 leaves an item ($4D + them, the $4E one $4F
// when no ... $90/$94/$98/$9C) in a free slot $10-$16; its $0560 = $0A.
void Engine::die(std::uint8_t a, std::uint8_t x, std::uint8_t y) {
    set_object(a, x, y);
    const std::uint8_t bits = r(static_cast<std::uint16_t>(0x560 + x)) & 0xC0;
    if (bits) {
        r(0xF3) = static_cast<std::uint8_t>(0x4C + (bits >> 6));
        if (r(0xF3) == 0x4E && (r(0x90) | r(0x94) | r(0x98) | r(0x9C)) == 0) ++r(0xF3);
        std::uint8_t k = 0x10;
        while (k != 0x17 && (r(static_cast<std::uint16_t>(0x400 + k)) & 0x80)) ++k;
        if (k != 0x17) {
            r(static_cast<std::uint16_t>(0x400 + k)) = 0x80;
            r(static_cast<std::uint16_t>(0x420 + k)) = r(0xF3);
            r(static_cast<std::uint16_t>(0x5C0 + k)) = r(static_cast<std::uint16_t>(0x5C0 + x));
            r(static_cast<std::uint16_t>(0x5E0 + k)) = r(static_cast<std::uint16_t>(0x5E0 + x));
            r(static_cast<std::uint16_t>(0x580 + k)) = 0;
            r(static_cast<std::uint16_t>(0x540 + k)) = r(static_cast<std::uint16_t>(0x5E0 + x)) >> 7;
            r(static_cast<std::uint16_t>(0x560 + k)) = 0x16;
        }
    }
    r(static_cast<std::uint16_t>(0x560 + x)) = 0x0A;
}
// $E297: the score for object x's kind ($E335 nibbles: an index of 5-digit amounts $E2E9) into the digits
// $06F2-$06FA (tiles $58-$61, every other byte), at most 99999 ... (not in the demonstration).
std::uint8_t Engine::score(std::uint8_t x, std::uint8_t y) {
    if (r(0x68)) return y;
    const std::uint8_t kind = r(static_cast<std::uint16_t>(0x420 + x));
    if (kind & 0x80) return y;
    std::uint8_t a = m.read(static_cast<std::uint16_t>(0xE335 + (kind >> 1)));
    a = (kind & 1) ? (a & 0x0F) : static_cast<std::uint8_t>(a >> 4);
    if (a == 0) return static_cast<std::uint8_t>(kind >> 1);
    return add_score(a, x);
}
std::uint8_t Engine::add_score(std::uint8_t a, std::uint8_t x) {
    if (a == 0) return 0; // (not reached: the callers pass amounts above 0)
    r(0xF4) = a;
    std::uint8_t y = static_cast<std::uint8_t>(a * 5);
    r(0xF4) = x;
    unsigned carry = 0;
    for (int d = 8; d >= 0; d -= 2, --y) {
        std::uint8_t v = static_cast<std::uint8_t>(r(static_cast<std::uint16_t>(0x6F2 + d)) + m.read(static_cast<std::uint16_t>(0xE2E9 + y)) + carry);
        carry = 0;
        if (v >= 0x62) {
            v = static_cast<std::uint8_t>(v - 0x0A); carry = 1;
            if (y != 0x29 && d == 0) { for (int k = 8; k >= 0; k -= 2) r(static_cast<std::uint16_t>(0x6F2 + k)) = 0x61; return y; } // $E2DD: 99999
        }
        r(static_cast<std::uint16_t>(0x6F2 + d)) = v;
    }
    return y;
}
}
