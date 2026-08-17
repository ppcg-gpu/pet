/* A file that only parses with the options the project builds it with:
 * the header it needs is somewhere only a -I says.
 */
#include "lib.h"

void scale(float a[128], float b[128])
{
#pragma scop
	for (int i = 0; i < 128; ++i)
		a[i] = b[i] * SCALE;
#pragma endscop
}
