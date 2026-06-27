//=============================================================
//  arbiter.cpp  —  Chrono Rift  |  CS 2006
//=============================================================

#include "../common.h"
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <stdarg.h>

static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static GameState* gs           = NULL;
static volatile sig_atomic_t npc_timed_out  = 0;
static volatile sig_atomic_t ultimate_ended = 0;
static volatile int          tick_paused    = 0;

//=============================================================
//  SIGNAL HANDLERS
//=============================================================
static void handle_sigterm(int) {
    printf("\n[Arbiter] SIGTERM — player quit\n");
    if (gs) {
        gs->result    = GAME_QUIT;
        gs->game_over = 1;
        for (int i = 0; i < MAX_PLAYERS; i++)
            sem_post(&gs->player_turn_sem[i]);
        for (int i = 0; i < MAX_ENEMIES; i++)
            sem_post(&gs->npc_turn_sem[i]);
    }
}

static void handle_sigalrm(int) {
    npc_timed_out  = 1;
    ultimate_ended = 1;
}

static void handle_sigint(int) {
    printf("\n[Arbiter] SIGINT — shutting down\n");
    if (gs) {
        gs->game_over = 1;
        for (int i = 0; i < MAX_PLAYERS; i++)
            sem_post(&gs->player_turn_sem[i]);
        for (int i = 0; i < MAX_ENEMIES; i++)
            sem_post(&gs->npc_turn_sem[i]);
        shm_destroy(gs);
    }
    exit(0);
}

//=============================================================
//  INIT
//=============================================================
static void init_sync() {
    pthread_mutexattr_t mattr;
    pthread_mutexattr_init(&mattr);
    pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&gs->state_mutex,           &mattr);
    pthread_mutex_init(&gs->artifact_mutex,        &mattr);
    pthread_mutex_init(&gs->artifacts.table_mutex, &mattr);
    pthread_mutex_init(&gs->action_log.mutex,      &mattr);
    pthread_mutexattr_destroy(&mattr);

    gs->action_log.head  = 0;
    gs->action_log.count = 0;
    for (int i = 0; i < LOG_BUFFER_SIZE; i++)
        gs->action_log.entries[i][0] = '\0';

    for (int i = 0; i < MAX_PLAYERS; i++)
        sem_init(&gs->player_turn_sem[i], 1, 0);
    sem_init(&gs->action_ready_sem,     1, 0);
    for (int i = 0; i < MAX_ENEMIES; i++)
        sem_init(&gs->npc_turn_sem[i], 1, 0);
    sem_init(&gs->npc_action_ready_sem, 1, 0);
    sem_init(&gs->render_sem,           1, 0);
}

