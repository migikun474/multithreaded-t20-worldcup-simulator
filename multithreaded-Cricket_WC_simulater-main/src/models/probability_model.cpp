#include <stdlib.h>
#include "player.h"

typedef enum {
    FIELD_CATCH_OUT = 0,
    FIELD_DROPPED,
    FIELD_RUN_OUT_ATTEMPT,
    FIELD_FOUR_OVER_BOUNDARY,
    FIELD_SIX_OVER_BOUNDARY
} FielderOutcome;

inline const char* fielder_outcome_name(FielderOutcome o) {
    switch (o) {
        case FIELD_CATCH_OUT:          return "CAUGHT OUT!";
        case FIELD_DROPPED:            return "Dropped!";
        case FIELD_RUN_OUT_ATTEMPT:    return "Run-out attempt!";
        case FIELD_FOUR_OVER_BOUNDARY: return "FOUR (over boundary)";
        case FIELD_SIX_OVER_BOUNDARY:  return "SIX (over boundary)";
    }
    return "?";
}

FielderOutcome fielder_resolve_aerial(const Player* fielder, int reaction_ms) {
    //                  CATCH  DROP  RUNOUT  FOUR  SIX
    int w[] =           {  8,   10,    12,   38,   32 };

    float dive = fielder->dive_ability;
    float spd  = fielder->speed;

    //  Dive ability 
    if (dive >= 8.0f) {
        w[0] += 12; w[3] -= 5; w[4] -= 5;
    } else if (dive >= 6.0f) {
        w[0] += 5;  w[3] -= 3;
    } else if (dive < 4.0f) {
        w[0] -= 4;  w[3] += 6;
        if (w[0] < 1) w[0] = 1;
    }

    //  Speed 
    if (spd >= 8.0f) {
        w[2] += 6; w[3] -= 4; w[4] -= 2;
    } else if (spd >= 6.0f) {
        w[2] += 3; w[3] -= 2;
    } else if (spd < 4.0f) {
        w[2] -= 3; w[4] += 5;
        if (w[2] < 1) w[2] = 1;
    }

    //  Reaction time bonus 
    // Exact-zone fielder gets fast reaction (10-60ms) → likely catches
    // Adjacent-zone fielder arrives later (70-170ms) → mostly boundary/runout
    if (reaction_ms < 30) {
        w[0] += 4;
    } else if (reaction_ms > 140) {
        w[0] -= 3;
        if (w[0] < 1) w[0] = 1;
    }

    // Clamp
    for (int i = 0; i < 5; ++i)
        if (w[i] < 0) w[i] = 0;

    int total = 0;
    for (int i = 0; i < 5; ++i) total += w[i];
    if (total <= 0) return FIELD_SIX_OVER_BOUNDARY;

    int r = rand() % total;
    int cum = 0;
    for (int i = 0; i < 5; ++i) {
        cum += w[i];
        if (r < cum) return (FielderOutcome)i;
    }
    return FIELD_SIX_OVER_BOUNDARY;
}

/*
 * run_out_is_successful
 * Accuracy-based: higher accuracy → higher success chance.
 *   accuracy ≥ 8  → 60% success
 *   accuracy ≥ 6  → 45% success
 *   accuracy < 4  → 25% success
 */
bool run_out_is_successful(const Player* fielder) {
    int threshold;
    if      (fielder->accuracy >= 8.0f) threshold = 60;
    else if (fielder->accuracy >= 6.0f) threshold = 45;
    else                                threshold = 25;
    return (rand() % 100) < threshold;
}