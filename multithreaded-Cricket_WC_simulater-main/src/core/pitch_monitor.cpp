// Controls exclusive access to the pitch for bowlers.
// Ensures only one bowler can use the pitch at a time and integrates with deadlock tracking.
#include <stdio.h>
#include "pitch_monitor.h"
#include "match_state.h"

extern MatchState g_match;

// Initializes the pitch monitor.
// Sets initial state so no bowler is holding the pitch.
void pitch_monitor_init(PitchMonitor* pm) {
    pthread_mutex_init(&pm->mutex, nullptr);
    pm->current_bowler_id = -1;
    pm->locked = false;
}
// Cleans up resources used by pitch monitor.
void pitch_monitor_destroy(PitchMonitor* pm) {
    pthread_mutex_destroy(&pm->mutex);
}
// Allows a bowler to acquire the pitch.
// Uses deadlock-aware locking to safely obtain mutex.
void pitch_monitor_acquire(PitchMonitor* pm, int bowler_id) {
    match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_PITCH_MONITOR_MUTEX, &pm->mutex);
    pm->current_bowler_id = bowler_id;
    pm->locked = true;
    // Intentionally held — released by pitch_monitor_release()
}
// Releases the pitch after the bowler finishes.
// Makes pitch available for next bowler.
void pitch_monitor_release(PitchMonitor* pm) {
    pm->current_bowler_id = -1;
    pm->locked = false;
    match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_PITCH_MONITOR_MUTEX, &pm->mutex);
}
// Checks whether the pitch is currently occupied by any bowler.
bool pitch_monitor_is_locked(PitchMonitor* pm) {
    return pm->locked;
}