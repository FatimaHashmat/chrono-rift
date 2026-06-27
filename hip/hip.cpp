//=============================================================
//  hip.cpp  —  Chrono Rift  |  CS 2006
//  Human Interfacing Process — one process per player.
//  Usage: ./hip <player_id>
//  Owns: one SFML window, one player thread, its player's input.
//=============================================================

#include "../common.h"
#include <SFML/Graphics.hpp>
#include <stdio.h>
#include <signal.h>
#include <string>

static GameState* gs        = NULL;
static int        MY_PLAYER = -1;

//=============================================================
//  SPRITE SHEET DATA
//=============================================================
struct AnimInfo {
    int frame_w;
    int frame_h;
    int frame_count;
};

// All sprites are 128px tall, frame_w = total_width / frame_count
// Players
static const AnimInfo P_ANIM[3][4] = {
    // Player1: Idle(6), Attack(4), Hurt(3), Dead(3)
    { {128,128,6}, {128,128,4}, {128,128,3}, {128,128,3} },
    // Player2: Idle(6), Attack(6), Hurt(2), Dead(3)
    { {128,128,6}, {128,128,6}, {128,128,2}, {128,128,3} },
    // Player3: Idle(6), Attack(4), Hurt(2), Dead(4)
    { {128,128,6}, {128,128,4}, {128,128,2}, {128,128,4} },
};

// Enemies
static const AnimInfo E_ANIM[3][4] = {
    // Enemy1: Idle(7), Attack(7), Hurt(3), Dead(3)
    { {128,128,7}, {128,128,7}, {128,128,3}, {128,128,3} },
    // Enemy2: Idle(7), Attack(10), Hurt(3), Dead(3)
    { {128,128,7}, {128,128,10}, {128,128,3}, {128,128,3} },
    // Enemy3: Idle(7), Attack(10), Hurt(3), Dead(3)
    { {128,128,7}, {128,128,10}, {128,128,3}, {128,128,3} },
};

// Anim indices
#define ANIM_IDLE   0
#define ANIM_ATTACK 1
#define ANIM_HURT   2
#define ANIM_DEAD   3

//=============================================================
//  PER-ENTITY ANIMATION STATE
//=============================================================
struct AnimState {
    int   anim_idx;       // current animation (IDLE/ATTACK/HURT/DEAD)
    int   cur_frame;
    float timer;
    float frame_dur;
    bool  done;           // one-shot finished
    bool  dead_shown;     // dead anim completed at least once

    // HP delta tracking — for triggering hurt/attack
    int   prev_hp;        // HP last frame
    int   prev_stamina;   // stamina last frame (enemy attack detection)
};

// Force-start a new animation (always restarts)
static void anim_force(AnimState& s, int anim_idx, float fps = 10.f) {
    s.anim_idx  = anim_idx;
    s.cur_frame = 0;
    s.timer     = 0.f;
    s.frame_dur = 1.f / fps;
    s.done      = false;
}

// Start animation only if not already playing this one
static void anim_set(AnimState& s, int anim_idx, float fps = 10.f) {
    if (s.anim_idx == anim_idx && !s.done) return;
    anim_force(s, anim_idx, fps);
}

static void anim_update(AnimState& s, float dt, int total_frames, bool loop) {
    if (s.done) return;
    s.timer += dt;
    if (s.timer >= s.frame_dur) {
        s.timer -= s.frame_dur;
        s.cur_frame++;
        if (s.cur_frame >= total_frames) {
            if (loop) {
                s.cur_frame = 0;
            } else {
                s.cur_frame = total_frames - 1;
                s.done = true;
                if (s.anim_idx == ANIM_DEAD) s.dead_shown = true;
            }
        }
    }
}

//=============================================================
//  SIGNAL HANDLERS
//=============================================================
static void handle_stun(int) {
    if (gs && MY_PLAYER >= 0)
        gs->input_state[MY_PLAYER].stunned_notify = 1;
}

static void handle_sigterm(int) {
    printf("\n[HIP-%d] Quit — sending SIGTERM to Arbiter\n", MY_PLAYER);
    if (gs && gs->arbiter_pid > 0)
        kill(gs->arbiter_pid, SIGTERM);
    if (gs) munmap(gs, sizeof(GameState));
    exit(0);
}

//=============================================================
//  DRAW HELPERS
//=============================================================
static void draw_bar(sf::RenderWindow& win,
                     float x, float y, float w, float h,
                     float frac, sf::Color fill, sf::Color bg) {
    sf::RectangleShape back({w, h});
    back.setPosition(x, y);
    back.setFillColor(bg);
    win.draw(back);
    if (frac > 0.f) {
        if (frac > 1.f) frac = 1.f;
        sf::RectangleShape front({w * frac, h});
        front.setPosition(x, y);
        front.setFillColor(fill);
        win.draw(front);
    }
}

static void draw_text(sf::RenderWindow& win, sf::Font& font,
                      const std::string& str, float x, float y,
                      unsigned size, sf::Color col) {
    sf::Text t;
    t.setFont(font);
    t.setCharacterSize(size);
    t.setFillColor(col);
    t.setString(str);
    t.setPosition(x, y);
    win.draw(t);
}

static bool in_rect(float mx, float my, float x, float y, float w, float h) {
    return mx >= x && mx <= x+w && my >= y && my <= y+h;
}

//=============================================================
//  RENDER THREAD
//=============================================================
static const float BTN_W = 120.f;
static const float BTN_H = 32.f;
static const float BTN_Y = 620.f;

