/* Printing the original code of every scop in place of the scop must
 * give the file back exactly as it was written.
 *
 * pet_transform_C_source copies the text around each scop and hands the
 * scop itself to the callback; a callback that prints the original code
 * therefore reproduces the input byte for byte.  That is the whole
 * check, and it is a strong one: an off-by-one in either offset, a
 * region read from the wrong file, or a file that could not be opened
 * all show up as a difference.
 *
 * Two scops in two functions, so that the copying between them is
 * exercised and not only the head and the tail.
 */
float first(float *a, int n)
{
	float s = 0;

#pragma scop
	for (int i = 0; i < n; ++i)
		s += a[i];
#pragma endscop

	return s;
}

/* Text between the two scops, which has to come over untouched. */

void second(float *a, int *c, int n)
{
#pragma scop
	for (int i = 0; i < n; ++i)
		if (c[i])
			a[i] = 1;
#pragma endscop
}
