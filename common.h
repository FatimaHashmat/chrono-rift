#pragma once

#include <semaphore.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ============================================================
//  ROLL NUMBER
//  Roll No: 5265  |  Last2=65  Last1=5  SecondLast=6
// ============================================================
#define ROLL_NUMBER         5265
#define ROLL_LAST2          65
#define ROLL_LAST1          5
#define ROLL_SECOND_LAST    6

#define SHM_NAME            "/chrono_rift_shm"

#define MAX_PLAYERS         4
#define MAX_ENEMIES         9
#define MAX_TOTAL_ENEMIES   10
#define INVENTORY_SLOTS     20
#define LONG_TERM_MAX       32

#define MAX_STAMINA_PLAYER  100
#define MAX_STAMINA_ENEMY   150
#define STUN_DURATION_SEC   3
#define ULTIMATE_PAUSE_SEC  10
#define NPC_TURN_TIMEOUT    3

#define STUN_DAMAGE_THRESHOLD  50
#define STUN_CHANCE           100

#define ECLIPSE_SPAWN_MIN_TURN  5
#define ECLIPSE_SPAWN_MAX_TURN  15

#define PLAYER_BASE_DMG     (ROLL_LAST1 + 10)
#define ENEMY_BASE_DMG      (ROLL_SECOND_LAST + 10)

// ============================================================
//  ENUMS
// ============================================================
typedef enum {
    ACTION_ATTACK_STRIKE,
    ACTION_ATTACK_EXHAUST,
    ACTION_USE_WEAPON,
    ACTION_SWAP_IN,
    ACTION_HEAL,
    ACTION_SKIP,
    ACTION_PICKUP_YES,
    ACTION_PICKUP_NO,
    ACTION_RELIC_YES,
    ACTION_RELIC_NO,
    ACTION_NONE
} ActionType;

typedef enum {
    STATE_IDLE,
    STATE_READY,
    STATE_ACTING,
    STATE_STUNNED,
    STATE_DEAD
} EntityState;

typedef enum {
    ARTIFACT_SOLAR_CORE,
    ARTIFACT_LUNAR_BLADE,
    ARTIFACT_ECLIPSE_RELIC,
    ARTIFACT_NONE
} ArtifactKind;

typedef enum {
    WEAPON_NONE = 0,
    WEAPON_SOLAR_CORE,
    WEAPON_LUNAR_BLADE,
    WEAPON_IRON_HALBERD,
    WEAPON_VENOM_DAGGER,
    WEAPON_THUNDERSTAFF,
    WEAPON_OBSIDIAN_AXE,
    WEAPON_FROSTBOW,
    WEAPON_SPLINTER_STICK,
    WEAPON_COUNT
} WeaponKind;

typedef enum {
    ENTITY_PLAYER,
    ENTITY_ENEMY
} EntityKind;

typedef enum {
    GAME_RUNNING,
    GAME_WIN,
    GAME_LOSE,
    GAME_QUIT
} GameResult;

// ============================================================
//  PLAYER INPUT STATE  — one slot per player/HIP process
// ============================================================
typedef enum {
    INPUT_IDLE,
    INPUT_CHOOSE_ACTION,
    INPUT_CHOOSE_ENEMY,
    INPUT_CHOOSE_SLOT,
    INPUT_CHOOSE_PICKUP,
    INPUT_CHOOSE_RELIC,
    INPUT_CHOOSE_SWAPIN,
    INPUT_DONE
} InputPhase;

typedef struct {
    InputPhase  phase;
    int         active_player;
    ActionType  chosen_action;
    int         chosen_enemy;
    int         chosen_slot;
    int         chosen_lt_index;
    int         pickup_accepted;
    int         stunned_notify;
} PlayerInputState;

// ============================================================
//  WEAPON TABLE
// ============================================================
typedef struct {
    WeaponKind  kind;
    int         slot_size;
    int         damage;
    char        name[32];
} WeaponInfo;

static const WeaponInfo WEAPON_TABLE[WEAPON_COUNT] = {
    { WEAPON_NONE,           0,   0, "None"           },
    { WEAPON_SOLAR_CORE,    10,  95, "Solar Core"     },
    { WEAPON_LUNAR_BLADE,   10,  90, "Lunar Blade"    },
    { WEAPON_IRON_HALBERD,   7,  55, "Iron Halberd"   },
    { WEAPON_VENOM_DAGGER,   4,  30, "Venom Dagger"   },
    { WEAPON_THUNDERSTAFF,   6,  50, "Thunderstaff"   },
    { WEAPON_OBSIDIAN_AXE,   5,  45, "Obsidian Axe"   },
    { WEAPON_FROSTBOW,       6,  48, "Frostbow"       },
    { WEAPON_SPLINTER_STICK, 2,  12, "Splinter Stick" },
};

// ============================================================
//  INVENTORY
// ============================================================
typedef struct {
    WeaponKind  weapon;
    int         owner_slot;
} InventorySlot;

typedef struct {
    WeaponKind  weapon;
    int         valid;
} LongTermEntry;