static void* render_thread(void*) {
    std::string title = "Chrono Rift — Player " + std::to_string(MY_PLAYER);
    sf::RenderWindow window(sf::VideoMode(1280, 720), title);
    window.setFramerateLimit(60);

    sf::Font font;
    bool has_font =
        font.loadFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf") ||
        font.loadFromFile("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf") ||
        font.loadFromFile("/usr/share/fonts/truetype/freefont/FreeSans.ttf");

    //----------------------------------------------------------
    //  LOAD BACKGROUND
    //----------------------------------------------------------
    sf::Texture bg_tex;
    sf::Sprite  bg_sprite;
    bool has_bg = bg_tex.loadFromFile("OS_Sprites/Background/Battleground1.png");
    if (has_bg) {
        bg_sprite.setTexture(bg_tex);
        // Scale to fill 1280x720
        float sx = 1280.f / bg_tex.getSize().x;
        float sy = 720.f  / bg_tex.getSize().y;
        bg_sprite.setScale(sx, sy);
    }

    //----------------------------------------------------------
    //  LOAD PLAYER SPRITE SHEETS  [3 chars][4 anims]
    //----------------------------------------------------------
    sf::Texture p_tex[3][4];
    sf::Sprite  p_spr[3][4];
    bool        p_loaded[3][4] = {};

    const char* p_anim_names[4] = {"Idle","Attack","Hurt","Dead"};
    for (int c = 0; c < 3; c++) {
        char folder[64];
        snprintf(folder, sizeof(folder), "OS_Sprites/Player%d", c+1);
        for (int a = 0; a < 4; a++) {
            char path[128];
            snprintf(path, sizeof(path), "%s/%s.png", folder, p_anim_names[a]);
            if (p_tex[c][a].loadFromFile(path)) {
                p_spr[c][a].setTexture(p_tex[c][a]);
                p_loaded[c][a] = true;
            }
        }
    }

    //----------------------------------------------------------
    //  LOAD ENEMY SPRITE SHEETS  [3 chars][4 anims]
    //----------------------------------------------------------
    sf::Texture e_tex[3][4];
    sf::Sprite  e_spr[3][4];
    bool        e_loaded[3][4] = {};

    for (int c = 0; c < 3; c++) {
        char folder[64];
        snprintf(folder, sizeof(folder), "OS_Sprites/Enemy%d", c+1);
        for (int a = 0; a < 4; a++) {
            char path[128];
            snprintf(path, sizeof(path), "%s/%s.png", folder, p_anim_names[a]);
            if (e_tex[c][a].loadFromFile(path)) {
                e_spr[c][a].setTexture(e_tex[c][a]);
                e_loaded[c][a] = true;
            }
        }
    }

    //----------------------------------------------------------
    //  ANIMATION STATES
    //----------------------------------------------------------
    AnimState p_astate[MAX_PLAYERS] = {};
    AnimState e_astate[MAX_ENEMIES] = {};
    for (int i = 0; i < MAX_PLAYERS; i++) {
        p_astate[i].anim_idx    = ANIM_IDLE;
        p_astate[i].frame_dur   = 0.1f;
        p_astate[i].prev_hp     = gs->players[i].hp;
        p_astate[i].prev_stamina= gs->players[i].stamina;
    }
    for (int i = 0; i < MAX_ENEMIES; i++) {
        e_astate[i].anim_idx    = ANIM_IDLE;
        e_astate[i].frame_dur   = 0.1f;
        e_astate[i].prev_hp     = gs->enemies[i].hp;
        e_astate[i].prev_stamina= gs->enemies[i].stamina;
    }

    // Wait for game to start
    while (gs->game_started == 0 && !gs->game_over) {
        sf::Event ev;
        while (window.pollEvent(ev))
            if (ev.type == sf::Event::Closed) { window.close(); return NULL; }
        window.clear(sf::Color(10,10,20));
        if (has_font)
            draw_text(window, font,
                      "Player " + std::to_string(MY_PLAYER) + " — Waiting for game...",
                      400, 340, 20, sf::Color::White);
        window.display();
        usleep(100000);
    }

    // Input state
    int selected_action = -1;
    int hovered_enemy   = -1;
    int selected_enemy  = -1;
    int hovered_slot    = -1;
    int selected_slot   = -1;

    PlayerInputState* pis = &gs->input_state[MY_PLAYER];

    sf::Clock clock;

    while (window.isOpen()) {
        float dt = clock.restart().asSeconds();
        if (dt > 0.1f) dt = 0.1f; // clamp

        sf::Vector2i mouse_px = sf::Mouse::getPosition(window);
        float mx = (float)mouse_px.x;
        float my = (float)mouse_px.y;

        //------------------------------------------------------
        //  UPDATE ANIMATION STATES  (HP-delta based)
        //------------------------------------------------------
        for (int i = 0; i < gs->num_players; i++) {
            PlayerChar* p = &gs->players[i];
            AnimState&  s = p_astate[i];
            int char_idx  = i % 3;

            // Dead — always wins, play once then freeze
            if (!p->active || p->state == STATE_DEAD) {
                if (s.anim_idx != ANIM_DEAD)
                    anim_force(s, ANIM_DEAD, 8.f);
            }
            // Player's own turn + submitted action → attack anim (one-shot)
            else if (gs->whose_turn == i &&
                     gs->whose_turn_kind == ENTITY_PLAYER &&
                     gs->player_action.ready &&
                     gs->player_action.player_id == i &&
                     s.anim_idx != ANIM_ATTACK) {
                anim_force(s, ANIM_ATTACK, 10.f);
            }
            // HP dropped since last frame → hurt anim (one-shot)
            else if (p->hp < s.prev_hp && s.anim_idx != ANIM_DEAD) {
                anim_force(s, ANIM_HURT, 10.f);
            }
            // One-shot finished or idle — go back to idle
            else if (s.anim_idx != ANIM_DEAD &&
                     (s.anim_idx == ANIM_IDLE || s.done)) {
                anim_set(s, ANIM_IDLE, 8.f);
            }

            s.prev_hp      = p->hp;
            s.prev_stamina = p->stamina;

            bool loop  = (s.anim_idx == ANIM_IDLE);
            int  total = P_ANIM[char_idx][s.anim_idx].frame_count;
            anim_update(s, dt, total, loop);
        }

      for (int i = 0; i < gs->num_enemies; i++) {
            EnemyChar* e = &gs->enemies[i];
            AnimState& s = e_astate[i];
            int char_idx = i % 3;

            // If enemy is active but anim thinks it's dead → new wave respawn, reset anim
            if (e->active && (s.anim_idx == ANIM_DEAD || s.dead_shown)) {
                s = {};  // zero out the whole AnimState
                s.anim_idx  = ANIM_IDLE;
                s.frame_dur = 0.1f;
                s.prev_hp   = e->hp;
                s.prev_stamina = e->stamina;
            }

            // Dead — always wins
            if (!e->active || e->state == STATE_DEAD) {
                if (s.anim_idx != ANIM_DEAD)
                    anim_force(s, ANIM_DEAD, 8.f);
            }
            // Enemy's turn + action submitted → attack anim (one-shot)
            else if (gs->whose_turn == i &&
                     gs->whose_turn_kind == ENTITY_ENEMY &&
                     gs->npc_action.ready &&
                     gs->npc_action.enemy_id == i &&
                     s.anim_idx != ANIM_ATTACK) {
                anim_force(s, ANIM_ATTACK, 10.f);
            }
            // HP dropped → hurt anim (one-shot)
            else if (e->hp < s.prev_hp && s.anim_idx != ANIM_DEAD) {
                anim_force(s, ANIM_HURT, 10.f);
            }
            // Done or idle → back to idle
            else if (s.anim_idx != ANIM_DEAD &&
                     (s.anim_idx == ANIM_IDLE || s.done)) {
                anim_set(s, ANIM_IDLE, 8.f);
            }

            s.prev_hp      = e->hp;
            s.prev_stamina = e->stamina;

            bool loop  = (s.anim_idx == ANIM_IDLE);
            int  total = E_ANIM[char_idx][s.anim_idx].frame_count;
            anim_update(s, dt, total, loop);
        }

        //------------------------------------------------------
        //  EVENT LOOP
        //------------------------------------------------------
        sf::Event ev;
        while (window.pollEvent(ev)) {
            if (ev.type == sf::Event::Closed) { window.close(); break; }

            InputPhase phase   = pis->phase;
            bool my_turn = (gs->whose_turn == MY_PLAYER &&
                            gs->whose_turn_kind == ENTITY_PLAYER);

            //-- Keyboard --
            if (ev.type == sf::Event::KeyPressed) {
                if (ev.key.code == sf::Keyboard::Escape) {
                    if (gs->arbiter_pid > 0) kill(gs->arbiter_pid, SIGTERM);
                    window.close(); break;
                }
                if (!my_turn) continue;

                if (phase == INPUT_CHOOSE_PICKUP) {
                    if (ev.key.code == sf::Keyboard::Y) {
                        pis->chosen_action   = ACTION_PICKUP_YES;
                        pis->pickup_accepted = 1;
                        pis->phase           = INPUT_DONE;
                    } else if (ev.key.code == sf::Keyboard::N) {
                        pis->chosen_action   = ACTION_PICKUP_NO;
                        pis->pickup_accepted = 0;
                        pis->phase           = INPUT_DONE;
                    }
                    continue;
                }
                if (phase == INPUT_CHOOSE_RELIC) {
                    if (ev.key.code == sf::Keyboard::Y) {
                        pis->chosen_action   = ACTION_RELIC_YES;
                        pis->pickup_accepted = 1;
                        pis->phase           = INPUT_DONE;
                    } else if (ev.key.code == sf::Keyboard::N) {
                        pis->chosen_action   = ACTION_RELIC_NO;
                        pis->pickup_accepted = 0;
                        pis->phase           = INPUT_DONE;
                    }
                    continue;
                }

                if (phase == INPUT_CHOOSE_ACTION) {
                    PlayerChar* p = &gs->players[MY_PLAYER];
                    switch (ev.key.code) {
                    case sf::Keyboard::Num1: case sf::Keyboard::Numpad1:
                        selected_action = 0;
                        pis->chosen_action = ACTION_ATTACK_STRIKE;
                        pis->phase = INPUT_CHOOSE_ENEMY; selected_enemy = -1; break;
                    case sf::Keyboard::Num2: case sf::Keyboard::Numpad2:
                        selected_action = 1;
                        pis->chosen_action = ACTION_ATTACK_EXHAUST;
                        pis->phase = INPUT_CHOOSE_ENEMY; selected_enemy = -1; break;
                    case sf::Keyboard::Num3: case sf::Keyboard::Numpad3: {
                        bool has_w = false;
                        for (int s = 0; s < INVENTORY_SLOTS; s++)
                            if (p->inventory[s].weapon != WEAPON_NONE &&
                                p->inventory[s].owner_slot == s) { has_w = true; break; }
                        if (has_w) {
                            selected_action = 2;
                            pis->chosen_action = ACTION_USE_WEAPON;
                            pis->phase = INPUT_CHOOSE_SLOT; selected_slot = -1;
                        }
                        break;
                    }
                    case sf::Keyboard::Num4: case sf::Keyboard::Numpad4: {
                        bool has_lt = false;
                        for (int s = 0; s < p->long_term_count; s++)
                            if (p->long_term[s].valid) { has_lt = true; break; }
                        if (has_lt) {
                            selected_action = 3;
                            pis->chosen_action = ACTION_SWAP_IN;
                            pis->phase = INPUT_CHOOSE_SWAPIN;
                        }
                        break;
                    }
                    case sf::Keyboard::Num5: case sf::Keyboard::Numpad5:
                        selected_action = 4; pis->chosen_action = ACTION_HEAL;
                        pis->phase = INPUT_DONE; break;
                    case sf::Keyboard::Space:
                    case sf::Keyboard::Num6: case sf::Keyboard::Numpad6:
                        selected_action = 5; pis->chosen_action = ACTION_SKIP;
                        pis->phase = INPUT_DONE; break;
                    default: break;
                    }
                }

                if ((phase == INPUT_CHOOSE_ENEMY || phase == INPUT_CHOOSE_SLOT ||
                     phase == INPUT_CHOOSE_SWAPIN) &&
                    ev.key.code == sf::Keyboard::BackSpace) {
                    selected_action = -1; selected_enemy = -1; selected_slot = -1;
                    pis->phase = INPUT_CHOOSE_ACTION;
                }
            }

            //-- Mouse click --
            if (ev.type == sf::Event::MouseButtonPressed &&
                ev.mouseButton.button == sf::Mouse::Left) {

                InputPhase phase2 = pis->phase;
                bool my_turn2 = (gs->whose_turn == MY_PLAYER &&
                                 gs->whose_turn_kind == ENTITY_PLAYER);
                if (!my_turn2) continue;

                if (phase2 == INPUT_CHOOSE_ACTION) {
                    PlayerChar* p = &gs->players[MY_PLAYER];
                    for (int b = 0; b < 6; b++) {
                        float bx = 20.f + b * (BTN_W + 8.f);
                        if (in_rect(mx, my, bx, BTN_Y, BTN_W, BTN_H)) {
                            switch (b) {
                            case 0: selected_action=0; pis->chosen_action=ACTION_ATTACK_STRIKE;
                                pis->phase=INPUT_CHOOSE_ENEMY; selected_enemy=-1; break;
                            case 1: selected_action=1; pis->chosen_action=ACTION_ATTACK_EXHAUST;
                                pis->phase=INPUT_CHOOSE_ENEMY; selected_enemy=-1; break;
                            case 2: {
                                bool has_w=false;
                                for(int s=0;s<INVENTORY_SLOTS;s++)
                                    if(p->inventory[s].weapon!=WEAPON_NONE&&
                                       p->inventory[s].owner_slot==s){has_w=true;break;}
                                if(has_w){selected_action=2;pis->chosen_action=ACTION_USE_WEAPON;
                                    pis->phase=INPUT_CHOOSE_SLOT;selected_slot=-1;}
                                break;
                            }
                            case 3: {
                                bool has_lt=false;
                                for(int s=0;s<p->long_term_count;s++)
                                    if(p->long_term[s].valid){has_lt=true;break;}
                                if(has_lt){selected_action=3;pis->chosen_action=ACTION_SWAP_IN;
                                    pis->phase=INPUT_CHOOSE_SWAPIN;}
                                break;
                            }
                            case 4: selected_action=4; pis->chosen_action=ACTION_HEAL;
                                pis->phase=INPUT_DONE; break;
                            case 5: selected_action=5; pis->chosen_action=ACTION_SKIP;
                                pis->phase=INPUT_DONE; break;
                            }
                            break;
                        }
                    }
                }

                // Click enemy sprite to target
                if (phase2 == INPUT_CHOOSE_ENEMY) {
                    // Enemy sprites: right side, 3 per row
                    for (int i = 0; i < gs->num_enemies; i++) {
                        if (!gs->enemies[i].active) continue;
                        float ex = 680.f + (i % 3) * 200.f;
                        float ey = 150.f + (i / 3) * 220.f;
                        if (in_rect(mx, my, ex, ey, 180.f, 180.f)) {
                            selected_enemy = i;
                            pis->chosen_enemy = i;
                            pis->phase = INPUT_DONE;
                            break;
                        }
                    }
                }

                if (phase2 == INPUT_CHOOSE_SLOT) {
                    PlayerChar* p = &gs->players[MY_PLAYER];
                    int drawn = 0;
                    for (int i = 0; i < INVENTORY_SLOTS; i++) {
                        if (p->inventory[i].weapon == WEAPON_NONE ||
                            p->inventory[i].owner_slot != i) continue;
                        float sx = 20.f + drawn * 135.f;
                        if (in_rect(mx, my, sx, 668.f, 125.f, 40.f)) {
                            selected_slot = i; pis->chosen_slot = i;
                            pis->phase = INPUT_CHOOSE_ENEMY; selected_enemy = -1; break;
                        }
                        drawn++;
                    }
                }

                if (phase2 == INPUT_CHOOSE_SWAPIN) {
                    PlayerChar* p = &gs->players[MY_PLAYER];
                    int lt_drawn = 0;
                    for (int i = 0; i < p->long_term_count; i++) {
                        if (!p->long_term[i].valid) continue;
                        float sx = 20.f + lt_drawn * 135.f;
                        if (in_rect(mx, my, sx, 668.f, 125.f, 40.f)) {
                            pis->chosen_lt_index = i;
                            pis->phase = INPUT_DONE; break;
                        }
                        lt_drawn++;
                    }
                }

                if (phase2 == INPUT_CHOOSE_PICKUP || phase2 == INPUT_CHOOSE_RELIC) {
                    if (in_rect(mx, my, 490.f, 340.f, 120.f, 36.f)) {
                        pis->chosen_action   = (phase2==INPUT_CHOOSE_PICKUP)
                                               ? ACTION_PICKUP_YES : ACTION_RELIC_YES;
                        pis->pickup_accepted = 1; pis->phase = INPUT_DONE;
                    }
                    if (in_rect(mx, my, 640.f, 340.f, 120.f, 36.f)) {
                        pis->chosen_action   = (phase2==INPUT_CHOOSE_PICKUP)
                                               ? ACTION_PICKUP_NO : ACTION_RELIC_NO;
                        pis->pickup_accepted = 0; pis->phase = INPUT_DONE;
                    }
                }
            }

            //-- Mouse move (hover) --
            if (ev.type == sf::Event::MouseMoved) {
                hovered_enemy = -1; hovered_slot = -1;
                for (int i = 0; i < gs->num_enemies; i++) {
                    if (!gs->enemies[i].active) continue;
                    float ex = 680.f + (i % 3) * 200.f;
                    float ey = 150.f + (i / 3) * 220.f;
                    if (in_rect(mx, my, ex, ey, 180.f, 180.f))
                        hovered_enemy = i;
                }
                PlayerChar* p = &gs->players[MY_PLAYER];
                int drawn = 0;
                for (int i = 0; i < INVENTORY_SLOTS; i++) {
                    if (p->inventory[i].weapon == WEAPON_NONE ||
                        p->inventory[i].owner_slot != i) continue;
                    float sx = 20.f + drawn * 135.f;
                    if (in_rect(mx, my, sx, 668.f, 125.f, 40.f))
                        hovered_slot = i;
                    drawn++;
                }
            }
        } // end event loop

        if (pis->phase == INPUT_IDLE) {
            selected_action = -1; selected_enemy = -1; selected_slot = -1;
        }

        bool my_turn_now = (gs->whose_turn == MY_PLAYER &&
                            gs->whose_turn_kind == ENTITY_PLAYER);

        //------------------------------------------------------
        //  DRAW
        //------------------------------------------------------
        window.clear(sf::Color(10, 10, 20));

        // Background
        if (has_bg)
            window.draw(bg_sprite);
        else {
            sf::RectangleShape fallback({1280.f, 720.f});
            fallback.setFillColor(sf::Color(15, 20, 35));
            window.draw(fallback);
        }

        // Semi-transparent overlay so UI is readable over bg
        sf::RectangleShape overlay({1280.f, 720.f});
        overlay.setFillColor(sf::Color(0, 0, 0, 100));
        window.draw(overlay);

        if (!has_font) { window.display(); continue; }

        //-- Title bar --
        sf::RectangleShape title_bar({1280.f, 28.f});
        title_bar.setFillColor(sf::Color(0,0,0,160));
        title_bar.setPosition(0, 0);
        window.draw(title_bar);
        draw_text(window, font,
                  "CHRONO RIFT  |  Player " + std::to_string(MY_PLAYER) +
                  "  |  Turn " + std::to_string(gs->turn_number) +
                  "  |  Kills: " + std::to_string(gs->enemies_killed) + "/10",
                  10, 5, 14, sf::Color(220, 180, 60));

        //==============================================================
        //  PLAYER SPRITES  — left side, row of up to 4
        //  Each sprite area: 160px wide, centered in slot
        //  Positioned at y=120, sprite scaled to ~180px tall
        //==============================================================
        const float P_AREA_W  = 160.f;
        const float P_SPRITE_H = 160.f;  // display height
        const float P_START_X  = 20.f;
        const float P_Y        = 90.f;   // top of sprite area
        const float P_BAR_Y    = P_Y + P_SPRITE_H + 4.f;

        for (int i = 0; i < gs->num_players; i++) {
            PlayerChar* p = &gs->players[i];
            AnimState&  s = p_astate[i];
            int char_idx  = i % 3;
            float slot_x  = P_START_X + i * (P_AREA_W + 10.f);

            // Skip if dead animation fully done
            if (!p->active && s.dead_shown) continue;

            int anim = s.anim_idx;
            int frame = s.cur_frame;

            // Draw sprite
            if (p_loaded[char_idx][anim]) {
                sf::Sprite& spr = p_spr[char_idx][anim];
                int fw = P_ANIM[char_idx][anim].frame_w;
                int fh = P_ANIM[char_idx][anim].frame_h;
                spr.setTextureRect(sf::IntRect(frame * fw, 0, fw, fh));

                float scale = P_SPRITE_H / (float)fh;
                spr.setScale(scale, scale);
                float sprite_w = fw * scale;
                float center_x = slot_x + (P_AREA_W - sprite_w) / 2.f;
                spr.setPosition(center_x, P_Y);

                // Tint if stunned
                if (p->state == STATE_STUNNED)
                    spr.setColor(sf::Color(255, 150, 150, 220));
                else if (!p->active)
                    spr.setColor(sf::Color(150, 150, 150, 180));
                else if (i == MY_PLAYER)
                    spr.setColor(sf::Color(200, 220, 255, 255));
                else
                    spr.setColor(sf::Color::White);

                window.draw(spr);
            } else {
                // Fallback colored rectangle
                sf::Color fc = p->active ? sf::Color(60,120,200) : sf::Color(60,60,60);
                sf::RectangleShape box({P_AREA_W - 10.f, P_SPRITE_H});
                box.setPosition(slot_x + 5.f, P_Y);
                box.setFillColor(fc);
                window.draw(box);
            }

            // "YOU" tag
            if (i == MY_PLAYER) {
                sf::RectangleShape tag({P_AREA_W, 16.f});
                tag.setPosition(slot_x, P_Y - 18.f);
                tag.setFillColor(sf::Color(0,100,200,180));
                window.draw(tag);
                draw_text(window, font, "YOU", slot_x + 60.f, P_Y - 17.f,
                          10, sf::Color::White);
            }

            // Turn indicator
            bool is_turn = (gs->whose_turn_kind == ENTITY_PLAYER && gs->whose_turn == i);
            if (is_turn && p->active) {
                sf::RectangleShape ind({P_AREA_W, 4.f});
                ind.setPosition(slot_x, P_Y + P_SPRITE_H);
                ind.setFillColor(sf::Color::Yellow);
                window.draw(ind);
            }

            // HP bar
            float hp_frac = p->max_hp > 0 ? (float)p->hp / p->max_hp : 0.f;
            draw_bar(window, slot_x, P_BAR_Y, P_AREA_W, 8.f, hp_frac,
                     sf::Color(60,200,60), sf::Color(20,50,20,180));
            draw_text(window, font,
                      "HP " + std::to_string(p->hp) + "/" + std::to_string(p->max_hp),
                      slot_x, P_BAR_Y + 9.f, 9, sf::Color(180,255,180));

            // Stamina bar
            float sta_frac = p->max_stamina > 0 ? (float)p->stamina / p->max_stamina : 0.f;
            draw_bar(window, slot_x, P_BAR_Y + 20.f, P_AREA_W, 8.f, sta_frac,
                     sf::Color(60,120,220), sf::Color(20,30,80,180));
            draw_text(window, font,
                      "STA " + std::to_string(p->stamina) + "/" + std::to_string(p->max_stamina),
                      slot_x, P_BAR_Y + 29.f, 9, sf::Color(160,200,255));

            // Status label
            float lbl_y = P_BAR_Y + 40.f;
            if (!p->active)
                draw_text(window, font, "DEAD", slot_x, lbl_y, 10, sf::Color(200,50,50));
            else if (p->state == STATE_STUNNED)
                draw_text(window, font, "STUNNED", slot_x, lbl_y, 10, sf::Color(255,100,100));
            if (p->ultimate_eligible)
                draw_text(window, font, "ULTIMATE!", slot_x, lbl_y + 12.f, 10,
                          sf::Color(255,215,0));

            // Player label
            char lbl[32];
            snprintf(lbl, 32, "P%d", i);
            draw_text(window, font, lbl, slot_x + 2.f, P_Y + 2.f, 10, sf::Color(200,200,255));
        }

        //==============================================================
        //  ENEMY SPRITES  — right side
        //  3 enemies per row, each area 180px wide
        //==============================================================
        const float E_AREA_W   = 180.f;
        const float E_SPRITE_H = 160.f;
        const float E_START_X  = 670.f;
        const float E_ROW_H    = 220.f;
        const float E_Y_START  = 80.f;

        InputPhase cur_phase = pis->phase;
        bool clickable_enemies = (cur_phase == INPUT_CHOOSE_ENEMY && my_turn_now);

        for (int i = 0; i < gs->num_enemies; i++) {
            EnemyChar* e = &gs->enemies[i];
            AnimState& s = e_astate[i];
            int char_idx = i % 3;

            float slot_x = E_START_X + (i % 3) * (E_AREA_W + 10.f);
            float slot_y = E_Y_START + (i / 3) * E_ROW_H;

            // Skip if dead anim done
            if (!e->active && s.dead_shown) continue;

            int anim  = s.anim_idx;
            int frame = s.cur_frame;

            bool hov = (hovered_enemy == i && clickable_enemies);
            bool sel = (selected_enemy == i);

            // Hover/select glow
            if (hov || sel) {
                sf::RectangleShape glow({E_AREA_W + 6.f, E_SPRITE_H + 6.f});
                glow.setPosition(slot_x - 3.f, slot_y - 3.f);
                glow.setFillColor(sel ? sf::Color(255,165,0,80)
                                      : sf::Color(255,255,100,50));
                glow.setOutlineThickness(2.f);
                glow.setOutlineColor(sel ? sf::Color(255,165,0)
                                         : sf::Color(255,255,100));
                window.draw(glow);
            }

            // Draw enemy sprite (flipped horizontally to face left/player)
            if (e_loaded[char_idx][anim]) {
                sf::Sprite& spr = e_spr[char_idx][anim];
                int fw = E_ANIM[char_idx][anim].frame_w;
                int fh = E_ANIM[char_idx][anim].frame_h;
                spr.setTextureRect(sf::IntRect(frame * fw, 0, fw, fh));

                float scale = E_SPRITE_H / (float)fh;
                float sprite_w = fw * scale;
                float center_x = slot_x + (E_AREA_W - sprite_w) / 2.f;

                // Flip horizontally (enemies face left)
                spr.setScale(-scale, scale);
                spr.setPosition(center_x + sprite_w, slot_y);

                if (e->state == STATE_STUNNED)
                    spr.setColor(sf::Color(255,150,150,220));
                else if (!e->active)
                    spr.setColor(sf::Color(150,150,150,180));
                else
                    spr.setColor(sf::Color::White);

                window.draw(spr);
            } else {
                sf::Color fc = e->active ? sf::Color(200,60,60) : sf::Color(60,60,60);
                sf::RectangleShape box({E_AREA_W - 10.f, E_SPRITE_H});
                box.setPosition(slot_x + 5.f, slot_y);
                box.setFillColor(fc);
                window.draw(box);
            }

            // Turn indicator
            bool is_turn = (gs->whose_turn_kind == ENTITY_ENEMY && gs->whose_turn == i);
            if (is_turn && e->active) {
                sf::RectangleShape ind({E_AREA_W, 4.f});
                ind.setPosition(slot_x, slot_y + E_SPRITE_H);
                ind.setFillColor(sf::Color(255,80,80));
                window.draw(ind);
            }

            float bar_y = slot_y + E_SPRITE_H + 4.f;

            // HP bar
            float hp_frac = e->max_hp > 0 ? (float)e->hp / e->max_hp : 0.f;
            draw_bar(window, slot_x, bar_y, E_AREA_W, 8.f, hp_frac,
                     sf::Color(200,60,60), sf::Color(60,10,10,180));
            draw_text(window, font,
                      "HP " + std::to_string(e->hp) + "/" + std::to_string(e->max_hp),
                      slot_x, bar_y + 9.f, 9, sf::Color(255,180,180));

            // Stamina bar
            float sta_frac = e->max_stamina > 0 ? (float)e->stamina / e->max_stamina : 0.f;
            draw_bar(window, slot_x, bar_y + 20.f, E_AREA_W, 8.f, sta_frac,
                     sf::Color(200,120,40), sf::Color(60,35,10,180));
            draw_text(window, font,
                      "STA " + std::to_string(e->stamina) + "/" + std::to_string(e->max_stamina),
                      slot_x, bar_y + 29.f, 9, sf::Color(255,200,150));

            // Status / weapon label
            float lbl_y = bar_y + 40.f;
            if (!e->active)
                draw_text(window, font, "DEAD", slot_x, lbl_y, 10, sf::Color(180,50,50));
            else if (e->state == STATE_STUNNED)
                draw_text(window, font, "STUNNED", slot_x, lbl_y, 10, sf::Color(255,100,100));
            if (e->held_weapon != WEAPON_NONE)
                draw_text(window, font,
                          std::string("W:") + WEAPON_TABLE[e->held_weapon].name,
                          slot_x, lbl_y + 12.f, 9, sf::Color(255,200,60));

            // Clickable hint bar
            if (clickable_enemies && e->active) {
                sf::RectangleShape hint({E_AREA_W, 3.f});
                hint.setPosition(slot_x, slot_y + E_SPRITE_H - 3.f);
                hint.setFillColor(sf::Color(255,165,0,200));
                window.draw(hint);
            }

            // Enemy label
            char lbl[32]; snprintf(lbl, 32, "E%d", i);
            draw_text(window, font, lbl, slot_x + 2.f, slot_y + 2.f, 10,
                      sf::Color(255,180,180));
        }

        //==============================================================
        //  BOTTOM UI PANEL
        //==============================================================
        sf::RectangleShape panel({1280.f, 110.f});
        panel.setPosition(0, 610.f);
        panel.setFillColor(sf::Color(0,0,0,200));
        window.draw(panel);

        // Artifact bar
        ArtifactTable* at = &gs->artifacts;
        char abuf[256];
        snprintf(abuf, sizeof(abuf),
                 "Solar Core: %s   Lunar Blade: %s   Eclipse Relic: %s",
                 at->solar_core_holder  < 0 ? "Free" : "Held",
                 at->lunar_blade_holder < 0 ? "Free" : "Held",
                 at->eclipse_relic_exists
                   ? (at->eclipse_relic_holder < 0 ? "Free" : "Held")
                   : "Not in play");
        draw_text(window, font, abuf, 10, 612.f, 10, sf::Color(160,210,255));

        // Ultimate banner
        if (gs->ultimate_active) {
            sf::RectangleShape banner({1280.f, 18.f});
            banner.setPosition(0, 626.f);
            banner.setFillColor(sf::Color(100,50,0,220));
            window.draw(banner);
            draw_text(window, font, "*** ULTIMATE ACTIVE — ENEMIES SUSPENDED ***",
                      20, 628.f, 11, sf::Color(255,215,0));
        }

        // Action buttons
        const char* btn_labels[] = {
            "1 Strike","2 Exhaust","3 Weapon","4 Swap In","5 Heal","6 Skip"
        };
        bool is_player_turn = my_turn_now &&
                              (cur_phase != INPUT_IDLE && cur_phase != INPUT_DONE);
        if (is_player_turn) {
            for (int b = 0; b < 6; b++) {
                float bx = 10.f + b * (BTN_W + 6.f);
                bool hov_btn = in_rect(mx, my, bx, BTN_Y, BTN_W, BTN_H);
                bool sel_btn = (selected_action == b);
                sf::Color btn_bg  = sel_btn ? sf::Color(100,80,10,220) :
                                    hov_btn ? sf::Color(60,60,100,220) :
                                              sf::Color(30,30,50,220);
                sf::Color btn_bdr = sel_btn ? sf::Color::Yellow :
                                    hov_btn ? sf::Color(120,120,200) :
                                              sf::Color(80,80,100);
                sf::RectangleShape btn({BTN_W, BTN_H});
                btn.setPosition(bx, BTN_Y);
                btn.setFillColor(btn_bg);
                btn.setOutlineThickness(1.f);
                btn.setOutlineColor(btn_bdr);
                window.draw(btn);
                draw_text(window, font, btn_labels[b], bx+6, BTN_Y+9, 11,
                          sf::Color::White);
            }
        } else if (!my_turn_now && gs->game_started) {
            std::string status;
            if (gs->whose_turn_kind == ENTITY_PLAYER) {
                if (gs->whose_turn == MY_PLAYER) status = "Your turn!";
                else status = "Player " + std::to_string(gs->whose_turn) + "'s turn...";
            } else {
                status = "Enemy " + std::to_string(gs->whose_turn) + " is acting...";
            }
            draw_text(window, font, status, 10, BTN_Y + 9, 13,
                      sf::Color(180,180,180));
        }

        // Prompt line
        float prompt_y = BTN_Y + BTN_H + 4.f;
        if (my_turn_now) {
            if (cur_phase == INPUT_CHOOSE_ENEMY)
                draw_text(window, font,
                          "Click an enemy to target  |  Backspace = cancel",
                          10, prompt_y, 11, sf::Color(255,220,100));
            else if (cur_phase == INPUT_CHOOSE_SLOT)
                draw_text(window, font,
                          "Click a weapon in inventory  |  Backspace = cancel",
                          10, prompt_y, 11, sf::Color(255,220,100));
            else if (cur_phase == INPUT_CHOOSE_SWAPIN)
                draw_text(window, font,
                          "Click a long-term weapon to swap in  |  Backspace = cancel",
                          10, prompt_y, 11, sf::Color(255,220,100));
        }

        //==============================================================
        //  INVENTORY BAR  (bottom strip)
        //==============================================================
        {
            PlayerChar* p = &gs->players[MY_PLAYER];
            int drawn = 0;
            bool inv_clickable = (cur_phase == INPUT_CHOOSE_SLOT && my_turn_now);

            if (cur_phase == INPUT_CHOOSE_SWAPIN && my_turn_now) {
                // Show long-term storage
                draw_text(window, font, "LONG-TERM:", 10, 665.f, 9,
                          sf::Color(255,200,60));
                int lt_drawn = 0;
                for (int i = 0; i < p->long_term_count; i++) {
                    if (!p->long_term[i].valid) continue;
                    float sx = 10.f + lt_drawn * 130.f;
                    bool hov_lt = in_rect(mx, my, sx, 668.f, 125.f, 38.f);
                    sf::RectangleShape slot({125.f, 38.f});
                    slot.setPosition(sx, 668.f);
                    slot.setFillColor(hov_lt ? sf::Color(70,60,20,220)
                                             : sf::Color(40,35,15,220));
                    slot.setOutlineThickness(1.f);
                    slot.setOutlineColor(sf::Color(255,200,60));
                    window.draw(slot);
                    WeaponKind wk = p->long_term[i].weapon;
                    draw_text(window, font,
                              std::string("[") + std::to_string(i) + "] " +
                              WEAPON_TABLE[wk].name, sx+3, 671.f, 9, sf::Color::White);
                    draw_text(window, font,
                              "DMG:" + std::to_string(WEAPON_TABLE[wk].damage),
                              sx+3, 683.f, 8, sf::Color(220,180,100));
                    lt_drawn++;
                }
            } else {
                // Show primary inventory
                draw_text(window, font, "INV:", 10, 665.f, 9, sf::Color(130,130,130));
                for (int i = 0; i < INVENTORY_SLOTS; i++) {
                    if (p->inventory[i].weapon == WEAPON_NONE ||
                        p->inventory[i].owner_slot != i) continue;
                    float sx = 10.f + drawn * 130.f;
                    bool hov = (hovered_slot == i) && inv_clickable;
                    bool sel = (selected_slot == i);
                    sf::RectangleShape slot({125.f, 38.f});
                    slot.setPosition(sx, 668.f);
                    slot.setFillColor(sel ? sf::Color(80,80,30,220) :
                                     hov ? sf::Color(60,60,90,220) :
                                           sf::Color(25,25,45,220));
                    slot.setOutlineThickness(1.f);
                    slot.setOutlineColor(inv_clickable ? sf::Color(200,160,60)
                                                       : sf::Color(50,50,70));
                    window.draw(slot);
                    WeaponKind wk = p->inventory[i].weapon;
                    draw_text(window, font,
                              std::string("[") + std::to_string(i) + "] " +
                              WEAPON_TABLE[wk].name,
                              sx+3, 671.f, 9, sf::Color::White);
                    draw_text(window, font,
                              "DMG:" + std::to_string(WEAPON_TABLE[wk].damage) +
                              " SZ:" + std::to_string(WEAPON_TABLE[wk].slot_size),
                              sx+3, 683.f, 8, sf::Color(180,180,180));
                    drawn++;
                }
                // Eclipse relic slot
                if (p->has_eclipse_relic) {
                    float sx = 10.f + drawn * 130.f;
                    sf::RectangleShape rslot({125.f, 38.f});
                    rslot.setPosition(sx, 668.f);
                    rslot.setFillColor(sf::Color(40,20,60,220));
                    rslot.setOutlineThickness(1.f);
                    rslot.setOutlineColor(sf::Color(180,100,255));
                    window.draw(rslot);
                    draw_text(window, font, "Eclipse Relic", sx+3, 671.f, 9,
                              sf::Color(180,100,255));
                    draw_text(window, font, "DMG halved", sx+3, 683.f, 8,
                              sf::Color(140,80,200));
                }
            }
        }

        //==============================================================
        //  ACTION LOG  — right side of bottom area
        //==============================================================
        {
            sf::RectangleShape logbg({390.f, 200.f});
            logbg.setPosition(880.f, 410.f);
            logbg.setFillColor(sf::Color(0,0,0,170));
            window.draw(logbg);
            draw_text(window, font, "LOG", 888.f, 413.f, 10, sf::Color(130,130,130));

            pthread_mutex_lock(&gs->action_log.mutex);
            int n    = gs->action_log.count;
            int head = gs->action_log.head;
            int show = (n < 12) ? n : 12;
            for (int row = 0; row < show; row++) {
                int idx = (head - 1 - row + LOG_BUFFER_SIZE) % LOG_BUFFER_SIZE;
                int brightness = 200 - row * 12;
                if (brightness < 80) brightness = 80;
                sf::Text line;
                line.setFont(font); line.setCharacterSize(11);
                line.setFillColor(sf::Color(brightness, brightness, brightness));
                line.setString(gs->action_log.entries[idx]);
                line.setPosition(888.f, 426.f + row * 14.f);
                window.draw(line);
            }
            pthread_mutex_unlock(&gs->action_log.mutex);
        }

        //==============================================================
        //  PICKUP / RELIC OVERLAY
        //==============================================================
        if (my_turn_now &&
            (cur_phase == INPUT_CHOOSE_PICKUP || cur_phase == INPUT_CHOOSE_RELIC)) {
            sf::RectangleShape dim({1280.f, 720.f});
            dim.setFillColor(sf::Color(0,0,0,160));
            window.draw(dim);

            sf::RectangleShape box({500.f, 140.f});
            box.setPosition(390.f, 280.f);
            box.setFillColor(sf::Color(20,20,40,240));
            box.setOutlineThickness(2.f);
            box.setOutlineColor(sf::Color(200,160,60));
            window.draw(box);

            if (cur_phase == INPUT_CHOOSE_PICKUP) {
                WeaponKind wk = gs->pending_drop.weapon;
                char hdr[128];
                snprintf(hdr, sizeof(hdr), "Enemy %d dropped: %s (DMG:%d, Slots:%d)",
                         gs->pending_drop.from_enemy_id,
                         WEAPON_TABLE[wk].name,
                         WEAPON_TABLE[wk].damage,
                         WEAPON_TABLE[wk].slot_size);
                draw_text(window, font, hdr, 400, 292, 13, sf::Color::White);
                draw_text(window, font, "Pick it up?", 570, 312, 13,
                          sf::Color(200,200,200));
            } else {
                draw_text(window, font, "*** Eclipse Relic has appeared! ***",
                          420, 292, 14, sf::Color(180,100,255));
                draw_text(window, font, "Effect: Incoming damage is HALVED.",
                          430, 312, 12, sf::Color(200,200,200));
                draw_text(window, font, "Pick it up?", 570, 330, 13,
                          sf::Color(200,200,200));
            }

            sf::RectangleShape yes_btn({120.f, 36.f});
            yes_btn.setPosition(490.f, 340.f);
            bool yes_hov = in_rect(mx, my, 490.f, 340.f, 120.f, 36.f);
            yes_btn.setFillColor(yes_hov ? sf::Color(40,140,40) : sf::Color(30,80,30));
            yes_btn.setOutlineThickness(1.f);
            yes_btn.setOutlineColor(sf::Color(60,200,60));
            window.draw(yes_btn);
            draw_text(window, font, "Yes [Y]", 506, 349, 13, sf::Color::White);

            sf::RectangleShape no_btn({120.f, 36.f});
            no_btn.setPosition(640.f, 340.f);
            bool no_hov = in_rect(mx, my, 640.f, 340.f, 120.f, 36.f);
            no_btn.setFillColor(no_hov ? sf::Color(140,40,40) : sf::Color(80,30,30));
            no_btn.setOutlineThickness(1.f);
            no_btn.setOutlineColor(sf::Color(200,60,60));
            window.draw(no_btn);
            draw_text(window, font, "No [N]", 656, 349, 13, sf::Color::White);
        }

        //==============================================================
        //  GAME OVER OVERLAY
        //==============================================================
        if (gs->game_over) {
            sf::RectangleShape ov({1280.f, 720.f});
            ov.setFillColor(sf::Color(0,0,0,190));
            window.draw(ov);
            std::string res_str = (gs->result == GAME_WIN)  ? "VICTORY!" :
                                  (gs->result == GAME_LOSE) ? "GAME OVER" : "QUIT";
            draw_text(window, font, res_str, 400, 280, 72,
                      gs->result == GAME_WIN ? sf::Color::Yellow : sf::Color::Red);
            draw_text(window, font,
                      "Kills: " + std::to_string(gs->enemies_killed) + "/10",
                      520, 370, 24, sf::Color::White);
        }

        //==============================================================
        //  STUN NOTIFICATION
        //==============================================================
        if (pis->stunned_notify) {
            static int notify_frames = 0;
            notify_frames++;
            sf::RectangleShape stun_box({400.f, 40.f});
            stun_box.setPosition(440.f, 220.f);
            stun_box.setFillColor(sf::Color(120,0,0,200));
            stun_box.setOutlineThickness(2.f);
            stun_box.setOutlineColor(sf::Color(255,80,80));
            window.draw(stun_box);
            draw_text(window, font, "*** YOU ARE STUNNED! ***",
                      460, 230, 20, sf::Color(255,80,80));
            if (notify_frames > 60) {
                pis->stunned_notify = 0;
                notify_frames = 0;
            }
        }

        window.display();

        if (gs->game_over) {
            sf::sleep(sf::seconds(3));
            window.close();
        }
    } // end main loop

    return NULL;
}

