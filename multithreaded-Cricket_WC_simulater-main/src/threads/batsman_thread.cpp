#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <cstdlib>
#include <string>
#include "match_state.h"
#include "player.h"
#include "event.h"
#include "scoreboard.h"
#include "scheduler.h"

extern MatchState  g_match;
extern EventQueue  g_event_queue;
extern EventQueue  g_commentary_queue;
extern Player      g_batsmen[];
extern Player      g_bowlers[];
extern Player      g_fielders[];
extern int         g_crease_counter;  // next batting position to assign
static constexpr int TOTAL_FIELDERS = 10;

typedef enum {
    BALL_DOT = 0,
    BALL_GROUNDED,
    BALL_WELL_TIMED,
    BALL_AERIAL,
    BALL_BOWLED,
    BALL_WIDE,
    BALL_NO_BALL,
    BALL_LBW,
    BALL_STUMPED
} BallOutcome;


//* Helper push event function, pushes to global event queue
static void push_event(EventType t, int bat_id, int runs, const std::string& msg) {
    Event e{};
    e.type       = t;
    e.bowler_id  = g_bowlers[g_match.current_bowler_idx % 5].id;
    e.batsman_id = bat_id;
    e.runs       = runs;
    e.over       = g_match.over;
    e.ball       = g_match.ball;
    e.total_balls = g_match.total_balls;
    e.message    = msg;
    event_queue_push(&g_event_queue, e);
}

//* Helper push event function, pushes to commentary event queue
static void push_commentary_event(EventType t, int bat_id, int fielder_id,
                                  int runs, FieldZone zone, const std::string& msg) {
    Event e{};
    e.type       = t;
    e.bowler_id  = g_bowlers[g_match.current_bowler_idx % 5].id;
    e.batsman_id = bat_id;
    e.fielder_id = fielder_id;
    e.runs       = runs;
    e.over       = g_match.over;
    e.ball       = g_match.ball;
    e.total_balls = g_match.total_balls;
    e.ball_zone  = zone;
    e.message    = msg;
    event_queue_push(&g_commentary_queue, e);
}


//* Helper function to populate ariel shot info for commentary, uniform random distribution to select
static void aerial_shot_info(const Player* bat, FieldZone& out_zone, std::string& out_desc) {
    struct { FieldZone zone; const char* desc; } shots[] = {
        { ZONE_FINE_LEG,   " glances towards fine leg" },
        { ZONE_SQUARE_LEG, " flicks towards square leg" },
        { ZONE_MIDWICKET,  " drives towards midwicket" },
        { ZONE_MID_ON,     " pushes towards mid-on" },
        { ZONE_MID_OFF,    " drives towards mid-off" },
        { ZONE_COVER,      " plays through cover" },
        { ZONE_POINT,      " cuts towards point" },
        { ZONE_THIRD_MAN,  " guides towards third man" },
    };

    float pi = bat->power_index;
    int w[8] = {10,10,10,10,10,10,10,10};

    if (pi >= 8.0f) {
        w[ZONE_MIDWICKET] = 25; w[ZONE_MID_ON] = 20; w[ZONE_FINE_LEG] = 15;
    } else if (pi >= 6.0f) {
        w[ZONE_MIDWICKET] = 18; w[ZONE_MID_ON] = 15; w[ZONE_COVER] = 13;
    } else {
        w[ZONE_MID_OFF] = 18; w[ZONE_COVER] = 18;
        w[ZONE_POINT] = 15;   w[ZONE_THIRD_MAN] = 15;
    }

    int total = 0;
    for (int i = 0; i < 8; ++i) total += w[i];
    int r = rand() % total, cum = 0, chosen = 0;
    for (int i = 0; i < 8; ++i) {
        cum += w[i];
        if (r < cum) { chosen = i; break; }
    }
    out_zone = shots[chosen].zone;
    out_desc = bat->name + shots[chosen].desc;
}

//* Same as above, just for case of dot ball
static std::string dot_desc(const std::string& name) {
    static const char* d[] = {
        " blocks solidly — dot ball",
        " leaves it alone — dot ball",
        " defended well — no run",
        " beaten! — dot ball"
    };
    return name + d[rand() % 4];
}