// ============================================================
//  PLAYER
// ============================================================
typedef struct {
    int         id;
    pid_t       pid;
    pthread_t   thread_id;

    int         hp;
    int         max_hp;
    int         damage;
    int         speed;
    int         stamina;
    int         max_stamina;

    EntityState state;
    int         stun_remaining_ms;
    int         saved_stamina;
    long        stun_expiry_ms;

    InventorySlot inventory[INVENTORY_SLOTS];
    LongTermEntry long_term[LONG_TERM_MAX];
    int           long_term_count;

    int         ultimate_eligible;
    int         has_eclipse_relic;
    int         active;
} PlayerChar;

// ============================================================
//  ENEMY
// ============================================================
typedef struct {
    int         id;
    pid_t       pid;
    pthread_t   thread_id;

    int         hp;
    int         max_hp;
    int         damage;
    int         speed;
    int         stamina;
    int         max_stamina;

    EntityState state;
    int         stun_remaining_ms;
    int         saved_stamina;
    long        stun_expiry_ms;

    WeaponKind  held_weapon;
    int         holds_solar_core;
    int         holds_lunar_blade;
    int         holds_eclipse_relic;

    int         active;
} EnemyChar;

// ============================================================
//  ACTION REQUEST  (HIP -> Arbiter)
// ============================================================
typedef struct {
    int         player_id;
    ActionType  action;
    int         target_enemy_id;
    int         weapon_slot;
    WeaponKind  swap_in_weapon;
    int         ready;
    int         processed;
} ActionRequest;

// ============================================================
//  NPC ACTION  (ASP -> Arbiter)
// ============================================================
typedef struct {
    int         enemy_id;
    ActionType  action;
    int         target_player_id;
    int         ready;
    int         processed;
} NpcAction;

// ============================================================
//  ARTIFACT TABLE
// ============================================================
typedef struct {
    int solar_core_holder;
    int lunar_blade_holder;
    int eclipse_relic_holder;
    int eclipse_relic_exists;

    int waiting_for_solar[MAX_PLAYERS + MAX_ENEMIES];
    int waiting_for_lunar[MAX_PLAYERS + MAX_ENEMIES];
    int waiting_for_eclipse[MAX_PLAYERS + MAX_ENEMIES];

    pthread_mutex_t table_mutex;
} ArtifactTable;

// ============================================================
//  PENDING WEAPON DROP
// ============================================================
typedef struct {
    int        active;
    WeaponKind weapon;
    int        from_enemy_id;
} PendingDrop;

// ============================================================
//  PENDING ECLIPSE RELIC
// ============================================================
typedef struct {
    int active;
} PendingRelic;

// ============================================================
//  ACTION LOG
// ============================================================
#define LOG_BUFFER_SIZE   16
#define LOG_ENTRY_LEN     128

typedef struct {
    char entries[LOG_BUFFER_SIZE][LOG_ENTRY_LEN];
    int  head;
    int  count;
    pthread_mutex_t mutex;
} ActionLog;

// ============================================================
//  GAME STATE
// ============================================================
typedef struct {
    int         shm_ready;
    int         game_started;

    pid_t       arbiter_pid;
    pid_t       asp_pid;

    // Co-op: each HIP registers its own pid slot by player index
    pid_t       hip_pid[MAX_PLAYERS];
    int         hip_count;              // incremented atomically by each HIP on connect

    int         num_players;
    int         num_enemies;            // enemies active in current wave
    int         enemies_killed;         // total kills across all waves
    int         total_enemies_spawned;  // total ever spawned (across all waves)

    PlayerChar  players[MAX_PLAYERS];
    EnemyChar   enemies[MAX_ENEMIES];

    int         whose_turn;
    EntityKind  whose_turn_kind;
    int         turn_number;

    ActionRequest player_action;
    NpcAction     npc_action;

    ArtifactTable artifacts;

    PendingDrop  pending_drop;
    PendingRelic pending_relic;
    int          eclipse_spawn_turn;

    ActionLog action_log;

    // Per-player input state — index = player_id
    PlayerInputState input_state[MAX_PLAYERS];

    GameResult  result;
    int         game_over;
    int         ultimate_active;

    // Wave system — arbiter sets 1 when a new wave is ready; ASP clears it
    volatile int asp_wave_ready;

    pthread_mutex_t state_mutex;
    pthread_mutex_t artifact_mutex;

    sem_t       player_turn_sem[MAX_PLAYERS];
    sem_t       action_ready_sem;
    sem_t       npc_turn_sem[MAX_ENEMIES];
    sem_t       npc_action_ready_sem;
    sem_t       render_sem;

} GameState;

// ============================================================
//  SHARED MEMORY HELPERS
// ============================================================
static inline GameState* shm_create() {
    shm_unlink(SHM_NAME);
    int fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (fd < 0) return NULL;
    if (ftruncate(fd, sizeof(GameState)) < 0) { close(fd); return NULL; }
    GameState* gs = (GameState*)mmap(NULL, sizeof(GameState),
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, 0);
    close(fd);
    if (gs == MAP_FAILED) return NULL;
    memset(gs, 0, sizeof(GameState));
    return gs;
}

static inline GameState* shm_attach() {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return NULL;
    GameState* gs = (GameState*)mmap(NULL, sizeof(GameState),
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, fd, 0);
    close(fd);
    return (gs == MAP_FAILED) ? NULL : gs;
}

static inline void shm_destroy(GameState* gs) {
    munmap(gs, sizeof(GameState));
    shm_unlink(SHM_NAME);
}