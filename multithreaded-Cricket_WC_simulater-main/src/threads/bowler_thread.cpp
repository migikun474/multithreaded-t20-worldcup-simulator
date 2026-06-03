#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <string>
#include "match_state.h"
#include "pitch_monitor.h"
#include "player.h"
#include "event.h"
#include "scoreboard.h"

extern MatchState   g_match;
extern PitchMonitor g_pitch;
extern EventQueue   g_event_queue;
extern EventQueue   g_commentary_queue;
extern Player       g_bowlers[];
extern Player       g_batsmen[];
extern Player       g_fielders[];
static constexpr int POWERPLAY_OUTSIDE_FIELDER_A = 0;
static constexpr int POWERPLAY_OUTSIDE_FIELDER_B = 8;

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

extern BallOutcome ball_generate_outcome(int intensity, const Player* batsman);
extern void scoreboard_print_over_summary(int over, int over_runs);

#define TOTAL_OVERS    20
#define BALLS_PER_OVER  6

//* Helper push event function, pushes to global event queue
static void push_event(EventType t, int bowler_id, int bat_id,
                       int runs, const std::string& msg) {
    Event e{};
    e.type        = t;
    e.bowler_id   = bowler_id;
    e.batsman_id  = bat_id;
    e.runs        = runs;
    e.over        = g_match.over;
    e.ball        = g_match.ball;
    e.total_balls = g_match.total_balls;
    e.message     = msg;
    event_queue_push(&g_event_queue, e);
}

//* Helper push event function, pushes to global commentary queue
static void push_commentary_event(EventType t, int bowler_id, int bat_id,
                                   int runs, const std::string& msg) {
    Event e{};
    e.type        = t;
    e.bowler_id   = bowler_id;
    e.batsman_id  = bat_id;
    e.runs        = runs;
    e.over        = g_match.over;
    e.ball        = g_match.ball;
    e.total_balls = g_match.total_balls;
    e.message     = msg;
    event_queue_push(&g_commentary_queue, e);
}

