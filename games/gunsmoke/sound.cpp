#include "engine.hpp"

// Gun.Smoke's sound driver (bank 6 $8000-$8980), ported routine by routine. Four channel blocks of $1F bytes at
// $0700 + $1F k (k: pulse 1, pulse 2, triangle, noise; APU registers $4000 + 4k), the instrument table at
// $077C/$077D. Music runs in the blocks' first half (its pointer $00/$01, duration $02/$03, ...), a sound effect takes
// a channel over ($C1/$CF: its channels; its pointer $D0/$D1, its envelope $10-$13). $C0: the playing music's and
// effect's priorities (low / high nibble), $C4: busy, $C7: extra music steps a frame, $C8/$C9: fading, $CA: frames.
namespace gunsmoke {
namespace {
constexpr std::uint16_t kSongs = 0x8A60;
}

std::uint8_t& Engine::ch(unsigned y) { return r(static_cast<std::uint16_t>(0x700 + r(0xCC) + y)); }
// $8562: a jump table after the JSR, entry x: the address goes through $D4/$D5 (jmp ($D4)).
void Engine::dispatch(std::uint16_t table, std::uint8_t x) {
    const std::uint16_t at = static_cast<std::uint16_t>(table + 2 * x);
    r(0xD4) = m.read(at); r(0xD5) = m.read(static_cast<std::uint16_t>(at + 1));
}
void Engine::apu(unsigned x, std::uint8_t value) { m.write(static_cast<std::uint16_t>(0x4000 + (x & 0xFF)), value); }

// $8003: a sound command (from task 0's queue).
void Engine::sound_play(std::uint8_t a) {
    if (a == 0xFC) throw Unported(0x812A);
    if (a == 0xFD) throw Unported(0x812E);
    if (a == 0xFE) { r(0xC4) = 1; r(0xCC) = 0; sound_stop_effect(); return; }
    if (a == 0xFF) { r(0xC4) = 1; r(0xCC) = 0; sound_stop_music(); return; }
    const std::uint8_t i = static_cast<std::uint8_t>(a << 1);
    r(0xC2) = m.read(static_cast<std::uint16_t>(kSongs + i)); r(0xC3) = m.read(static_cast<std::uint16_t>(kSongs + i + 1));
    const auto song = [&](unsigned y) { return m.read(static_cast<std::uint16_t>((r(0xC2) | r(0xC3) << 8) + y)); };
    const std::uint8_t x = song(0);
    if (x & 0x0F) { // $8045: music
        r(0xC5) = r(0xC0) & 0x0F;
        if (x < r(0xC5)) return;
        r(0xC5) = x;
        r(0xC0) = (r(0xC0) & 0xF0) | r(0xC5);
        r(0xC4) = 1; r(0xCC) = 0; r(0xC7) = 0; r(0xC8) = 0;
        r(0xC5) = 4;
        std::uint8_t step = 1;
        for (;;) {
            const unsigned p = (r(0xC2) | r(0xC3) << 8) + step;
            r(0xC2) = static_cast<std::uint8_t>(p); r(0xC3) = static_cast<std::uint8_t>(p >> 8);
            unsigned k = r(0xCC);
            for (unsigned y = 0; y < 2; ++y) r(static_cast<std::uint16_t>(0x700 + k++)) = song(y);
            for (unsigned y = 0x0E; y; --y) r(static_cast<std::uint16_t>(0x700 + k++)) = 0;
            if (!(r(0xC1) & 1)) sound_clear_tail();
            sound_next_channel_rotate();
            if (--r(0xC5) == 0) break;
            step = 2;
        }
        r(0x77C) = song(2); r(0x77D) = song(3);
        r(0xC1) >>= 4; // $8221
        r(0xC4) = 0;
        return;
    }
    // $80BC: a sound effect
    r(0xC5) = r(0xC0) & 0xF0;
    if (x < r(0xC5)) return;
    r(0xC5) = x;
    r(0xC0) = (r(0xC0) & 0x0F) | r(0xC5);
    r(0xC4) = 1; r(0xCC) = 0;
    const unsigned p = (r(0xC2) | r(0xC3) << 8) + 2;
    r(0xD0) = static_cast<std::uint8_t>(p); r(0xD1) = static_cast<std::uint8_t>(p >> 8);
    r(0xD2) = 0; r(0xD3) = 0;
    const std::uint8_t mask = song(1) & 0x0F;
    std::uint8_t both = mask | r(0xC1);
    r(0xC1) = mask;
    r(0xC5) = 4; r(0xC6) = 2;
    do {
        const bool taken = both & 1;
        both >>= 1;
        if (taken) {
            sound_clear_tail();
            if (!(r(0xC1) & 1)) sound_restore_music();
        }
        sound_next_channel_rotate();
        r(0xC6) = static_cast<std::uint8_t>(r(0xC6) + 4);
    } while (--r(0xC5));
    r(0xC1) >>= 4; // $8221
    r(0xCF) = r(0xC1);
    r(0xC4) = 0;
}
// $813F (command $FE): the effect stopped, the music back on its channels.
void Engine::sound_stop_effect() {
    r(0xC0) &= 0x0F;
    r(0xC5) = 4; r(0xC6) = 2;
    do {
        if (r(0xC1) & 1) { sound_clear_tail(); sound_restore_music(); }
        sound_next_channel_rotate();
        r(0xC6) = static_cast<std::uint8_t>(r(0xC6) + 4);
    } while (--r(0xC5));
    r(0xC1) = 0; r(0xCF) = 0; r(0xC4) = 0;
}
// $8193 (command $FF): the music stopped.
void Engine::sound_stop_music() {
    r(0xC0) &= 0xF0;
    r(0xC7) = 0; r(0xC8) = 0;
    r(0xC5) = 4;
    do {
        r(static_cast<std::uint16_t>(0x700 + r(0xCC))) = 0; r(static_cast<std::uint16_t>(0x701 + r(0xCC))) = 0;
        r(0xCC) = static_cast<std::uint8_t>(r(0xCC) + 0x1F); // $8219
    } while (--r(0xC5));
    r(0xC4) = 0;
}
// $8171: a channel the effect leaves: its music note again (or silence, $822A).
void Engine::sound_restore_music() {
    const unsigned k = r(0xCC);
    if (r(static_cast<std::uint16_t>(0x70A + k)) | r(static_cast<std::uint16_t>(0x70B + k))) { sound_reload_instrument(); return; }
    sound_silence(r(0xC5), r(0xC6));
    if (r(static_cast<std::uint16_t>(0x700 + k)) | r(static_cast<std::uint16_t>(0x701 + k))) sound_reload_instrument();
}
// $81CC: the channel's instrument (number at $06 & $1F) from the table $077C into $14-$17.
void Engine::sound_reload_instrument() {
    const std::uint8_t c5 = r(0xC5), c6 = r(0xC6);
    r(0xC5) = r(0x77C); r(0xC6) = r(0x77D);
    unsigned x = r(0xCC) + 6u;
    std::uint8_t y = static_cast<std::uint8_t>((r(static_cast<std::uint16_t>(0x700 + x)) & 0x1F) * 4);
    x += 0x0E;
    for (int i = 0; i < 4; ++i) r(static_cast<std::uint16_t>(0x700 + x++)) = m.read(static_cast<std::uint16_t>((r(0xC5) | r(0xC6) << 8) + y++));
    r(0xC6) = c6; r(0xC5) = c5;
}
// $81BA: the channel's bytes $10-$1E cleared.
void Engine::sound_clear_tail() { for (unsigned y = 0; y < 0x0F; ++y) r(static_cast<std::uint16_t>(0x710 + r(0xCC) + y)) = 0; }
// $820F: $C1 rotated right a bit (bit 0 to bit 7), the next channel.
void Engine::sound_next_channel_rotate() {
    const bool c = r(0xC1) & 1;
    r(0xC1) >>= 1;
    if (c) r(0xC1) |= 0x80;
    r(0xCC) = static_cast<std::uint8_t>(r(0xCC) + 0x1F);
}
// $822A: a channel silenced (the noise: $4015 = 7; else its period registers x, x + 1 zero).
void Engine::sound_silence(std::uint8_t y, std::uint8_t x) {
    if (y == 1) { apu(0x15, 7); return; }
    apu(x, 0); apu(static_cast<unsigned>(x) + 1, 0);
}

// $823F: a frame of sound: each channel's effect and music, then the fade.
void Engine::sound_tick() {
    ++r(0xCA);
    if (r(0xC4)) return;
    r(0xCC) = 0; r(0xCD) = 7; r(0xCB) = 0; r(0xCE) = 4;
    for (;;) {
        ++ch(0x18); ++ch(0x1D);
        if (r(0xCF) & 1) sound_effect_step();
        bool silent = true;
        if (!(r(0x30) & 1) && (ch(0) | ch(1))) { ++ch(0x0E); sound_music_steps(); silent = false; }
        if (silent && !(r(0xCF) & 1)) sound_silence(r(0xCE), static_cast<std::uint8_t>(r(0xCB) + 2));
        const bool c = r(0xCF) & 1;
        r(0xCF) >>= 1;
        if (c) r(0xCF) |= 0x80;
        if (--r(0xCE) == 0) break;
        r(0xCB) = static_cast<std::uint8_t>(r(0xCB) + 4);
        r(0xCC) = static_cast<std::uint8_t>(r(0xCC) + 0x1F);
    }
    const std::uint8_t fade = r(0xC8) & 0x7F;
    if (fade && fade == r(0xCA)) {
        r(0xCA) &= 1;
        if (++r(0xC9) == 0x10) {
            if (!(r(0xC8) & 0x80)) r(0xC8) = 0;
            r(0xC9) = 0x0F;
        }
    }
    if (r(0xD2)) --r(0xD2);
    r(0xCF) >>= 4;
}
// $86C0: the music's steps this frame ($C7 extra, at least one).
void Engine::sound_music_steps() {
    std::uint8_t n = r(0xC7);
    if (n == 0) { sound_music_step(); return; }
    do sound_music_step(); while (--n);
}
// $86CF: a channel's music step: twice on odd frames with bit 7 of $05; the duration $02/$03 down by 4: when it runs
// out the next commands ($8712), else the volume and pitch ($82F6).
void Engine::sound_music_step() {
    if ((ch(5) & 0x80) && (r(0xCA) & 1)) sound_music_tick();
    sound_music_tick();
}
void Engine::sound_music_tick() {
    if (ch(2) | ch(3)) {
        const bool borrow = ch(2) < 4;
        ch(2) = static_cast<std::uint8_t>(ch(2) - 4);
        ch(3) = static_cast<std::uint8_t>(ch(3) - (borrow ? 1 : 0));
        if (ch(2) | ch(3)) {
            if ((ch(0x0A) | ch(0x0B)) == 0) return;
            sound_output();
            return;
        }
    }
    ch(5) &= 0x7F;
    sound_commands();
}
// $8941: the next music byte.
std::uint8_t Engine::sound_music_byte() {
    r(0xD4) = ch(0); r(0xD5) = ch(1);
    const std::uint16_t p = static_cast<std::uint16_t>(r(0xD4) | r(0xD5) << 8);
    const std::uint8_t x = m.read(p);
    const std::uint16_t q = static_cast<std::uint16_t>(p + 1);
    ch(0) = static_cast<std::uint8_t>(q); ch(1) = static_cast<std::uint8_t>(q >> 8);
    return x;
}
// $8960: the duration a * $04 into $02/$03.
void Engine::sound_duration(std::uint8_t a) {
    r(0xD4) = a; r(0xD5) = 0;
    std::uint8_t y = ch(4);
    unsigned sum = 0;
    do {
        sum += r(0xD4);
        if (sum > 0xFF) { sum &= 0xFF; ++r(0xD5); }
    } while (--y);
    ch(2) = static_cast<std::uint8_t>(sum); ch(3) = r(0xD5);
}
// $8712: the music's commands up to a note: $0x the table $87CF, $2x a note with its repeat count, $30 doubled
// steps, else a note (bits 7-5: its length $8981, bits 4-0: its pitch from the period table $07/$08; 0 a rest).
void Engine::sound_commands() {
    for (;;) {
        const std::uint8_t x = sound_music_byte();
        const std::uint8_t kind = x & 0xF0;
        if (kind == 0) {
            dispatch(0x87CF, x); // $87CC: jsr $8562
            switch (x) {
                case 0: ch(4) = sound_music_byte(); continue;
                case 1: ch(9) = sound_music_byte(); continue;
                case 2: { const std::uint8_t a = sound_music_byte(); r(0xD4) = a; ch(0x0C) = (ch(0x0C) & 0x3F) | a; continue; }
                case 3: {
                    std::uint8_t a = sound_music_byte();
                    if (r(0xCE) != 2) { r(0xD4) = a; a = (ch(0x0C) & 0xC0) | a; }
                    ch(0x0C) = a;
                    continue;
                }
                case 4: { // a loop: count (0 forever), address
                    const std::uint8_t n = sound_music_byte();
                    if (n) {
                        r(0xD4) = ch(5) & 0x7F;
                        if (n == r(0xD4)) { // done: past the address
                            ch(5) &= 0x80;
                            const unsigned q = (ch(0) | ch(1) << 8) + 2;
                            ch(0) = static_cast<std::uint8_t>(q); ch(1) = static_cast<std::uint8_t>(q >> 8);
                            continue;
                        }
                        ++r(0xD4);
                        ch(5) = (ch(5) & 0x80) | r(0xD4);
                    }
                    const std::uint8_t lo = sound_music_byte(), hi = sound_music_byte();
                    ch(0) = lo; ch(1) = hi;
                    continue;
                }
                case 5: {
                    const std::uint8_t a = sound_music_byte();
                    r(0xD4) = 0x91; r(0xD5) = 0x89;
                    const unsigned q = 0x8991u + static_cast<std::uint8_t>(a << 1);
                    ch(7) = static_cast<std::uint8_t>(q); ch(8) = static_cast<std::uint8_t>(q >> 8);
                    continue;
                }
                case 6: {
                    const std::uint8_t a = sound_music_byte();
                    sound_duration(m.read(static_cast<std::uint16_t>(0x8989 + (a >> 5))));
                    sound_note_end(a); // X: the byte $8941 read
                    return;
                }
                case 7: {
                    const std::uint8_t a = sound_music_byte();
                    ch(0x0D) = a;
                    ch(0x0F) = sound_music_byte();
                    r(0xD4) = a;
                    if (a & 0x7F) sound_vibrato_reset();
                    continue;
                }
                case 8: {
                    const std::uint8_t a = sound_music_byte();
                    r(0xD4) = a;
                    ch(6) = (ch(6) & 0xE0) | a;
                    if (!(r(0xCF) & 1)) sound_instrument(a);
                    continue;
                }
                case 9: // the track's end
                    ch(0) = 0; ch(1) = 0;
                    r(0xC0) &= 0xF0;
                    if (r(0xCF) & 1) return;
                    sound_silence(r(0xCE), static_cast<std::uint8_t>(r(0xCB) + 2));
                    return;
                default: throw Unported(0x87CC);
            }
        }
        if (kind == 0x20) { // a note with a repeat count (bits 2-0) for its following note's bits 7-5 in $06
            const std::uint8_t keep = x & 7;
            sound_commands();
            r(0xD4) = static_cast<std::uint8_t>(keep << 5);
            ch(6) = (ch(6) & 0x1F) | r(0xD4);
            return;
        }
        if (kind == 0x30) { ch(5) |= 0x80; continue; }
        sound_duration(m.read(static_cast<std::uint16_t>(0x8981 + (x >> 5))));
        sound_note_end(x);
        return;
    }
}
// $8740: the note's end: its repeat count in $06 (a tie: $8554), else its pitch into $0A/$0B and the envelope reset.
void Engine::sound_note_end(std::uint8_t x) {
    if (ch(6) & 0xE0) {
        r(0xD4) = static_cast<std::uint8_t>((ch(6) & 0xE0) - 0x20);
        ch(6) = (ch(6) & 0x1F) | r(0xD4);
        if (r(0xCF) & 1) return;
        sound_tie();
        return;
    }
    std::uint8_t a = x & 0x1F, hi = 0;
    if (a != 0) {
        if (r(0xCE) == 1) hi = 0;
        else {
            a = static_cast<std::uint8_t>(a << 1);
            const unsigned s = ch(7) + a;
            r(0xD4) = static_cast<std::uint8_t>(s);
            r(0xD5) = static_cast<std::uint8_t>(ch(8) + (s >> 8));
            const std::uint16_t p = static_cast<std::uint16_t>(r(0xD4) | r(0xD5) << 8);
            hi = m.read(static_cast<std::uint16_t>(p + 1));
            a = m.read(p);
        }
    }
    ch(0x0A) = a; ch(0x0B) = hi;
    r(0xD4) = ch(0x0D);
    if (r(0xD4) & 0x7F) sound_vibrato_reset();
    if (r(0xCF) & 1) return;
    r(0xD4) = 0x0C;
    sound_envelope_reset();
    sound_note_off_check();
}
// $88B5.
void Engine::sound_vibrato_reset() {
    ch(0x0E) = 0;
    r(0xD4) = (r(0xD4) & 0x80) ? 0x0F : 0;
    ch(0x0F) = (ch(0x0F) & 0xF0) | r(0xD4);
}
// $88ED: instrument x from the table $077C into $14-$17.
void Engine::sound_instrument(std::uint8_t x) {
    const unsigned s = x * 4u + r(0x77C);
    r(0xD4) = static_cast<std::uint8_t>(s);
    r(0xD5) = static_cast<std::uint8_t>(r(0x77D) + (s >> 8));
    for (unsigned y = 0x14; y < 0x18; ++y) {
        ch(y) = m.read(static_cast<std::uint16_t>(r(0xD4) | r(0xD5) << 8));
        if (y == 0x17) break;
        const unsigned q = (r(0xD4) | r(0xD5) << 8) + 1;
        r(0xD4) = static_cast<std::uint8_t>(q); r(0xD5) = static_cast<std::uint8_t>(q >> 8);
    }
}
// $8508: the noise channel on ($4015 = 7) or the period registers zero (a fresh note).
void Engine::sound_note_off_check() {
    if (r(0xCE) == 1) { apu(0x15, 7); return; }
    const unsigned x = r(0xCB) + 2u;
    apu(x, 0); apu(x + 1, 0);
}
// $8522: a note's envelope from the start ($D4: the byte with its volume).
void Engine::sound_envelope_reset() {
    ch(0x14) &= 0x7F;
    if (ch(0x16) & 0x80) {
        std::uint8_t a = ch(r(0xD4));
        if (r(0xCE) != 2) a &= 0x0F;
        ch(0x1E) = a;
    }
    for (unsigned y = 0x18; y < 0x1E; ++y) ch(y) = 0;
    ch(0x1C) = 0xFF;
}
// $8554: the same, its last period high byte kept (a tie).
void Engine::sound_tie() {
    const std::uint8_t keep = ch(0x1C);
    sound_envelope_reset();
    ch(0x1C) = keep;
}
// $82F6: the channel's volume ($0C; the fade $C8/$C9; the vibrato-like volume swing $0D-$0F) to the APU, then $8386.
void Engine::sound_output() {
    std::uint8_t a = ch(0x0C);
    if (r(0xCE) != 2) a &= 0x0F;
    r(0xD4) = a;
    if (r(0xC8) & 0x7F) {
        std::uint8_t y = r(0xC9);
        if (r(0xCE) == 2) { unsigned s = r(0xC9); for (int i = 0; i < 0x0C; ++i) s = (s + r(0xC9)) & 0xFF; y = static_cast<std::uint8_t>(s); }
        if (!(r(0xC8) & 0x80)) {
            std::uint8_t x = 0xFF;
            for (;;) {
                ++x;
                if (x == r(0xD4)) break;
                if (--y == 0) { r(0xD4) = x; break; }
            }
        } else {
            for (;;) {
                if (--r(0xD4) == 0) break;
                if (--y == 0) break;
            }
        }
    }
    if (r(0xCE) != 2) {
        const std::uint8_t x = ch(0x0D);
        a = x & 0x7F;
        if (a) {
            bool zero = false;
            if (a != ch(0x0E)) a = ch(0x0F) & 0x0F;
            else {
                ch(0x0E) = 0;
                r(0xD5) = static_cast<std::uint8_t>(ch(0x0F) >> 4);
                if (x & 0x80) r(0xD5) = static_cast<std::uint8_t>(-r(0xD5));
                a = static_cast<std::uint8_t>((ch(0x0F) & 0x0F) + r(0xD5));
                if (a & 0x80) { a = 0; zero = true; }
            }
            if (!zero && a >= r(0xD4)) a = r(0xD4);
            r(0xD4) = a;
            ch(0x0F) = (ch(0x0F) & 0xF0) | r(0xD4);
        }
    }
    if (r(0xCF) & 1) { r(0xD5) = 9; sound_pitch(); return; }
    r(0xD5) = 0x0C;
    sound_volume();
}
// $8399: the volume envelope ($16: its rate, $17 its step, $1E the level), the duty bits from byte $D5 & $7F, to
// register $4000 + $CB; then $8408.
void Engine::sound_volume() {
    const std::uint8_t rate = ch(0x16) & 0x7F;
    if (rate) {
        if (rate == ch(0x1D)) {
            ch(0x1D) = 0;
            std::uint8_t a = static_cast<std::uint8_t>(ch(0x17) + ch(0x1E));
            bool negate = true;
            if (a == 0 || (a & 0x80)) ch(0x1E) = 1;
            else {
                ch(0x1E) = a;
                if (a < 0x10) negate = false;
                else ch(0x1E) = 0x0F;
            }
            if (negate) ch(0x17) = static_cast<std::uint8_t>(-ch(0x17));
        }
        if (ch(0x1E) < r(0xD4)) r(0xD4) = ch(0x1E);
    }
    if (r(0xCE) != 2) r(0xD4) = (ch(r(0xD5) & 0x7F) & 0xF0) | r(0xD4);
    apu(r(0xCB), r(0xD4));
    r(0xD5) = (r(0xD5) & 0x80) ? 0x90 : 9;
    sound_pitch();
}
// $8408: the pitch slide (byte $D5 & $7F: a signed step added to the next two), the vibrato ($14/$15/$19-$1B), the
// period to $4002/$4003 + $CB (the high byte only when changed, $1C); the noise's period and mode.
void Engine::sound_pitch() {
    std::uint8_t y = r(0xD5) & 0x7F;
    const std::uint8_t d = ch(y);
    if (d) {
        const std::uint8_t sign = (d & 0x80) ? 0xFF : 0;
        ++y;
        const unsigned s = ch(y) + d;
        ch(y) = static_cast<std::uint8_t>(s);
        ++y;
        ch(y) = static_cast<std::uint8_t>(ch(y) + sign + (s >> 8));
    }
    if (!(r(0xD5) & 0x80) && (r(0xCF) & 1)) return;
    const std::uint8_t vib = ch(0x14) & 0x7F;
    if (vib && vib == ch(0x18)) {
        ch(0x18) = 0;
        std::uint8_t x = 0;
        r(0xD4) = static_cast<std::uint8_t>(ch(0x15) >> 5);
        if (ch(0x19) & 0x80) { r(0xD4) = static_cast<std::uint8_t>(-r(0xD4)); x = 0xFF; }
        const unsigned s = r(0xD4) + ch(0x1A);
        ch(0x1A) = static_cast<std::uint8_t>(s);
        ch(0x1B) = static_cast<std::uint8_t>(ch(0x1B) + x + (s >> 8));
        r(0xD4) = ch(0x15) & 0x1F;
        ++ch(0x19);
        if ((ch(0x19) & 0x7F) == r(0xD4)) {
            ch(0x19) &= 0x80;
            if (ch(0x14) & 0x80) ch(0x14) &= 0x7F;
            else {
                ch(0x14) |= 0x80;
                ch(0x19) ^= 0x80;
            }
        }
    }
    r(0xD5) = static_cast<std::uint8_t>((r(0xD5) & 0x7F) + 1);
    unsigned s = ch(0x1A) + ch(r(0xD5));
    std::uint8_t x = static_cast<std::uint8_t>(s);
    ++r(0xD5);
    std::uint8_t hi = static_cast<std::uint8_t>(ch(0x1B) + ch(r(0xD5)) + (s >> 8));
    if (r(0xCE) == 1) {
        apu(0x15, 0x0F);
        x &= 0x0F;
        ++r(0xD5);
        r(0xD4) = ch(r(0xD5)) & 0x80;
        x |= r(0xD4);
        hi = 0;
    }
    const unsigned reg = r(0xCB) + 2u;
    apu(reg, x);
    if (hi == ch(0x1C)) return;
    ch(0x1C) = hi;
    apu(reg + 1, hi | 8);
}
// $8579: a channel's effect step: a held note ($D2 frames) keeps its volume ($13) and pitch ($11/$12); else the
// effect's commands from ($D0): a note (bit 7: $0F a tie, else bits 2-0 + the next byte the period), or the table
// $85D1: a hold, $10, the duty / volume ($13), a loop, the envelope ($14-$17), the end.
void Engine::sound_effect_step() {
    if (r(0xD2) != 0) {
        if ((ch(0x11) | ch(0x12)) == 0) return;
        std::uint8_t a = ch(0x13);
        if (r(0xCE) != 2) a &= 0x0F;
        r(0xD4) = a;
        r(0xD5) = 0x93;
        sound_volume();
        return;
    }
    for (;;) {
        const std::uint8_t x = sound_effect_byte();
        if (x & 0x80) {
            const std::uint8_t a = x & 0x0F;
            if (a == 0x0F) { sound_effect_byte(); sound_tie(); return; }
            r(0xD4) = a & 7;
            ch(0x11) = sound_effect_byte();
            ch(0x12) = r(0xD4);
            r(0xD4) = 0x13;
            sound_envelope_reset();
            sound_note_off_check();
            return;
        }
        dispatch(0x85D1, x); // $85CE: jsr $8562
        switch (x) {
            case 0: r(0xD2) = sound_effect_byte(); continue;
            case 1: ch(0x10) = sound_effect_byte(); continue;
            case 2: { const std::uint8_t a = sound_effect_byte(); r(0xD4) = a; ch(0x13) = (ch(0x13) & 0x3F) | a; continue; }
            case 3: {
                std::uint8_t a = sound_effect_byte();
                if (r(0xCE) != 2) { r(0xD4) = a; a = (ch(0x13) & 0xC0) | a; }
                ch(0x13) = a;
                continue;
            }
            case 4: {
                const std::uint8_t n = sound_effect_byte();
                if (n) {
                    if (n == r(0xD3)) {
                        r(0xD3) = 0;
                        const unsigned q = (r(0xD0) | r(0xD1) << 8) + 2;
                        r(0xD0) = static_cast<std::uint8_t>(q); r(0xD1) = static_cast<std::uint8_t>(q >> 8);
                        continue;
                    }
                    ++r(0xD3);
                }
                r(0xD4) = sound_effect_byte();
                r(0xD1) = sound_effect_byte();
                r(0xD0) = r(0xD4);
                continue;
            }
            case 5:
                for (r(0xD4) = 0x14; r(0xD4) != 0x18; ++r(0xD4)) { const std::uint8_t b = sound_effect_byte(); ch(r(0xD4)) = b; }
                continue;
            case 6: { // the effect's end: the channel back to the music
                const unsigned q = (r(0xD0) | r(0xD1) << 8) - 1;
                r(0xD0) = static_cast<std::uint8_t>(q); r(0xD1) = static_cast<std::uint8_t>(q >> 8);
                r(0xC0) &= 0x0F; r(0xC1) = 0; r(0xCF) &= 0xFE;
                if ((ch(0x0A) | ch(0x0B)) == 0) {
                    sound_silence(r(0xCE), static_cast<std::uint8_t>(r(0xCB) + 2));
                    if ((ch(0) | ch(1)) == 0) return;
                }
                sound_instrument(ch(6) & 0x1F);
                r(0xD4) = 0x0C;
                sound_envelope_reset();
                return;
            }
            default: throw Unported(0x85CE);
        }
    }
}
// $86AC: the effect's next byte.
std::uint8_t Engine::sound_effect_byte() {
    const std::uint16_t p = static_cast<std::uint16_t>(r(0xD0) | r(0xD1) << 8);
    const std::uint8_t x = m.read(p);
    const std::uint16_t q = static_cast<std::uint16_t>(p + 1);
    r(0xD0) = static_cast<std::uint8_t>(q); r(0xD1) = static_cast<std::uint8_t>(q >> 8);
    return x;
}
}