//=============================================================
//  PLAYER THREAD
//=============================================================
static void* player_thread(void*) {
    printf("[HIP-%d] Player thread started\n", MY_PLAYER);
    PlayerInputState* pis = &gs->input_state[MY_PLAYER];

    while (!gs->game_over) {
        sem_wait(&gs->player_turn_sem[MY_PLAYER]);
        if (gs->game_over) break;
        if (!gs->players[MY_PLAYER].active) continue;

        if (gs->whose_turn != MY_PLAYER ||
            gs->whose_turn_kind != ENTITY_PLAYER) {
            printf("[HIP-%d] Spurious wakeup, skipping\n", MY_PLAYER);
            continue;
        }

        printf("[HIP-%d] My turn — waiting for SFML input\n", MY_PLAYER);

        pthread_mutex_lock(&gs->state_mutex);
        pis->active_player   = MY_PLAYER;
        pis->chosen_action   = ACTION_NONE;
        pis->chosen_enemy    = -1;
        pis->chosen_slot     = -1;
        pis->chosen_lt_index = -1;
        pis->pickup_accepted = 0;

        if (gs->pending_relic.active)
            pis->phase = INPUT_CHOOSE_RELIC;
        else if (gs->pending_drop.active)
            pis->phase = INPUT_CHOOSE_PICKUP;
        else
            pis->phase = INPUT_CHOOSE_ACTION;
        pthread_mutex_unlock(&gs->state_mutex);

        while (pis->phase != INPUT_DONE && !gs->game_over)
            usleep(20000);

        if (gs->game_over) break;

        ActionRequest req;
        memset(&req, 0, sizeof(ActionRequest));
        req.player_id       = MY_PLAYER;
        req.action          = pis->chosen_action;
        req.target_enemy_id = pis->chosen_enemy;
        req.weapon_slot     = pis->chosen_slot;

        if (req.action == ACTION_SWAP_IN) {
            int lt_idx = pis->chosen_lt_index;
            PlayerChar* p = &gs->players[MY_PLAYER];
            if (lt_idx >= 0 && lt_idx < p->long_term_count &&
                p->long_term[lt_idx].valid) {
                req.swap_in_weapon = p->long_term[lt_idx].weapon;
            } else {
                req.action = ACTION_SKIP;
            }
        }

        pthread_mutex_lock(&gs->state_mutex);
        gs->player_action       = req;
        gs->player_action.ready = 1;
        pthread_mutex_unlock(&gs->state_mutex);

        sem_post(&gs->action_ready_sem);
        printf("[HIP-%d] Submitted action %d\n", MY_PLAYER, req.action);
    }

    printf("[HIP-%d] Player thread exiting\n", MY_PLAYER);
    return NULL;
}

