#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "match_state.h"
#include "player.h"
#include "event.h"

extern MatchState g_match;
extern EventQueue g_event_queue;
extern EventQueue g_commentary_queue;
extern Player g_bowlers[];
extern Player g_batsmen[];
extern Player g_fielders[];

static constexpr int EXIT_CODE_DEADLOCK = 2;

static const char *resource_name(int r)
{
    switch (r)
    {
    case DL_RES_SCORE_MUTEX:
        return "SCORE_MUTEX";
    case DL_RES_BALL_MUTEX:
        return "BALL_MUTEX";
    case DL_RES_SCHED_MUTEX:
        return "SCHED_MUTEX";
    case DL_RES_END_MUTEX_0:
        return "END_MUTEX_0";
    case DL_RES_END_MUTEX_1:
        return "END_MUTEX_1";
    case DL_RES_CREASE_SEM:
        return "CREASE_SEM";
    case DL_RES_RESOLVE_MUTEX:
        return "FIELDER_RESOLVE_MUTEX";
    case DL_RES_PITCH_MONITOR_MUTEX:
        return "PITCH_MONITOR_MUTEX";
    case DL_RES_SHUTDOWN_MUTEX:
        return "SHUTDOWN_MUTEX";
    default:
        return "UNKNOWN_RESOURCE";
    }
}

static std::string deadlock_actor_label(const char *thread_type, int thread_id)
{
    static std::unordered_map<int, std::string> batsman_slot_name;
    if (thread_type == nullptr)
        return "UNKNOWN";
    std::string t(thread_type);
    if (t == "BOWLER" && thread_id >= 0 && thread_id < 5)
    {
        return "BOWLER:" + g_bowlers[thread_id].name;
    }
    if (t == "FIELDER" && thread_id >= 0 && thread_id < 10)
    {
        return "FIELDER:" + g_fielders[thread_id].name;
    }
    if (t == "BATSMAN" && thread_id >= 0 && thread_id < 2)
    {
        int idx = (thread_id == 0) ? g_match.striker_idx : g_match.non_striker_idx;
        if (idx >= 0 && idx < 11)
        {
            batsman_slot_name[thread_id] = g_batsmen[idx].name;
            return "BATSMAN:" + g_batsmen[idx].name;
        }
        auto it = batsman_slot_name.find(thread_id);
        if (it != batsman_slot_name.end())
            return "BATSMAN:" + it->second;
        return "BATSMAN:slot" + std::to_string(thread_id);
    }
    return t + ":" + std::to_string(thread_id);
}

static bool request_lte_work(const int request_row[DEADLOCK_RESOURCE_COUNT],
                             const int work[DEADLOCK_RESOURCE_COUNT])
{
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        if (request_row[j] > work[j])
            return false;
    }
    return true;
}

static bool detect_deadlock_and_collect(std::vector<int> &deadlocked_threads,
                                        int available[DEADLOCK_RESOURCE_COUNT],
                                        int allocation[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT],
                                        int request[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT],
                                        bool thread_active[DEADLOCK_THREAD_COUNT],
                                        char thread_type[DEADLOCK_THREAD_COUNT][16],
                                        int thread_id[DEADLOCK_THREAD_COUNT],
                                        char trigger_type[16],
                                        int *trigger_id,
                                        int *trigger_resource)
{
    pthread_mutex_lock(&g_match.deadlock_mutex);
    match_state_deadlock_recompute_available(&g_match);
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        available[j] = g_match.deadlock_available[j];
    }
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        thread_active[i] = g_match.deadlock_thread_active[i];
        snprintf(thread_type[i], 16, "%s", g_match.deadlock_thread_type[i]);
        thread_id[i] = g_match.deadlock_thread_id[i];
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            allocation[i][j] = g_match.deadlock_allocation[i][j];
            request[i][j] = g_match.deadlock_request[i][j];
        }
    }
    snprintf(trigger_type, 16, "%s", g_match.deadlock_trigger_thread_type);
    *trigger_id = g_match.deadlock_trigger_thread_id;
    *trigger_resource = g_match.deadlock_trigger_resource;
    pthread_mutex_unlock(&g_match.deadlock_mutex);

    int work[DEADLOCK_RESOURCE_COUNT];
    bool finish[DEADLOCK_THREAD_COUNT];
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        work[j] = available[j];
    }
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        if (!thread_active[i])
        {
            finish[i] = true;
            continue;
        }
        int allocation_sum = 0;
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            allocation_sum += allocation[i][j];
        }
        finish[i] = (allocation_sum == 0);
    }

    while (true)
    {
        int found = -1;
        for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
        {
            if (!finish[i] && request_lte_work(request[i], work))
            {
                found = i;
                break;
            }
        }
        if (found < 0)
            break;
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            work[j] += allocation[found][j];
        }
        finish[found] = true;
    }

    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        if (!finish[i])
            deadlocked_threads.push_back(i);
    }
    return !deadlocked_threads.empty();
}

