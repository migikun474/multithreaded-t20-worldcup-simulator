#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include "match_state.h"
#include "player.h"
#include "scheduler.h"
#include "event.h"

//* Defining external references to globals
extern MatchState g_match;
extern EventQueue g_event_queue;
extern Player g_bowlers[];
extern Player g_batsmen[];

//* classic implementation of fetching next id based on current id using modulo operator for round robin
static int rr_next(int current, int n)
{
    return (current + 1) % n;
}

//* finding the top 2 priority bowlers among all the initialized bowler stats
static void ensure_priority_pair(Player *bowlers, int n)
{
    if (g_match.death_over_top_idx >= 0 && g_match.death_over_second_idx >= 0)
        return; //* if it is already set, then function call is redundant, return

    //* O(n) search for top 2
    int top = 0;
    for (int i = 1; i < n; ++i)
        if (bowlers[i].priority > bowlers[top].priority)
            top = i;

    int second = -1;
    for (int i = 0; i < n; ++i)
    {
        if (i == top)
            continue;
        if (second < 0 || bowlers[i].priority > bowlers[second].priority)
            second = i;
    }

    //* setting and then returning
    g_match.death_over_top_idx = top;
    g_match.death_over_second_idx = (second >= 0) ? second : top;

    return;
}

//* We use this function for scheduling next bowler while checking if the next bowler is not among the exceptionals.
//* We basically linearly search for the next candidate in order of ids (not priority), and if the id is not equal to the exceptions, we return that id
//* In worst case, loop will run only 3 times.
static int rr_next_excluding(int current, int n, int ex1, int ex2)
{
    for (int step = 1; step <= n; ++step)
    {
        int candidate = (current + step) % n;
        if (candidate != ex1 && candidate != ex2)
            return candidate;
    }
    return rr_next(current, n); //* just in case of fallback
}

//* This function is the main function for scheduling bowlers, it looks at current over count, and makes sure that
//* (1,3) and (17,19) => Highest priority bowler
//* (2,4) and (18,20) => Second highest priority bowler
//* Remaining overs are roundrobin among all other bowlers excluding these both.
static int phase_quota_next(Player *bowlers, int n, int intensity)
{
    (void)intensity;
    ensure_priority_pair(bowlers, n);
    int top_idx = g_match.death_over_top_idx;
    int second_idx = g_match.death_over_second_idx;
    int next_over = g_match.over + 1;

    // Powerplay: overs 1-6 => top gets 2 (1,3), second gets 2 (2,4), others RR (5,6)
    if (next_over == 1 || next_over == 3)
        return top_idx;
    if (next_over == 2 || next_over == 4)
        return second_idx;
    if (next_over == 5 || next_over == 6)
    {
        return rr_next_excluding(g_match.current_bowler_idx, n, top_idx, second_idx);
    }

    // Death overs: 17-20 => top gets 2 (17,19), second gets 2 (18,20)
    if (next_over == 17 || next_over == 19)
        return top_idx;
    if (next_over == 18 || next_over == 20)
        return second_idx;

    return rr_next_excluding(g_match.current_bowler_idx, n, top_idx, second_idx);
}

//* wrapper function.
int scheduler_next_bowler(Player *bowlers, int n, MatchState *ms)
{
    return phase_quota_next(bowlers, n, ms->match_intensity);
}

void *scheduler_thread_fn(void * /*arg*/)
{

    //? =====================================================================================================================================================
    //? Register current thread's existence to the match, which makes the umpire track this thread for deadlocks and stores metadata like thread type and id
    //? =====================================================================================================================================================
    match_state_deadlock_register_thread(&g_match, DL_THREAD_SCHEDULER, "SCHEDULER", 0);
    printf("[SCHEDULER] Thread started. Algorithm: %s\n",
           scheduler_algo_name(g_match.scheduler_algo));

    //? Core scheduler loop begins here
    while (!g_match.match_over)
    {
        match_state_deadlock_mutex_lock(&g_match, DL_THREAD_SCHEDULER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);

        //? Aquire lock and wait until either match is completed or over is finished.

        while (!g_match.over_complete && !g_match.match_over)
        {
            match_state_deadlock_cond_wait(&g_match, DL_THREAD_SCHEDULER, DL_RES_SCHED_MUTEX, &g_match.sched_cv, &g_match.sched_mutex);
        }
        if (g_match.match_over)
        {
            //? If match is completed, unlock mutex and return.
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_SCHEDULER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
            break;
        }
        if (g_match.demo_deadlock_mode)
        {
            //? If match is not completed, and we are in deadlock demo mode, create a strong possibility of deadlock by inverting order of requesting mutexes
            usleep(10000);
            match_state_deadlock_mutex_lock(&g_match, DL_THREAD_SCHEDULER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
            usleep(10000);
            match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_SCHEDULER, DL_RES_BALL_MUTEX, &g_match.ball_mutex);
        }

        //? since we are starting a new over, mark over complete as false.
        g_match.over_complete = false;
        match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_SCHEDULER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);

        //? =====================================================================================================================================================
        //? In the next code block, we first get the id of the next bowler using the helper functions defined at the start of the file
        //? Then we create an empty instance of the event struct declared in include/event.h, and populate it with an over complete event, and push into the 
        //? event queue. after this, we update the current bowler id in the match global, and release mutexes
        //? =====================================================================================================================================================

        int old_idx = g_match.current_bowler_idx;
        int next = scheduler_next_bowler(
            g_bowlers, 5, &g_match);

        match_state_deadlock_mutex_lock(&g_match, DL_THREAD_SCHEDULER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);
        g_match.current_bowler_idx = next;
        g_match.next_bowler_ready = true;
        pthread_cond_signal(&g_match.sched_cv);
        match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_SCHEDULER, DL_RES_SCHED_MUTEX, &g_match.sched_mutex);

        printf("[SCHEDULER] Context switch: %s (over %d.%d bowled) → %s  [algo: %s]\n",
               g_bowlers[old_idx].name.c_str(),
               g_bowlers[old_idx].overs_bowled,
               g_bowlers[old_idx].balls_bowled % 6,
               g_bowlers[next].name.c_str(),
               scheduler_algo_name(g_match.scheduler_algo));

        Event e{};
        e.type = EVT_OVER_COMPLETE;
        e.over = g_match.over;
        e.message = std::string("Over complete. Scheduler: ") +
                    g_bowlers[old_idx].name + " → " + g_bowlers[next].name +
                    " (" + scheduler_algo_name(g_match.scheduler_algo) + ")";
        event_queue_push(&g_event_queue, e);
    }

    match_state_deadlock_unregister_thread(&g_match, DL_THREAD_SCHEDULER);
    printf("[SCHEDULER] Thread exiting.\n");
    return nullptr;
}

