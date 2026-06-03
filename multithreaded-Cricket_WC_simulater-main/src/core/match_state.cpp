#include <semaphore.h>
#include <string.h>
#include <algorithm>
#include <stdio.h>
#include "match_state.h"
// Initializes the entire match state before simulation starts.
// Sets default values, resets flags, and initializes all synchronization primitives.
void match_state_init(MatchState* ms) {
    memset(ms, 0, sizeof(*ms));

    ms->runs             = 0;
    ms->wickets          = 0;
    ms->over             = 0;
    ms->ball             = 0;
    ms->total_balls      = 0;
    ms->striker_idx      = 0;
    ms->non_striker_idx  = 1;   
    ms->match_intensity  = 0;
    ms->current_bowler_idx = 0;
    ms->death_over_top_idx = -1;
    ms->death_over_second_idx = -1;
    ms->last_ball_was_wicket = false;
    ms->innings_number = 1;
    ms->target_runs = 0;

    ms->ball_available    = false;
    ms->ball_in_air       = false;
    ms->result_consumed   = true;   
    ms->match_over        = false;
    ms->over_complete     = false;
    ms->next_bowler_ready = false;
    ms->allow_umpire_exit = false;
    ms->free_hit_active   = false;

    pthread_mutex_init(&ms->score_mutex, nullptr);
    pthread_mutex_init(&ms->ball_mutex,  nullptr);
    pthread_mutex_init(&ms->sched_mutex, nullptr);
    pthread_cond_init(&ms->ball_bowled_cv,   nullptr);
    pthread_cond_init(&ms->ball_resolved_cv, nullptr);
    pthread_cond_init(&ms->result_ready_cv,  nullptr);
    pthread_cond_init(&ms->sched_cv,         nullptr);
    pthread_mutex_init(&ms->shutdown_mutex,  nullptr);
    pthread_cond_init(&ms->shutdown_cv,      nullptr);
    
    // NEW: Initialize all zone CVs
    for (int i = 0; i < ZONE_COUNT; ++i) {
        pthread_cond_init(&ms->zone_cv[i], nullptr);
    }

    sem_init(&ms->crease_sem, 0, 2);


    pthread_mutex_init(&ms->deadlock_mutex, nullptr);
    ms->deadlock_total[DL_RES_SCORE_MUTEX] = 1;
    ms->deadlock_total[DL_RES_BALL_MUTEX]  = 1;
    ms->deadlock_total[DL_RES_SCHED_MUTEX] = 1;
    ms->deadlock_total[DL_RES_END_MUTEX_0] = 1;
    ms->deadlock_total[DL_RES_END_MUTEX_1] = 1;
    ms->deadlock_total[DL_RES_CREASE_SEM]  = 2;
    ms->deadlock_total[DL_RES_RESOLVE_MUTEX] = 1;
    ms->deadlock_total[DL_RES_PITCH_MONITOR_MUTEX] = 1;
    ms->deadlock_total[DL_RES_SHUTDOWN_MUTEX] = 1;
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j) {
        ms->deadlock_available[j] = ms->deadlock_total[j];
    }
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i) {
        ms->deadlock_thread_active[i] = false;
        strcpy(ms->deadlock_thread_type[i], "UNREGISTERED");
        ms->deadlock_thread_id[i] = -1;
    }
    strcpy(ms->deadlock_trigger_thread_type, "unknown");
    ms->deadlock_trigger_thread_id = -1;
    ms->deadlock_trigger_resource  = -1;
}
// Cleans up all resources used in match state.
// Destroys mutexes, condition variables, and semaphores.
void match_state_destroy(MatchState* ms) {
    pthread_mutex_destroy(&ms->score_mutex);
    pthread_mutex_destroy(&ms->ball_mutex);
    pthread_mutex_destroy(&ms->sched_mutex);
    pthread_cond_destroy(&ms->ball_bowled_cv);
    pthread_cond_destroy(&ms->ball_resolved_cv);
    pthread_cond_destroy(&ms->result_ready_cv);
    pthread_cond_destroy(&ms->sched_cv);
    pthread_mutex_destroy(&ms->shutdown_mutex);
    pthread_cond_destroy(&ms->shutdown_cv);
    
    // Destroy all zone CVs
    for (int i = 0; i < ZONE_COUNT; ++i) {
        pthread_cond_destroy(&ms->zone_cv[i]);
    }
    
    sem_destroy(&ms->crease_sem);
    pthread_mutex_destroy(&ms->deadlock_mutex);
}
// Signals that the match has ended.
// Wakes up all threads waiting on ball events, scheduler, and fielding zones.
void match_state_signal_end(MatchState* ms) {
    pthread_mutex_lock(&ms->ball_mutex);
    ms->match_over = true;
    pthread_cond_broadcast(&ms->ball_bowled_cv);
    pthread_cond_broadcast(&ms->ball_resolved_cv);
    pthread_cond_broadcast(&ms->result_ready_cv);
    
    // Broadcast to ALL fielders to wake them up for teardown
    for (int i = 0; i < ZONE_COUNT; ++i) {
        pthread_cond_broadcast(&ms->zone_cv[i]);
    }
    pthread_mutex_unlock(&ms->ball_mutex);

    pthread_mutex_lock(&ms->sched_mutex);
    pthread_cond_broadcast(&ms->sched_cv);
    pthread_mutex_unlock(&ms->sched_mutex);
}

