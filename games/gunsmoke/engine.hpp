#pragma once
#include "compat/nes/machine.hpp"
#include <array>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

// Gun.Smoke (NES, USA) ported natively: the original's routines in C++ over rg::nes::Machine, checked frame by
// frame against the original (tools/nes/gunsmoke-oracle.mjs, tools/native/verify_gunsmoke.cpp). Addresses in the
// comments are the original's; the user's PRG is read for its tables.
//
// The original runs up to four cooperative tasks ($C000): slot k at $10+4k (state: 0 free, 1 waiting $11+4k frames,
// 2 running, 4 ready, 8 start at $12/$13+4k), each with its own stack. A task waits with $C0B0 ("one frame") and the
// NMI counts the waits down. Here a task is a C++ coroutine; a wait suspends the whole chain of its calls.
namespace gunsmoke {
class Engine;

// A routine that may wait for frames. co_await-ing one runs it until it ends (or waits: then the task suspends).
class Routine {
public:
    struct promise_type {
        std::coroutine_handle<> continuation;
        std::exception_ptr error;
        Routine get_return_object() { return Routine{std::coroutine_handle<promise_type>::from_promise(*this)}; }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                const auto c = h.promise().continuation; return c ? c : std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };
    explicit Routine(std::coroutine_handle<promise_type> h) : h_(h) {}
    Routine(Routine&& o) noexcept : h_(o.h_) { o.h_ = {}; }
    Routine(const Routine&) = delete;
    ~Routine() { if (h_) h_.destroy(); }
    bool await_ready() const noexcept { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept { h_.promise().continuation = caller; return h_; }
    void await_resume() { if (h_.promise().error) std::rethrow_exception(h_.promise().error); }
    std::coroutine_handle<promise_type> handle() const noexcept { return h_; }
private:
    std::coroutine_handle<promise_type> h_;
};

// A part of the original not ported yet was reached (the verifier reports the address).
struct Unported : std::runtime_error { explicit Unported(unsigned address); };
// The original's jump back to the title ($1:B000 with the stack reset): task 0 starts over there (fade: the demo's
// record ran out, $C1BC).
struct RestartTitle { bool fade; };

class Engine {
public:
    explicit Engine(std::vector<std::uint8_t> prg);
    rg::nes::Machine m;
    // Two players (an enhancement): off is the original. Player 2's pad: m.pads[1].
    bool coop = false;
    // Player 2's saved state (for the state hash of lockstep peers).
    std::vector<std::uint8_t> second_state() const;
    // Co-op: which of the last uploaded OAM's 64 sprites are player 2's (for the host's colour options, drawn only).
    const std::array<bool, 64>& her_oam() const noexcept { return her_drawn_; }
    // $E651 (reset) up to the main loop: RAM cleared, the sound queue empty, task 0 to start at $E6AD.
    void power_on();
    // The main loop ($C000) until every task waits: what runs between two NMIs. The original's scan goes on after an
    // NMI from where the NMI interrupted it ($20 is never set), so which ready task runs first depends on its cycle
    // timing: first_slot is that slot (the oracle records it); in play the port takes slot 0 first (a known difference
    // in the order of the tasks within a frame).
    // stop_mask: tasks to leave for after the NMI (the original's NMI came before their turn: a long frame).
    void run_tasks(unsigned first_slot = 0, unsigned stop_mask = 0);
    // $E4C6: the NMI (PPU uploads from the buffers, scroll, the frame counter $22, the tasks' waits).
    void nmi(int busy = -1); // busy: the task a lag NMI interrupts in the original (its wait not counted yet)

