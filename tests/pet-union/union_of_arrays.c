/* A UNION OF ARRAYS: ONE STORAGE, MANY WINDOWS, MANY OFFSETS.
 *
 * union_through_pointer.c is a union of SCALARS, which is the bit-cast
 * idiom.  This is the shape a storage group wants: one union whose members
 * are arrays spanning the whole allocation, with each buffer a window inside
 * them addressed by an offset in the INDEX.  g->i[1024] is the buffer 4096
 * bytes in; g->f[4096 + i] is a different one further along.  Offsets and
 * overlap are both carried that way, which neither a struct (no overlap) nor
 * a union of scalars (no offsets) can do.
 *
 * Before the entry through subscripts, union_field tested the expression
 * itself, and "g->f[i]" is an ArraySubscriptExpr rather than a MemberExpr.
 * Every array member therefore missed the union machinery entirely: f and i
 * were two independent arrays, the two float loops were fused, and the read
 * of the int member moved past them and took the float bits that had just
 * been written over it.  Measured in llama-dspark as
 * tests/ppcg/arena-probe.py, where the index came back as 1149255680 --
 * 0x44804000, which is 1026.0f, which is exactly what the second loop wrote
 * at that element.
 *
 * The two float loops either side of the int read are the incentive: isl
 * fuses a producer with its consumer, and fusing them is what moves a write
 * over the read.  Without a reason to fuse, nothing moves and the test
 * passes while proving nothing.
 */
#include <stdint.h>
union grp { float f[8192]; int32_t i[8192]; };
void f(union grp * g, const float * s, int32_t * out)
{
#pragma scop
	for (int i = 0; i < 4096; i++) g->f[4096 + i] = s[i];
	out[0] = g->i[1024];
	for (int i = 0; i < 4096; i++)
		g->f[i] = g->f[4096 + i] + 1.0f;
#pragma endscop
}
