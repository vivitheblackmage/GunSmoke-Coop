#include "apu.hpp"
#include <algorithm>
#include <cmath>

namespace rg::nes {
namespace {
constexpr std::array<std::uint8_t, 32> kLength = {10, 254, 20, 2,  40, 4,  80, 6,  160, 8,  60, 10, 14, 12, 26, 14,
                                                  12, 16,  24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
constexpr std::array<std::array<std::uint8_t, 8>, 4> kDuty = {{{0, 1, 0, 0, 0, 0, 0, 0},
                                                               {0, 1, 1, 0, 0, 0, 0, 0},
                                                               {0, 1, 1, 1, 1, 0, 0, 0},
                                                               {1, 0, 0, 1, 1, 1, 1, 1}}};
constexpr std::array<std::uint16_t, 16> kNoisePeriod = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
// The frame counter's steps in CPU cycles (NTSC): quarter frames at each, half frames at the 2nd and the last.
constexpr std::array<std::uint32_t, 4> kFour = {7457, 14913, 22371, 29829};
constexpr std::array<std::uint32_t, 4> kFive = {7457, 14913, 22371, 37281};
}

Apu::Apu(unsigned sample_rate) : rate_(sample_rate ? sample_rate : 48000), sample_step_(kCpuHz / rate_) {
    pulse_[1].second = true;
}

void Apu::Envelope::quarter() {
    if (start) { start = false; decay = 15; divider = volume; return; }
    if (divider) { --divider; return; }
    divider = volume;
    if (decay) --decay;
    else if (loop) decay = 15;
}

std::uint16_t Apu::Pulse::target() const noexcept {
    const std::uint16_t change = period >> sweep_shift;
    if (!sweep_negate) return static_cast<std::uint16_t>(period + change);
    // Pulse 1 subtracts with one's complement (one less).
    const int t = period - change - (second ? 0 : 1);
    return static_cast<std::uint16_t>(t < 0 ? 0 : t);
}
void Apu::Pulse::clock() { // every other CPU cycle
    if (timer) { --timer; return; }
    timer = period;
    step = static_cast<std::uint8_t>((step + 1) & 7);
}
void Apu::Pulse::half() {
    if (length && !halt) --length;
    if (sweep_divider == 0 && sweep_on && sweep_shift && !muted()) period = target();
    if (sweep_divider == 0 || sweep_reload) { sweep_divider = sweep_period; sweep_reload = false; }
    else --sweep_divider;
}
std::uint8_t Apu::Pulse::output() const noexcept {
    if (!length || muted() || !kDuty[duty][step]) return 0;
    return envelope.output();
}

void Apu::Triangle::clock() { // every CPU cycle
    if (timer) { --timer; return; }
    timer = period;
    if (length && linear && period >= 2) step = static_cast<std::uint8_t>((step + 1) & 31); // ultrasonic periods hold
}
void Apu::Triangle::quarter() {
    if (reload) linear = linear_reload;
    else if (linear) --linear;
    if (!control) reload = false;
}
std::uint8_t Apu::Triangle::output() const noexcept { return step < 16 ? static_cast<std::uint8_t>(15 - step) : static_cast<std::uint8_t>(step - 16); }

void Apu::Noise::clock() { // every CPU cycle (the period table counts CPU cycles)
    if (timer) { --timer; return; }
    timer = static_cast<std::uint16_t>(period - 1);
    const std::uint16_t feedback = (shift ^ (shift >> (mode ? 6 : 1))) & 1;
    shift = static_cast<std::uint16_t>((shift >> 1) | (feedback << 14));
}

void Apu::write(unsigned reg, std::uint8_t value) {
    if (reg < 8) {
        Pulse& p = pulse_[reg >> 2];
        switch (reg & 3) {
            case 0:
                p.duty = value >> 6; p.halt = value & 0x20; p.envelope.loop = p.halt;
                p.envelope.constant = value & 0x10; p.envelope.volume = value & 0x0F;
                break;
            case 1:
                p.sweep_on = value & 0x80; p.sweep_period = (value >> 4) & 7; p.sweep_negate = value & 8;
                p.sweep_shift = value & 7; p.sweep_reload = true;
                break;
            case 2: p.period = static_cast<std::uint16_t>((p.period & 0x700) | value); break;
            case 3:
                p.period = static_cast<std::uint16_t>((p.period & 0xFF) | (value & 7) << 8);
                if (p.enabled) p.length = kLength[value >> 3];
                p.step = 0; p.envelope.start = true;
                break;
        }
        return;
    }
    switch (reg) {
        case 0x08: triangle_.control = value & 0x80; triangle_.linear_reload = value & 0x7F; break;
        case 0x0A: triangle_.period = static_cast<std::uint16_t>((triangle_.period & 0x700) | value); break;
        case 0x0B:
            triangle_.period = static_cast<std::uint16_t>((triangle_.period & 0xFF) | (value & 7) << 8);
            if (triangle_.enabled) triangle_.length = kLength[value >> 3];
            triangle_.reload = true;
            break;
        case 0x0C:
            noise_.halt = value & 0x20; noise_.envelope.loop = noise_.halt;
            noise_.envelope.constant = value & 0x10; noise_.envelope.volume = value & 0x0F;
            break;
        case 0x0E: noise_.mode = value & 0x80; noise_.period = kNoisePeriod[value & 0x0F]; break;
        case 0x0F:
            if (noise_.enabled) noise_.length = kLength[value >> 3];
            noise_.envelope.start = true;
            break;
        case 0x15:
            pulse_[0].enabled = value & 1; if (!pulse_[0].enabled) pulse_[0].length = 0;
            pulse_[1].enabled = value & 2; if (!pulse_[1].enabled) pulse_[1].length = 0;
            triangle_.enabled = value & 4; if (!triangle_.enabled) triangle_.length = 0;
            noise_.enabled = value & 8; if (!noise_.enabled) noise_.length = 0;
            break;
        case 0x17:
            five_step_ = value & 0x80;
            frame_cycle_ = 0;
            if (five_step_) { quarter_frame(); half_frame(); }
            break;
        default: break; // $4009, $400D, the DMC ($4010-$4013)
    }
}

void Apu::quarter_frame() {
    pulse_[0].envelope.quarter(); pulse_[1].envelope.quarter();
    triangle_.quarter();
    noise_.envelope.quarter();
}
void Apu::half_frame() {
    pulse_[0].half(); pulse_[1].half();
    if (triangle_.length && !triangle_.control) --triangle_.length;
    if (noise_.length && !noise_.halt) --noise_.length;
}

float Apu::mix() const noexcept {
    const unsigned p = pulse_[0].output() + pulse_[1].output();
    const float pulse = p ? 95.88f / (8128.0f / static_cast<float>(p) + 100.0f) : 0.0f;
    const float t = triangle_.output() / 8227.0f, n = noise_.output() / 12241.0f;
    const float tnd = (t + n) > 0.0f ? 159.79f / (1.0f / (t + n) + 100.0f) : 0.0f;
    return pulse + tnd; // 0 .. ~1
}

void Apu::run(std::uint32_t cycles, std::vector<std::int16_t>& out) {
    const auto& steps = five_step_ ? kFive : kFour;
    for (std::uint32_t c = 0; c < cycles; ++c) {
        ++frame_cycle_;
        for (unsigned i = 0; i < 4; ++i) {
            if (frame_cycle_ != steps[i]) continue;
            if (five_step_ && i == 3) { quarter_frame(); half_frame(); }
            else { quarter_frame(); if (i == 1 || i == 3) half_frame(); }
        }
        if (frame_cycle_ >= steps[3]) frame_cycle_ = 0;
        triangle_.clock();
        noise_.clock();
        odd_ = !odd_;
        if (odd_) { pulse_[0].clock(); pulse_[1].clock(); }
        sum_ += mix(); ++sum_count_;
        sample_phase_ += 1.0;
        if (sample_phase_ >= sample_step_) {
            sample_phase_ -= sample_step_;
            const float in = static_cast<float>(sum_ / sum_count_);
            sum_ = 0.0; sum_count_ = 0;
            // A first-order high-pass near 90 Hz removes the DC the non-linear mix carries.
            const float a = 1.0f / (1.0f + 2.0f * 3.14159265f * 90.0f / static_cast<float>(rate_));
            high_pass_out_ = a * (high_pass_out_ + in - high_pass_in_);
            high_pass_in_ = in;
            const float s = std::clamp(high_pass_out_ * 1.6f, -1.0f, 1.0f);
            out.push_back(static_cast<std::int16_t>(std::lround(s * 32767.0f)));
        }
    }
}
}