    // Task waits: the awaitable of $C0B0/$C087 (A frames).
    struct Wait {
        Engine& e; std::uint8_t frames;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> h) noexcept;
        void await_resume() const noexcept {}
    };
    // A task ending itself ($C0A0: its state 0): suspended, never resumed.
    struct Stop {
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<>) const noexcept {}
        void await_resume() const noexcept {}
    };
private:
    struct Task { Routine* top = nullptr; std::coroutine_handle<> resume; std::vector<Routine> owned; };
    std::array<Task, 4> tasks_{};
    unsigned current_ = 0; // $21
    std::uint8_t draw_y_ = 0;      // Y after $C978 / $CA0C (and so after $C945 when an object was drawn)
    std::uint8_t aim_y_ = 0;       // Y after $C7E6: the table index
    std::uint8_t palette_y_ = 0;   // Y after $C4E4
    // Co-op: the other player's block ($70-$88, $90-$9F, $F5/$F7, OAM $0200-$0217) while the RAM holds one.
    struct Player {
        std::array<std::uint8_t, 0x19> block{};
        std::array<std::uint8_t, 0x10> guns{};
        std::uint8_t pad_new = 0, pad_held = 0;
        std::array<std::uint8_t, 0x18> oam{};
        bool active = false;
    };
    Player other_;
    unsigned ram_owner_ = 0;      // whose player the RAM holds (0: player 1)
    bool coop_busy_ = false;      // a death or a mount runs: the other player is not checked against objects
    bool her_turn_ = false;       // player 2's hit, mount or bomb runs with her block in
    std::array<bool, 64> her_ram_{}, her_drawn_{}; // player 2's sprites in $0200 / in the OAM uploaded at the NMI
    void her_sprites();
    bool coop_out_ = false;       // set by player_hit: the hit player had no lives left (the other plays on)
    void swap_players();
    void coop_init();
    void coop_place_other();
    void coop_other_frame();
    void coop_other_pad();
    void coop_target(std::uint8_t x, std::uint8_t& ty, std::uint8_t& tx);
    std::uint8_t collision_y_ = 0; // Y after $C6C4: the metatile quarter
    std::uint8_t fade_x_ = 0; // X after $C49F: $A2 from the object scan, or $C4E4's while $A4 holds the sprites

    Routine start(std::uint16_t entry);
    Wait wait(std::uint8_t frames = 1) { return Wait{*this, frames}; }
    std::uint8_t& r(std::uint16_t a) { return m[a]; }

