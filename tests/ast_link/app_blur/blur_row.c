#include "blur.h"
inline void blur_row(float out[N], float in[N])
{
	for (int j = 1; j < N - 1; ++j)
		out[j] = (in[j - 1] + in[j] + in[j + 1]) / 3.0f;
}