static int rand_range(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

static void init_players(int n) {
    int speed = MAX_STAMINA_PLAYER / n;
    for (int i = 0; i < n; i++) {
        PlayerChar* p = &gs->players[i];
        memset(p, 0, sizeof(PlayerChar));
        p->id          = i;
        p->hp          = ROLL_NUMBER + rand_range(100, 1000);
        p->max_hp      = p->hp;
        p->damage      = PLAYER_BASE_DMG;
        p->speed       = speed;
        p->stamina     = 0;
        p->max_stamina = MAX_STAMINA_PLAYER;
        p->state       = STATE_IDLE;
        p->active      = 1;
        p->saved_stamina  = 0;
        p->stun_expiry_ms = 0;
        p->has_eclipse_relic = 0;
        for (int s = 0; s < INVENTORY_SLOTS; s++) {
            p->inventory[s].weapon     = WEAPON_NONE;
            p->inventory[s].owner_slot = -1;
        }
        printf("[Arbiter] Player %d  HP=%d  DMG=%d  SPD=%d\n",
               i, p->hp, p->damage, p->speed);
    }
}

// Init enemies starting at slot 'start', count 'count'
static void init_enemies_range(int start, int count) {
    for (int i = start; i < start + count; i++) {
        EnemyChar* e = &gs->enemies[i];
        memset(e, 0, sizeof(EnemyChar));
        e->id          = i;
        e->hp          = ROLL_LAST2 + rand_range(50, 200);
        e->max_hp      = e->hp;
        e->damage      = ENEMY_BASE_DMG;
        e->speed       = rand_range(10, 30);
        e->stamina     = 0;
        e->max_stamina = MAX_STAMINA_ENEMY;
        e->state       = STATE_IDLE;
        e->active      = 1;
        e->held_weapon = WEAPON_NONE;
        e->holds_solar_core    = 0;
        e->holds_lunar_blade   = 0;
        e->holds_eclipse_relic = 0;
        e->saved_stamina  = 0;
        e->stun_expiry_ms = 0;
        printf("[Arbiter] Enemy  %d  HP=%d  DMG=%d  SPD=%d\n",
               i, e->hp, e->damage, e->speed);
    }
}

static void init_artifacts() {
    ArtifactTable* at = &gs->artifacts;
    at->solar_core_holder    = -1;
    at->lunar_blade_holder   = -1;
    at->eclipse_relic_holder = -1;
    at->eclipse_relic_exists = 0;
    memset(at->waiting_for_solar,   0, sizeof(at->waiting_for_solar));
    memset(at->waiting_for_lunar,   0, sizeof(at->waiting_for_lunar));
    memset(at->waiting_for_eclipse, 0, sizeof(at->waiting_for_eclipse));
}

//=============================================================
//  ACTION LOG
//=============================================================
static void log_action(const char* fmt, ...) {
    if (!gs) return;
    char buf[LOG_ENTRY_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    pthread_mutex_lock(&gs->action_log.mutex);
    int idx = gs->action_log.head;
    strncpy(gs->action_log.entries[idx], buf, LOG_ENTRY_LEN - 1);
    gs->action_log.entries[idx][LOG_ENTRY_LEN - 1] = '\0';
    gs->action_log.head = (idx + 1) % LOG_BUFFER_SIZE;
    if (gs->action_log.count < LOG_BUFFER_SIZE)
        gs->action_log.count++;
    pthread_mutex_unlock(&gs->action_log.mutex);
}

//=============================================================
//  ARTIFACT LOCKING
//=============================================================
static int player_has_solar(PlayerChar* p) {
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (p->inventory[i].owner_slot == i &&
            p->inventory[i].weapon == WEAPON_SOLAR_CORE)
            return 1;
    return 0;
}

static int player_has_lunar(PlayerChar* p) {
    for (int i = 0; i < INVENTORY_SLOTS; i++)
        if (p->inventory[i].owner_slot == i &&
            p->inventory[i].weapon == WEAPON_LUNAR_BLADE)
            return 1;
    return 0;
}

static void update_wait_flags() {
    pthread_mutex_lock(&gs->artifacts.table_mutex);
    ArtifactTable* at = &gs->artifacts;

    for (int i = 0; i < MAX_PLAYERS + MAX_ENEMIES; i++) {
        at->waiting_for_solar[i]   = 0;
        at->waiting_for_lunar[i]   = 0;
        at->waiting_for_eclipse[i] = 0;
    }

    for (int i = 0; i < gs->num_players; i++) {
        PlayerChar* p = &gs->players[i];
        if (!p->active) continue;
        int has_s = player_has_solar(p);
        int has_l = player_has_lunar(p);
        if (has_s && !has_l && at->lunar_blade_holder >= 0)
            at->waiting_for_lunar[i] = 1;
        if (has_l && !has_s && at->solar_core_holder >= 0)
            at->waiting_for_solar[i] = 1;
    }

    for (int i = 0; i < gs->num_enemies; i++) {
        EnemyChar* e = &gs->enemies[i];
        if (!e->active) continue;
        int idx = MAX_PLAYERS + i;
        if (e->holds_solar_core && !e->holds_lunar_blade && at->lunar_blade_holder >= 0)
            at->waiting_for_lunar[idx] = 1;
        if (e->holds_lunar_blade && !e->holds_solar_core && at->solar_core_holder >= 0)
            at->waiting_for_solar[idx] = 1;
    }

    pthread_mutex_unlock(&gs->artifacts.table_mutex);
}

static void lock_artifact_for_player(ArtifactKind kind, int player_id) {
    pthread_mutex_lock(&gs->artifacts.table_mutex);
    switch (kind) {
        case ARTIFACT_SOLAR_CORE:
            gs->artifacts.solar_core_holder = player_id; break;
        case ARTIFACT_LUNAR_BLADE:
            gs->artifacts.lunar_blade_holder = player_id; break;
        case ARTIFACT_ECLIPSE_RELIC:
            gs->artifacts.eclipse_relic_holder = player_id;
            gs->players[player_id].has_eclipse_relic = 1; break;
        default: break;
    }
    pthread_mutex_unlock(&gs->artifacts.table_mutex);
    update_wait_flags();
}

static void lock_artifact_for_enemy(ArtifactKind kind, int enemy_id) {
    pthread_mutex_lock(&gs->artifacts.table_mutex);
    int encoded = MAX_PLAYERS + enemy_id;
    switch (kind) {
        case ARTIFACT_SOLAR_CORE:
            gs->artifacts.solar_core_holder = encoded;
            gs->enemies[enemy_id].holds_solar_core = 1; break;
        case ARTIFACT_LUNAR_BLADE:
            gs->artifacts.lunar_blade_holder = encoded;
            gs->enemies[enemy_id].holds_lunar_blade = 1; break;
        case ARTIFACT_ECLIPSE_RELIC:
            gs->artifacts.eclipse_relic_holder = encoded;
            gs->enemies[enemy_id].holds_eclipse_relic = 1; break;
        default: break;
    }
    pthread_mutex_unlock(&gs->artifacts.table_mutex);
    update_wait_flags();
}

static void release_artifact(ArtifactKind kind) {
    pthread_mutex_lock(&gs->artifacts.table_mutex);
    int prev_holder = -1;
    switch (kind) {
        case ARTIFACT_SOLAR_CORE:
            prev_holder = gs->artifacts.solar_core_holder;
            gs->artifacts.solar_core_holder = -1;
            if (prev_holder >= MAX_PLAYERS) {
                int eid = prev_holder - MAX_PLAYERS;
                if (eid >= 0 && eid < MAX_ENEMIES)
                    gs->enemies[eid].holds_solar_core = 0;
            }
            break;
        case ARTIFACT_LUNAR_BLADE:
            prev_holder = gs->artifacts.lunar_blade_holder;
            gs->artifacts.lunar_blade_holder = -1;
            if (prev_holder >= MAX_PLAYERS) {
                int eid = prev_holder - MAX_PLAYERS;
                if (eid >= 0 && eid < MAX_ENEMIES)
                    gs->enemies[eid].holds_lunar_blade = 0;
            }
            break;
        case ARTIFACT_ECLIPSE_RELIC:
            prev_holder = gs->artifacts.eclipse_relic_holder;
            gs->artifacts.eclipse_relic_holder = -1;
            if (prev_holder >= 0 && prev_holder < MAX_PLAYERS)
                gs->players[prev_holder].has_eclipse_relic = 0;
            else if (prev_holder >= MAX_PLAYERS) {
                int eid = prev_holder - MAX_PLAYERS;
                if (eid >= 0 && eid < MAX_ENEMIES)
                    gs->enemies[eid].holds_eclipse_relic = 0;
            }
            break;
        default: break;
    }
    pthread_mutex_unlock(&gs->artifacts.table_mutex);
    update_wait_flags();
}

//=============================================================
//  WEAPON DROP HELPERS
//=============================================================
static void handle_enemy_death_drop(EnemyChar* dead_enemy) {
    if (dead_enemy->holds_solar_core) {
        release_artifact(ARTIFACT_SOLAR_CORE);
        log_action("E%d's Solar Core lock released", dead_enemy->id);
    }
    if (dead_enemy->holds_lunar_blade) {
        release_artifact(ARTIFACT_LUNAR_BLADE);
        log_action("E%d's Lunar Blade lock released", dead_enemy->id);
    }
    if (dead_enemy->holds_eclipse_relic) {
        release_artifact(ARTIFACT_ECLIPSE_RELIC);
        log_action("E%d's Eclipse Relic lock released", dead_enemy->id);
    }

    if (dead_enemy->held_weapon != WEAPON_NONE) {
        printf("[Arbiter] Enemy %d's weapon (%s) is lost\n",
               dead_enemy->id, WEAPON_TABLE[dead_enemy->held_weapon].name);
        log_action("E%d's %s was destroyed", dead_enemy->id,
                   WEAPON_TABLE[dead_enemy->held_weapon].name);
        dead_enemy->held_weapon = WEAPON_NONE;
        return;
    }

    if (rand() % 100 < 50) {
        WeaponKind dropped;
        int roll = rand() % 100;
        if (roll < 5)       dropped = WEAPON_SOLAR_CORE;
        else if (roll < 10) dropped = WEAPON_LUNAR_BLADE;
        else {
            int regular_count = WEAPON_COUNT - 3;
            dropped = (WeaponKind)(3 + rand() % regular_count);
        }

        gs->pending_drop.active        = 1;
        gs->pending_drop.weapon        = dropped;
        gs->pending_drop.from_enemy_id = dead_enemy->id;

        printf("[Arbiter] Enemy %d dropped: %s (pending player decision)\n",
               dead_enemy->id, WEAPON_TABLE[dropped].name);
        log_action("E%d dropped %s!", dead_enemy->id, WEAPON_TABLE[dropped].name);
    } else {
        printf("[Arbiter] Enemy %d dropped nothing\n", dead_enemy->id);
    }
}

static void enemy_auto_pickup() {
    if (!gs->pending_drop.active) return;

    int candidates[MAX_ENEMIES], count = 0;
    for (int i = 0; i < gs->num_enemies; i++) {
        if (gs->enemies[i].active &&
            gs->enemies[i].held_weapon == WEAPON_NONE)
            candidates[count++] = i;
    }

    if (count > 0) {
        int chosen = candidates[rand() % count];
        WeaponKind wk = gs->pending_drop.weapon;
        gs->enemies[chosen].held_weapon = wk;
        if (wk == WEAPON_SOLAR_CORE) {
            lock_artifact_for_enemy(ARTIFACT_SOLAR_CORE, chosen);
            log_action("Solar Core LOCKED by E%d", chosen);
        } else if (wk == WEAPON_LUNAR_BLADE) {
            lock_artifact_for_enemy(ARTIFACT_LUNAR_BLADE, chosen);
            log_action("Lunar Blade LOCKED by E%d", chosen);
        }
        printf("[Arbiter] Enemy %d picked up the dropped %s\n",
               chosen, WEAPON_TABLE[wk].name);
        log_action("E%d picked up %s", chosen, WEAPON_TABLE[wk].name);
    } else {
        printf("[Arbiter] No enemy available — dropped %s vanished\n",
               WEAPON_TABLE[gs->pending_drop.weapon].name);
        log_action("Dropped %s vanished", WEAPON_TABLE[gs->pending_drop.weapon].name);
    }

    gs->pending_drop.active        = 0;
    gs->pending_drop.weapon        = WEAPON_NONE;
    gs->pending_drop.from_enemy_id = -1;
}

//=============================================================
//  ECLIPSE RELIC
//=============================================================
static void spawn_eclipse_relic() {
    if (gs->artifacts.eclipse_relic_exists) return;
    gs->artifacts.eclipse_relic_exists = 1;
    gs->pending_relic.active = 1;
    printf("[Arbiter] *** Eclipse Relic has appeared! ***\n");
    log_action("*** Eclipse Relic has appeared! ***");
}

static void enemy_auto_pickup_relic() {
    if (!gs->pending_relic.active) return;

    int candidates[MAX_ENEMIES], count = 0;
    for (int i = 0; i < gs->num_enemies; i++) {
        if (gs->enemies[i].active && !gs->enemies[i].holds_eclipse_relic)
            candidates[count++] = i;
    }

    if (count > 0) {
        int chosen = candidates[rand() % count];
        lock_artifact_for_enemy(ARTIFACT_ECLIPSE_RELIC, chosen);
        printf("[Arbiter] Enemy %d picked up the Eclipse Relic\n", chosen);
        log_action("E%d picked up Eclipse Relic (lock acquired)", chosen);
    } else {
        printf("[Arbiter] No enemy available — Eclipse Relic remains free\n");
        log_action("Eclipse Relic remains on ground");
    }
    gs->pending_relic.active = 0;
}

//=============================================================
//  STUN MECHANIC
//=============================================================
static void try_stun_player(int player_id, int weapon_dmg) {
    if (weapon_dmg < STUN_DAMAGE_THRESHOLD) return;
    if (rand() % 100 >= STUN_CHANCE) return;
    PlayerChar* p = &gs->players[player_id];
    if (!p->active || p->state == STATE_DEAD) return;
    if (p->state == STATE_STUNNED) return;
    p->saved_stamina  = p->stamina;
    p->state          = STATE_STUNNED;
    p->stun_expiry_ms = now_ms() + STUN_DURATION_SEC * 1000;
    printf("[Arbiter] *** Player %d STUNNED for %d seconds! ***\n",
           player_id, STUN_DURATION_SEC);
    log_action("*** P%d STUNNED for %ds! ***", player_id, STUN_DURATION_SEC);
    if (gs->hip_pid[player_id] > 0)
        kill(gs->hip_pid[player_id], SIGUSR1);
}

static void try_stun_enemy(int enemy_id, int weapon_dmg) {
    if (weapon_dmg < STUN_DAMAGE_THRESHOLD) return;
    if (rand() % 100 >= STUN_CHANCE) return;
    EnemyChar* e = &gs->enemies[enemy_id];
    if (!e->active || e->state == STATE_DEAD) return;
    if (e->state == STATE_STUNNED) return;
    e->saved_stamina  = e->stamina;
    e->state          = STATE_STUNNED;
    e->stun_expiry_ms = now_ms() + STUN_DURATION_SEC * 1000;
    printf("[Arbiter] *** Enemy %d STUNNED for %d seconds! ***\n",
           enemy_id, STUN_DURATION_SEC);
    log_action("*** E%d STUNNED for %ds! ***", enemy_id, STUN_DURATION_SEC);
    if (gs->asp_pid > 0)
        kill(gs->asp_pid, SIGUSR1);
}

static void check_stun_expiry() {
    long now = now_ms();
    for (int i = 0; i < gs->num_players; i++) {
        PlayerChar* p = &gs->players[i];
        if (p->state == STATE_STUNNED && now >= p->stun_expiry_ms) {
            p->stamina        = p->saved_stamina;
            p->stun_expiry_ms = 0;
            p->state = (p->stamina >= p->max_stamina) ? STATE_READY : STATE_IDLE;
            printf("[Arbiter] Player %d stun expired (Sta:%d)\n", i, p->stamina);
            log_action("P%d stun expired", i);
        }
    }
    for (int i = 0; i < gs->num_enemies; i++) {
        EnemyChar* e = &gs->enemies[i];
        if (e->state == STATE_STUNNED && now >= e->stun_expiry_ms) {
            e->stamina        = e->saved_stamina;
            e->stun_expiry_ms = 0;
            e->state = (e->stamina >= e->max_stamina) ? STATE_READY : STATE_IDLE;
            printf("[Arbiter] Enemy %d stun expired (Sta:%d)\n", i, e->stamina);
            log_action("E%d stun expired", i);
        }
    }
}

//=============================================================
//  STAMINA TICK
//=============================================================
static void tick_stamina() {
    for (int i = 0; i < gs->num_players; i++) {
        PlayerChar* p = &gs->players[i];
        if (!p->active || p->state == STATE_DEAD  ||
            p->state == STATE_STUNNED ||
            p->state == STATE_READY   ||
            p->state == STATE_ACTING) continue;
        p->stamina += p->speed;
        if (p->stamina >= p->max_stamina) {
            p->stamina = p->max_stamina;
            p->state   = STATE_READY;
        }
    }
    for (int i = 0; i < gs->num_enemies; i++) {
        EnemyChar* e = &gs->enemies[i];
        if (!e->active || e->state == STATE_DEAD  ||
            e->state == STATE_STUNNED ||
            e->state == STATE_READY   ||
            e->state == STATE_ACTING) continue;
        e->stamina += e->speed;
        if (e->stamina >= e->max_stamina) {
            e->stamina = e->max_stamina;
            e->state   = STATE_READY;
        }
    }
}

static int find_next_actor() {
    int        best_id       = -1;
    double     best_fraction = -1.0;
    EntityKind best_kind     = ENTITY_PLAYER;

    for (int i = 0; i < gs->num_players; i++) {
        PlayerChar* p = &gs->players[i];
        if (p->active && p->state == STATE_READY) {
            double frac = (double)p->stamina / p->max_stamina;
            if (frac > best_fraction) {
                best_fraction = frac;
                best_id       = i;
                best_kind     = ENTITY_PLAYER;
            }
        }
    }
    for (int i = 0; i < gs->num_enemies; i++) {
        EnemyChar* e = &gs->enemies[i];
        if (e->active && e->state == STATE_READY) {
            double frac = (double)e->stamina / e->max_stamina;
            if (frac > best_fraction) {
                best_fraction = frac;
                best_id       = i;
                best_kind     = ENTITY_ENEMY;
            }
        }
    }

    if (best_id < 0) return 0;
    gs->whose_turn      = best_id;
    gs->whose_turn_kind = best_kind;
    return 1;
}

//=============================================================
//  INVENTORY ALLOCATOR
//=============================================================
static int find_free_slots(PlayerChar* p, int needed) {
    for (int i = 0; i <= INVENTORY_SLOTS - needed; i++) {
        int ok = 1;
        for (int j = i; j < i + needed; j++)
            if (p->inventory[j].weapon != WEAPON_NONE) { ok = 0; break; }
        if (ok) return i;
    }
    return -1;
}

static void place_weapon_at(PlayerChar* p, int start, WeaponKind wk) {
    int sz = WEAPON_TABLE[wk].slot_size;
    for (int i = start; i < start + sz && i < INVENTORY_SLOTS; i++) {
        p->inventory[i].weapon     = wk;
        p->inventory[i].owner_slot = start;
    }
}

static void swap_out_at(PlayerChar* p, int start) {
    WeaponKind wk = p->inventory[start].weapon;
    if (wk == WEAPON_NONE) return;
    if (p->long_term_count < LONG_TERM_MAX) {
        p->long_term[p->long_term_count].weapon = wk;
        p->long_term[p->long_term_count].valid  = 1;
        p->long_term_count++;
    }
    int sz = WEAPON_TABLE[wk].slot_size;
    for (int i = start; i < start + sz && i < INVENTORY_SLOTS; i++) {
        p->inventory[i].weapon     = WEAPON_NONE;
        p->inventory[i].owner_slot = -1;
    }
    if (wk == WEAPON_SOLAR_CORE) {
        release_artifact(ARTIFACT_SOLAR_CORE);
        log_action("P%d released Solar Core lock", p->id);
    } else if (wk == WEAPON_LUNAR_BLADE) {
        release_artifact(ARTIFACT_LUNAR_BLADE);
        log_action("P%d released Lunar Blade lock", p->id);
    }
    printf("[Arbiter] Player %d: swapped out %s\n", p->id, WEAPON_TABLE[wk].name);
    log_action("P%d stored %s in long-term", p->id, WEAPON_TABLE[wk].name);
}

static int inventory_add(PlayerChar* p, WeaponKind wk) {
    int needed = WEAPON_TABLE[wk].slot_size;
    if (needed == 0) return 0;
    int slot = find_free_slots(p, needed);
    if (slot >= 0) {
        place_weapon_at(p, slot, wk);
        printf("[Arbiter] Player %d: picked up %s at slot %d\n",
               p->id, WEAPON_TABLE[wk].name, slot);
        if (wk == WEAPON_SOLAR_CORE) {
            lock_artifact_for_player(ARTIFACT_SOLAR_CORE, p->id);
            log_action("Solar Core LOCKED by P%d", p->id);
        } else if (wk == WEAPON_LUNAR_BLADE) {
            lock_artifact_for_player(ARTIFACT_LUNAR_BLADE, p->id);
            log_action("Lunar Blade LOCKED by P%d", p->id);
        }
        return 1;
    }
    printf("[Arbiter] Player %d: making room for %s\n", p->id, WEAPON_TABLE[wk].name);
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        if (p->inventory[i].weapon != WEAPON_NONE &&
            p->inventory[i].owner_slot == i) {
            swap_out_at(p, i);
            slot = find_free_slots(p, needed);
            if (slot >= 0) {
                place_weapon_at(p, slot, wk);
                printf("[Arbiter] Player %d: placed %s at slot %d\n",
                       p->id, WEAPON_TABLE[wk].name, slot);
                if (wk == WEAPON_SOLAR_CORE) {
                    lock_artifact_for_player(ARTIFACT_SOLAR_CORE, p->id);
                    log_action("Solar Core LOCKED by P%d", p->id);
                } else if (wk == WEAPON_LUNAR_BLADE) {
                    lock_artifact_for_player(ARTIFACT_LUNAR_BLADE, p->id);
                    log_action("Lunar Blade LOCKED by P%d", p->id);
                }
                return 1;
            }
        }
    }
    return 0;
}

static void update_ultimate_flag(PlayerChar* p) {
    int has_solar = player_has_solar(p);
    int has_lunar = player_has_lunar(p);
    p->ultimate_eligible = (has_solar && has_lunar) ? 1 : 0;
}

//=============================================================
//  ACTION EXECUTION
//=============================================================
static void apply_player_action(ActionRequest* req) {
    PlayerChar* p = &gs->players[req->player_id];
    p->state = STATE_ACTING;

    switch (req->action) {

    case ACTION_ATTACK_STRIKE: {
        EnemyChar* e = &gs->enemies[req->target_enemy_id];
        int dmg = p->damage;
        if (e->holds_eclipse_relic) dmg = dmg / 2;
        e->hp -= dmg;
        printf("[Arbiter] Player %d STRIKES Enemy %d  -%d HP  (HP:%d)\n",
               p->id, e->id, dmg, e->hp);
        log_action("P%d strikes E%d for %d (HP:%d)", p->id, e->id, dmg, e->hp);
        if (e->hp <= 0) {
            e->hp = 0; e->active = 0; e->state = STATE_DEAD;
            gs->enemies_killed++;
            printf("[Arbiter] Enemy %d defeated! Kills:%d\n",
                   e->id, gs->enemies_killed);
            log_action("** Enemy %d defeated! (Kills %d/10) **",
                       e->id, gs->enemies_killed);
            handle_enemy_death_drop(e);
        }
        p->stamina = 0;
        break;
    }

    case ACTION_ATTACK_EXHAUST: {
        EnemyChar* e = &gs->enemies[req->target_enemy_id];
        e->stamina -= p->damage;
        if (e->stamina < 0) e->stamina = 0;
        if (e->state == STATE_READY) e->state = STATE_IDLE;
        printf("[Arbiter] Player %d EXHAUSTS Enemy %d  (Sta:%d)\n",
               p->id, e->id, e->stamina);
        log_action("P%d exhausts E%d (Sta:%d)", p->id, e->id, e->stamina);
        p->stamina = 0;
        break;
    }

    case ACTION_USE_WEAPON: {
        int slot = req->weapon_slot;
        if (slot >= 0 && slot < INVENTORY_SLOTS &&
            p->inventory[slot].weapon != WEAPON_NONE) {
            WeaponKind wk = p->inventory[slot].weapon;
            int dmg = WEAPON_TABLE[wk].damage;
            EnemyChar* e = &gs->enemies[req->target_enemy_id];
            if (e->holds_eclipse_relic) dmg = dmg / 2;
            e->hp -= dmg;
            printf("[Arbiter] Player %d uses [%s] on Enemy %d  -%d HP  (HP:%d)\n",
                   p->id, WEAPON_TABLE[wk].name, e->id, dmg, e->hp);
            log_action("P%d uses %s on E%d for %d (HP:%d)",
                       p->id, WEAPON_TABLE[wk].name, e->id, dmg, e->hp);
            if (e->hp <= 0) {
                e->hp = 0; e->active = 0; e->state = STATE_DEAD;
                gs->enemies_killed++;
                printf("[Arbiter] Enemy %d defeated! Kills:%d\n",
                       e->id, gs->enemies_killed);
                log_action("** Enemy %d defeated! (Kills %d/10) **",
                           e->id, gs->enemies_killed);
                handle_enemy_death_drop(e);
            } else {
                try_stun_enemy(e->id, dmg);
            }
            if (p->ultimate_eligible) {
                printf("[Arbiter] *** ULTIMATE — Player %d! ASP suspended 10s ***\n",
                       p->id);
                log_action("*** ULTIMATE! P%d activated — enemies frozen 10s ***", p->id);
                gs->ultimate_active = 1;
                ultimate_ended      = 0;
                kill(gs->asp_pid, SIGSTOP);
                alarm(ULTIMATE_PAUSE_SEC);
            }
        }
        p->stamina = 0;
        break;
    }

    case ACTION_SWAP_IN: {
        WeaponKind wk = req->swap_in_weapon;
        for (int i = 0; i < p->long_term_count; i++) {
            if (p->long_term[i].valid && p->long_term[i].weapon == wk) {
                p->long_term[i].valid = 0;
                inventory_add(p, wk);
                update_ultimate_flag(p);
                printf("[Arbiter] Player %d swapped in %s\n",
                       p->id, WEAPON_TABLE[wk].name);
                log_action("P%d swapped in %s", p->id, WEAPON_TABLE[wk].name);
                break;
            }
        }
        p->stamina = 0;
        break;
    }

    case ACTION_HEAL: {
        int heal = p->max_hp / 10;
        p->hp = (p->hp + heal > p->max_hp) ? p->max_hp : p->hp + heal;
        printf("[Arbiter] Player %d HEALS +%d  (HP:%d/%d)\n",
               p->id, heal, p->hp, p->max_hp);
        log_action("P%d heals +%d (HP:%d)", p->id, heal, p->hp);
        p->stamina = 0;
        break;
    }

    case ACTION_SKIP:
        printf("[Arbiter] Player %d SKIPS\n", p->id);
        log_action("P%d skips", p->id);
        p->stamina = p->max_stamina / 2;
        break;

    case ACTION_PICKUP_YES: {
        if (gs->pending_drop.active) {
            WeaponKind wk = gs->pending_drop.weapon;
            int success = inventory_add(p, wk);
            if (success) {
                update_ultimate_flag(p);
                printf("[Arbiter] Player %d picked up %s\n",
                       p->id, WEAPON_TABLE[wk].name);
                log_action("P%d picked up %s", p->id, WEAPON_TABLE[wk].name);
            } else {
                printf("[Arbiter] Player %d FAILED to pick up %s — auto-given to enemy\n",
                       p->id, WEAPON_TABLE[wk].name);
                log_action("P%d couldn't fit %s — enemy gets it", p->id, WEAPON_TABLE[wk].name);
                enemy_auto_pickup();
            }
            gs->pending_drop.active        = 0;
            gs->pending_drop.weapon        = WEAPON_NONE;
            gs->pending_drop.from_enemy_id = -1;
        }
        p->stamina = p->max_stamina / 2;
        break;
    }

    case ACTION_PICKUP_NO: {
        printf("[Arbiter] Player %d declined the dropped weapon\n", p->id);
        log_action("P%d declined the dropped weapon", p->id);
        enemy_auto_pickup();
        p->stamina = p->max_stamina / 2;
        break;
    }

    case ACTION_RELIC_YES: {
        if (gs->pending_relic.active) {
            lock_artifact_for_player(ARTIFACT_ECLIPSE_RELIC, p->id);
            printf("[Arbiter] Player %d picked up the Eclipse Relic\n", p->id);
            log_action("P%d picked up Eclipse Relic (lock acquired)", p->id);
            gs->pending_relic.active = 0;
        }
        p->stamina = p->max_stamina / 2;
        break;
    }

    case ACTION_RELIC_NO: {
        printf("[Arbiter] Player %d declined the Eclipse Relic\n", p->id);
        log_action("P%d declined the Eclipse Relic", p->id);
        enemy_auto_pickup_relic();
        p->stamina = p->max_stamina / 2;
        break;
    }

    default:
        printf("[Arbiter] Unknown action — skip\n");
        p->stamina = 0;
        break;
    }

    p->state = STATE_IDLE;
}

static void apply_npc_action(NpcAction* req) {
    EnemyChar* e = &gs->enemies[req->enemy_id];
    e->state = STATE_ACTING;

    switch (req->action) {
    case ACTION_ATTACK_STRIKE: {
        PlayerChar* p = &gs->players[req->target_player_id];
        int dmg = e->damage;
        int used_weapon = 0;
        if (e->held_weapon != WEAPON_NONE) {
            dmg = WEAPON_TABLE[e->held_weapon].damage;
            used_weapon = 1;
        }
        if (p->has_eclipse_relic) dmg = dmg / 2;
        p->hp -= dmg;
        if (used_weapon) {
            printf("[Arbiter] Enemy %d STRIKES Player %d with [%s]  -%d HP  (HP:%d)\n",
                   e->id, p->id, WEAPON_TABLE[e->held_weapon].name, dmg, p->hp);
            log_action("E%d strikes P%d with %s for %d (HP:%d)",
                       e->id, p->id, WEAPON_TABLE[e->held_weapon].name, dmg, p->hp);
        } else {
            printf("[Arbiter] Enemy %d STRIKES Player %d  -%d HP  (HP:%d)\n",
                   e->id, p->id, dmg, p->hp);
            log_action("E%d strikes P%d for %d (HP:%d)", e->id, p->id, dmg, p->hp);
        }
        if (p->hp <= 0) {
            p->hp = 0; p->active = 0; p->state = STATE_DEAD;
            printf("[Arbiter] Player %d defeated!\n", p->id);
            log_action("** Player %d defeated! **", p->id);
            if (p->has_eclipse_relic) {
                release_artifact(ARTIFACT_ECLIPSE_RELIC);
                log_action("P%d's Eclipse Relic lock released", p->id);
            }
        } else if (used_weapon) {
            try_stun_player(p->id, dmg);
        }
        e->stamina = 0;
        break;
    }
    case ACTION_SKIP:
        printf("[Arbiter] Enemy %d SKIPS\n", e->id);
        log_action("E%d skips", e->id);
        e->stamina = e->max_stamina / 2;
        break;
    default:
        e->stamina = e->max_stamina / 2;
        break;
    }

    e->state = STATE_IDLE;
}

//=============================================================
//  WIN / LOSE / QUIT
//=============================================================
static int check_game_over() {
    if (gs->enemies_killed >= MAX_TOTAL_ENEMIES) {
        gs->result = GAME_WIN; gs->game_over = 1;
        printf("[Arbiter] *** PLAYERS WIN! ***\n");
        log_action("*** PLAYERS WIN! 10 enemies defeated! ***");
        return 1;
    }
    int alive = 0;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].active) alive++;
    if (alive == 0) {
        gs->result = GAME_LOSE; gs->game_over = 1;
        printf("[Arbiter] *** GAME OVER ***\n");
        log_action("*** GAME OVER — All players defeated ***");
        return 1;
    }
    if (gs->result == GAME_QUIT) {
        gs->game_over = 1;
        return 1;
    }
    return 0;
}

