/* Check that a call handed more arguments than its summary has
 * parameters does not read past the end of the summary.
 *
 * A summary is built from the parameters a function declares, and a
 * variadic function declares fewer than a call to it passes: rec names
 * two and is called with three.  The call inside rec is left standing
 * where it is -- a function already being put in place is not put
 * inside itself -- so the summary is where the scop learns what that
 * call touches, and the loop that reads it used to be bounded by the
 * call's argument count.  The third argument has no parameter behind
 * it, and asking the summary about it asks for a position it does not
 * have.
 *
 * Without the clamp in call_plug_in_summary this says "position out of
 * bounds" and pet stops with a non-zero status.  The scop that comes
 * out is the same either way, so the status is what tells the two
 * apart, and the test framework is what looks at it.
 */
void rec(int n, float *b, ...)
{
	b[0] = 0;
	rec(n, b, 1.0f);
}

void caller(float *a)
{
#pragma scop
	rec(3, a, 1.0f);
#pragma endscop
}
