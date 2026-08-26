/* What union_through_pointer.c produced before the fix, kept as the record.
 *
 * `b[0].f = (hi[0] + 1.0f);` is ABSENT.  ppcg exited 0, removed the pragma,
 * and under --verbose said what it had done:
 *
 *     Eliminated dead instances: { S_2[] }
 *
 * The two reads also swapped, which is harmless here and shows that nothing
 * ordered them: the relation the union path installed named the whole array
 * from an empty domain, so no dependence connected any of the three.
 *
 * The relation, dumped either side of the fix, is the whole difference:
 *
 *     union as an object    { [] -> b_f[b[] -> f[]] }
 *     through a pointer     { [] -> b_f[b[o0] -> f[]] : o0 >= 0 }
 *
 * pet_expr_access_get_may_read projects out the argument dimensions.  An
 * object base has none, so nothing was lost and union_member.c was right; a
 * pointer base is b[i0], and projecting i0 away turned an exact access into
 * a may-access over everything.  Installed as a must-write, it pinned down
 * no element, so the store was not live out of the scop.
 */
void f(union bits * b, float * hi, const float * s, int32_t * out)
{
	/* ppcg generated CPU code */

	{
	  for (int c0 = 0; c0 <= 4095; c0 += 1)
	    hi[c0] = s[c0];
	  out[1] = b[0].i;
	  out[0] = b[0].i;
	}
}
