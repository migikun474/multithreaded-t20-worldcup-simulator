#include <stdlib.h>
// Thin wrappers kept for link compatibility
int random_int(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}
float random_float_01() {
    return (float)rand() / (float)RAND_MAX;
}