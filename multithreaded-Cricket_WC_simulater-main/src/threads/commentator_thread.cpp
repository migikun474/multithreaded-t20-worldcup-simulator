#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include "event.h"
#include "match_state.h"
#include "player.h"

extern EventQueue g_commentary_queue;   // reads from here
extern EventQueue g_event_queue;        // pushes EVT_COMMENTARY here for logger
extern MatchState g_match;
extern Player     g_batsmen[];
extern Player     g_bowlers[];
extern Player     g_fielders[];

/*
 * commentator_thread.cpp
 *
 * Consumes events from g_commentary_queue (a dedicated queue fed by bowler,
 * batsman, and fielder threads). For each event it generates rich, varied
 * commentary text and pushes an EVT_COMMENTARY event to g_event_queue,
 * which the logger writes to logs/commentary_log.txt.
 *
 * OS concept: Producer-consumer with a second, specialised message queue.
 * The commentator acts as both consumer (from g_commentary_queue) and
 * producer (into g_event_queue) demonstrating pipeline message-passing.
 */

/* ── Template banks ──────────────────────────────────────────────────────── */

static const char* dot_ball_lines[] = {
    "Defended solidly — the pressure mounts with that dot ball!",
    "Beaten outside off! The bowler is pumped up!",
    "Plays and misses — the crowd goes quiet for a moment.",
    "Excellent line and length, nothing to hit there — good bowling!",
    "Backs away but can't connect — the fielding side sensing an opportunity.",
    "Blocked back down the pitch. No run. It's a duel out here!",
};
static const int dot_ball_n = 6;

static const char* single_lines[] = {
    "Cleverly rotated — keeps the scoreboard ticking!",
    "A nudge into the gap, quick single taken, good running between the wickets.",
    "Soft hands, drops it and calls — sharp cricket!",
    "Worked to the leg side for one — the non-striker was already moving!",
    "Punched off the back foot, they jog through for one.",
};
static const int single_n = 5;

static const char* double_lines[] = {
    "Driven firmly, two fielders converge but the batsmen get back for two!",
    "Excellent running — they've pushed hard and turned one into two!",
    "Plays it wide of mid-off and comes back for the second — good cricket.",
    "The outfield is quick today — they pick up two with ease.",
};
static const int double_n = 4;

static const char* four_lines[] = {
    "FOUR! That's creamed through the gap — nothing the fielder could do!",
    "FOUR! Textbook shot — the crowd erupts!",
    "FOUR! Picks the gap to perfection — the fielder doesn't even move!",
    "FOUR! What timing! The ball races to the boundary rope!",
    "FOUR! Oh that's effortless — pure class from the batsman!",
    "FOUR! Down to fine leg in a flash — the fielder had no chance!",
};
static const int four_n = 6;

static const char* six_lines[] = {
    "SIX! GONE! That's into the second tier — absolutely massive!",
    "SIX! That ball is still travelling! What a hit!",
    "SIX! Cleared the boundary with ease — the stadium is on its feet!",
    "SIX! MAXIMUM! He's smashed it out of the ground!",
    "SIX! Picked the length early and dispatched it with contempt!",
    "SIX! That's the big shot the crowd has been waiting for!",
};
static const int six_n = 6;

static const char* ball_in_air_lines[] = {
    "It's in the air! The fielder sprints — this could be a wicket!",
    "UP! UP! UP! The ball is soaring — can anyone get under it?",
    "High into the sky — the fielders are converging, this is tense!",
    "The batsman has gone for it — lofted shot, ball in the air!",
    "Aerial delivery — every fielder in that zone is on the move!",
};
static const int air_n = 5;

static const char* catch_lines[] = {
    "CAUGHT! Brilliant! That's an absolute screamer of a catch!",
    "CAUGHT! Pouched it! Didn't even have to move — sure hands!",
    "CAUGHT! He saw it all the way and took it comfortably. OUT!",
    "CAUGHT! Dives forward and takes a blinder — sensational fielding!",
    "CAUGHT! The fielder times the jump perfectly — what a grab!",
};
static const int catch_n = 5;

static const char* dropped_lines[] = {
    "DROPPED! Oh no — that's an absolute sitter and he's put it down!",
    "DROPPED! The crowd groans — that should have been out! A life for the batsman!",
    "DROPPED! Went up for it, lost it in the lights — costly mistake!",
    "DROPPED! The fielder dives but it goes right through the fingers!",
};
static const int dropped_n = 4;

