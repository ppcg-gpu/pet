/* Check that a call under a return has its body put in place.
 *
 * A C interface is written as "return f(x)", and a call left as a call
 * there leaves the body of everything behind the interface outside the
 * scop.  The return is the final statement of the body, which is what
 * makes it a statement a scop can hold.
 */
int add_one(int x)
{
	return x + 1;
}

int wrap(int x)
{
#pragma scop
	return add_one(x);
#pragma endscop
}
