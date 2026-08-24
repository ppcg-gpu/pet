#include "width_of.h"

extern "C" int wide(void) { return width_of<short>::bits + width_of<long>::bits; }
