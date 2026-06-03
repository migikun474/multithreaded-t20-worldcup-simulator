#pragma once
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include "player.h"   // FieldZone

typedef enum { ALGO_FCFS = 0, ALGO_SJF, ALGO_PRIORITY } SchedulerAlgo;

inline const char* scheduler_algo_name(SchedulerAlgo a) {
    switch (a) {
        case ALGO_FCFS:       return "Round Robin (Phase Quota)";
        case ALGO_SJF:      return "Shortest Job First";
        case ALGO_PRIORITY: return "Priority";
    }
    return "Unknown";
}



enum {
    DEADLOCK_THREAD_COUNT   = 18, // 1 bowler + 2 batsmen + 10 fielders + 1 umpire + 1 scheduler + 1 logger + 1 commentator + 1 main
    DEADLOCK_RESOURCE_COUNT = 9
};

typedef enum {
    DL_RES_SCORE_MUTEX = 0,
    DL_RES_BALL_MUTEX  = 1,
    DL_RES_SCHED_MUTEX = 2,
    DL_RES_END_MUTEX_0 = 3,
    DL_RES_END_MUTEX_1 = 4,
    DL_RES_CREASE_SEM  = 5,
    DL_RES_RESOLVE_MUTEX = 6,
    DL_RES_PITCH_MONITOR_MUTEX = 7,
    DL_RES_SHUTDOWN_MUTEX = 8
} DeadlockResource;

typedef enum {
    DL_THREAD_BOWLER = 0,
    DL_THREAD_BATSMAN_0 = 1,
    DL_THREAD_BATSMAN_1 = 2,
    DL_THREAD_FIELDER_0 = 3,
    DL_THREAD_FIELDER_9 = DL_THREAD_FIELDER_0 + 9,
    DL_THREAD_UMPIRE = 13,
    DL_THREAD_SCHEDULER = 14,
    DL_THREAD_LOGGER = 15,
    DL_THREAD_COMMENTATOR = 16,
    DL_THREAD_MAIN = 17
} DeadlockThread;

typedef struct {
    // Scoreboard fields (protected by score_mutex)
    int  runs;
    int  wickets;
    int  over;
    int  ball;
    int  total_balls;
    int  striker_idx;
    int  non_striker_idx;

    // Synchronisation primitives
    pthread_mutex_t    score_mutex;
    pthread_mutex_t    ball_mutex;
    pthread_cond_t     ball_bowled_cv;
    pthread_cond_t     zone_cv[ZONE_COUNT]; 
    pthread_cond_t     result_ready_cv;
    pthread_cond_t     ball_resolved_cv;    // Kept for race losers

    // Ball-state flags
    bool ball_available;
    bool ball_in_air;
    bool result_consumed;
    bool match_over;
    bool free_hit_active;   // true when current delivery is a free hit
    int  innings_number;
    int  target_runs;       // 0 in first innings; chase ends when runs >= target


    // Deadlock detector state (Request/Allocation/Available model)
    int deadlock_total[DEADLOCK_RESOURCE_COUNT];
    int deadlock_available[DEADLOCK_RESOURCE_COUNT];
    int deadlock_allocation[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT];
    int deadlock_request[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT];
    bool deadlock_thread_active[DEADLOCK_THREAD_COUNT];
    char deadlock_thread_type[DEADLOCK_THREAD_COUNT][16];
    int  deadlock_thread_id[DEADLOCK_THREAD_COUNT];
    char deadlock_trigger_thread_type[16];
    int  deadlock_trigger_thread_id;
    int  deadlock_trigger_resource;
    pthread_mutex_t deadlock_mutex;

    // Last delivery outcome
    int  last_runs;
    int  last_event_type;
    bool last_ball_was_wicket;

    // Crease semaphore (capacity = 2)
    sem_t crease_sem;

    // Scheduler control
    SchedulerAlgo scheduler_algo;
    bool over_complete;
    bool next_bowler_ready;
    bool demo_deadlock_mode;
    pthread_mutex_t sched_mutex;
    pthread_cond_t  sched_cv;
    pthread_mutex_t shutdown_mutex;
    pthread_cond_t  shutdown_cv;
    bool allow_umpire_exit;

    // Match intensity (0-10)
    int match_intensity;

    // Current bowler index
    int current_bowler_idx;
    int death_over_top_idx;
    int death_over_second_idx;

    // Zone of current aerial ball
    FieldZone ball_zone;
} MatchState;

void match_state_init(MatchState* ms);
void match_state_destroy(MatchState* ms);
void match_state_signal_end(MatchState* ms);
bool match_state_target_reached(const MatchState* ms);
bool match_state_finish_chase_if_complete(MatchState* ms);
// Returns true if the match is over (all overs completed or all out)
void match_state_request_umpire_exit(MatchState* ms);
void match_state_deadlock_register_thread(MatchState* ms, int thread_idx, const char* thread_type, int thread_id);
void match_state_deadlock_unregister_thread(MatchState* ms, int thread_idx);
void match_state_deadlock_note_request(MatchState* ms, int thread_idx, int resource_idx, int amount);
void match_state_deadlock_note_grant(MatchState* ms, int thread_idx, int resource_idx, int amount);
void match_state_deadlock_note_release(MatchState* ms, int thread_idx, int resource_idx, int amount);
// Note: For mutexes, amount is always 1; for semaphores, it can be >1
void match_state_deadlock_recompute_available(MatchState* ms);
void match_state_deadlock_mutex_lock(MatchState* ms, int thread_idx, int resource_idx, pthread_mutex_t* mutex);
void match_state_deadlock_mutex_unlock(MatchState* ms, int thread_idx, int resource_idx, pthread_mutex_t* mutex);
void match_state_deadlock_cond_wait(MatchState* ms, int thread_idx, int resource_idx, pthread_cond_t* cond, pthread_mutex_t* mutex);
// Note: For cond_wait, the resource_idx is the resource that the thread is waiting for (e.g. ball_mutex for umpire waiting for ball_bowled_cv)
void match_state_deadlock_sem_wait(MatchState* ms, int thread_idx, int resource_idx, sem_t* sem);
void match_state_deadlock_sem_post(MatchState* ms, int thread_idx, int resource_idx, sem_t* sem);
