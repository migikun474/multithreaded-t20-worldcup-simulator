#pragma once
#include <pthread.h>
#include <string>
#include "player.h"   // FieldZone

typedef enum {
    EVT_BALL_BOWLED = 0,
    EVT_NO_BALL,
    EVT_WIDE,
    EVT_BALL_IN_AIR,
    EVT_RUNS_SCORED,
    EVT_CATCH_OUT,
    EVT_RUN_OUT,
    EVT_FOUR,
    EVT_SIX,
    EVT_BATSMAN_OUT,
    EVT_OVER_COMPLETE,
    EVT_MATCH_OVER,
    EVT_DEADLOCK_DETECTED,
    EVT_LOG_MESSAGE,      
    EVT_COMMENTARY,         // commentary line (commentator → logger)
    EVT_OVERTHROW,
    EVT_FREE_HIT           // free hit delivery
} EventType;

inline const char* event_type_name(EventType t) {
    switch (t) {
        case EVT_BALL_BOWLED:       return "BALL_BOWLED";
        case EVT_NO_BALL:           return "NO_BALL";
        case EVT_WIDE:              return "WIDE";
        case EVT_BALL_IN_AIR:       return "BALL_IN_AIR";
        case EVT_RUNS_SCORED:       return "RUNS_SCORED";
        case EVT_CATCH_OUT:         return "CATCH_OUT";
        case EVT_RUN_OUT:           return "RUN_OUT";
        case EVT_FOUR:              return "FOUR";
        case EVT_SIX:               return "SIX";
        case EVT_BATSMAN_OUT:       return "BATSMAN_OUT";
        case EVT_OVER_COMPLETE:     return "OVER_COMPLETE";
        case EVT_MATCH_OVER:        return "MATCH_OVER";
        case EVT_DEADLOCK_DETECTED: return "DEADLOCK_DETECTED";
        case EVT_LOG_MESSAGE:       return "LOG";
        case EVT_COMMENTARY:        return "COMMENTARY";
        case EVT_OVERTHROW:         return "OVERTHROW";
        case EVT_FREE_HIT:          return "FREE_HIT";
    }
    return "UNKNOWN";
}

typedef struct {
    EventType   type;
    int         bowler_id;
    int         batsman_id;
    int         fielder_id = -1;   // -1 if not applicable
    int         runs;
    int         over;
    int         ball;
    int         total_balls;       
    FieldZone   ball_zone;    // zone the ball travelled to (aerial shots)
    std::string message;      
    long        timestamp_us;
} Event;

#define EVENT_QUEUE_CAPACITY 256

typedef struct {
    Event           items[EVENT_QUEUE_CAPACITY];
    int             head, tail, count;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
    bool            shutdown;
} EventQueue;

void  event_queue_init(EventQueue* q);
void  event_queue_destroy(EventQueue* q);
void  event_queue_push(EventQueue* q, const Event& e);
Event event_queue_pop(EventQueue* q);
void  event_queue_shutdown(EventQueue* q);