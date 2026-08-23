/* One of two units that define the same struct differently.
 *
 * The link brings everything into this unit, so this is the definition
 * of "thing" that stands; the one in unit_b.c cannot become the same
 * entity as it, and neither can the function whose type is written in
 * terms of it.  The importer says so by name -- "thing: Record
 * NameConflict" -- and that refusal is what the test is about.
 *
 * Two units disagreeing about a struct is what a real link of a real
 * project runs into: a header read under different macros, a type that
 * changed between the two files that were built at different times.
 * The map is asked for anyway, and it must come back.
 */
struct thing { int a; };

float compute(struct thing *t)
{
	return t->a;
}
