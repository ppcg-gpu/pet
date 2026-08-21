/* Check that the wrapper around a worked-out value is not a thing.
 *
 * Clang wraps an expression whose value it has worked out in a node of
 * its own -- the condition of an "if constexpr" is one, and the
 * standard library is written out of them.  The wrapper says how the
 * expression was read, not what it is, and a scop that stopped at one
 * stopped at every use of the library.
 *
 * It stands after a statement of the loop, so that a scop which cannot
 * read it ends there rather than beginning after it.
 */
void f(int n, int a[], int b[])
{
	for (int i = 0; i < n; ++i) {
		a[i] = b[i] + 1;
		if constexpr (!__is_trivially_copyable(int))
			b[0] = 1;
	}
}
