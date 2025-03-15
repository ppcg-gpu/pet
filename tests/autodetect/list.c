/* A program with an access to a recursive data type.
 * Check that this access is not considered part of the autodetected scop.
 */

struct list {
	int el;
	struct list *next;
};

void t(struct list *l);

void foo()
{
	struct list *l;
	t(l);

	int a = 0;
	return;
}