bool match_state_target_reached(const MatchState* ms) {
    return ms && ms->target_runs > 0 && ms->runs >= ms->target_runs;
}

bool match_state_finish_chase_if_complete(MatchState* ms) {
    if (!match_state_target_reached(ms)) return false;
    ms->match_over = true;
    return true;
}
// Requests safe termination of the umpire thread.
// Notifies via shutdown condition variable.
void match_state_request_umpire_exit(MatchState* ms) {
    pthread_mutex_lock(&ms->shutdown_mutex);
    ms->allow_umpire_exit = true;
    pthread_cond_broadcast(&ms->shutdown_cv);
    pthread_mutex_unlock(&ms->shutdown_mutex);
}
// Recomputes available resources for deadlock detection.
// Updates available count based on current allocations.
void match_state_deadlock_recompute_available(MatchState* ms) {
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j) {
        int used = 0;
        for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i) {
            used += ms->deadlock_allocation[i][j];
        }
        ms->deadlock_available[j] = std::max(0, ms->deadlock_total[j] - used);
    }
}
// Registers a thread in the deadlock tracking system.
// Stores its type and ID for monitoring.
void match_state_deadlock_register_thread(MatchState* ms, int thread_idx, const char* thread_type, int thread_id) {
    if (thread_idx < 0 || thread_idx >= DEADLOCK_THREAD_COUNT) return;
    pthread_mutex_lock(&ms->deadlock_mutex);
    ms->deadlock_thread_active[thread_idx] = true;
    snprintf(ms->deadlock_thread_type[thread_idx],
             sizeof(ms->deadlock_thread_type[thread_idx]),
             "%s", (thread_type && *thread_type) ? thread_type : "UNKNOWN");
    ms->deadlock_thread_id[thread_idx] = thread_id;
    pthread_mutex_unlock(&ms->deadlock_mutex);
}
// Unregisters a thread from deadlock tracking.
// Clears its resource allocations and requests.
void match_state_deadlock_unregister_thread(MatchState* ms, int thread_idx) {
    if (thread_idx < 0 || thread_idx >= DEADLOCK_THREAD_COUNT) return;
    pthread_mutex_lock(&ms->deadlock_mutex);
    ms->deadlock_thread_active[thread_idx] = false;
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j) {
        ms->deadlock_request[thread_idx][j] = 0;
        ms->deadlock_allocation[thread_idx][j] = 0;
    }
    pthread_mutex_unlock(&ms->deadlock_mutex);
}
// Records a resource request made by a thread.
// Also marks this as the potential deadlock trigger point.
void match_state_deadlock_note_request(MatchState* ms, int thread_idx, int resource_idx, int amount) {
    if (thread_idx < 0 || thread_idx >= DEADLOCK_THREAD_COUNT ||
        resource_idx < 0 || resource_idx >= DEADLOCK_RESOURCE_COUNT ||
        amount <= 0) return;
    pthread_mutex_lock(&ms->deadlock_mutex);
    ms->deadlock_request[thread_idx][resource_idx] = amount;
    snprintf(ms->deadlock_trigger_thread_type, sizeof(ms->deadlock_trigger_thread_type),
             "%s", ms->deadlock_thread_type[thread_idx]);
    ms->deadlock_trigger_thread_id = ms->deadlock_thread_id[thread_idx];
    ms->deadlock_trigger_resource  = resource_idx;
    pthread_mutex_unlock(&ms->deadlock_mutex);
}
// Updates allocation when a resource is granted.
// Reduces pending request and increases allocation count.
void match_state_deadlock_note_grant(MatchState* ms, int thread_idx, int resource_idx, int amount) {
    if (thread_idx < 0 || thread_idx >= DEADLOCK_THREAD_COUNT ||
        resource_idx < 0 || resource_idx >= DEADLOCK_RESOURCE_COUNT ||
        amount <= 0) return;
    pthread_mutex_lock(&ms->deadlock_mutex);
    int& req = ms->deadlock_request[thread_idx][resource_idx];
    req = std::max(0, req - amount);
    ms->deadlock_allocation[thread_idx][resource_idx] += amount;
    pthread_mutex_unlock(&ms->deadlock_mutex);
}
// Updates allocation when a resource is released.
// Frees up the resource for others.
void match_state_deadlock_note_release(MatchState* ms, int thread_idx, int resource_idx, int amount) {
    if (thread_idx < 0 || thread_idx >= DEADLOCK_THREAD_COUNT ||
        resource_idx < 0 || resource_idx >= DEADLOCK_RESOURCE_COUNT ||
        amount <= 0) return;
    pthread_mutex_lock(&ms->deadlock_mutex);
    int& alloc = ms->deadlock_allocation[thread_idx][resource_idx];
    alloc = std::max(0, alloc - amount);
    pthread_mutex_unlock(&ms->deadlock_mutex);
}
// Deadlock-aware wrapper for mutex lock.
// Tracks request and grant around pthread_mutex_lock.
void match_state_deadlock_mutex_lock(MatchState* ms, int thread_idx, int resource_idx, pthread_mutex_t* mutex) {
    match_state_deadlock_note_request(ms, thread_idx, resource_idx, 1);
    pthread_mutex_lock(mutex);
    match_state_deadlock_note_grant(ms, thread_idx, resource_idx, 1);
}
// Deadlock-aware wrapper for mutex unlock.
// Updates tracking before releasing mutex.
void match_state_deadlock_mutex_unlock(MatchState* ms, int thread_idx, int resource_idx, pthread_mutex_t* mutex) {
    match_state_deadlock_note_release(ms, thread_idx, resource_idx, 1);
    pthread_mutex_unlock(mutex);
}
// Deadlock-aware condition wait.
// Releases resource before wait and reacquires after waking.
void match_state_deadlock_cond_wait(MatchState* ms, int thread_idx, int resource_idx, pthread_cond_t* cond, pthread_mutex_t* mutex) {
    match_state_deadlock_note_release(ms, thread_idx, resource_idx, 1);
    pthread_cond_wait(cond, mutex);
    match_state_deadlock_note_grant(ms, thread_idx, resource_idx, 1);
}
// Deadlock-aware semaphore wait.
// Tracks request and allocation for sem_wait.
void match_state_deadlock_sem_wait(MatchState* ms, int thread_idx, int resource_idx, sem_t* sem) {
    match_state_deadlock_note_request(ms, thread_idx, resource_idx, 1);
    sem_wait(sem);
    match_state_deadlock_note_grant(ms, thread_idx, resource_idx, 1);
}
// Deadlock-aware semaphore post.
// Updates tracking before releasing semaphore.
void match_state_deadlock_sem_post(MatchState* ms, int thread_idx, int resource_idx, sem_t* sem) {
    match_state_deadlock_note_release(ms, thread_idx, resource_idx, 1);
    sem_post(sem);
}
