#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <string>
#include "match_state.h"
#include "player.h"
#include "scheduler.h"
#include "event.h"
#include "scoreboard.h"

extern MatchState g_match;
extern EventQueue g_event_queue;
extern EventQueue g_commentary_queue;
extern Player g_fielders[];
extern Player g_batsmen[];
extern Player g_bowlers[];
extern int g_crease_counter;

typedef enum
{
    FIELD_CATCH_OUT = 0,
    FIELD_DROPPED,
    FIELD_RUN_OUT_ATTEMPT,
    FIELD_FOUR_OVER_BOUNDARY,
    FIELD_SIX_OVER_BOUNDARY
} FielderOutcome;

extern FielderOutcome fielder_resolve_aerial(const Player *fielder, int reaction_ms);
extern bool run_out_is_successful(const Player *fielder);

static pthread_mutex_t resolve_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile bool ball_being_resolved = false;

//* ========================================================================================
//* compute runs on the basis of stats of batsman and fielder and add some random noise.
//* ========================================================================================
int compute_runs(const Player *bat, const Player *fielder)
{
    float batting_score = bat->power_index * 0.5f + (bat->strike_rate / 100.0f) * 3.0f + (bat->bat_avg / 50.0f) * 2.0f;

    float field_score = fielder->speed * 0.5f + fielder->accuracy * 0.3f + fielder->dive_ability * 0.4f;

    float net = batting_score - field_score;
    float noise = (rand() % 100) / 100.0f * 2.0f - 1.0f; // [-1, +1]
    net += noise;

    if (net < -1.0f)
        return 0;
    if (net < 1.5f)
        return 1;
    if (net < 3.5f)
        return 2;
    return 3;
}

//* Display which over is currently going on using total bals
static void display_coords(int *d_over, int *d_ball)
{
    int tb = g_match.total_balls;
    *d_over = (tb > 0) ? (tb - 1) / 6 : 0;
    *d_ball = (tb > 0) ? (tb - 1) % 6 + 1 : 0;
}

//* Helper push event function, pushes to global event queue
static void push_event(EventType t, int fid, int bid, int runs,
                       const std::string &msg)
{
    Event e{};
    e.type = t;
    e.bowler_id = g_bowlers[g_match.current_bowler_idx % 5].id;
    e.fielder_id = fid;
    e.batsman_id = bid;
    e.runs = runs;
    e.over = g_match.over;
    e.ball = g_match.ball;
    e.total_balls = g_match.total_balls;
    e.message = msg;
    event_queue_push(&g_event_queue, e);
}

//* Helper push event function, pushes to global commentary queue
static void push_commentary_event(EventType t, int fid, int bid,
                                  int runs, FieldZone zone,
                                  const std::string &msg)
{
    Event e{};
    e.type = t;
    e.bowler_id = g_bowlers[g_match.current_bowler_idx % 5].id;
    e.fielder_id = fid;
    e.batsman_id = bid;
    e.runs = runs;
    e.over = g_match.over;
    e.ball = g_match.ball;
    e.total_balls = g_match.total_balls;
    e.ball_zone = zone;
    e.message = msg;
    event_queue_push(&g_commentary_queue, e);
}

//* We decide new batsman based on next batsman which is resolved by the helper defined in scheduler thread and declared in scheduler.h
static bool bring_new_batsman()
{
    if (g_match.wickets >= 10)
        return false;

    int idx = scheduler_next_batsman(
        g_match.scheduler_algo, g_batsmen, 11, &g_match);

    if (idx < 0)
        return false;

    g_batsmen[idx].called_up = true;
    g_batsmen[idx].crease_order = g_crease_counter++;
    g_match.striker_idx = idx;
    printf("  [CREASE] %s walks in at No.%d  [%d/%d]  [algo: %s]\n",
           g_batsmen[idx].name.c_str(), g_batsmen[idx].crease_order,
           g_match.runs, g_match.wickets,
           scheduler_algo_name(g_match.scheduler_algo));
    return true;
}

