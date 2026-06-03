#include <iostream>
#include <pthread.h>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>
#include <fstream>
#include "match_state.h"
#include "player.h"
#include "scheduler.h"
#include "pitch_monitor.h"
#include "event.h"

//  Global shared state 
MatchState      g_match;
PitchMonitor    g_pitch;
EventQueue      g_event_queue;       // main queue: all threads → logger
EventQueue      g_commentary_queue;  // commentary pipeline: threads → commentator

//  Thread IDs 
pthread_t bowler_tid;
pthread_t batsman_tids[2];
pthread_t fielder_tids[10];
pthread_t umpire_tid;
pthread_t scheduler_tid;
pthread_t logger_tid;
pthread_t commentator_tid;

//  Player rosters 
Player g_bowlers[5];    // England bowling attack
Player g_batsmen[11];   // India batting order
Player g_fielders[10];  // England fielders
std::string g_batting_team_name = "India";
std::string g_bowling_team_name = "England";
static constexpr int POWERPLAY_OUTSIDE_FIELDER_A = 0; // cover
static constexpr int POWERPLAY_OUTSIDE_FIELDER_B = 8; // square leg

//  Crease-arrival counter (protected by score_mutex) 

// Tracks the batting position (1-based) of the next batsman to walk in.
// Openers are assigned 1 and 2 at init; every new batsman gets the next slot.
int g_crease_counter = 3;  // next incoming batsman will be No.3

//  Forward declarations 
void* bowler_thread_fn(void*);
void* batsman_thread_fn(void*);
void* fielder_thread_fn(void*);
void* umpire_thread_fn(void*);
void* scheduler_thread_fn(void*);
void* logger_thread_fn(void*);
void* commentator_thread_fn(void*);

struct InningsSummary {
    std::string batting_team;
    std::string bowling_team;
    int runs;
    int wickets;
    int total_balls;
};

struct BowlerSeed {
    const char* name;
    int priority;
};

struct BatsmanSeed {
    const char* name;
    int est;
    float avg;
    float sr;
    float pi;
};

struct FielderSeed {
    const char* name;
    float d;
    float a;
    float s;
    FieldZone z;
};

void init_players_for_innings(int innings_number);
void print_banner();
void print_scorecard();
InningsSummary run_innings(int innings_number, SchedulerAlgo algo, int target_runs, bool demo_deadlock_mode);
void print_match_result(const InningsSummary& first, const InningsSummary& second);

static constexpr const char* MATCH_LOG_PATH = "logs/match_log.txt";
static constexpr const char* EVENTS_CSV_PATH = "logs/events.csv";
static constexpr const char* COMMENTARY_LOG_PATH = "logs/commentary_log.txt";

static bool copy_log_file(const char* src, const char* dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "[MAIN] Failed to open source log for copy: " << src << "\n";
        return false;
    }
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[MAIN] Failed to open destination log for copy: " << dst << "\n";
        return false;
    }
    out << in.rdbuf();
    if (!out.good()) {
        std::cerr << "[MAIN] Failed while copying log contents from " << src
                  << " to " << dst << "\n";
        return false;
    }
    return true;
}

