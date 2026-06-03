#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <string>
#include "event.h"
#include "match_state.h"
#include "player.h"

extern EventQueue g_event_queue;
extern MatchState g_match;
extern Player     g_batsmen[];
extern Player     g_bowlers[];
extern Player     g_fielders[];
extern std::string g_batting_team_name;
extern std::string g_bowling_team_name;

/*
 * Logger thread — sole writer to match_log.txt, events.csv, and commentary_log.txt.
 *
 * EVT_COMMENTARY events (produced by the commentator thread) are routed to
 * commentary_log.txt only, keeping play-by-play and commentary logs separate.
 * All other events go to match_log.txt and events.csv as before.
 *
 */

static long get_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

void* logger_thread_fn(void* /*arg*/) {
    // Open all three log files
    const bool append = (g_match.innings_number > 1);
    FILE* log_txt  = fopen("logs/match_log.txt",     append ? "a" : "w");
    FILE* log_csv  = fopen("logs/events.csv",         append ? "a" : "w");
    FILE* log_cmnt = fopen("logs/commentary_log.txt", append ? "a" : "w");

    if (!log_txt || !log_csv || !log_cmnt) {
        if (log_txt)  fclose(log_txt);
        if (log_csv)  fclose(log_csv);
        if (log_cmnt) fclose(log_cmnt);
        log_txt  = fopen("/tmp/match_log.txt",     append ? "a" : "w");
        log_csv  = fopen("/tmp/events.csv",         append ? "a" : "w");
        log_cmnt = fopen("/tmp/commentary_log.txt", append ? "a" : "w");
    }

    // Match log header
    if (log_txt && !append) {
        fprintf(log_txt,
            "T20 World Cup 2026 — India vs England  |  Full Match\n"
            "=========================================================\n\n"
            "%-14s  %-6s  %-20s  %-18s  %-18s  %-16s  %s\n",
            "Timestamp(us)", "Over", "Event",
            "Bowler", "Batsman", "Fielder", "Description");
        fprintf(log_txt, "%s\n", std::string(110, '-').c_str());
    }
    if (log_txt) {
        fprintf(log_txt, "\nINNINGS %d: %s batting, %s bowling%s%d\n",
                g_match.innings_number,
                g_batting_team_name.c_str(),
                g_bowling_team_name.c_str(),
                g_match.target_runs > 0 ? " | target=" : "",
                g_match.target_runs > 0 ? g_match.target_runs : 0);
        fflush(log_txt);
    }

    // CSV header
    if (log_csv && !append) {
        fprintf(log_csv,
            "timestamp_us,over,ball,event_type,"
            "bowler_name,batsman_name,fielder_name,"
            "runs_off_ball,total_runs,total_wickets,"
            "dismissal_type,match_phase,message\n");
    }

    // Commentary log header
    if (log_cmnt && !append) {
        fprintf(log_cmnt,
            "T20 World Cup 2026 — Commentary Log\n"
            "=====================================\n\n"
            "%-14s  %-6s  %s\n",
            "Timestamp(us)", "Over", "Commentary");
        fprintf(log_cmnt, "%s\n", std::string(80, '-').c_str());
    }
    if (log_cmnt) {
        fprintf(log_cmnt, "\nINNINGS %d: %s batting, %s bowling\n",
                g_match.innings_number,
                g_batting_team_name.c_str(),
                g_bowling_team_name.c_str());
        fflush(log_cmnt);
    }

    printf("[LOGGER] Thread started. Writing to logs/\n");

    long base_us = get_us();

    while (true) {
        Event e = event_queue_pop(&g_event_queue);

        if (e.type == EVT_MATCH_OVER && e.message == "shutdown") break;

        long ts = get_us() - base_us;

        // ── Commentary events → commentary_log.txt only ──────────────────
        if (e.type == EVT_COMMENTARY) {
            int display_over = e.total_balls > 0 ? (e.total_balls - 1) / 6 : e.over;
            int display_ball = e.total_balls > 0 ? (e.total_balls - 1) % 6 + 1 : e.ball;
            if (log_cmnt) {
                fprintf(log_cmnt, "%-14ld  %2d.%-3d  %s\n",
                        ts, display_over, display_ball,
                        e.message.c_str());
                fflush(log_cmnt);
            }
            if (e.type == EVT_MATCH_OVER) break;
            continue;
        }

        // ── All other events → match_log.txt + events.csv ───────────────
        std::string bowler_name  = (e.bowler_id  >= 0 && e.bowler_id  < 5)  ? g_bowlers[e.bowler_id].name  : "—";
        std::string batsman_name = (e.batsman_id >= 0 && e.batsman_id < 11) ? g_batsmen[e.batsman_id].name : "—";
        std::string fielder_name = (e.fielder_id >= 0 && e.fielder_id < 10) ? g_fielders[e.fielder_id].name: "—";

        int display_over = e.total_balls > 0 ? (e.total_balls - 1) / 6 : e.over;
        int display_ball = e.total_balls > 0 ? (e.total_balls - 1) % 6 + 1 : e.ball;
std::string dismissal = "—";
        if      (e.type == EVT_CATCH_OUT)  dismissal = "caught";
        else if (e.type == EVT_RUN_OUT)    dismissal = "run out";
        else if (e.type == EVT_OVERTHROW)  dismissal = "overthrow"; 
        else if (e.type == EVT_BATSMAN_OUT)  {
            if (e.batsman_id >= 0 && e.batsman_id < 11)
                dismissal = g_batsmen[e.batsman_id].out_type;
        }
        else if (e.type == EVT_DEADLOCK_DETECTED) dismissal = "run out (deadlock)";

        std::string phase;
        if      (display_over < 6)  phase = "Powerplay";
        else if (display_over < 11) phase = "Middle";
        else if (display_over < 16) phase = "Middle-late";
        else                        phase = "Death";

        if (log_txt) {
            fprintf(log_txt, "%-14ld  %2d.%-3d  %-20s  %-18s  %-18s  %-16s  %s\n",
                    ts, display_over, display_ball,
                    event_type_name(e.type),
                    bowler_name.c_str(), batsman_name.c_str(), fielder_name.c_str(),
                    e.message.c_str());
            fflush(log_txt);
        }

        if (log_csv) {
            std::string msg = e.message;
            for (size_t p = 0; (p = msg.find('"', p)) != std::string::npos; p += 2)
                msg.replace(p, 1, "\"\"");
            fprintf(log_csv,
                "%ld,%d,%d,%s,"
                "%s,%s,%s,"
                "%d,%d,%d,"
                "%s,%s,"
                "\"%s\"\n",
                ts, display_over, display_ball,
                event_type_name(e.type),
                bowler_name.c_str(), batsman_name.c_str(), fielder_name.c_str(),
                e.runs, g_match.runs, g_match.wickets,
                dismissal.c_str(), phase.c_str(),
                msg.c_str());
            fflush(log_csv);
        }

        if (e.type == EVT_MATCH_OVER) break;
    }

    if (log_txt)  fclose(log_txt);
    if (log_csv)  fclose(log_csv);
    if (log_cmnt) fclose(log_cmnt);
    event_queue_shutdown(&g_event_queue);
    printf("[LOGGER] Thread exiting. Logs written to logs/\n");
    return nullptr;
}
