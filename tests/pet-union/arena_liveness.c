/* A WRITE LATER MEMBER MUST NOT KILL AN EARLIER MEMBER'S LIVENESS.
 *
 * Two arrays share storage by annotation. `a` writes its whole range;
 * later `b` -- a DIFFERENT array in the C -- writes over the same bytes.
 * Nobody inside the scop reads either write: both are live out, because
 * the caller can still reach each array's bytes through that array.
 *
 * If liveness is judged over the COMPOSED relations, `b`'s must-write
 * covers `a`'s may-write on the representative, compute_live_out
 * subtracts it away, and `a`'s loop is eliminated as dead -- while the
 * caller still sees `a` unwritten. Measured on the 402-node scop: 2115
 * arrays losing a write against 1951 without the annotation, and nine
 * of the eleven parameters losing writes were the representatives the
 * pragmas named first; the scheduler then died in "unable to carry
 * dependences" with zero bands. The nopragma control on the same file
 * scheduled 315 bands.
 *
 * Liveness must be judged over the arrays the source names. This file
 * is that rule in nine lines: with the fix, both loops are scheduled and
 * `a[c0]` is present in the output; without it, the first loop is gone
 * and ppcg reports "arrays losing a write: a".
 */
#include <stdint.h>
void f(float * a, float * b, int32_t * out)
{
#pragma ppcg arena a 0 b 0
#pragma scop
	for (int i = 0; i < 4096; i++) a[i] = 1.0f + (float) i;
	for (int i = 0; i < 4096; i++) b[i] = 2.0f + (float) i;
	out[0] = 0;
#pragma endscop
}