static bool run_simulation_and_copy_logs(const char* exe_path, const char* flag, const char* suffix) {
    pid_t pid = fork();
    if (pid < 0) {
        std::perror("fork");
        return false;
    }
    if (pid == 0) {
        execl(exe_path, exe_path, flag, (char*)nullptr);
        std::perror("execl");
        _exit(1);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        std::perror("waitpid");
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;

    const std::string sfx = std::string("_") + suffix;
    bool ok = true;
    ok &= copy_log_file(MATCH_LOG_PATH, ("logs/match_log" + sfx + ".txt").c_str());
    ok &= copy_log_file(EVENTS_CSV_PATH, ("logs/events" + sfx + ".csv").c_str());
    ok &= copy_log_file(COMMENTARY_LOG_PATH, ("logs/commentary_log" + sfx + ".txt").c_str());
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc == 1) {
        std::cout << "[MAIN] Running FCFS, SJF and Priority full-match simulations...\n";
        bool ok_fcfs = run_simulation_and_copy_logs(argv[0], "--fcfs", "fcfs");
        bool ok_sjf  = run_simulation_and_copy_logs(argv[0], "--sjf", "sjf");
        bool ok_prio = run_simulation_and_copy_logs(argv[0], "--priority", "priority");
        if (!ok_fcfs || !ok_sjf || !ok_prio) {
            std::cerr << "[MAIN] Failed to complete dual-run simulation/log capture.\n";
            return 1;
        }
        std::cout << "[MAIN] Generated logs/events_fcfs.csv, logs/events_sjf.csv and logs/events_priority.csv\n";
        if (remove("./logs/match_log.txt") == 0) {
            std::cout << "[MAIN] Deleted match_log.txt\n";
        } else {
            perror("[MAIN] Failed to delete match_log.txt");
        }

        if (remove("./logs/commentary_log.txt") == 0) {
            std::cout << "[MAIN] Deleted commentary_log.txt\n";
        } else {
            perror("[MAIN] Failed to delete commentary_log.txt");
        }

        if (remove("./logs/events.csv") == 0) {
            std::cout << "[MAIN] Deleted events.csv\n";
        } else {
            perror("[MAIN] Failed to delete events.csv");
        }
        return 0;
    }

    srand((unsigned)time(nullptr));

    SchedulerAlgo algo = ALGO_FCFS;
    bool demo_deadlock_mode = false;
    if (argc >= 2) {
        std::string flag(argv[1]);
        if      (flag == "--fcfs")       algo = ALGO_FCFS;
        else if (flag == "--sjf")        algo = ALGO_SJF;
        else if (flag == "--priority")   algo = ALGO_PRIORITY;
        else if (flag == "--deadlock-demo") {
            algo = ALGO_FCFS;
            demo_deadlock_mode = true;
        }
    }

    print_banner();
    InningsSummary first = run_innings(1, algo, 0, demo_deadlock_mode);
    InningsSummary second = run_innings(2, algo, first.runs + 1, demo_deadlock_mode);
    print_match_result(first, second);
    return 0;
}

InningsSummary run_innings(int innings_number, SchedulerAlgo algo, int target_runs, bool demo_deadlock_mode) {
    g_crease_counter = 3;

    match_state_init(&g_match);
    g_match.innings_number = innings_number;
    g_match.scheduler_algo = algo;
    g_match.demo_deadlock_mode = demo_deadlock_mode;
    g_match.target_runs = target_runs;
    match_state_deadlock_register_thread(&g_match, DL_THREAD_MAIN, "MAIN", 0);
    pitch_monitor_init(&g_pitch);
    event_queue_init(&g_event_queue);
    event_queue_init(&g_commentary_queue);
    init_players_for_innings(innings_number);

    // Seed opening bowler as the highest-priority option so powerplay quota
           int top_idx = 0;
    // accounting can satisfy 2 overs for top + 2 overs for second in overs 1-6.
   
    for (int i = 1; i < 5; ++i) {
        if (g_bowlers[i].priority > g_bowlers[top_idx].priority) top_idx = i;
    }
    int second_idx = -1;
    for (int i = 0; i < 5; ++i) {
        if (i == top_idx) continue;
        if (second_idx < 0 || g_bowlers[i].priority > g_bowlers[second_idx].priority) second_idx = i;
    }
    g_match.current_bowler_idx = top_idx;
    g_match.death_over_top_idx = top_idx;
    g_match.death_over_second_idx = (second_idx >= 0) ? second_idx : top_idx;


    if (g_match.demo_deadlock_mode) {
        std::cout << "[MAIN] DEADLOCK DEMO MODE enabled (intentional lock-order inversion)\n\n";
    }

    std::cout << "\n[MAIN] Innings " << innings_number << ": "
              << g_batting_team_name << " batting, "
              << g_bowling_team_name << " bowling";
    if (target_runs > 0) {
        std::cout << " | Target: " << target_runs;
    }
    std::cout << "\n";

    //  Launch all threads 
    pthread_create(&logger_tid,      nullptr, logger_thread_fn,      nullptr);
    pthread_create(&commentator_tid, nullptr, commentator_thread_fn, nullptr);
    pthread_create(&umpire_tid,      nullptr, umpire_thread_fn,      nullptr);
    pthread_create(&scheduler_tid,   nullptr, scheduler_thread_fn,   nullptr);

    int fielder_ids[10];
    for (int i = 0; i < 10; ++i) {
        fielder_ids[i] = i;
        pthread_create(&fielder_tids[i], nullptr, fielder_thread_fn, &fielder_ids[i]);
    }

    int bat_ids[2] = {0, 1};
    pthread_create(&batsman_tids[0], nullptr, batsman_thread_fn, &bat_ids[0]);
    pthread_create(&batsman_tids[1], nullptr, batsman_thread_fn, &bat_ids[1]);

    pthread_create(&bowler_tid, nullptr, bowler_thread_fn, nullptr);

    //  Wait for innings to end ─
    pthread_join(bowler_tid, nullptr);
    match_state_signal_end(&g_match);

    pthread_join(batsman_tids[0], nullptr);
    pthread_join(batsman_tids[1], nullptr);
    for (int i = 0; i < 10; ++i) pthread_join(fielder_tids[i], nullptr);
    pthread_join(scheduler_tid,   nullptr);
    pthread_join(commentator_tid, nullptr);
    pthread_join(logger_tid,      nullptr);
    match_state_request_umpire_exit(&g_match);
    pthread_join(umpire_tid,      nullptr);

    print_scorecard();

    InningsSummary summary{
        g_batting_team_name,
        g_bowling_team_name,
        g_match.runs,
        g_match.wickets,
        g_match.total_balls
    };

    match_state_deadlock_unregister_thread(&g_match, DL_THREAD_MAIN);
    match_state_destroy(&g_match);
    pitch_monitor_destroy(&g_pitch);
    event_queue_destroy(&g_event_queue);
    event_queue_destroy(&g_commentary_queue);
    return summary;
}

