/* Check that a statement holding nothing does not begin the scop.
 *
 * Autodetect skips over what it cannot read until it can, and then
 * stops at the first thing it cannot read again.  The trap below holds
 * nothing, so it should be skipped as well; if it began the scop, the
 * declaration after it would end it right away and the loop would be
 * left out.
 */
void die(const char *msg) __attribute__((noreturn));

struct node {
	struct node *next;
	int v;
};

void f(struct node *r, int n, int a[n])
{
	if (!r)
		die("null");

	struct node *p = r->next;

	for (int i = 0; i < n; ++i)
		a[i] = i;
}
