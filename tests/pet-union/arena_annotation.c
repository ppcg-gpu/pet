/* SEPARATELY DECLARED ARRAYS THAT ARE ONE PIECE OF STORAGE.
 *
 * The union files in this directory describe one storage written under
 * several types by giving it one declaration.  A generator that emits C from
 * a graph cannot always do that: its buffers are function parameters, they
 * are handed to it by an allocator that reuses a dead tensor's bytes for a
 * later one, and several of them are declared two-dimensional because the
 * rules that reach them through a data-dependent row need the unknown as the
 * outer subscript.
 *
 * Three ways of putting such buffers into one declaration were refused by
 * measurement before this existed, at 402 nodes of the DeepSeek-V4-Flash
 * graph:
 *
 *   a union of arrays          carries offsets and overlap, but a member
 *                              that must be two-dimensional cannot be
 *                              scaled, and 26 buffers of the group that
 *                              crashes the rung begin part-way through a row
 *   flat addressing            316 parallel bands became 0.  Flattening puts
 *                              the data-dependent index back under a
 *                              multiplication -- csa_k_all_2_238[i2] with
 *                              argument ic_1279 * 128 -- and pet then
 *                              refuses to print the index at all
 *   a byte arena with a cast   the same multiplication, and the cast is
 *                              refused outright
 *
 * What is left changes no C.  The arrays keep their names, their types, their
 * two-dimensional declarations and their printed subscripts; an annotation
 * says which of them share storage and at what byte offset, and only the
 * ACCESS RELATION is composed into one of them.  The composition is
 * offset/unit + sum over dimensions of index*stride/unit, which stays affine
 * in any number of dimensions, so the wall the other three hit is never
 * approached.
 *
 * THE ANNOTATION NAMES ONE OF THE ARRAYS AS THE STORAGE.  There is no
 * invented arena name on purpose: an array pet has never seen declared is
 * scop-local to it, killed at the end of the scop, and every write composed
 * into it becomes dead -- which is the loss ppcg 58c6418 was written to make
 * loud, and it must not arrive here disguised as a feature.  The array at
 * offset 0 is the representative, exactly as the smallest member stands for
 * a union.
 *
 * IT SITS INSIDE THE FUNCTION because it names parameters, and a parameter
 * does not exist before the function that declares it.  With the pragma above
 * the definition pet resolves no name and says so.
 *
 * Here idx is 4096 bytes into the storage lo names, which is lo[1024], and
 * the second loop writes it at iteration 1024 -- long after the read at
 * iteration 0.  Without the annotation the two loops fuse and the read is
 * carried past the write, returning 1149255680: 0x44804000, which is 1026.0f,
 * which is exactly what lo[1024] was given.  The companion is llama-dspark's
 * tests/ppcg/arena-probe.py, where this form is `annot` and the same kernel
 * without the pragma is `alias`, kept red on purpose so that the green means
 * something.
 */
#include <stdint.h>
void f(float * lo, float * hi, const int32_t * idx, const float * s,
       int32_t * out)
{
#pragma ppcg arena lo 0 idx 4096 hi 16384
#pragma scop
	for (int i = 0; i < 4096; i++) hi[i] = s[i];
	out[0] = idx[0];
	for (int i = 0; i < 4096; i++) lo[i] = hi[i] + 1.0f;
#pragma endscop
}
