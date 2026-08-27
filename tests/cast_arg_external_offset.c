void ggml_vec_dot_f32(int n, float *dst, int ds, const float *a, int as,
		      const float *b, int bs, int nrc);

static float indexer(int n, int nh, const float *q, int qs, const float *k)
{
	float score = 0.0f;
	for (int h = 0; h < nh; ++h) {
		float qk = 0.0f;
		ggml_vec_dot_f32(n, &qk, 0, q + h*qs, 0, k, 0, 1);
		score += qk;
	}
	return score;
}

void outer(float *src, float *k, float *o, int n)
{
#pragma scop
	for (int t = 0; t < n; ++t)
		o[t] = indexer(128, 4, &src[t*8192], 128, k);
#pragma endscop
}
