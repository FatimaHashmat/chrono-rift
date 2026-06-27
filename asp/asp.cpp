//=============================================================
//  asp.cpp  —  Chrono Rift  |  CS 2006
//  Automated Strategic Process — enemy AI threads only.
//  Supports multi-wave respawning via asp_wave_ready flag.
//=============================================================

#include "../common.h"
#include <stdio.h>
#include <signal.h>

static GameState* gs = NULL;

//=============================================================
//  SIGNAL HANDLERS
//=============================================================
static void handle_stun(int) {
    static const char msg[] = "[ASP] *** STUNNED ***\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

static void handle_sigterm(int) {
    printf("[ASP] SIGTERM — exiting\n");
    if (gs) munmap(gs, sizeof(GameState));
    exit(0);
}

//=============================================================
//  NPC AI
//=============================================================
static void decide_action(int enemy_id, NpcAction* out) {
    out->enemy_id  = enemy_id;
    out->ready     = 0;
    out->processed = 0;

    int active[MAX_PLAYERS], count = 0;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].active) active[count++] = i;

    if (count == 0) { out->action = ACTION_SKIP; return; }

    if (rand() % 10 == 0) {
        out->action = ACTION_SKIP;
        printf("[ASP] Enemy %d SKIP\n", enemy_id);
        return;
    }

    int target = active[rand() % count];
    for (int i = 0; i < count; i++) {
        PlayerChar* p = &gs->players[active[i]];
        if (p->hp < p->max_hp * 3 / 10) { target = active[i]; break; }
    }

    out->action           = ACTION_ATTACK_STRIKE;
    out->target_player_id = target;
    printf("[ASP] Enemy %d STRIKE Player %d\n", enemy_id, target);
}

//=============================================================
//  ENEMY THREAD
//=============================================================
typedef struct { int enemy_id; } EnemyArg;

static void* enemy_thread(void* arg) {
    int id = ((EnemyArg*)arg)->enemy_id;
    printf("[ASP] Thread for Enemy %d started\n", id);

    while (!gs->game_over) {
        sem_wait(&gs->npc_turn_sem[id]);
        if (gs->game_over) break;
        if (!gs->enemies[id].active) {
            // Enemy is dead — exit this thread so the wave can complete
            printf("[ASP] Enemy %d is dead — thread exiting\n", id);
            break;
        }
        if (gs->whose_turn != id || gs->whose_turn_kind != ENTITY_ENEMY) {
            printf("[ASP] Enemy %d: spurious wakeup\n", id);
            continue;
        }

        usleep(300000);

        NpcAction action;
        memset(&action, 0, sizeof(NpcAction));
        decide_action(id, &action);

        pthread_mutex_lock(&gs->state_mutex);
        gs->npc_action       = action;
        gs->npc_action.ready = 1;
        pthread_mutex_unlock(&gs->state_mutex);

        sem_post(&gs->npc_action_ready_sem);
    }

    printf("[ASP] Enemy %d thread exiting\n", id);
    return NULL;
}

//=============================================================
//  SPAWN ONE WAVE OF THREADS
//  Blocks until all threads in the wave have exited.
//=============================================================
static void run_wave(int wave_count) {
    pthread_t threads[MAX_ENEMIES];
    EnemyArg  args[MAX_ENEMIES];

    printf("[ASP] Starting wave: %d enemy threads\n", wave_count);

    for (int i = 0; i < wave_count; i++) {
        args[i].enemy_id = i;
        pthread_create(&threads[i], NULL, enemy_thread, &args[i]);
    }

    for (int i = 0; i < wave_count; i++)
        pthread_join(threads[i], NULL);

    printf("[ASP] Wave of %d enemies finished\n", wave_count);
}

//=============================================================
//  MAIN
//=============================================================
int main() {
    signal(SIGUSR1,  handle_stun);
    signal(SIGTERM,  handle_sigterm);

    printf("[ASP] Attaching to shared memory...\n");
    int retries = 0;
    while ((gs = shm_attach()) == NULL) {
        usleep(200000);
        if (++retries > 25) {
            fprintf(stderr, "[ASP] Could not attach\n");
            return 1;
        }
    }

    while (gs->shm_ready == 0)
        usleep(50000);

    printf("[ASP] Attached\n");
    gs->asp_pid = getpid();
    printf("[ASP] PID = %d registered\n", gs->asp_pid);

    srand(ROLL_NUMBER + (unsigned)time(NULL) + 1);

    while (gs->game_started == 0 && !gs->game_over)
        usleep(100000);

    // -------------------------------------------------------
    //  Wave loop
    //  Run the first wave immediately (arbiter already set
    //  num_enemies before game_started was raised).
    //  After each wave, wait for arbiter to set asp_wave_ready
    //  before launching the next batch of threads.
    // -------------------------------------------------------
    run_wave(gs->num_enemies);

    while (!gs->game_over) {
        // Wait for arbiter to prepare a new wave
        printf("[ASP] Waiting for next wave...\n");
        while (!gs->game_over && gs->asp_wave_ready == 0)
            usleep(100000);

        if (gs->game_over) break;

        // Consume the flag and start the next wave
        gs->asp_wave_ready = 0;
        run_wave(gs->num_enemies);
    }

    printf("[ASP] Exiting\n");
    munmap(gs, sizeof(GameState));
    return 0;
}