//* Helper to display info of current over
static void display_over(int& d_over, int& d_ball) {
    int tb = g_match.total_balls;
    d_over = (tb - 1) / 6;
    d_ball = (tb - 1) % 6 + 1;
    if (tb == 0) { d_over = 0; d_ball = 0; return; }
}


void* batsman_thread_fn(void* arg) {


    //* Initialize and register thread to match state so that umpire tracks it for deadlocks. Then take control of pitch.

    int thread_slot = *(int*)arg;
    int deadlock_thread = (thread_slot == 0) ? DL_THREAD_BATSMAN_0 : DL_THREAD_BATSMAN_1;
    match_state_deadlock_register_thread(&g_match, deadlock_thread, "BATSMAN", thread_slot);
    match_state_deadlock_sem_wait(&g_match, deadlock_thread, DL_RES_CREASE_SEM, &g_match.crease_sem);

    while (!g_match.match_over) {

        //* Wait for ball mutex to be available and then proceed only if match is not over and ball mutex is available

        match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        while (!g_match.ball_available && !g_match.match_over)
            match_state_deadlock_cond_wait(&g_match, deadlock_thread, DL_RES_BALL_MUTEX,
                                           &g_match.ball_bowled_cv, &g_match.ball_mutex);


        //* If match is over, then release mutex and break loop so that batsman thread can exit cleanly
        if (g_match.match_over) {
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            break;
        }

        //* if batsman isn't striker than sleep release and go to sleep so that it doesn't starve striker
        if (thread_slot != 0) {
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            usleep(20000); 
            continue;
        }

        
        
        g_match.ball_available = false;
        BallOutcome outcome     = (BallOutcome)g_match.last_event_type;
        bool is_free_hit        = g_match.free_hit_active;
        g_match.free_hit_active = false;
        match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);


        /* ============================================================================================
         * The next part of the code performs ball resolution
         * 
         * 1. STATE UPDATE: Increments striker metrics and initializes event descriptors.
         * 2. DISPATCH LOGIC: 
         *      - DOT: Resets ball velocity/runs and logs a neutral event
         *      - IN-PLAY (Grounded/Aerial): It calculates the target FieldZone and utilizes pthread_cond_broadcast to trigger fielder threads in the immediate and adjacent sectors.
         *      - SHORT-CIRCUIT: If AERIAL is hit to an zone wher there are no fielders outside the 30 yard zone in the first 6 overs (powerplay), directly decide outcome as 4 or 6, else wake fielders
         *      - DISMISSALS: lbw, bowled and stumped, checks if ball is free hit, and if not and outcome event is this, doesn't dismiss batsman.
         * ============================================================================================ */
        
        Player* striker = &g_batsmen[g_match.striker_idx];
        striker->balls_faced++;

        std::string desc;
        bool wicket       = false;
        bool sent_to_field = false;

        /* ── Outcome switch (identical to original) ─────────────────────── */
        switch (outcome) {

            case BALL_DOT:
                g_match.last_runs = 0;
                desc = dot_desc(striker->name);
                push_event(EVT_RUNS_SCORED, striker->id, 0, desc);
                push_commentary_event(EVT_RUNS_SCORED, striker->id, -1, 0, ZONE_MID_ON, desc);
                break;

            case BALL_GROUNDED:
            case BALL_WELL_TIMED:
            case BALL_AERIAL: {
                sent_to_field = true;
                FieldZone zone;
                aerial_shot_info(striker, zone, desc);
                if      (outcome == BALL_AERIAL)     desc += " — in the air!";
                else if (outcome == BALL_WELL_TIMED) desc += " — struck well!";
                else                                  desc += " — along the ground";

                match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
                g_match.ball_zone   = zone;
                int z = zone;
                int left  = (z - 1 + ZONE_COUNT) % ZONE_COUNT;
                int right = (z + 1) % ZONE_COUNT;

                bool has_outside_fielder = false;
                for (int i = 0; i < TOTAL_FIELDERS; ++i) {
                    if (!g_fielders[i].in30YardZone &&
                        zones_are_adjacent(g_fielders[i].field_zone, zone)) {
                        has_outside_fielder = true;
                        break;
                    }
                }

                if (outcome == BALL_AERIAL && !has_outside_fielder) {
                    g_match.ball_in_air = false;
                } else {
                    g_match.ball_in_air = true;
                    pthread_cond_broadcast(&g_match.zone_cv[z]);
                    pthread_cond_broadcast(&g_match.zone_cv[left]);
                    pthread_cond_broadcast(&g_match.zone_cv[right]);
                }
                match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);

                if (outcome == BALL_AERIAL && !has_outside_fielder) {
                    sent_to_field = false;
                    int boundary_runs = (rand() % 100 < 60) ? 4 : 6;
                    EventType boundary_evt = (boundary_runs == 6) ? EVT_SIX : EVT_FOUR;

                    if (boundary_runs == 6) {
                        desc += " — no deep fielder nearby outside the 30-yard circle, SIX!";
                        striker->sixes++;
                    } else {
                        desc += " — no deep fielder nearby outside the 30-yard circle, FOUR!";
                        striker->fours++;
                    }
                    striker->runs_scored += boundary_runs;

                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    g_match.runs += boundary_runs;
                    g_match.last_runs = boundary_runs;
                    match_state_finish_chase_if_complete(&g_match);
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

                    push_event(boundary_evt, striker->id, boundary_runs, desc);
                    push_commentary_event(boundary_evt, striker->id, -1, boundary_runs, zone, desc);
                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
                    g_match.result_consumed = true;
                    pthread_cond_signal(&g_match.result_ready_cv);
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
                } else {
                    push_event(EVT_BALL_IN_AIR, striker->id, 0, desc);
                    push_commentary_event(EVT_BALL_IN_AIR, striker->id, -1, 0, zone, desc);
                }
                break;
            }

            case BALL_BOWLED:
                if (is_free_hit) {
                    g_match.last_runs = 0;
                    desc = striker->name + " — hits the stumps but it's a FREE HIT! Not out!";
                    push_event(EVT_RUNS_SCORED, striker->id, 0, desc);
                    push_commentary_event(EVT_RUNS_SCORED, striker->id, -1, 0, ZONE_MID_ON, desc);
                } else {
                    wicket = true;
                    striker->is_out = true;
                    striker->out_type = "bowled";
                    g_match.last_runs = 0;
                    g_match.last_ball_was_wicket = true;
                    desc = striker->name + " is BOWLED!";
                    push_event(EVT_BATSMAN_OUT, striker->id, 0, desc);
                    push_commentary_event(EVT_BATSMAN_OUT, striker->id, -1, 0, ZONE_MID_ON, desc);
                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    g_match.wickets++;
                    g_bowlers[g_match.current_bowler_idx % 5].wickets_taken++;
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                }
                break;

            case BALL_LBW:
                if (is_free_hit) {
                    g_match.last_runs = 0;
                    desc = striker->name + " — LBW appeal but it's a FREE HIT! Not out!";
                    push_event(EVT_RUNS_SCORED, striker->id, 0, desc);
                    push_commentary_event(EVT_RUNS_SCORED, striker->id, -1, 0, ZONE_MID_ON, desc);
                } else {
                    wicket = true;
                    striker->is_out = true;
                    striker->out_type = "lbw";
                    g_match.last_runs = 0;
                    g_match.last_ball_was_wicket = true;
                    desc = striker->name + " LBW!";
                    push_event(EVT_BATSMAN_OUT, striker->id, 0, desc);
                    push_commentary_event(EVT_BATSMAN_OUT, striker->id, -1, 0, ZONE_MID_ON, desc);
                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    g_match.wickets++;
                    g_bowlers[g_match.current_bowler_idx % 5].wickets_taken++;
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                }
                break;

            case BALL_STUMPED:
                if (is_free_hit) {
                    g_match.last_runs = 0;
                    desc = striker->name + " — stumping attempt but it's a FREE HIT! Not out!";
                    push_event(EVT_RUNS_SCORED, striker->id, 0, desc);
                    push_commentary_event(EVT_RUNS_SCORED, striker->id, -1, 0, ZONE_MID_ON, desc);
                } else {
                    wicket = true;
                    striker->is_out = true;
                    striker->out_type = "stumped";
                    g_match.last_runs = 0;
                    g_match.last_ball_was_wicket = true;
                    desc = striker->name + " STUMPED!";
                    push_event(EVT_BATSMAN_OUT, striker->id, 0, desc);
                    push_commentary_event(EVT_BATSMAN_OUT, striker->id, -1, 0, ZONE_MID_ON, desc);
                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    g_match.wickets++;
                    g_bowlers[g_match.current_bowler_idx % 5].wickets_taken++;
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                }
                break;

            default:
                g_match.last_runs = 0;
                break;
        }

        if (wicket) {
            match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

            /*
             * Ask the scheduler who comes in next.
             * For SJF  : scans the full pavilion by burst time.
             * For FCFS : returns first uncalled, not-out slot in order.
             * Returns -1 if no one is left (all out).
             */
            int idx = scheduler_next_batsman(
                g_match.scheduler_algo, g_batsmen, 11, &g_match);

            bool can_replace = (idx >= 0) && (g_match.wickets < 10);

            if (can_replace) {
                /*
                 * Mark this slot as "at the crease" so the next wicket's SJF
                 * scan doesn't re-select the same player.
                 * This is the ONLY place called_up is set to true after init.
                 */
                g_batsmen[idx].called_up = true;

                /*
                 * Record the actual batting position (order to crease) for
                 * this player, then increment the counter for the next one.
                 * g_crease_counter starts at 3 (openers are 1 & 2).
                 */
                g_batsmen[idx].crease_order = g_crease_counter++;

                // * Incoming batsman takes the striker's end (the dismissed
                // * batsman's crease).  Non-striker stays unchanged.
                g_match.striker_idx = idx;

                printf("  [CREASE] %s walks in at No.%d  [%d/%d]  [algo: %s]\n",
                       g_batsmen[idx].name.c_str(),
                       g_batsmen[idx].crease_order,
                       g_match.runs,
                       g_match.wickets,
                       scheduler_algo_name(g_match.scheduler_algo));
            }

            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

            if (!can_replace) {
                //* all out — end innings
                g_match.match_over = true;
            } else {
                //* Release crease semaphore so the new batsman's thread slot can pick up; then re-acquire for this slot.
                match_state_deadlock_sem_post(&g_match, deadlock_thread, DL_RES_CREASE_SEM, &g_match.crease_sem);
                match_state_deadlock_sem_wait(&g_match, deadlock_thread, DL_RES_CREASE_SEM, &g_match.crease_sem);
            }
        }

        //* Print only if not handled by fielder
        if (!sent_to_field) {
            int d_over, d_ball;
            display_over(d_over, d_ball);

            const char* badge = "";
            if (wicket) {
                switch (outcome) {
                    case BALL_BOWLED:  badge = "BWL"; break;
                    case BALL_LBW:     badge = "LBW"; break;
                    case BALL_STUMPED: badge = "ST";  break;
                    default:           badge = "W";   break;
                }
            }

            scoreboard_print_event(
                d_over, d_ball,
                g_match.runs, g_match.wickets,
                badge, desc.c_str()
            );

            if (wicket)
                printf("  *** WICKET *** [%d/%d]\n", g_match.runs, g_match.wickets);

            fflush(stdout);

            match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            g_match.result_consumed = true;
            pthread_cond_signal(&g_match.result_ready_cv);
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        }

        // usleep(60000);
    }

    match_state_deadlock_sem_post(&g_match, deadlock_thread, DL_RES_CREASE_SEM, &g_match.crease_sem);
    match_state_deadlock_unregister_thread(&g_match, deadlock_thread);
    printf("[BATSMAN slot=%d] Thread exiting.\n", thread_slot);
    return nullptr;
}
