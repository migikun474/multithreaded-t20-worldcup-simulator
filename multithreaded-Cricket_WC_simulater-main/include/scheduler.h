#pragma once
#include "match_state.h"
#include "player.h"

int scheduler_next_bowler(SchedulerAlgo algo,
                          int current_bowler_idx,
                          Player* bowlers,
                          int num_bowlers,
                          MatchState* ms);

const char* scheduler_algo_name(SchedulerAlgo a);

int scheduler_next_batsman(SchedulerAlgo algo,
                           Player* batsmen,
                           int num_batsmen,
                           MatchState* ms);