static void log_deadlock_to_file(const char *trigger_type, int trigger_id, int trigger_resource,
                                 const std::vector<int> &deadlocked_threads,
                                 bool thread_active[DEADLOCK_THREAD_COUNT],
                                 char thread_type[DEADLOCK_THREAD_COUNT][16],
                                 int thread_id[DEADLOCK_THREAD_COUNT],
                                 const int available[DEADLOCK_RESOURCE_COUNT],
                                 int allocation[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT],
                                 int request[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT])
{
    FILE *fp = fopen("logs/deadlock_log.txt", "a");
    if (!fp)
    {
        fprintf(stderr, "[UMPIRE] Failed to open logs/deadlock_log.txt for deadlock logging.\n");
        return;
    }
    fprintf(fp, "===== DEADLOCK DETECTED =====\n");
    std::string trigger_label = deadlock_actor_label(trigger_type, trigger_id);
    fprintf(fp, "Triggering thread: %s resource=%s\n",
            trigger_label.c_str(), resource_name(trigger_resource));
    fprintf(fp, "Deadlocked thread slots: ");
    for (size_t i = 0; i < deadlocked_threads.size(); ++i)
    {
        int t = deadlocked_threads[i];
        std::string actor_label = deadlock_actor_label(thread_type[t], thread_id[t]);
        fprintf(fp, "%d(%s)%s", t, actor_label.c_str(),
                (i + 1 < deadlocked_threads.size()) ? ", " : "");
    }
    fprintf(fp, "\n");

    fprintf(fp, "Available = [");
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        fprintf(fp, "%d%s", available[j], (j + 1 < DEADLOCK_RESOURCE_COUNT) ? ", " : "");
    }
    fprintf(fp, "]\n");
    fprintf(fp, "Resources = [");
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        fprintf(fp, "%s%s", resource_name(j), (j + 1 < DEADLOCK_RESOURCE_COUNT) ? ", " : "");
    }
    fprintf(fp, "]\n");

    fprintf(fp, "Allocation matrix:\n");
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        if (!thread_active[i])
            continue;
        std::string actor_label = deadlock_actor_label(thread_type[i], thread_id[i]);
        fprintf(fp, "  T%d(%s): [", i, actor_label.c_str());
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            fprintf(fp, "%d%s", allocation[i][j], (j + 1 < DEADLOCK_RESOURCE_COUNT) ? ", " : "");
        }
        fprintf(fp, "]\n");
    }

    fprintf(fp, "Request matrix:\n");
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        if (!thread_active[i])
            continue;
        std::string actor_label = deadlock_actor_label(thread_type[i], thread_id[i]);
        fprintf(fp, "  T%d(%s): [", i, actor_label.c_str());
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            fprintf(fp, "%d%s", request[i][j], (j + 1 < DEADLOCK_RESOURCE_COUNT) ? ", " : "");
        }
        fprintf(fp, "]\n");
    }
    fprintf(fp, "Allocation grid (rows=threads, cols=resources):\n");
    fprintf(fp, "%-22s", "Thread");
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        fprintf(fp, " %-12s", resource_name(j));
    }
    fprintf(fp, "\n");
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        if (!thread_active[i])
            continue;
        char row_name[64];
        std::string actor_label = deadlock_actor_label(thread_type[i], thread_id[i]);
        snprintf(row_name, sizeof(row_name), "T%d(%.49s)", i, actor_label.c_str());
        fprintf(fp, "%-22s", row_name);
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            fprintf(fp, " %-12d", allocation[i][j]);
        }
        fprintf(fp, "\n");
    }
    fprintf(fp, "Request grid (rows=threads, cols=resources):\n");
    fprintf(fp, "%-22s", "Thread");
    for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
    {
        fprintf(fp, " %-12s", resource_name(j));
    }
    fprintf(fp, "\n");
    for (int i = 0; i < DEADLOCK_THREAD_COUNT; ++i)
    {
        if (!thread_active[i])
            continue;
        char row_name[64];
        std::string actor_label = deadlock_actor_label(thread_type[i], thread_id[i]);
        snprintf(row_name, sizeof(row_name), "T%d(%.49s)", i, actor_label.c_str());
        fprintf(fp, "%-22s", row_name);
        for (int j = 0; j < DEADLOCK_RESOURCE_COUNT; ++j)
        {
            fprintf(fp, " %-12d", request[i][j]);
        }
        fprintf(fp, "\n");
    }
    fprintf(fp, "\n");
    fclose(fp);
}

