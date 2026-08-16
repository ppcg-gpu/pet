#include "common.h"
/* Same name as the one in sum.c, but a different function. */
static float clamp(float x) { return x < 0.0f ? 0.0f : x; }
void scale(Matrix *m, float f)
{
	for (int i = 0; i < m->n; ++i)
		m->data[i] = clamp(m->data[i] * f);
}
