#include "blur.h"
/* The loop body is written in another translation unit, so on its own
 * this is a one dimensional scop over an opaque call.
 */
void blur(float out[N][N], float in[N][N])
{
	for (int i = 0; i < N; ++i)
		blur_row(out[i], in[i]);
}