static void terminate_on_deadlock(const char *trigger_type, int trigger_id, int trigger_resource)
{
    printf("\n[UMPIRE] DEADLOCK DETECTED by standard algorithm.\n"
           "         Trigger thread: type=%s id=%d resource=%s\n"
           "         Terminating all threads and program.\n",
           trigger_type, trigger_id, resource_name(trigger_resource));

    fflush(stdout);
    // Fail-fast on deadlock: avoid taking other locks here because one of them is
    // already part of the detected circular wait.
    _Exit(EXIT_CODE_DEADLOCK);
}

void *umpire_thread_fn(void * /*arg*/)
{
    match_state_deadlock_register_thread(&g_match, DL_THREAD_UMPIRE, "UMPIRE", 0);
    printf("[UMPIRE] Thread started. Monitoring for deadlocks and innings end.\n");

    while (!g_match.match_over)
    {
        // auto start = now();
        // sleep(5);
        usleep(150000);
        // ── Deadlock detection via Available/Allocation/Request ────────────
        int available[DEADLOCK_RESOURCE_COUNT];
        int allocation[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT];
        int request[DEADLOCK_THREAD_COUNT][DEADLOCK_RESOURCE_COUNT];
        bool thread_active[DEADLOCK_THREAD_COUNT];
        char thread_type[DEADLOCK_THREAD_COUNT][16];
        int thread_id[DEADLOCK_THREAD_COUNT];
        char trigger_type[16];
        int trigger_id = -1;
        int trigger_resource = -1;
        std::vector<int> deadlocked_threads;
        bool is_deadlocked = detect_deadlock_and_collect(
            deadlocked_threads, available, allocation, request,
            thread_active, thread_type, thread_id,
            trigger_type, &trigger_id, &trigger_resource);

        if (is_deadlocked)
        {
            log_deadlock_to_file(trigger_type, trigger_id, trigger_resource,
                                 deadlocked_threads, thread_active, thread_type, thread_id,
                                 available, allocation, request);
            terminate_on_deadlock(trigger_type, trigger_id, trigger_resource);
        }

        // ── Enforce innings end conditions ──────────────────────────────────
        match_state_deadlock_mutex_lock(&g_match, DL_THREAD_UMPIRE, DL_RES_SCORE_MUTEX, &g_match.score_mutex);
        bool all_out = (g_match.wickets >= 10);
        bool overs_done = (g_match.over >= 20);
        bool chase_complete = match_state_target_reached(&g_match);
        if (chase_complete) {
            g_match.match_over = true;
        }
        match_state_deadlock_mutex_unlock(&g_match, DL_THREAD_UMPIRE, DL_RES_SCORE_MUTEX, &g_match.score_mutex);

        if (all_out || overs_done || chase_complete)
        {
            match_state_signal_end(&g_match);
            printf("[UMPIRE] Innings over — %s. Score: %d/%d in %d.%d overs\n",
                   chase_complete ? "Target chased down"
                                  : (all_out ? "All out" : "20 overs bowled"),
                   g_match.runs, g_match.wickets,
                   g_match.over, g_match.ball);
            break;
        }
    }

    match_state_deadlock_note_request(&g_match, DL_THREAD_UMPIRE, DL_RES_SHUTDOWN_MUTEX, 1);
    pthread_mutex_lock(&g_match.shutdown_mutex);
    match_state_deadlock_note_grant(&g_match, DL_THREAD_UMPIRE, DL_RES_SHUTDOWN_MUTEX, 1);
    while (!g_match.allow_umpire_exit)
        pthread_cond_wait(&g_match.shutdown_cv, &g_match.shutdown_mutex);
    match_state_deadlock_note_release(&g_match, DL_THREAD_UMPIRE, DL_RES_SHUTDOWN_MUTEX, 1);
    pthread_mutex_unlock(&g_match.shutdown_mutex);

    match_state_deadlock_unregister_thread(&g_match, DL_THREAD_UMPIRE);
    printf("[UMPIRE] Thread exiting.\n");
    return nullptr;
}
