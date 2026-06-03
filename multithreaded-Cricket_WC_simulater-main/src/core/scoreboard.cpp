// Handles printing match events and over summaries in a formatted way.
// This module is only responsible for output (no game logic).
#include <stdio.h>
#include <pthread.h>
#include "match_state.h"

extern MatchState g_match;

// Prints a single ball event in scoreboard format.
// Displays over.ball, score, optional badge (like W, 4, 6), and description.
void scoreboard_print_event(int over, int ball,
                             int runs, int wickets,
                             const char* badge,
                             const char* desc) {
// If badge is present, print it in formatted column
    if (badge && badge[0] != '\0') {
        printf("  %2d.%d │ %3d/%-2d │ [%-4s] │ %s\n",
               over, ball, runs, wickets, badge, desc);
    } else {
        printf("  %2d.%d │ %3d/%-2d │        │ %s\n",
               over, ball, runs, wickets, desc);
    }
    fflush(stdout);
}


 //scoreboard_print_over_summary — and end-of-over banner.
void scoreboard_print_over_summary(int over, int over_runs) {
    printf("\n  ┌─ End of Over %-2d ──────────────────────────── +%d runs ─┐\n\n",
           over, over_runs);
    fflush(stdout);
}