//* Helper function that signals that ball result has been resolved (especially in case of overthrows)
static void signal_result_done(int deadlock_thread)
{
    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_RESOLVE_MUTEX, &resolve_mutex);
    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
    ball_being_resolved = false;
    g_match.result_consumed = true;
    pthread_cond_signal(&g_match.result_ready_cv);
    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_RESOLVE_MUTEX, &resolve_mutex);
}

void *fielder_thread_fn(void *arg)
{

    //* Initialize and register thread to match state so that umpire tracks it for deadlocks. Then enter main loop

    int id = *(int *)arg;
    Player *fielder = &g_fielders[id];
    int deadlock_thread = DL_THREAD_FIELDER_0 + id;
    match_state_deadlock_register_thread(&g_match, deadlock_thread, "FIELDER", id);

    while (!g_match.match_over)
    {

        //* Subscribe to the condition variable which represents the zone our fielder is present in.

        match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        while (!g_match.ball_in_air && !g_match.match_over)
            match_state_deadlock_cond_wait(&g_match, deadlock_thread, DL_RES_BALL_MUTEX,
                                           &g_match.zone_cv[fielder->field_zone], &g_match.ball_mutex);

        if (g_match.match_over)
        {
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            break;
        }

        //* If fielder woke up, we know we are either in ball zone or adjacent, add reaction time penalty to the fielder threads based on which zone they are present in
        //* Also, check if shot type is ball in air, and if so and fielder is in 30 yard zone, then sleep on the ball being resolved cv.

        FieldZone ball_zone_snap = g_match.ball_zone;
        int shot_type = g_match.last_event_type;
        if (shot_type == 3 /* BALL_AERIAL */ &&
            (fielder->in30YardZone || !zones_are_adjacent(fielder->field_zone, ball_zone_snap)))
        {
            while (g_match.ball_in_air && !g_match.match_over)
            {
                match_state_deadlock_cond_wait(&g_match, deadlock_thread, DL_RES_BALL_MUTEX,
                                               &g_match.ball_resolved_cv, &g_match.ball_mutex);
            }
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            continue;
        }
        match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);

        bool exact_zone = (fielder->field_zone == ball_zone_snap);

        /* ── Reaction time (faster if the ball is in your exact zone) ── */
        int speed_bonus = (int)(fielder->speed * 2.0f);
        int reaction_ms;
        if (exact_zone)
            reaction_ms = (10 - speed_bonus / 2) + rand() % 50;
        else
            reaction_ms = (70 - speed_bonus) + rand() % 100;
        if (reaction_ms < 5)
            reaction_ms = 5;

        usleep(reaction_ms * 1000);
        if (g_match.match_over)
            break;

        /* ── Race for the resolve_mutex ───────────────────────────────── */
        match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_RESOLVE_MUTEX, &resolve_mutex);

        match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        bool still_live = g_match.ball_in_air;
        match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);

        if (!still_live || ball_being_resolved)
        {
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_RESOLVE_MUTEX, &resolve_mutex);
            /* Lost the race — wait for the winner to finish */
            match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            while (g_match.ball_in_air && !g_match.match_over)
                match_state_deadlock_cond_wait(&g_match, deadlock_thread, DL_RES_BALL_MUTEX,
                                               &g_match.ball_resolved_cv, &g_match.ball_mutex);
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            continue;
        }

        /* We won — claim the ball */
        ball_being_resolved = true;

        match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        g_match.ball_in_air = false;
        bool is_free_hit = g_match.free_hit_active;
        pthread_cond_broadcast(&g_match.ball_resolved_cv); // wake race losers
        match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_BALL_MUTEX, &g_match.ball_mutex);

        match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_RESOLVE_MUTEX, &resolve_mutex);

        if (g_match.match_over || g_match.wickets >= 10)
        {
            signal_result_done(deadlock_thread);
            break;
        }

        //* ── Resolve the ball outcome
        std::string desc;
        int total_runs = 0;
        EventType evt = EVT_RUNS_SCORED;
        const char *badge = "";
        Player *striker = &g_batsmen[g_match.striker_idx];

        int d_over, d_ball;
        display_coords(&d_over, &d_ball);

        /* ── GROUNDED / WELL-TIMED shot (no aerial component) */
        if (shot_type == 1 /* BALL_GROUNDED */ || shot_type == 2 /* BALL_WELL_TIMED */)
        {

            int base_runs = compute_runs(striker, fielder);
            if (shot_type == 2)
                base_runs += 1; // well-timed boost

            if (base_runs >= 4)
            {
                total_runs = 4;
                evt = EVT_FOUR;
                badge = "4";
                desc = striker->name + " finds the gap — FOUR!"; 
                striker->runs_scored += 4;
                striker->fours++;
            }
            else
            {
                total_runs = base_runs;
                desc = striker->name + " takes " +
                       std::to_string(base_runs) + " run";
                if (base_runs != 1)
                    desc += "s";
                striker->runs_scored += base_runs;
            }

            match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
            g_match.runs += total_runs;
            g_match.last_runs = total_runs;
            if (total_runs % 2 == 1)
            {
                int tmp = g_match.striker_idx;
                g_match.striker_idx = g_match.non_striker_idx;
                g_match.non_striker_idx = tmp;
            }
            match_state_finish_chase_if_complete(&g_match);
            match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

            /* ── AERIAL ball ───────────────────────────────────────────────── */
        }
        else
        {

            FielderOutcome fo = fielder_resolve_aerial(fielder, reaction_ms);

            switch (fo)
            {

            /* ── Catch out ──────────────────────────────────────── */
            case FIELD_CATCH_OUT:
                striker->is_out = true;
                striker->out_type = "caught";
                g_match.last_ball_was_wicket = true;
                desc = striker->name + " CAUGHT by " + fielder->name +
                       " at " + field_zone_name(ball_zone_snap) + "! Superb take!";
                if (is_free_hit)
                    desc += " (FREE HIT — catch IS valid!)";
                evt = EVT_CATCH_OUT;
                total_runs = 0;
                badge = "W";
                match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                g_match.wickets++;
                g_match.last_runs = 0;
                g_bowlers[g_match.current_bowler_idx % 5].wickets_taken++;
                if (!bring_new_batsman())
                    g_match.match_over = true;
                match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                printf("  *** WICKET%s *** [%d/%d]\n",
                       is_free_hit ? " (FREE HIT — caught out!)" : "",
                       g_match.runs, g_match.wickets);
                break;

            /* ── Dropped catch → four ───────────────────────────── */
            case FIELD_DROPPED:
                total_runs = 4;
                badge = "4";
                desc = fielder->name + " grasses it at " +
                       field_zone_name(ball_zone_snap) +
                       "! " + striker->name + " escapes — FOUR!";
                evt = EVT_FOUR;
                striker->runs_scored += 4;
                striker->fours++;
                match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                g_match.runs += 4;
                g_match.last_runs = 4;
                match_state_finish_chase_if_complete(&g_match);
                match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                break;

            /* ── Four over boundary ─────────────────────────────── */
            case FIELD_FOUR_OVER_BOUNDARY:
                total_runs = 4;
                badge = "4";
                desc = "Ball clears the infield towards " +
                       std::string(field_zone_name(ball_zone_snap)) +
                       " — FOUR! " + fielder->name + " dives but can't stop it!";
                evt = EVT_FOUR;
                striker->runs_scored += 4;
                striker->fours++;
                match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                g_match.runs += 4;
                g_match.last_runs = 4;
                match_state_finish_chase_if_complete(&g_match);
                match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                break;

            /* ── Six over boundary ──────────────────────────────── */
            case FIELD_SIX_OVER_BOUNDARY:
                total_runs = 6;
                badge = "6";
                desc = striker->name + " clears the rope at " +
                       field_zone_name(ball_zone_snap) +
                       " — SIX! " + fielder->name + " watches it sail over!";
                evt = EVT_SIX;
                striker->runs_scored += 6;
                striker->sixes++;
                match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                g_match.runs += 6;
                g_match.last_runs = 6;
                match_state_finish_chase_if_complete(&g_match);
                match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                break;

            /* ── Run-out attempt ────────────────────────────────── */
            case FIELD_RUN_OUT_ATTEMPT:
                if (run_out_is_successful(fielder))
                {
                    /* Successful run-out */
                    striker->is_out = true;
                    striker->out_type = "run out";
                    g_match.last_ball_was_wicket = true;
                    desc = "Direct hit from " + fielder->name +
                           " at " + field_zone_name(ball_zone_snap) + "! " +
                           striker->name + " RUN OUT — short of the crease!";
                    evt = EVT_RUN_OUT;
                    total_runs = 0;
                    badge = "W";
                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    g_match.wickets++;
                    g_match.last_runs = 0;
                    if (!bring_new_batsman())
                        g_match.match_over = true;
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    printf("  *** WICKET *** [%d/%d]\n",
                           g_match.runs, g_match.wickets);
                }
                else
                {
                    /* Failed run-out — batsmen complete run(s) */
                    bool is_overthrow = (rand() % 100 < 20); // 20% chance
                    int overthrow_runs = is_overthrow ? (rand() % 3) + 1 : 0;

                    int base_runs = compute_runs(striker, fielder);
                    if (base_runs == 0)
                        base_runs = 1; // minimum 1 (run was attempted)

                    total_runs = base_runs + overthrow_runs;

                    desc = "Run-out attempt by " + fielder->name +
                           " — batsmen make it! " +
                           std::to_string(base_runs) + " run";
                    if (base_runs > 1)
                        desc += "s";
                    if (is_overthrow)
                        desc += " + OVERTHROW! Extra " +
                                std::to_string(overthrow_runs) + " runs";

                    evt = EVT_RUNS_SCORED;
                    striker->runs_scored += base_runs;

                    match_state_deadlock_mutex_lock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
                    g_match.runs += total_runs;
                    g_match.last_runs = total_runs;
                    if (total_runs % 2 == 1)
                    {
                        int tmp = g_match.striker_idx;
                        g_match.striker_idx = g_match.non_striker_idx;
                        g_match.non_striker_idx = tmp;
                    }
                    match_state_finish_chase_if_complete(&g_match);
                    match_state_deadlock_mutex_unlock(&g_match, deadlock_thread, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

                    /* Push base run event */
                    push_event(EVT_RUNS_SCORED, id, striker->id, base_runs, desc);
                    push_commentary_event(EVT_RUNS_SCORED, id, striker->id,
                                          base_runs, ball_zone_snap, desc);

                    /* Push separate overthrow event if applicable */
                    if (is_overthrow)
                    {
                        std::string oth_desc = "Overthrow by " + fielder->name +
                                               "! Extra " +
                                               std::to_string(overthrow_runs) +
                                               " runs";
                        Event oe{};
                        oe.type = EVT_OVERTHROW;
                        oe.fielder_id = id;
                        oe.batsman_id = striker->id;
                        oe.runs = overthrow_runs;
                        oe.over = g_match.over;
                        oe.ball = g_match.ball;
                        oe.ball_zone = ball_zone_snap;
                        oe.message = oth_desc;
                        event_queue_push(&g_event_queue, oe);
                        event_queue_push(&g_commentary_queue, oe);
                    }

                    /* Print terminal line then signal bowler */
                    scoreboard_print_event(d_over, d_ball,
                                           g_match.runs, g_match.wickets,
                                           "",
                                           desc.c_str());
                    signal_result_done(deadlock_thread);
                    continue; // skip the generic push+print below
                }
                break; // end FIELD_RUN_OUT_ATTEMPT
            } 
        } 

        /* ── Generic push + terminal print for all other outcomes ─────── */
        push_event(evt, id, striker->id, total_runs, desc);
        push_commentary_event(evt, id, striker->id, total_runs, ball_zone_snap, desc);

        scoreboard_print_event(d_over, d_ball,
                               g_match.runs, g_match.wickets,
                               badge,
                               desc.c_str());

        signal_result_done(deadlock_thread);
    } 

    match_state_deadlock_unregister_thread(&g_match, deadlock_thread);
    return nullptr;
}