//  Player initialisation 
void init_players_for_innings(int innings_number) {
    g_batting_team_name = (innings_number == 1) ? "India" : "England";
    g_bowling_team_name = (innings_number == 1) ? "England" : "India";

    const BowlerSeed eng_bowlers[] = {
        { "J.Anderson",   3 },
        { "J.Archer",     5 },
        { "A.Rashid",     4 },
        { "M.Wood",       4 },
        { "S.Curran",     3 },
    };
    const BowlerSeed ind_bowlers[] = {
        { "J.Bumrah",    5 },
        { "Arshdeep",   4 },
        { "H.Pandya",   3 },
        { "R.Jadeja",   4 },
        { "A.Patel",    3 },
    };
    const BowlerSeed* active_bowlers = (innings_number == 1) ? eng_bowlers : ind_bowlers;
    for (int i = 0; i < 5; ++i) {
        player_init(&g_bowlers[i], i, active_bowlers[i].name, ROLE_BOWLER);
        g_bowlers[i].priority = active_bowlers[i].priority;
    }

    //  India batting order — with full skill profiles 
    //          name            est_dur  bat_avg        sr     power
    const BatsmanSeed ind[] = {
        { "R.Sharma",   45,  30.0f, 135.0f, 8.0f },   //  0 – opener/captain
        { "I.Kishan",     42,  38.0f, 145.0f, 7.0f },   //  1 – opener
        { "V.Kohli",    50,  55.0f, 145.0f, 7.0f },   //  2 – #3 Anchor 
        { "S.Iyer",     30,  32.0f, 150.0f, 8.0f },   //  3 – #4
        { "S.K.Yadav",  28,  28.0f, 170.0f, 9.0f },   //  4 – SKY (big hitter)
        { "H.Pandya",   22,  25.0f, 155.0f, 8.0f },   //  5 – allrounder
        { "R.Pant",     20,  22.0f, 160.0f, 9.0f },   //  6 – WK-bat (aggressive)
        { "R.Jadeja",   14,  15.0f, 130.0f, 5.0f },   //  7 – allrounder
        { "A.Patel",    9,  12.0f, 110.0f, 4.0f },   //  8 – lower order
        { "Arshdeep",    5,   8.0f, 100.0f, 3.0f },   //  9 – #10
        { "J.Bumrah",    3,   5.0f,  90.0f, 2.0f },   // 10 – #11
    };
    const BatsmanSeed eng[] = {
        { "J.Roy",        38,  31.0f, 145.0f, 8.0f },
        { "J.Bairstow",   42,  35.0f, 150.0f, 8.0f },
        { "D.Malan",     45,  38.0f, 135.0f, 6.0f },
        { "J.Root",      40,  37.0f, 130.0f, 5.0f },
        { "E.Morgan",    30,  28.0f, 140.0f, 8.0f },
        { "B.Stokes",    28,  30.0f, 145.0f, 8.0f },
        { "L.Livingston",20,  22.0f, 165.0f, 9.0f },
        { "M.Ali",       18,  21.0f, 145.0f, 7.0f },
        { "S.Curran",    12,  16.0f, 125.0f, 5.0f },
        { "J.Archer",     7,  10.0f, 105.0f, 4.0f },
        { "A.Rashid",     5,   8.0f,  95.0f, 3.0f },
    };
    const BatsmanSeed* active_batsmen = (innings_number == 1) ? ind : eng;
    for (int i = 0; i < 11; ++i) {
        player_init(&g_batsmen[i], i, active_batsmen[i].name, ROLE_BATSMAN);
        g_batsmen[i].estimated_duration = active_batsmen[i].est;
        g_batsmen[i].bat_avg = active_batsmen[i].avg;
        g_batsmen[i].strike_rate = active_batsmen[i].sr;
        g_batsmen[i].power_index = active_batsmen[i].pi;
        g_batsmen[i].called_up = false;
    }

    g_batsmen[0].called_up = true;
    g_batsmen[1].called_up = true;
    g_batsmen[0].crease_order = 1;
    g_batsmen[1].crease_order = 2;

    //  England fielders — with skill profiles and zone assignments 
    //            name            dive      acc   speed      zone
    const FielderSeed fld[] = {
        { "J.Roy",        7.0f, 7.0f, 8.0f, ZONE_COVER       },  // 0 – deep cover
        { "J.Bairstow",   6.0f, 8.0f, 7.0f, ZONE_MID_OFF     },  // 1 – WK (off side)
        { "B.Stokes",     9.0f, 9.0f, 8.0f, ZONE_THIRD_MAN   },  // 2 – slip/third man
        { "J.Root",       7.0f, 7.0f, 7.0f, ZONE_POINT       },  // 3 – gully/point
        { "E.Morgan",     5.0f, 6.0f, 6.0f, ZONE_MIDWICKET   },  // 4 – midwicket
        { "L.Livingston", 8.0f, 8.0f, 8.0f, ZONE_MID_ON      },  // 5 – long-on
        { "D.Malan",      6.0f, 7.0f, 6.0f, ZONE_MID_OFF     },  // 6 – mid-off
        { "M.Ali",        7.0f, 7.0f, 7.0f, ZONE_FINE_LEG    },  // 7 – short fine-leg
        { "T.Banton",     8.0f, 8.0f, 9.0f, ZONE_SQUARE_LEG  },  // 8 – deep sq-leg
        { "O.Robinson",   5.0f, 6.0f, 5.0f, ZONE_THIRD_MAN   },  // 9 – third man
    };
    const FielderSeed ind_fld[] = {
        { "R.Sharma",   6.0f, 7.0f, 6.0f, ZONE_COVER      },
        { "I.Kishan",   7.0f, 8.0f, 8.0f, ZONE_POINT      },
        { "V.Kohli",    8.0f, 9.0f, 8.0f, ZONE_MIDWICKET  },
        { "S.Iyer",     6.0f, 7.0f, 7.0f, ZONE_MID_OFF    },
        { "S.K.Yadav",  8.0f, 8.0f, 8.0f, ZONE_SQUARE_LEG },
        { "H.Pandya",   7.0f, 8.0f, 8.0f, ZONE_MID_ON     },
        { "R.Pant",     7.0f, 8.0f, 7.0f, ZONE_FINE_LEG   },
        { "R.Jadeja",   9.0f, 9.0f, 8.0f, ZONE_COVER      },
        { "A.Patel",    7.0f, 8.0f, 7.0f, ZONE_THIRD_MAN  },
        { "Arshdeep",   6.0f, 7.0f, 6.0f, ZONE_FINE_LEG   },
    };
    const FielderSeed* active_fielders = (innings_number == 1) ? fld : ind_fld;
    for (int i = 0; i < 10; ++i) {
        player_init(&g_fielders[i], i, active_fielders[i].name, ROLE_FIELDER);
        g_fielders[i].dive_ability = active_fielders[i].d;
        g_fielders[i].accuracy     = active_fielders[i].a;
        g_fielders[i].speed        = active_fielders[i].s;
        g_fielders[i].field_zone   = active_fielders[i].z;
        g_fielders[i].in30YardZone = true;
    }
    // Powerplay setup: 8 in-circle, 2 outside the 30-yard circle.
    // Keep one outside in each half of the field for realistic initial spread.
    g_fielders[POWERPLAY_OUTSIDE_FIELDER_A].in30YardZone = false;
    g_fielders[POWERPLAY_OUTSIDE_FIELDER_B].in30YardZone = false;
}

