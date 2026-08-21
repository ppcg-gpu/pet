/* Check that brackets around what is reached into are not a thing.
 *
 * Parentheses say how an expression was written, not what it is, and a
 * base written with them around it used to end the scop for that reason
 * alone -- the path that works out which element is meant knew nothing
 * about them.
 */
struct s {
	int v;
};

void f(struct s *p, int n, int a[n])
{
	for (int i = 0; i < n; ++i)
		a[i] = (p)->v + (a)[i];
}
