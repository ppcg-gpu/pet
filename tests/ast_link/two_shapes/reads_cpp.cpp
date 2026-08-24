#include "blk.h"

extern "C" int cpp_reads(blk *b) { return b->data.dm; }
