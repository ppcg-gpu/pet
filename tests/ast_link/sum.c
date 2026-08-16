#include "common.h"
static float clamp(float x) { return x > 1.0f ? 1.0f : x; }
float sum(Matrix *m)
{
	float s = 0.0f;
	for (int i = 0; i < m->n; ++i)
		s += clamp(m->data[i]);
	return s;
}
