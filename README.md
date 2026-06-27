# Chrono Rift

A multi-process, turn-based tactical battle game built as a course project for **CS 2006 — Operating Systems** at NUCES FAST Islamabad. The game is a hands-on demonstration of core OS concepts including POSIX shared memory, POSIX threads, semaphore-based synchronisation, signal handling, and process scheduling — all implemented from scratch in C++.

**Group Members**
- Fatima Hashmat 
- Haleema Sadia 

---

## Table of Contents

- [Overview](#overview)
- [Architecture](#architecture)
- [OS Concepts Demonstrated](#os-concepts-demonstrated)
- [Gameplay](#gameplay)
- [Multi-Player Support](#multi-player-support)
- [Scheduling Model](#scheduling-model)
- [Project Structure](#project-structure)
- [Building and Running](#building-and-running)
- [Docker Setup](#docker-setup)
- [Report](#report)

---

## Overview

Chrono Rift is a co-operative turn-based battle game where 1–4 human players team up to eliminate waves of enemies. The goal is to defeat a total of 10 enemies across multiple waves without all player characters being wiped out.

The entire system runs as three separate OS processes communicating exclusively through **POSIX shared memory** — no pipes are used anywhere in the implementation, as per the project specification.

---

## Architecture

The system is divided into three independent processes:

**Arbiter (`arbiter/`)**
The central authority of the game. It owns the global `GameState` struct in shared memory, enforces turn order using a stamina-based scheduler, resolves all player and enemy actions, manages wave respawning, handles weapon drops, and runs a background deadlock detection thread for the artifact system.

**HIP — Human Interfacing Process (`hip/`)**
One HIP process is launched per human player. Each HIP renders the full game world (player sprites, enemy sprites, HP/stamina bars, action log) via SFML in real time by reading directly from shared memory. It has separate threads for rendering and input handling, and writes the player's chosen action back to shared memory for the Arbiter to process.

**ASP — Automated Strategic Process (`asp/`)**
A single ASP process manages all enemies. Each enemy entity runs in its own dedicated `pthread`. The ASP supports multi-wave respawning: after a wave is cleared, the Arbiter raises the `asp_wave_ready` flag and the ASP spawns a fresh batch of enemy threads for the next wave.

---

## OS Concepts Demonstrated

| Concept | Implementation |
|---|---|
| **Shared Memory (IPC)** | All game state held in a single POSIX shm segment (`/chrono_rift_shm`), accessible by all three processes |
| **Unnamed Semaphores** | Per-player and per-enemy semaphores coordinate turn signalling without pipes |
| **POSIX Threads** | Each enemy NPC runs its own thread in ASP; HIP has separate render and input threads |
| **Process-Shared Mutexes** | `state_mutex`, `artifact_mutex`, and `action_log.mutex` use `PTHREAD_PROCESS_SHARED` |
| **Signal Handling** | `SIGTERM` triggers graceful shutdown; `SIGUSR1` delivers stun notification; `SIGSTOP`/`SIGCONT` pause ASP during Ultimate moves |
| **Temporal Scheduling** | Stamina-based scheduler with normalised fraction comparison for fairness across entity types |
| **Deadlock Detection** | Background thread in Arbiter monitors circular waits on artifacts and forces release |
| **Wave Management** | Arbiter respawns enemy batches via `asp_wave_ready` flag; ASP polls and re-spawns threads accordingly |

---

## Gameplay

**Actions available each turn:**
- **Strike** — Standard attack dealing base damage
- **Exhaust** — Heavy attack; costs stamina
- **Use Weapon** — Attack with an equipped weapon for bonus damage
- **Swap In** — Equip a weapon from long-term storage into the active inventory
- **Heal** — Recover HP at the cost of stamina
- **Skip** — Pass the turn

**Weapons:** Eight weapon types exist (Solar Core, Lunar Blade, Iron Halberd, Venom Dagger, Thunderstaff, Obsidian Axe, Frostbow, Splinter Stick), each with unique damage values and inventory slot costs.

**Artifacts:** Three special artifacts — Solar Core, Lunar Blade, and Eclipse Relic — can be held by entities. The Arbiter's deadlock detection prevents circular artifact waiting.

**Win / Loss condition:**
- **Win:** Eliminate all 10 enemies across all waves
- **Loss:** All player characters reach 0 HP

---

## Multi-Player Support

Chrono Rift supports **1 to 4 human players simultaneously**, each running as their own HIP process with their own SFML window.

To play with multiple players, simply launch one `./build/hip` process per player in separate terminals (or separate machines sharing the same host). Each HIP automatically registers itself with the Arbiter via shared memory and is assigned a player slot (Player 0, Player 1, etc.). The game does not start until all expected players have connected.

Players act in turn order determined by the stamina scheduler. When it is your turn, your HIP window becomes interactive and prompts you to choose an action. All other players' windows continue to display the live game state in real time while they wait.

> **Note:** All HIP processes must be launched on the same host (or inside the same Docker container) since shared memory is host-local.

---

## Scheduling Model

Chrono Rift uses a **stamina-based temporal scheduling model**. Every entity accumulates stamina each tick at a rate equal to its speed attribute. An entity becomes eligible to act when its stamina reaches the maximum threshold. After acting, its stamina resets to zero.

To ensure fairness between players (max stamina 100) and enemies (max stamina 150), the scheduler compares **normalised stamina fractions** rather than raw values:

```
fraction = current_stamina / max_stamina
```

This means a fully charged player (fraction = 1.0) is always selected over a partially charged enemy (fraction = 0.87), regardless of the raw stamina difference.

**Representative turnaround times:**

| Entity | Max Stamina | Speed | First Turn |
|---|---|---|---|
| Player (1-player party) | 100 | 100 | 1.0 sec |
| Player (2-player party) | 100 | 50 | 2.0 sec |
| Player (4-player party) | 100 | 25 | 4.0 sec |
| Enemy (fast, speed=30) | 150 | 30 | 5.0 sec |
| Enemy (mid, speed=20) | 150 | 20 | 7.5 sec |
| Enemy (slow, speed=10) | 150 | 10 | 15.0 sec |

Player stamina is **preserved across waves**, so a player who had accumulated stamina before a wave ends carries it into the next wave.

---

## Project Structure

```
chrono-rift/
├── common.h                  # Shared structs, enums, SHM helpers (used by all processes)
├── Makefile                  # Build all three binaries into build/
├── Dockerfile                # Ubuntu 22.04 + SFML environment
├── requirements.txt          # Extra apt packages (if any)
├── arbiter/
│   └── arbiter.cpp           # Central game authority process
├── hip/
│   └── hip.cpp               # Human player interface process (one per player)
├── asp/
│   └── asp.cpp               # Enemy AI process (one thread per enemy)
├── OS_Sprites/               # Sprite assets used by HIP for rendering
└── 24I-0526_FatimaHashmat_24I-0654_HaleemaSadia_OSProjectReport.pdf
```

---

## Building and Running

### Prerequisites

- Ubuntu 22.04 (or use the provided Docker container)
- `g++` with C++17 support
- SFML (`libsfml-dev`)

### Build

```bash
make
```

Compiled binaries are placed in `build/`.

### Run

Open separate terminals for each process. **The Arbiter must be started first.**

**Terminal 1 — Arbiter:**
```bash
./build/arbiter
```

**Terminal 2 — ASP (enemy AI):**
```bash
./build/asp
```

**Terminal 3 — HIP for Player 0:**
```bash
./build/hip
```

**For a 2-player game, open a 4th terminal — HIP for Player 1:**
```bash
./build/hip
```

Each additional `./build/hip` you launch adds another human player (up to 4 total). Launch all HIP processes before the game begins.

### Clean

```bash
make clean
```

---

## Docker Setup

A `Dockerfile` is provided for a reproducible build environment (Ubuntu 22.04 with SFML and build tools pre-installed).

**Build the image:**
```bash
docker build -t chrono-rift .
```

**Run the container** (with X11 forwarding for the SFML windows):
```bash
docker run -it \
  -e DISPLAY=$DISPLAY \
  -v /tmp/.X11-unix:/tmp/.X11-unix \
  -v $(pwd):/app \
  chrono-rift
```

Inside the container:
```bash
make
# then run arbiter, asp, and one or more hip processes in separate terminals
```

---

## Report

The full project report (`24I-0526_FatimaHashmat_24I-0654_HaleemaSadia_OSProjectReport.pdf`) includes:
- Detailed architecture explanation
- Turnaround time analysis with worked examples
- Gameplay screenshots with OS concept annotations
- Complete OS concepts summary table