//=============================================================
//  WAVE RESPAWN
//  Called after every action — triggers a new wave when all
//  current enemies are dead and the kill cap isn't reached yet.
//=============================================================
static void check_wave_respawn() {
    if (gs->game_over) return;
    if (gs->enemies_killed >= MAX_TOTAL_ENEMIES) return;
    if (gs->total_enemies_spawned >= MAX_TOTAL_ENEMIES) return;
    if (gs->asp_wave_ready) return;   // ASP hasn't consumed last wave yet

    // Check if all current enemies are dead
    int alive = 0;
    for (int i = 0; i < gs->num_enemies; i++)
        if (gs->enemies[i].active) alive++;
    if (alive > 0) return;

    // Clear any leftover pending drop from the previous wave
    gs->pending_drop.active        = 0;
    gs->pending_drop.weapon        = WEAPON_NONE;
    gs->pending_drop.from_enemy_id = -1;

    // Clear stale artifact locks held by dead enemies (safety net)
    if (gs->artifacts.solar_core_holder >= MAX_PLAYERS)
        release_artifact(ARTIFACT_SOLAR_CORE);
    if (gs->artifacts.lunar_blade_holder >= MAX_PLAYERS)
        release_artifact(ARTIFACT_LUNAR_BLADE);
    if (gs->artifacts.eclipse_relic_holder >= MAX_PLAYERS)
        release_artifact(ARTIFACT_ECLIPSE_RELIC);

    // Decide wave size — can't exceed remaining quota
    int remaining = MAX_TOTAL_ENEMIES - gs->total_enemies_spawned;
    int wave_size = rand_range(2, MAX_ENEMIES);
    if (wave_size > remaining) wave_size = remaining;

    gs->num_enemies            = wave_size;
    gs->total_enemies_spawned += wave_size;

    // Re-init enemy slots from index 0
    init_enemies_range(0, wave_size);

    // Reset semaphores for these slots
    for (int i = 0; i < wave_size; i++) {
        sem_destroy(&gs->npc_turn_sem[i]);
        sem_init(&gs->npc_turn_sem[i], 1, 0);
    }

    printf("[Arbiter] === NEW WAVE: %d enemies  (spawned %d/%d total) ===\n",
           wave_size, gs->total_enemies_spawned, MAX_TOTAL_ENEMIES);
    log_action("=== NEW WAVE: %d enemies (total %d/%d) ===",
               wave_size, gs->total_enemies_spawned, MAX_TOTAL_ENEMIES);

    // Signal ASP to launch new threads
    gs->asp_wave_ready = 1;
}

