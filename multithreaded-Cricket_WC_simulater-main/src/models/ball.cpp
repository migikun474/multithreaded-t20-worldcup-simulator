#include <stdio.h>
#include <stdlib.h>
#include "player.h"

typedef enum {
    BALL_DOT = 0,
    BALL_GROUNDED,     
    BALL_WELL_TIMED,   
    BALL_AERIAL,
    BALL_BOWLED,
    BALL_WIDE,
    BALL_NO_BALL,
    BALL_LBW,
    BALL_STUMPED
} BallOutcome;

inline const char* ball_outcome_name(BallOutcome o) {
    switch (o) {
        case BALL_DOT:     return "Dot ball";
        case BALL_GROUNDED: return "Grounded shot";
        case BALL_WELL_TIMED: return "Well-timed shot";
        case BALL_AERIAL:  return "Aerial — fielders scramble!";
        case BALL_BOWLED:  return "BOWLED OUT!";
        case BALL_WIDE:    return "Wide";
        case BALL_NO_BALL: return "No Ball";
        case BALL_LBW:     return "LBW!";
        case BALL_STUMPED: return "STUMPED!";
    }
    return "?";
}

BallOutcome ball_generate_outcome(int match_intensity, const Player* batsman) {
    //  Base weights (sum ≈ 104):
    //  DOT   1s   2s   3s   4s   6s  AIR  BOWL  WIDE  NOBALL  LBW  STUMP
    // DOT, GROUND, TIMED, AERIAL, BOWLED, WIDE, NOBALL, LBW, STUMPED
    int w[] = { 30, 36, 18, 8, 2, 4, 3, 2, 1 };
    const int n = 9;

    // ─Death-over boundary boost 
    // Death overs → more attacking shots
    w[2] += match_intensity;  // WELL_TIMED
    w[3] += (match_intensity >= 7) ? 1 : 0;  // slight aerial boost in attacking phases
    w[0] -= (match_intensity / 2);
    if (w[0] < 5) w[0] = 5;

    if (batsman != nullptr) {
        float sr  = batsman->strike_rate;
        float pi  = batsman->power_index;
        float avg = batsman->bat_avg;
        int   idx = batsman->id;

        // Strike rate → more grounded + attacking
        if (sr >= 160.0f) {
            w[2] += 6; w[1] += 4; w[0] -= 4;
        } else if (sr >= 140.0f) {
            w[2] += 3; w[1] += 2; w[0] -= 2;
        } else if (sr < 110.0f) {
            w[2] -= 2; w[0] += 4;
        }

        // Power → affects WELL_TIMED + aerial
        if (pi >= 8.0f) {
            w[2] += 5; w[3] += 2;
        } else if (pi >= 6.0f) {
            w[2] += 2; w[3] += 1;
        } else if (pi < 4.0f) {
            w[2] -= 2; w[3] -= 1;
        }

        // Average → wicket resistance
        if (avg >= 45.0f) {
            w[4] -= 2; w[7] -= 2; w[8] -= 1;
        } else if (avg < 15.0f) {
            w[4] += 3; w[7] += 2; w[8] += 1;
        }

        // Clamp wicket weights after avg adjustment
        if (w[7]  < 1) w[7]  = 1;

        //  Tail-ender extra penalty (batting idx >= 7) 
        // These are genuine bowlers who can't bat
        if (idx >= 7) {
            w[7] += 2;   // BOWLED
            w[0] += 3;   // more dots (struggling to read bowling)
            w[4] -= 2;   // fewer fours
            if (w[4] < 1) w[4] = 1;
        }
    }

    // Clamp all weights >= 0
    for (int i = 0; i < n; ++i)
        if (w[i] < 0) w[i] = 0;

    int total = 0;
    for (int i = 0; i < n; ++i) total += w[i];
    if (total <= 0) return BALL_DOT;

    int r = rand() % total;
    int cum = 0;
    for (int i = 0; i < n; ++i) {
        cum += w[i];
        if (r < cum) return (BallOutcome)i;
    }
    return BALL_DOT;
}
