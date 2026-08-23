/* The other of two units that define the same struct differently.
 *
 * "thing" here has two double fields where unit_a.c has one int field,
 * so it is refused, and so is "other", whose type is written in terms
 * of it.  Nothing here is meant to be linked successfully: the point
 * is what the link does with what it cannot take.
 */
struct thing { double a; double b; };

double other(struct thing *t)
{
	return t->a + t->b;
}
