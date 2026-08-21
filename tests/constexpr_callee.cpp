/* Check that a call named by a constant is a call to what it names.
 *
 * This is how ggml writes the conversion of one element: a constant in
 * the body names a constant of a table, and that one names the
 * function.  Nothing about the call is decided while the program runs,
 * but the call does not name a function, so the body behind it -- every
 * element of every unary and binary operation -- used to stay outside
 * the scop.
 */
static float op_relu(float x)
{
	return x > 0.f ? x : 0.f;
}

struct table {
	static constexpr float (*to_f32)(float) = op_relu;
};

void f(int n, float *y, const float *x)
{
	constexpr auto conv = table::to_f32;

#pragma scop
	for (int i = 0; i < n; ++i)
		y[i] = conv(x[i]);
#pragma endscop
}
