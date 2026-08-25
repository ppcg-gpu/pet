/* A scop pragma written with an identifier.
 *
 * Where a scop stands is all it otherwise has, and a position is no
 * name: it moves when a line is added above it, and two scops of one
 * file are told apart only by counting.  The identifier is what lets
 * anything outside the source ask for this scop rather than another,
 * and what a file generated from it can be called after.
 *
 * That a scop written without an identifier stays unnamed is said by
 * every other recorded scop in this directory: none of them carries a
 * name key, and they were recorded before the pragma took one.
 */
void f(int *a, int *b, int n)
{
	int s = 0;

#pragma scop accumulate
	for (int i = 0; i < n; ++i)
		s += a[i];
	b[0] = s;
#pragma endscop
}
