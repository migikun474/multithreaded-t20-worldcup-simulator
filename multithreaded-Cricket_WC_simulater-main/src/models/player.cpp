#include <stdio.h>
#include "player.h"

void player_init(Player* p, int id, const char* name, PlayerRole role) {
    p->id               = id;
    p->name             = name;
    p->role             = role;
    p->runs_scored      = 0;
    p->balls_faced      = 0;
    p->fours            = 0;
    p->sixes            = 0;
    p->is_out           = false;
    p->out_type         = "";
    p->balls_bowled     = 0;
    p->runs_given       = 0;
    p->wickets_taken    = 0;
    p->overs_bowled     = 0;
    p->priority         = (role == ROLE_BOWLER) ? (5 - id) : id;
    p->estimated_duration = (role == ROLE_BATSMAN) ? (30 - id * 2) : 0;

    p->bat_avg      = 20.0f;
    p->strike_rate  = 120.0f;
    p->power_index  = 5.0f;
    p->dive_ability = 5.0f;
    p->accuracy     = 5.0f;
    p->speed        = 5.0f;
    p->field_zone   = ZONE_MID_ON;
    p->in30YardZone = true;
}