static const char* run_out_attempt_lines[] = {
    "Direct hit! Was he in? The umpire checks... the drama!",
    "Bullet throw to the keeper — it's a close call at the crease!",
    "Snap throw, slide, DIVE — what a run out attempt!",
    "Sprint, gather, throw — superb ground fielding under pressure!",
};
static const int runout_n = 4;

static const char* wicket_extra_lines[] = {
    "The batsman has to go — what a blow for the batting side!",
    "The dressing room shakes — that's a massive wicket!",
    "New batter walking in — can they steady the ship?",
    "WICKET! The bowling side is absolutely fired up!",
};
static const int wicket_extra_n = 4;

static const char* wide_lines[] = {
    "Wide! Too far outside off — the umpire is unmoved, arm goes out.",
    "Wide called! The bowler will be unhappy with that — free run.",
    "Straying down the leg side — Wide! Extra run on the board.",
};
static const int wide_n = 3;

static const char* noball_lines[] = {
    "No Ball! The front foot is well over the crease — free hit coming up!",
    "No Ball called! The batting team will love that — free hit next ball!",
    "Overstepped! The umpire calls No Ball — an extra run and a free hit.",
};
static const int noball_n = 3;

static const char* bowled_extra[] = {
    "The stumps are shattered — bowled him! What a delivery!",
    "Off stump out of the ground — beaten all ends up!",
    "Clean bowled! The batsman was all at sea with that one!",
};
static const int bowled_extra_n = 3;

static const char* lbw_extra[] = {
    "Plumb! That was always hitting leg stump — no doubt about it!",
    "The finger goes up! LBW — the batsman looks shell-shocked.",
    "LBW! Trapped in front — the ball would have hit leg and middle!",
};
static const int lbw_extra_n = 3;

static const char* stumped_extra[] = {
    "Miles out of his crease — the keeper whips the bails off in a flash!",
    "STUMPED! Danced down and missed — the keeper was waiting!",
    "The batsman had no idea where the ball was — stumped easily!",
};
static const int stumped_extra_n = 3;

static const char* ball_bowled_lines[] = {
    "Here comes the bowler — steaming in hard!",
    "Running in to bowl — the batsman shapes up.",
    "The bowler marks his run-up — the field is set.",
    "Comes in off the long run — let's see what he has for us.",
    "Back of a length delivery on the way — this could be interesting!",
};
static const int bowled_line_n = 5;

static const char* overthrow_lines[] = {
    "OVERTHROW! The fielder's throw has gone astray — extra runs on the board!",
    "OVERTHROW! Misfield in the deep — the batsmen capitalise immediately!",
    "OVERTHROW! Poor throw from the fielder and the ball races to the boundary!",
    "OVERTHROW! The fielder panicked under pressure — extra runs gifted away!",
    "OVERTHROW! Bad communication in the field — the batting side won't complain!",
    "OVERTHROW! The throw was off-target — chaos in the field, runs being taken!",
};
static const int overthrow_n = 6;

/* ── Helper ──────────────────────────────────────────────────────────────── */

/* ── Helper ──────────────────────────────────────────────────────────────── */
static const char* rnd(const char** arr, int n) {
    return arr[rand() % n];
}

static void push_commentary(const std::string& text, int over, int ball) {
    Event e{};
    e.type    = EVT_COMMENTARY;
    e.over    = over;
    e.ball    = ball;
    e.message = text;
    event_queue_push(&g_event_queue, e);
}

