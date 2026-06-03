#pragma once


void scoreboard_print_event(int over, int ball,
                             int runs, int wickets,
                             const char* badge,
                             const char* desc);

void scoreboard_print_over_summary(int over, int over_runs);