void* bowler_thread_fn(void* /*arg*/) {

    //* Initialize and register thread to match state so that umpire tracks it for deadlocks. Then perform the over


    match_state_deadlock_register_thread(&g_match, DL_THREAD_BOWLER, "BOWLER", 0);
    printf("[BOWLER] Thread started. India batting, England bowling.\n");

    for (int over_num = 0; over_num < TOTAL_OVERS; ++over_num) {

        if (g_match.match_over) break;
        if (g_match.wickets >= 10) { g_match.match_over = true; break; }

        Player* bowler  = &g_bowlers[g_match.current_bowler_idx % 5];
        Player* striker = &g_batsmen[g_match.striker_idx];

        int legal_balls = 0;
        int over_runs   = 0;

        printf("\n  ═══ Over %d — %s bowling (%s) ═══════════════════════\n",
               over_num+1,
               bowler->name.c_str(),
               (bowler->id % 3 == 2) ? "Spin" : "Pace");

        while (legal_balls < BALLS_PER_OVER) {

            if (g_match.match_over) break;
            if (g_match.wickets >= 10) { g_match.match_over = true; break; }

            pitch_monitor_acquire(&g_pitch, bowler->id);

            // Refresh striker pointer (may have changed after a wicket)
            striker = &g_batsmen[g_match.striker_idx];

            BallOutcome outcome = ball_generate_outcome(
                g_match.match_intensity, striker);

            /* ── WIDE ─────────────────────────────────────────────────── */
            if (outcome == BALL_WIDE) {
                match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                g_match.runs++;
                match_state_finish_chase_if_complete(&g_match);
                int snap_runs    = g_match.runs;
                int snap_wickets = g_match.wickets;
                match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

                std::string wmsg = std::string(bowler->name) +
                                   " bowls a wide — +1 run (ball NOT counted)";
                push_event(EVT_WIDE, bowler->id, g_match.striker_idx, 1, wmsg);
                push_commentary_event(EVT_WIDE, bowler->id, g_match.striker_idx, 1, wmsg);
                bowler->runs_given++;

                /* Print to terminal — use over_num/legal_balls as display
                   co-ords since total_balls has NOT been incremented yet. */
                scoreboard_print_event(over_num, legal_balls + 1,
                                       snap_runs, snap_wickets,
                                       "WIDE", wmsg.c_str());

                pitch_monitor_release(&g_pitch);
                // usleep(60000);
                continue;
            }

            /* ── NO BALL ──────────────────────────────────────────────── */
            if (outcome == BALL_NO_BALL) {
                match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                g_match.runs++;
                match_state_finish_chase_if_complete(&g_match);
                int snap_runs    = g_match.runs;
                int snap_wickets = g_match.wickets;
                match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

                std::string nbmsg = std::string(bowler->name) +
                                    " oversteps — No Ball! +1 run. FREE HIT next!";
                push_event(EVT_NO_BALL, bowler->id, g_match.striker_idx, 1, nbmsg);
                push_commentary_event(EVT_NO_BALL, bowler->id, g_match.striker_idx, 1, nbmsg);
                bowler->runs_given++;

                scoreboard_print_event(over_num, legal_balls + 1,
                                       snap_runs, snap_wickets,
                                       "NB", nbmsg.c_str());

                /* set the free-hit flag for the very next legal delivery. */
                match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
                g_match.free_hit_active = true;
                match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);

                pitch_monitor_release(&g_pitch);
                // usleep(60000);
                continue;
            }

            /* ── LEGAL BALL — advance counter ─────────────────────────── */
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
            g_match.total_balls++;
            g_match.ball = g_match.total_balls % 6;
            g_match.over = g_match.total_balls / 6;
            int cur_over = g_match.over;
            if      (cur_over >= 19) g_match.match_intensity = 10;
            else if (cur_over >= 15) g_match.match_intensity = 7;
            else if (cur_over >= 10) g_match.match_intensity = 4;
            else                     g_match.match_intensity = cur_over / 3;
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

            // Powerplay restriction model:
            // overs 1-6: only two fielders outside 30-yard circle
            // overs 7-20: all fielders outside 30-yard circle
            if (cur_over < 6) {
                match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
                for (int fi = 0; fi < 10; ++fi) {
                    g_fielders[fi].in30YardZone =
                        !((fi == POWERPLAY_OUTSIDE_FIELDER_A) || (fi == POWERPLAY_OUTSIDE_FIELDER_B));
                }
                match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            } else {
                match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
                for (int fi = 0; fi < 10; ++fi) {
                    g_fielders[fi].in30YardZone = false;
                }
                match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            }

            /* Signal batsman */
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            bool is_free_hit = g_match.free_hit_active;
            /* free_hit_active is consumed / cleared by batsman_thread */
            g_match.ball_available        = true;
            g_match.result_consumed       = false;
            g_match.last_event_type       = (int)outcome;
            g_match.last_ball_was_wicket  = false;

            int d_over = (g_match.total_balls - 1) / 6;
            int d_ball = (g_match.total_balls - 1) % 6 + 1;

            std::string fh_prefix = is_free_hit ? "[FREE HIT] " : "";
            std::string bowl_msg  = fh_prefix + "Ball " +
                std::to_string(d_over) + "." + std::to_string(d_ball) +
                " — bowled by " + bowler->name +
                " to " + striker->name;
            EventType bowl_evt = is_free_hit ? EVT_FREE_HIT : EVT_BALL_BOWLED;
            push_event(bowl_evt, bowler->id, g_match.striker_idx, 0, bowl_msg);
            push_commentary_event(bowl_evt, bowler->id, g_match.striker_idx, 0, bowl_msg);

            if (is_free_hit) {
                printf("  *** FREE HIT! Batsman safe from bowled/LBW/stumped ***\n");
                fflush(stdout);
            }

            pthread_cond_broadcast(&g_match.ball_bowled_cv);

            while (!g_match.result_consumed && !g_match.match_over)
                match_state_deadlock_cond_wait(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX,
                                               &g_match.result_ready_cv, &g_match.ball_mutex);
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);

            pitch_monitor_release(&g_pitch);

            legal_balls++;
            bowler->balls_bowled++;
            over_runs += g_match.last_runs;
            // usleep(100000);
        }

        bowler->overs_bowled++;
        bowler->runs_given += over_runs;

        /* Rotate strike at end of over (unless last ball was a wicket) */
        if (!g_match.last_ball_was_wicket && !g_match.match_over) {
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
            int tmp                 = g_match.striker_idx;
            g_match.striker_idx     = g_match.non_striker_idx;
            g_match.non_striker_idx = tmp;
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
        }

        if (g_match.match_over || g_match.wickets >= 10 || g_match.over >= TOTAL_OVERS) 
        {
            if(!g_match.match_over)
            {
                int old_idx = g_match.current_bowler_idx;
                Event e{};
                e.type = EVT_OVER_COMPLETE;
                e.over = g_match.over;
                if(g_match.wickets >= 10)
                {
                    e.message = std::string("All wickets down, last bowler: ") +
                                g_bowlers[old_idx].name +
                                " (" + scheduler_algo_name(g_match.scheduler_algo) + ")";
                }else
                {
                    e.message = std::string("All overs completed, last bowler: ") +
                                g_bowlers[old_idx].name +
                                " (" + scheduler_algo_name(g_match.scheduler_algo) + ")";            
                }
                event_queue_push(&g_event_queue, e);
            }
            g_match.match_over = true;
            break;
        }

        scoreboard_print_over_summary(over_num + 1, over_runs);

        if (g_match.demo_deadlock_mode) {
            // Intentional inversion for deadlock demonstration:
            // bowler takes BALL -> SCHED while scheduler takes SCHED -> BALL.
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
            g_match.over_complete = true;
            pthread_cond_signal(&g_match.sched_cv);
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);

            // Re-take SCHED while still holding BALL; scheduler does opposite order.
            usleep(15000);
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        } else {
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
            g_match.over_complete = true;
            pthread_cond_signal(&g_match.sched_cv);
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
        }

        match_state_deadlock_mutex_lock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
        while (!g_match.next_bowler_ready && !g_match.match_over)
            match_state_deadlock_cond_wait(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX,
                                           &g_match.sched_cv, &g_match.sched_mutex);
        g_match.next_bowler_ready = false;
        match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_BOWLER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
    }

    g_match.match_over = true;
    push_event(EVT_MATCH_OVER, -1, -1, 0, "Innings complete");
    event_queue_shutdown(&g_commentary_queue);

    match_state_deadlock_unregister_thread(&g_match, DL_THREAD_BOWLER);
    printf("[BOWLER] Thread exiting — innings over.\n");
    return nullptr;
}