/* ── Main thread function ─────────────────────────────────────────────────── */
void* commentator_thread_fn(void* /*arg*/) {
    printf("[COMMENTATOR] Thread started. Commentary being generated.\n");

    while (true) {
        Event e = event_queue_pop(&g_commentary_queue);  // blocks

        // Shutdown signal (queue shutdown throws, or match over event)
        if (e.type == EVT_MATCH_OVER) break;

        std::string batsman_name = (e.batsman_id >= 0 && e.batsman_id < 11)
            ? g_batsmen[e.batsman_id].name : "Batsman";
        std::string fielder_name = (e.fielder_id >= 0 && e.fielder_id < 10)
            ? g_fielders[e.fielder_id].name : "";
        std::string bowler_name  = (e.bowler_id  >= 0 && e.bowler_id  < 5)
            ? g_bowlers[e.bowler_id].name  : "";

        std::string zone_str = field_zone_name(e.ball_zone);
        std::string commentary;

        switch (e.type) {

            case EVT_BALL_BOWLED:
                commentary = "[" + bowler_name + " → " + batsman_name + "] " +
                             rnd(ball_bowled_lines, bowled_line_n);
                break;

            case EVT_RUNS_SCORED:
                if (e.runs == 0) {
                    commentary = "[" + batsman_name + "] " +
                                 rnd(dot_ball_lines, dot_ball_n);
                } else if (e.runs == 1) {
                    commentary = "[" + batsman_name + "] " +
                                 rnd(single_lines, single_n);
                } else if (e.runs == 2) {
                    commentary = "[" + batsman_name + "] " +
                                 rnd(double_lines, double_n);
                } else {
                    commentary = "[" + batsman_name + "] " +
                                 rnd(dot_ball_lines, dot_ball_n);
                }
                break;

            case EVT_FOUR:
                if (!fielder_name.empty() && e.fielder_id >= 0) {
                    // Fielder was involved — dropped catch at boundary
                    commentary = "[" + batsman_name + " c " + fielder_name + " (dropped)] " +
                                 rnd(dropped_lines, dropped_n);
                } else {
                    commentary = "[" + batsman_name + "] " +
                                 rnd(four_lines, four_n);
                }
                break;

            case EVT_SIX:
                commentary = "[" + batsman_name + "] " +
                             rnd(six_lines, six_n);
                if (!fielder_name.empty())
                    commentary += " " + fielder_name + " can only watch from the boundary!";
                break;

            case EVT_BALL_IN_AIR:
                commentary = "[" + batsman_name + "] " +
                             rnd(ball_in_air_lines, air_n) +
                             " Ball heading towards " + zone_str + "!";
                break;

            case EVT_CATCH_OUT:
                commentary = "[" + batsman_name + " c " + fielder_name + "] " +
                             rnd(catch_lines, catch_n) + " " +
                             rnd(wicket_extra_lines, wicket_extra_n);
                break;

            case EVT_RUN_OUT:
                commentary = "[" + batsman_name + " run out " + fielder_name + "] " +
                             rnd(run_out_attempt_lines, runout_n) + " " +
                             rnd(wicket_extra_lines, wicket_extra_n);
                break;

            case EVT_BATSMAN_OUT: {
                // Figure out dismissal type from player struct
                std::string dtype = (e.batsman_id >= 0 && e.batsman_id < 11)
                    ? g_batsmen[e.batsman_id].out_type : "";
                if (dtype == "bowled")
                    commentary = "[" + batsman_name + " b " + bowler_name + "] " +
                                 rnd(bowled_extra, bowled_extra_n) + " " +
                                 rnd(wicket_extra_lines, wicket_extra_n);
                else if (dtype == "lbw")
                    commentary = "[" + batsman_name + " lbw b " + bowler_name + "] " +
                                 rnd(lbw_extra, lbw_extra_n) + " " +
                                 rnd(wicket_extra_lines, wicket_extra_n);
                else if (dtype == "stumped")
                    commentary = "[" + batsman_name + " st] " +
                                 rnd(stumped_extra, stumped_extra_n) + " " +
                                 rnd(wicket_extra_lines, wicket_extra_n);
                else
                    commentary = "[" + batsman_name + " OUT] " +
                                 rnd(wicket_extra_lines, wicket_extra_n);
                break;
            }

            case EVT_WIDE:
                commentary = "[" + bowler_name + "] " +
                             rnd(wide_lines, wide_n);
                break;

            case EVT_NO_BALL:
                commentary = "[" + bowler_name + "] " +
                             rnd(noball_lines, noball_n);
                break;

         case EVT_OVERTHROW:
                commentary = "[" + fielder_name + " → OVERTHROW] " +
                             rnd(overthrow_lines, overthrow_n) +
                             " " + std::to_string(e.runs) +
                             (e.runs == 1 ? " extra run." : " extra runs.");
                break;

            case EVT_DEADLOCK_DETECTED:
                commentary = "CHAOS on the pitch! Both batsmen were mid-pitch — "
                             "the third umpire has intervened! " +
                             batsman_name + " is given run out after a mix-up!";
                break;

            default:
                // Skip events we don't generate commentary for
                continue;

        }

        if (!commentary.empty()) {
            push_commentary(commentary, e.over, e.ball);
        }
    }

    printf("[COMMENTATOR] Thread exiting.\n");
    return nullptr;
}