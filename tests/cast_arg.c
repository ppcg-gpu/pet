/* A pointer cast written on the argument of a call that is put in place.
 *
 * A cast from one pointer to another says how the memory behind the
 * pointer is to be read, not which memory that is, so a scop looking
 * for what an argument refers to has to see through it.  Before it did,
 * the argument reached the test for an access as a CStyleCastExpr,
 * which is not one, and the call was refused: three such arguments
 * stopped a whole graph of an inference engine, where the weights are
 * blocks of quantised values and every kernel is handed a pointer cast
 * to the block type it reads.
 *
 * The three shapes are the three that were found there, and they had to
 * be answered in that order: the cast alone, and then the parentheses,
 * because a cast written over a subscript brings its own pair and a
 * walk that stopped at them would get past the cast and no further.
 *
 * What is not here is a cast that converts -- an int to a float, a
 * narrowing, a change of sign.  Those are still built as casts, since
 * there the value that arrives is not the value that was written.
 */
typedef struct { signed char q[32]; float d; } blk;

static void take(const blk *b, float *o, int n)
{
	for (int i = 0; i < n; ++i)
		o[i] = b[0].d;
}

void outer(float w2[64][64], float *w, float *o, int n)
{
#pragma scop
	take((const blk *) w, o, n);
	take((const blk *) (w2[0]), o, n);
	take((const blk *) (&w[4]), o, n);
#pragma endscop
}