//=============================================================
//  DEADLOCK MONITOR
//=============================================================
static void* deadlock_monitor(void*) {
    while (!gs->game_over) {
        sleep(2);
        if (gs->game_over) break;

        pthread_mutex_lock(&gs->artifacts.table_mutex);
        ArtifactTable* at = &gs->artifacts;
        int sh = at->solar_core_holder;
        int lh = at->lunar_blade_holder;

        if (sh >= 0 && lh >= 0 && sh != lh &&
            at->waiting_for_lunar[sh] && at->waiting_for_solar[lh]) {

            printf("[Arbiter] DEADLOCK detected — forcing Solar Core release (holder=%d)\n", sh);
            log_action("DEADLOCK detected! Forcing Solar Core release");

            if (sh < MAX_PLAYERS && sh < gs->num_players) {
                PlayerChar* p = &gs->players[sh];
                for (int i = 0; i < INVENTORY_SLOTS; i++) {
                    if (p->inventory[i].weapon == WEAPON_SOLAR_CORE &&
                        p->inventory[i].owner_slot == i) {
                        int sz = WEAPON_TABLE[WEAPON_SOLAR_CORE].slot_size;
                        for (int j = i; j < i + sz && j < INVENTORY_SLOTS; j++) {
                            p->inventory[j].weapon     = WEAPON_NONE;
                            p->inventory[j].owner_slot = -1;
                        }
                        break;
                    }
                }
                p->ultimate_eligible = 0;
            } else if (sh >= MAX_PLAYERS) {
                int eid = sh - MAX_PLAYERS;
                if (eid >= 0 && eid < MAX_ENEMIES) {
                    if (gs->enemies[eid].held_weapon == WEAPON_SOLAR_CORE)
                        gs->enemies[eid].held_weapon = WEAPON_NONE;
                    gs->enemies[eid].holds_solar_core = 0;
                }
            }
            at->solar_core_holder     = -1;
            at->waiting_for_solar[lh] = 0;
        }
        pthread_mutex_unlock(&gs->artifacts.table_mutex);
        update_wait_flags();
    }
    return NULL;
}

