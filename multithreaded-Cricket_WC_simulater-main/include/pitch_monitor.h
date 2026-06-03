#pragma once
#include <pthread.h>
#include <stdbool.h>

typedef struct {
    pthread_mutex_t mutex;
    int    current_bowler_id;  // -1 = free
    bool   locked;
} PitchMonitor;

void pitch_monitor_init(PitchMonitor* pm);
void pitch_monitor_destroy(PitchMonitor* pm);
void pitch_monitor_acquire(PitchMonitor* pm, int bowler_id);
void pitch_monitor_release(PitchMonitor* pm);
bool pitch_monitor_is_locked(PitchMonitor* pm);