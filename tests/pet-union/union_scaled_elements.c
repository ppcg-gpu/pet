/* A UNION WHOSE MEMBERS HAVE DIFFERENT ELEMENT SIZES.
 *
 * union_of_arrays.c has float beside int32_t, and a subscript steps over
 * four bytes in either, so one member can stand for the other unchanged.
 * The storage groups this exists for are not like that: at 402 nodes of the
 * DeepSeek-V4-Flash graph, five of the twelve unmerged groups hold element
 * sizes 2, 4 and 8 at once, and one of those five is the group that crashes
 * the rung.
 *
 * The canonical member is the one with the SMALLEST element, which is what
 * makes this one construction instead of two.  With the first member
 * standing for the storage, an access through a larger element covers a
 * RANGE of canonical elements while an access through a smaller one lies
 * INSIDE a canonical element at a floored index -- and a union of f16, f32
 * and i64 needs both at once.  Taking the smallest leaves only the range.
 *
 * A RANGE and not one element, because a write's footprint must not be
 * understated.  g->f[i] here touches g->h[2i] and g->h[2i+1]; naming only 2i
 * leaves 2i+1 unclaimed, and an unclaimed byte is a licence for exactly the
 * reordering this exists to forbid.
 *
 * The companion is llama-dspark's tests/ppcg/arena-probe.py, which runs this
 * shape both ways round -- the smaller member declared first in one form and
 * last in the other -- so that the choice of canonical is measured rather
 * than assumed.  Before this, both returned 16384 for an index whose value
 * is 7: 0x4000, the low half of the 1026.0f the fused loop had written over
 * those two bytes.
 */
#include <stdint.h>
union grp2 { uint16_t h[16384]; float f[8192]; };
void f(union grp2 * g, const float * s, int32_t * out)
{
#pragma scop
	for (int i = 0; i < 4096; i++) g->f[4096 + i] = s[i];
	out[0] = g->h[2048];
	for (int i = 0; i < 4096; i++)
		g->f[i] = g->f[4096 + i] + 1.0f;
#pragma endscop
}
