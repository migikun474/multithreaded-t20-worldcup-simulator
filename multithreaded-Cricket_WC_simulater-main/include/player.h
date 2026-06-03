#pragma once
#include <string>

typedef enum { ROLE_BOWLER = 0, ROLE_BATSMAN, ROLE_FIELDER } PlayerRole;

typedef enum {
    ZONE_FINE_LEG    = 0,
    ZONE_SQUARE_LEG  = 1,
    ZONE_MIDWICKET   = 2,
    ZONE_MID_ON      = 3,
    ZONE_MID_OFF     = 4,
    ZONE_COVER       = 5,
    ZONE_POINT       = 6,
    ZONE_THIRD_MAN   = 7,
    ZONE_COUNT       = 8
} FieldZone;

inline const char* field_zone_name(FieldZone z) {
    switch (z) {
        case ZONE_FINE_LEG:   return "fine leg";
        case ZONE_SQUARE_LEG: return "square leg";
        case ZONE_MIDWICKET:  return "midwicket";
        case ZONE_MID_ON:     return "mid-on";
        case ZONE_MID_OFF:    return "mid-off";
        case ZONE_COVER:      return "cover";
        case ZONE_POINT:      return "point";
        case ZONE_THIRD_MAN:  return "third man";
        default:              return "unknown";
    }
}

/* Returns true if zone a and b are the same or adjacent (±1 in circular order) */
inline bool zones_are_adjacent(FieldZone a, FieldZone b) {
    if (a == b) return true;
    int diff = (int)a - (int)b;
    if (diff < 0) diff = -diff;
    return (diff == 1) || (diff == (int)(ZONE_COUNT - 1));
}

typedef struct {
    int         id;
    std::string name;
    PlayerRole  role;
    bool called_up = false  ;

    int runs_scored;
    int balls_faced;
    int fours;
    int sixes;
    bool is_out;
    
    std::string out_type;

    int balls_bowled;
    int runs_given;
    int wickets_taken;
    int overs_bowled;

    int priority;
    int estimated_duration;

    int crease_order;   // 0 = not yet walked in; set when player takes crease

    float bat_avg;      // career batting average  (e.g. 45.0)
    float strike_rate;  // career strike rate      (e.g. 140.0)
    float power_index;  // power-hitting ability   0–10

    float dive_ability; // agility / dive ability  0–10
    float accuracy;     // throwing/catch accuracy  0–10
    float speed;        // ground speed / agility   0–10

    FieldZone field_zone;
    bool in30YardZone;
} Player;

void player_init(Player* p, int id, const char* name, PlayerRole role);
