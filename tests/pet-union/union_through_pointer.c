/* A UNION REACHED THROUGH A POINTER LOST ITS WRITE.
 *
 * union_member.c in this directory is the shape d334d06 was written for: the
 * union is an OBJECT, its members are one storage, and the schedule respects
 * that.  This is the same kernel with the union reached through a pointer,
 * which is how a caller's memory is described, and it was miscompiled --
 * see union_through_pointer_miscompiled.c for what came out.
 *
 * The store `b->f = hi[0] + 1.0f;` is not dead: `out[1] = b->i;` reads it
 * back, and b points at the caller's memory either way.  It was deleted
 * anyway, with no diagnostic and a zero exit.
 *
 * The two float loops around the int read are the incentive: isl fuses a
 * producer with its consumer, and fusing them moves a write over the read.
 * Without a reason to fuse, nothing moves and the test passes while proving
 * nothing -- measured, and the reason this file is written this way.
 */
#include <stdint.h>
union bits { float f; int32_t i; };
void f(union bits * b, float * hi, const float * s, int32_t * out)
{
#pragma scop
	for (int i = 0; i < 4096; i++) hi[i] = s[i];
	out[0] = b->i;
	b->f = hi[0] + 1.0f;
	out[1] = b->i;
#pragma endscop
}