//=============================================================
//  MAIN
//=============================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ./hip <player_id>\n");
        return 1;
    }
    MY_PLAYER = atoi(argv[1]);
    if (MY_PLAYER < 0 || MY_PLAYER >= MAX_PLAYERS) {
        fprintf(stderr, "[HIP] Invalid player_id %d\n", MY_PLAYER);
        return 1;
    }

    signal(SIGUSR1, handle_stun);
    signal(SIGTERM, handle_sigterm);

    printf("[HIP-%d] Attaching to shared memory...\n", MY_PLAYER);
    int retries = 0;
    while ((gs = shm_attach()) == NULL) {
        usleep(200000);
        if (++retries > 25) {
            fprintf(stderr, "[HIP-%d] Could not attach\n", MY_PLAYER);
            return 1;
        }
    }

    while (gs->shm_ready == 0)
        usleep(50000);

    pthread_mutex_lock(&gs->state_mutex);
    gs->hip_pid[MY_PLAYER] = getpid();
    gs->hip_count++;
    pthread_mutex_unlock(&gs->state_mutex);

    printf("[HIP-%d] PID = %d registered (hip_count=%d)\n",
           MY_PLAYER, (int)getpid(), gs->hip_count);

    while (gs->game_started == 0 && !gs->game_over)
        usleep(100000);

    PlayerInputState* pis = &gs->input_state[MY_PLAYER];
    pis->phase          = INPUT_IDLE;
    pis->active_player  = MY_PLAYER;
    pis->chosen_action  = ACTION_NONE;
    pis->chosen_enemy   = -1;
    pis->chosen_slot    = -1;
    pis->stunned_notify = 0;

    pthread_t render;
    pthread_create(&render, NULL, render_thread, NULL);

    pthread_t pt;
    pthread_create(&pt, NULL, player_thread, NULL);

    pthread_join(pt,     NULL);
    pthread_join(render, NULL);

    printf("[HIP-%d] Exiting\n", MY_PLAYER);
    munmap(gs, sizeof(GameState));
    return 0;
}