//=============================================================
//  TURN HELPERS
//=============================================================
static void do_player_turn(int player_id) {
    gs->player_action.ready     = 0;
    gs->player_action.processed = 0;
    gs->player_action.player_id = player_id;

    PlayerInputState* pis = &gs->input_state[player_id];
    pis->phase         = INPUT_IDLE;
    pis->chosen_action = ACTION_NONE;
    pis->chosen_enemy  = -1;
    pis->chosen_slot   = -1;

    tick_paused = 1;
    sem_post(&gs->player_turn_sem[player_id]);

    while (!gs->player_action.ready && !gs->game_over)
        usleep(50000);

    tick_paused = 0;
    if (gs->game_over) return;

    pis->phase = INPUT_IDLE;

    apply_player_action(&gs->player_action);
    gs->player_action.processed = 1;
    sem_post(&gs->render_sem);
}

static void do_npc_turn(int enemy_id) {
    gs->npc_action.ready     = 0;
    gs->npc_action.processed = 0;
    gs->npc_action.enemy_id  = enemy_id;

    while (sem_trywait(&gs->npc_turn_sem[enemy_id]) == 0) {}
    while (sem_trywait(&gs->npc_action_ready_sem) == 0) {}

    npc_timed_out = 0;
    alarm(NPC_TURN_TIMEOUT);
    sem_post(&gs->npc_turn_sem[enemy_id]);

    while (!npc_timed_out && !gs->npc_action.ready && !gs->game_over)
        usleep(50000);

    alarm(0);

    if (npc_timed_out || !gs->npc_action.ready) {
        printf("[Arbiter] NPC timeout — Enemy %d SKIPS\n", enemy_id);
        log_action("E%d timed out (forced skip)", enemy_id);
        gs->npc_action.enemy_id = enemy_id;
        gs->npc_action.action   = ACTION_SKIP;
        gs->npc_action.ready    = 1;
    }

    npc_timed_out = 0;
    apply_npc_action(&gs->npc_action);
    gs->npc_action.processed = 1;
    sem_post(&gs->render_sem);
}