    // Bank 7 routines (no waits).
    void start_task(std::uint8_t task, std::uint16_t entry);   // $C06F with $23/$24
    void select_bank(std::uint8_t bank);                        // $C361
    void bank_write(std::uint8_t bank);                         // $C363
    void sound_command(std::uint8_t command, std::uint8_t x);   // $C36A (X saved in $F3)
    void read_pads();                                           // $C15A
    void nmi_off();                                             // $C0BB
    void nmi_on();                                              // $C0C5
    void rendering_off();                                       // $C0CF
    void rendering_on();                                        // $C0D9
    void vram_script();                                         // $C0DF
    void inline_pointer(std::uint16_t call_site);               // $C134 for a JSR at call_site
    void copy_tiles(std::uint8_t a, std::uint8_t x, std::uint8_t y, std::uint16_t call_site); // $C266
    void upload_tiles();                                        // $C26F: $02 tiles between ($00) and VRAM $03/$04
    void fill_nametable(std::uint8_t a, std::uint8_t x, std::uint8_t y); // $C209
    void fill_pages(std::uint8_t a, std::uint8_t x, std::uint8_t y);     // $C24E
    void hide_sprites();                                        // $C353
    void title_graphics();                                      // $C41F
    std::uint8_t load_palette();                                // $C4E4 (returns X: past the last colour)
    void objects();                                             // $C945
    void draw_object(std::uint8_t x);                           // $C978
    void set_object(std::uint8_t a, std::uint8_t& x, std::uint8_t y); // $C796 (Y kept in $32)
    void game_init(std::uint8_t y);                             // $E711 (Y: the caller's)
    void poster_setup();                                        // $0:BA80
    void stage_tiles();                                         // $E8A4 (the poster's), then $E75D-$E7B4 (the stage's)
    void stage_graphics();                                      // $E75D-$E7B8
    void save_rows();                                           // $F68B
    void restore_rows();                                        // $F6A1
    void row_tiles(std::uint8_t column);                        // $E9F7
    void row_attributes();                                      // $EA57
    void next_row();                                            // $E98F
    std::uint8_t collision(std::uint8_t x, std::uint8_t y);     // $C6C4: bits 7-6 of the metatile quarter at (x, y)
    void place_player(std::uint8_t y);                          // $C733
    void clear_play();                                          // $E88D
    void scroll_step();                                         // $E93A-$E9F2: one line of scroll, its row's part
    void stage_events();                                        // $EACC
    void stage_event_step();                                    // $EB06
    void player();                                              // $EB82-$ED4C: the player's move and sprites' places
    void fire();                                                // $ED89-$EF12: the walk animation, the shots
    void player_sprites();                                      // $EF13-$F011
    void shots();                                               // $F1E2: the player's shots ($08-$0D)
    void enemy_shots();                                         // $E375
    void spawn();                                               // $FA30: the stage's enemies at this row
    void spawn_entry();                                         // $FA78: the entry at ($43), then the next
    void enemy(std::uint16_t handler, std::uint8_t x);          // bank 6's handler for object x ($B000 table)
    void gunman(std::uint8_t x);                                // $6:B284
    void rifleman(std::uint8_t x);                              // $6:B501
    void sniper(std::uint8_t x);                                // $6:B080
    void jumper(std::uint8_t x);                                // $6:B40B
    void swooper(std::uint8_t x);                               // $6:B46E (kind $63)
    void swooper_wait(std::uint8_t x);                          // $6:B4E4 (kind $64)
    void swooper_start(std::uint8_t x);                         // $6:B48C
    void bandit(std::uint8_t x);                                // $6:B0E5 (kind $56)
    void bandit_walk(std::uint8_t x);                           // $6:B153
    void bandit_fan(std::uint8_t x);                            // $6:B24B: three shots
    void zigzag(std::uint8_t x);                                // $6:B5BF
    void crawler(std::uint8_t x);                               // $6:B671
    void kneeler(std::uint8_t x);                               // $6:B775
    void still(std::uint8_t x);                                 // $6:B840: animation from its start
    void lurker(std::uint8_t x);                                // $6:B82F
    void riser(std::uint8_t x);                                 // $6:B849
    void popper(std::uint8_t x);                                // $6:B858
    void popper_aim(std::uint8_t x);                            // $6:B872
    void strider(std::uint8_t x);                               // $6:B8F4
    void thrower(std::uint8_t x);                               // $6:BA51
    void dynamiter(std::uint8_t x);                             // $6:BB29
    void falling(std::uint8_t x);                               // $E17B (kinds $40-$4C)
    void dropped_item(std::uint8_t x);                          // $E1AD (kinds $4D-$4F)
    std::uint8_t mirror(std::uint8_t a);                        // $E1E5: direction a mirrored left-right
    bool far_from_player(std::uint8_t x);                       // $6:B444: |dy| or |dx| >= $38
    std::uint8_t player_dy(std::uint8_t x);                     // $C7C7
    std::uint8_t player_dx(std::uint8_t x);                     // $C7D4
    std::uint8_t aim(std::uint8_t x);                           // $C7E6: the direction (0-31) from x to the player, in $37
    std::uint8_t aim_at(std::uint8_t x, std::uint8_t ty, std::uint8_t tx); // $C7E1: toward (tx, ty) (Y after: aim_y_)
    void maybe_fire(std::uint8_t x);                            // $C8AE
    void turn(std::uint8_t x);                                  // $C8C9: the direction one step toward $37
    std::uint8_t wall_ahead(std::uint8_t x);                    // $C8F8
    std::uint8_t move(std::uint8_t x);                          // $E11D: by its direction $04A0 (returns Y: the table index)
    std::uint8_t move_by(std::uint8_t x, std::uint8_t a);       // $E125 (returns Y)
    void enemy_shot(std::uint8_t x, std::uint8_t a);            // $E452
    bool touch_player(std::uint8_t x);                          // $CA83 (false: the object is gone, no drawing)
    bool shot_hits(std::uint8_t x);                             // $CC83
    void die(std::uint8_t a, std::uint8_t x, std::uint8_t y);   // $CD4E
    std::uint8_t score(std::uint8_t x, std::uint8_t y = 0);     // $E297 (y: Y on entry; returns Y)
    void status_sprites(bool hide);                             // $FA15 (hide) / $FA18
    void row_back();                                            // $F73E
    void window_row(std::uint16_t call_site);                   // $F788 (2 bytes after the JSR)
    void status_row();                                          // $F7E8
    void shop_row();                                            // $F8D7
    void status_step(std::uint8_t& x);                          // $0:B828
    void cursor(bool wide);                                     // $0:B871 / $0:B875 (wide)
    std::uint8_t buy(std::uint8_t x);                           // $0:BDF7
    void pay(std::uint8_t count, std::uint8_t x);               // $0:BE46
    void counters(std::uint8_t x);                              // $CBE2: the four guns' shots up
    void clear_enemies(std::uint8_t y);                         // $CDAB: the enemies scored and killed, their shots gone (Y on entry)
    // Y in and out: the original's Y register where a later $C796 keeps it in $32.
    std::uint8_t boss_1_move(std::uint8_t x, std::uint8_t y);   // $0:A0DD
    std::uint8_t boss_1_fire(std::uint8_t x, std::uint8_t y);   // $0:A11B
    std::uint8_t boss_2_move(std::uint8_t x, std::uint8_t y);   // $1:A0E8
    std::uint8_t boss_2_fire(std::uint8_t x, std::uint8_t y);   // $1:A105
    std::uint8_t boss_3_tick(std::uint8_t x, std::uint8_t y);   // $2:A175
    std::uint8_t boss_4_plan(std::uint8_t x, std::uint8_t y);   // $3:A131
    std::uint8_t boss_5_tick(std::uint8_t x, std::uint8_t y);   // $4:A11F
    std::uint8_t boss_6_move(std::uint8_t x, std::uint8_t y);   // $5:A120
    std::uint8_t boss_5_throw(std::uint8_t x, std::uint8_t y);  // $4:A138
    std::uint8_t boss_5_arc(std::uint8_t x, std::uint8_t ty, std::uint8_t tx); // $4:A1A4
    std::uint8_t boss_3_fan(std::uint8_t x, std::uint8_t y);    // $2:A21E: shots $62 from slot x down to 2, directions y on by 2
    bool boss_hit(std::uint8_t& y);                             // $E1F9 (true: the boss is beaten, $E244 follows)
    std::uint8_t add_score(std::uint8_t amount, std::uint8_t x); // $E2B0: amount index into $E2E9 (X kept in $F4; returns Y)
    void item(std::uint8_t x, std::uint8_t kind);               // $CB3D
    void power_down();                                          // $CC67
    void draw_frame(std::uint8_t frame);                        // $CA0C: frame's sprites at Y $36 / X $37 ($F4: mirrored)
    void sprite_text(std::uint16_t data);                       // $C30C: sprite rows (count, Y, attribute, X, tiles; 0 skips)
    void clear_objects();                                       // $F379

