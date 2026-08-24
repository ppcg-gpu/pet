#include "table.h"

float tentative[4];
float seeded[4] = {2.5f, 4.0f, 8.0f, 16.0f};

float first_of_table(void) { return tentative[0] + seeded[0]; }