//=============================================================
//  STAMINA TICK THREAD
//=============================================================
static void* tick_thread_fn(void*) {
    while (!gs->game_over) {
        usleep(100000);
        if (tick_paused) continue;
        pthread_mutex_lock(&gs->state_mutex);
        check_stun_expiry();
        tick_stamina();
        pthread_mutex_unlock(&gs->state_mutex);
    }
    return NULL;
}

//=============================================================
//  MAIN
//=============================================================
int main() {
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT,  handle_sigint);
    signal(SIGALRM, handle_sigalrm);

    gs = shm_create();
    if (!gs) { fprintf(stderr, "[Arbiter] shm_create failed\n"); return 1; }
    printf("[Arbiter] Shared memory created (%zu bytes)\n", sizeof(GameState));

    init_sync();
    srand(ROLL_NUMBER + (unsigned)time(NULL));

    gs->arbiter_pid           = getpid();
    gs->game_over             = 0;
    gs->turn_number           = 0;
    gs->enemies_killed        = 0;
    gs->total_enemies_spawned = 0;
    gs->asp_wave_ready        = 0;
    gs->result                = GAME_RUNNING;
    gs->ultimate_active       = 0;
    gs->game_started          = 0;
    gs->hip_count             = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        gs->hip_pid[i] = 0;
    printf("[Arbiter] PID = %d\n", gs->arbiter_pid);

    int num_players = 0;
    while (num_players < 1 || num_players > MAX_PLAYERS) {
        printf("Enter party size (1-%d): ", MAX_PLAYERS);
        fflush(stdout);
        char buf[16];
        if (fgets(buf, sizeof(buf), stdin) != NULL)
            num_players = atoi(buf);
    }
    gs->num_players = num_players;

    // First wave — random size, capped at MAX_TOTAL_ENEMIES
    int first_wave = rand_range(2, MAX_ENEMIES);
    if (first_wave > MAX_TOTAL_ENEMIES) first_wave = MAX_TOTAL_ENEMIES;
    gs->num_enemies            = first_wave;
    gs->total_enemies_spawned  = first_wave;

    printf("[Arbiter] Players: %d  |  First wave: %d enemies  (goal: %d kills)\n",
           gs->num_players, gs->num_enemies, MAX_TOTAL_ENEMIES);

    init_players(gs->num_players);
    init_enemies_range(0, gs->num_enemies);
    init_artifacts();

    gs->pending_drop.active        = 0;
    gs->pending_drop.weapon        = WEAPON_NONE;
    gs->pending_drop.from_enemy_id = -1;
    gs->pending_relic.active       = 0;

    gs->eclipse_spawn_turn = rand_range(ECLIPSE_SPAWN_MIN_TURN, ECLIPSE_SPAWN_MAX_TURN);
    printf("[Arbiter] Eclipse Relic will spawn at Turn %d\n", gs->eclipse_spawn_turn);

    gs->shm_ready = 1;

    // Wait for all HIPs and ASP to connect
    printf("[Arbiter] Waiting for %d HIP(s) and ASP...\n", gs->num_players);
    while (gs->hip_count < gs->num_players || gs->asp_pid == 0)
        usleep(100000);

    printf("[Arbiter] All processes connected. ASP=%d\n", gs->asp_pid);
    for (int i = 0; i < gs->num_players; i++)
        printf("[Arbiter] HIP[%d] = PID %d\n", i, gs->hip_pid[i]);

    pthread_t dl_thread;
    pthread_create(&dl_thread, NULL, deadlock_monitor, NULL);

    pthread_t tick_thread;
    pthread_create(&tick_thread, NULL, tick_thread_fn, NULL);

    gs->game_started = 1;
    printf("[Arbiter] Game starting!\n\n");
    log_action("=== Battle begins! %d player(s) vs %d enemies (goal: %d kills) ===",
               gs->num_players, gs->num_enemies, MAX_TOTAL_ENEMIES);

    while (!gs->game_over) {
        usleep(10000);

        pthread_mutex_lock(&gs->state_mutex);
        int found    = find_next_actor();
        int actor_id = gs->whose_turn;
        EntityKind kind = gs->whose_turn_kind;

        if (found) {
            if (kind == ENTITY_PLAYER)
                gs->players[actor_id].state = STATE_ACTING;
            else
                gs->enemies[actor_id].state = STATE_ACTING;
        }
        pthread_mutex_unlock(&gs->state_mutex);

        if (!found) continue;

        gs->turn_number++;
        printf("\n[Arbiter] === Turn %d ===\n", gs->turn_number);

        if (!gs->artifacts.eclipse_relic_exists &&
            gs->turn_number >= gs->eclipse_spawn_turn) {
            spawn_eclipse_relic();
        }

        if (kind == ENTITY_PLAYER) {
            do_player_turn(actor_id);
        } else {
            if (gs->ultimate_active) {
                printf("[Arbiter] ASP suspended — Enemy %d waits\n", actor_id);
                pthread_mutex_lock(&gs->state_mutex);
                gs->enemies[actor_id].stamina = 0;
                gs->enemies[actor_id].state   = STATE_IDLE;
                pthread_mutex_unlock(&gs->state_mutex);
            } else {
                do_npc_turn(actor_id);
            }
        }

        if (ultimate_ended && gs->ultimate_active) {
            printf("[Arbiter] Ultimate window expired — resuming ASP\n");
            log_action("Ultimate window expired");
            gs->ultimate_active = 0;
            ultimate_ended      = 0;
            kill(gs->asp_pid, SIGCONT);
        }

        // Check win/lose first, then check if a new wave should spawn
        if (!check_game_over()) {
            check_wave_respawn();
        }
    }

    printf("\n[Arbiter] Result: %s\n",
           gs->result == GAME_WIN  ? "WIN"  :
           gs->result == GAME_LOSE ? "LOSE" : "QUIT");

    // Unblock all semaphores so threads can exit cleanly
    for (int i = 0; i < MAX_PLAYERS; i++)
        sem_post(&gs->player_turn_sem[i]);
    for (int i = 0; i < MAX_ENEMIES; i++)
        sem_post(&gs->npc_turn_sem[i]);

    pthread_join(tick_thread, NULL);
    pthread_join(dl_thread,   NULL);

    for (int i = 0; i < MAX_PLAYERS; i++)
        sem_destroy(&gs->player_turn_sem[i]);
    sem_destroy(&gs->action_ready_sem);
    for (int i = 0; i < MAX_ENEMIES; i++)
        sem_destroy(&gs->npc_turn_sem[i]);
    sem_destroy(&gs->npc_action_ready_sem);
    sem_destroy(&gs->render_sem);
    pthread_mutex_destroy(&gs->state_mutex);
    pthread_mutex_destroy(&gs->artifact_mutex);
    pthread_mutex_destroy(&gs->artifacts.table_mutex);
    pthread_mutex_destroy(&gs->action_log.mutex);

    shm_destroy(gs);
    printf("[Arbiter] Shutdown complete\n");
    return 0;
}