    // Routines that wait.
    Routine wait_frames(std::uint8_t& x);                       // $C0B4: X frames
    Routine fade(std::uint8_t a, std::uint8_t x, std::uint8_t y); // $C49F: $C496 in (a $30, x $F0), $C49C out ($10, $10)
    Routine task0();                                            // $E6AD
    Routine title_and_game();                                   // $E703: the title, the game from stage $41
    void boss_again();                                          // $FAEE: the boss's tasks started again
    Routine title(bool again = false);                          // $1:B003 (again: $1:B000 -> $B01C)
    Routine title_again(bool fade);                             // $B000 from the demo, then the game
    void restart_title(unsigned task, bool fade);               // task 0 replaced by title_again
    Routine attract();                                          // $1:B26B
    void attract_frame();                                       // $1:B3F9
    Routine attract_line(std::uint8_t x);                       // $1:B363
    void attract_address(std::uint8_t x);                       // $1:B3E6
    Routine random_task();                                      // $FF00
    Routine type_text(std::uint8_t delay, std::uint8_t sound, std::uint16_t text, std::uint8_t& x); // $C2C4
    Routine poster();                                           // $0:BA80 (with its wait)
    Routine wanted_poster();                                    // $0:B896
    Routine stage_load(bool poster = true);                     // $E757 (no poster: $E75D, the demo)
    Routine draw_stage();                                       // $F6B7
    Routine draw_rows(std::uint8_t count, std::uint8_t back, bool redraw); // $F6B7 / $F6C5 (redraw), from $F6D7
    Routine open_window();                                      // $F571
    Routine status();                                           // $0:B800
    Routine shop();                                             // $0:BD4E
    Routine type_here(std::uint8_t delay, std::uint8_t sound, std::uint8_t& x); // $C2B8: ($01) = count, text; at $036A/B
    Routine play();                                             // $E91D: the game loop
    Routine enter_player();                                     // $E85E
    Routine player_hit();                                       // $F31C
    Routine game_over();                                        // $F3F4
    Routine horse_mount();                                      // $FBD9
    Routine boss_intro();                                       // $C38B
    Routine boss_1();                                           // $0:A000 (task 1)
    Routine boss_1_shots();                                     // $0:A1E5 (task 2)
    Routine boss_2();                                           // $1:A000 (task 1)
    Routine boss_2_shots();                                     // $1:A17F (task 2)
    Routine boss_3();                                           // $2:A000 (task 1)
    Routine boss_3_shots();                                     // $2:A1EE (task 2)
    Routine boss_4();                                           // $3:A000 (task 1)
    Routine boss_4_shots();                                     // $3:A23C (task 2)
    Routine boss_5();                                           // $4:A000 (task 1)
    Routine boss_5_shots();                                     // $4:A24B (task 2)
    Routine boss_6();                                           // $5:A000 (task 1)
    Routine boss_6_shots();                                     // $5:A1B4 (task 2)
    Routine boss_end();                                         // $E244: the boss beaten (task 1 ends)
    Routine stage_won();                                        // $0:B932: the poster shot through, the bounty, the next stage
    Routine ending();                                           // $E8DB, $1:B7C0
    Routine ending_text();                                      // $1:B917
    Routine ending_line(std::uint8_t x);                        // $1:B97C: line x written whole
    void ending_address(std::uint8_t x);                        // $1:B9C0
    void ending_pointer(std::uint8_t x);                        // $1:B9F0
    void high_score();                                          // $F434
    // The sound driver (bank 6, sound.cpp).
    void sound_tick();                                          // $8000 ($823F)
    void sound_play(std::uint8_t command);                      // $8003
    std::uint8_t& ch(unsigned y);                               // ($CC),y: the current channel's block
    void apu(unsigned x, std::uint8_t value);                   // $4000 + x
    void dispatch(std::uint16_t table, std::uint8_t x);         // $8562
    void sound_stop_effect();                                   // $813F
    void sound_stop_music();                                    // $8193
    void sound_restore_music();                                 // $8171
    void sound_reload_instrument();                             // $81CC
    void sound_clear_tail();                                    // $81BA
    void sound_next_channel_rotate();                           // $820F
    void sound_silence(std::uint8_t y, std::uint8_t x);         // $822A
    void sound_music_steps();                                   // $86C0
    void sound_music_step();                                    // $86CF
    void sound_music_tick();                                    // $86DF
    std::uint8_t sound_music_byte();                            // $8941
    void sound_duration(std::uint8_t a);                        // $8960
    void sound_commands();                                      // $8712
    void sound_note_end(std::uint8_t x);                        // $8740
    void sound_vibrato_reset();                                 // $88B5
    void sound_instrument(std::uint8_t x);                      // $88ED
    void sound_note_off_check();                                // $8508
    void sound_envelope_reset();                                // $8522
    void sound_tie();                                           // $8554
    void sound_output();                                        // $82F6
    void sound_volume();                                        // $8399
    void sound_pitch();                                         // $8408
    void sound_effect_step();                                   // $8579
    std::uint8_t sound_effect_byte();                           // $86AC
};
}
