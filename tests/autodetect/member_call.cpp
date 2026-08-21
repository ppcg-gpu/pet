/* Check that a method call is a call.
 *
 * getStmtClass answers with the most derived node, and a call to a
 * method is a CXXMemberCallExpr, which the extraction did not name
 * among the kinds of call it knew.  A whole subclass of calls was
 * therefore not a call at all, and a loop holding one held nothing --
 * which is where the scop of llama_decode ended, on ctx->decode(batch).
 *
 * What the scop keeps here is the one statement of the loop; the body
 * of the method is a separate matter, since it reaches its members
 * through this.
 */
struct box {
	int k;

	int get(int i) const
	{
		return k + i;
	}
};

void f(int n, int a[], const box &b)
{
	for (int i = 0; i < n; ++i)
		a[i] = b.get(i);
}