int scheduler_next_batsman(SchedulerAlgo algo,
                           Player *batsmen,
                           int num_batsmen,
                           MatchState *ms)
{

    /* Helper lambda: is this slot still waiting in the pavilion? */
    auto is_waiting = [&](int i) -> bool
    {
        if (i < 0 || i >= num_batsmen)
            return false;
        if (batsmen[i].is_out)
            return false; // already dismissed
        if (batsmen[i].called_up)
            return false; // already at crease / used
        if (i == ms->striker_idx)
            return false; // currently batting
        if (i == ms->non_striker_idx)
            return false; // currently at non-striker end
        return true;
    };

    /* ── Count how many batsmen are genuinely still waiting ───────────── */
    int waiting_count = 0;
    for (int i = 0; i < num_batsmen; ++i)
        if (is_waiting(i))
            waiting_count++;

    if (waiting_count == 0)
    {
        /* No one left — signal all out */
        printf("[SCHEDULER] No more batsmen available.\n");
        return -1; // caller must set match_over
    }

    if (algo == ALGO_SJF)
    {
        printf("[SCHEDULER] SJF – eligible batsmen:\n");
        for (int i = 0; i < num_batsmen; ++i)
        {
            if (!is_waiting(i))
                continue;
            float sr = batsmen[i].strike_rate;
            if (sr <= 0.0f)
                sr = 100.0f;
            float burst = (batsmen[i].bat_avg / sr) * 100.0f;
            printf("  %-14s  avg=%5.1f  sr=%5.1f  burst=%5.1f  est_dur=%d\n",
                   batsmen[i].name.c_str(), batsmen[i].bat_avg, sr, burst,
                   batsmen[i].estimated_duration);
        }
    }
    /* ── FCFS for RR and PRIORITY ─────────────────────────────────────── */
    else if (algo != ALGO_SJF)
    {
        for (int i = 0; i < num_batsmen; ++i)
        {
            if (is_waiting(i))
            {
                printf("[SCHEDULER] FCFS: next batsman → %s  (position %d)\n",
                       batsmen[i].name.c_str(), i + 1);
                return i;
            }
        }
        return -1; // shouldn't reach here
    }
    // for sjf now

    /* ── SJF: scan ALL waiting batsmen, pick smallest burst ──────────── */
    int best_idx = -1;
    float best_burst = 1e9f;

    for (int i = 0; i < num_batsmen; ++i)
    {
        if (!is_waiting(i))
            continue;

        float sr = batsmen[i].strike_rate;
        if (sr <= 0.0f)
            sr = 100.0f;
        float burst = (batsmen[i].bat_avg / sr) * 100.0f;

        printf("[SCHEDULER] SJF candidate: %-14s  avg=%5.1f  SR=%5.1f  burst≈%5.1f balls\n",
               batsmen[i].name.c_str(),
               batsmen[i].bat_avg,
               batsmen[i].strike_rate,
               burst);

        if (burst < best_burst)
        {
            best_burst = burst;
            best_idx = i;
        }
    }

    if (best_idx >= 0)
        printf("[SCHEDULER] SJF → promotes %s  (shortest burst ≈ %.1f balls)\n",
               batsmen[best_idx].name.c_str(), best_burst);
    else
        printf("[SCHEDULER] SJF: no candidate found — innings over.\n");

    return best_idx;
}
