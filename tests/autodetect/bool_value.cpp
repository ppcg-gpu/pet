/* Check that true and false are values.
 *
 * They are what a C++ program writes where C writes 1 and 0, and a scop
 * that cannot read them ends at the first one.  A statement is written
 * below rather than a bare return, so that what is read of them is
 * visible in the scop.
 */
void f(int n, int a[], bool keep)
{
	for (int i = 0; i < n; ++i)
		a[i] = keep ? true : false;
}
