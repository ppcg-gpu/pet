/* A pointer with an integer added to it, handed to a call that is put
 * in place.
 *
 * "p + i" and "&p[i]" are one address written two ways.  Only the
 * second was an access as far as a scop was concerned, so the first
 * was refused wherever it appeared as an argument -- and it appears
 * wherever a kernel is handed a slice of a weight buffer:
 *
 *     ggml_dot_q8_0_blocks((const block_q8_0 *) (w + t*4352), ...)
 *
 * twenty-four times over on one graph of an inference engine.
 *
 * Two arms, and the second is the one that matters.  The first is the
 * shape found in that graph: a cast over an offset that is affine.  The
 * second offsets by i*i, which is not, and it is here because the fix
 * does not ask whether an offset is affine -- a subscript does not ask
 * either, and the whole point of writing one as the other is that the
 * two answer alike.  Recorded here, the second arm holds that claim: it
 * is the same scop that "&w[i*i]" gives, in one dimension.
 *
 * A first attempt at this assembled the access rather than handing a
 * subscript to the reader that already builds them, and produced one
 * dimension more than the bracketed spelling -- the base already
 * carried a dimension and a subscript went on top.  Nothing complained;
 * it showed up only as a scop of the wrong shape.  That is what the
 * second arm is recorded against.
 */
typedef struct { signed char q[32]; float d; } blk;

static void take(const blk *b, float *o, int i)
{
	o[i] = b[0].d;
}

static void takef(const float *b, float *o, int i)
{
	o[i] = b[0];
}

void offset(float *w, float *o, int n)
{
#pragma scop
	for (int t = 0; t < n; ++t)
		take((const blk *) (w + t*4352), o, t);
#pragma endscop
}
