#include "width_of.h"

extern "C" int narrow(void) { return width_of<char>::bits + width_of<int>::bits; }
