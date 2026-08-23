/* A call inside a body that is itself put in place of a call.
 * outer is given &a[3] and hands inner &a[3 + 2], so the write
 * is to a[5].  Each of the two calls keeps the index it was given
 * in a temporary of its own, and the two must not be one variable.
 */
void inner(int *b)
{
	b[0] = 1;
}

void outer(int *a)
{
	inner(&a[2]);
}

void f()
{
	int a[10];
#pragma scop
	outer(&a[3]);
#pragma endscop
}
