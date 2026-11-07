#pragma once
#include <array>
#include <cstdint>
#include <vector>

// The NES APU (2A03, NTSC) for natively ported NES games: register writes in, 16-bit mono PCM out. Two pulse
// channels (duty, envelope, sweep, length counter), the triangle (linear counter), the noise (15-bit LFSR) and the
// frame counter (4/5-step, quarter and half frames), mixed with the console's non-linear formulas, averaged down to
// the host's sample rate with a DC-blocking high-pass (the console's output capacitor). No DMC (its registers are
// accepted and ignored) and no frame IRQ. General: no game code. Timing is in CPU cycles (1789773 Hz).
namespace rg::nes {
class Apu {
public:
    static constexpr double kCpuHz = 1789773.0;
    explicit Apu(unsigned sample_rate = 48000);
    // A write to $4000 + reg (reg 0-$17).
    void write(unsigned reg, std::uint8_t value);
    // Runs `cycles` CPU cycles, appending the samples they produce.
    void run(std::uint32_t cycles, std::vector<std::int16_t>& out);
    unsigned sample_rate() const noexcept { return rate_; }

private:
    struct Envelope {
        bool start = false, loop = false, constant = false;
        std::uint8_t volume = 0, divider = 0, decay = 0;
        void quarter();
        std::uint8_t output() const noexcept { return constant ? volume : decay; }
    };
    struct Pulse {
        bool second = false, enabled = false, halt = false;
        std::uint8_t duty = 0, step = 0, length = 0;
        std::uint16_t period = 0, timer = 0;
        bool sweep_on = false, sweep_negate = false, sweep_reload = false;
        std::uint8_t sweep_period = 0, sweep_shift = 0, sweep_divider = 0;
        Envelope envelope;
        std::uint16_t target() const noexcept;
        bool muted() const noexcept { return period < 8 || target() > 0x7FF; }
        void clock();
        void half();
        std::uint8_t output() const noexcept;
    };
    struct Triangle {
        bool enabled = false, control = false, reload = false;
        std::uint8_t length = 0, linear = 0, linear_reload = 0, step = 0;
        std::uint16_t period = 0, timer = 0;
        void clock();
        void quarter();
        std::uint8_t output() const noexcept;
    };
    struct Noise {
        bool enabled = false, halt = false, mode = false;
        std::uint8_t length = 0;
        std::uint16_t period = 4, timer = 0, shift = 1;
        Envelope envelope;
        void clock();
        std::uint8_t output() const noexcept { return (length && !(shift & 1)) ? envelope.output() : 0; }
    };

    void quarter_frame();
    void half_frame();
    float mix() const noexcept;

    unsigned rate_;
    Pulse pulse_[2];
    Triangle triangle_;
    Noise noise_;
    bool five_step_ = false, odd_ = false;
    std::uint32_t frame_cycle_ = 0;
    double sample_step_, sample_phase_ = 0.0, sum_ = 0.0;
    std::uint32_t sum_count_ = 0;
    float high_pass_in_ = 0.0f, high_pass_out_ = 0.0f;
};
}
