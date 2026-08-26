/* A scop pragma written with something that is not an identifier.
 *
 * The name does not take, and the scop is taken anyway: this file's
 * recorded scop is the one scop_named.c would have if its identifier
 * were removed, carrying no name key.  Refusing the scop instead would
 * throw away a region that was marked by hand and is perfectly good,
 * over a name nobody could have read.
 *
 * "accum-ulate" is a hyphen away from an identifier, which is what a
 * typo looks like: it lexes as accum, minus, ulate.  Keeping the first
 * word would name this scop accum, and nothing in the file says that is
 * what was meant, so the name is dropped whole rather than guessed at.
 *
 * The warning that says so is not checked here.  These tests compare
 * recorded scops and say nothing about diagnostics.
 */
void f(int *a, int *b, int n)
{
	int s = 0;

#pragma scop accum-ulate
	for (int i = 0; i < n; ++i)
		s += a[i];
	b[0] = s;
#pragma endscop
}