//  Print banner ─
void print_banner() {
    std::cout
        << "═══════════════════════════════════════════════════════════\n"
        << "   T20 WORLD CUP 2026  —  India  vs  England\n"
        << "   Full T20 Match  |  Concurrent Match Engine (pthreads)\n"
        << "   OS Assignment (CSC-204)  |  Two innings chase simulation\n"
        << "═══════════════════════════════════════════════════════════\n\n";
}

//  Final scorecard ─
void print_scorecard() {
    std::cout
        << "\n═══════════════════════════════════════════════════════════\n"
        << "                  " << g_batting_team_name << " INNINGS SCORECARD\n"
        << "═══════════════════════════════════════════════════════════\n";

    match_state_deadlock_note_request(&g_match, DL_THREAD_MAIN, DL_RES_SCORE_MUTEX, 1);
    pthread_mutex_lock(&g_match.score_mutex);
    match_state_deadlock_note_grant(&g_match, DL_THREAD_MAIN, DL_RES_SCORE_MUTEX, 1);
    int fr = g_match.runs, fw = g_match.wickets;

    match_state_deadlock_note_release(&g_match, DL_THREAD_MAIN, DL_RES_SCORE_MUTEX, 1);
    pthread_mutex_unlock(&g_match.score_mutex);

    printf("\n  BATTING — %s\n", g_batting_team_name.c_str());
    printf("  %-16s %4s %4s %3s %3s %5s Dismissal\n", "Player","R","B","4s","6s", "SR");
    printf("  %s\n", std::string(60,'-').c_str());

    /* Build a sorted list of indices by crease_order (actual walk-in sequence) */
    int order[11];
    int n_batted = 0;
    for (int i = 0; i < 11; ++i) {
        if (g_batsmen[i].crease_order > 0)
            order[n_batted++] = i;
    }
    /* Insertion-sort by crease_order (at most 11 elements) */
    for (int a = 1; a < n_batted; ++a) {
        int key = order[a], b = a - 1;
        while (b >= 0 && g_batsmen[order[b]].crease_order > g_batsmen[key].crease_order) {
            order[b + 1] = order[b];
            --b;
        }
        order[b + 1] = key;
    }

    for (int a = 0; a < n_batted; ++a) {
        Player& p = g_batsmen[order[a]];
        if (p.balls_faced > 0 || p.is_out) {
            printf("  %-16s %4d %4d %3d %3d %5.1f  %s\n",
                   p.name.c_str(), p.runs_scored, p.balls_faced,
                   p.fours, p.sixes, ((double)(p.runs_scored)/(p.balls_faced))*100,
                   p.is_out ? p.out_type.c_str() : "not out");
        }
    }

    printf("\n  BOWLING — %s\n", g_bowling_team_name.c_str());
    printf("  %-16s %5s %4s %3s %6s\n", "Bowler","Overs","Runs","Wkts","Econ");
    printf("  %s\n", std::string(46,'-').c_str());
    for (int i = 0; i < 5; ++i) {
        Player& b = g_bowlers[i];
        if (b.balls_bowled > 0) {
            int full = b.balls_bowled / 6, part = b.balls_bowled % 6;
            float econ = full > 0 ? (float)b.runs_given / full : 0.0f;
            printf("  %-16s  %d.%d  %4d %3d  %5.1f\n",
                   b.name.c_str(), full, part,
                   b.runs_given, b.wickets_taken, econ);
        }
    }

    // Derive display overs reliably from total_balls
    // e.g. 120 balls -> 20.0, 73 balls -> 12.1
    int total_b    = g_match.total_balls;
    int display_over = total_b / 6;
    int display_ball = total_b % 6;
    printf("\n  Total: %d/%d  in %d.%d overs\n", fr, fw, display_over, display_ball);
    if (g_match.target_runs > 0) {
        printf("  Target: %d\n", g_match.target_runs);
    }
    std::cout << "═══════════════════════════════════════════════════════════\n";
}

void print_match_result(const InningsSummary& first, const InningsSummary& second) {
    std::cout << "\n═══════════════════════════════════════════════════════════\n"
              << "                      MATCH RESULT\n"
              << "═══════════════════════════════════════════════════════════\n";
    if (second.runs >= first.runs + 1) {
        std::cout << "  " << second.batting_team << " won by "
                  << (10 - second.wickets) << " wicket"
                  << ((10 - second.wickets) == 1 ? "" : "s") << ".\n";
    } else if (second.runs == first.runs) {
        std::cout << "  Match tied.\n";
    } else {
        std::cout << "  " << first.batting_team << " won by "
                  << (first.runs - second.runs) << " run"
                  << ((first.runs - second.runs) == 1 ? "" : "s") << ".\n";
    }
    std::cout << "  " << first.batting_team << ": " << first.runs << "/" << first.wickets
              << "  |  " << second.batting_team << ": " << second.runs << "/" << second.wickets
              << "\n═══════════════════════════════════════════════════════════\n";
}
