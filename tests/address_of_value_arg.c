/* An address handed to a body that uses the parameter as a value.
 *
 * `helper` never subscripts `p`: it passes the pointer on.  Its access is
 * therefore to the pointer variable itself, which is zero-dimensional.
 * The argument is `&x`, so the substitution is told an ADDRESS is being
 * put in place, and the machinery for that combines the two by taking the
 * outermost dimension of the body's access as an offset into x and
 * appending the rest -- which needs the body's access to have one.
 *
 * It has none, and the invariant that says so was checked but not
 * answered: patch.c refused with "an address was patched onto an access
 * with no dimension to give up", pet exited 1, and every unit of a real
 * program that includes a stream header died the same way.  libstdc++
 * reaches this constantly -- "&__val" handed to a body that forwards
 * "__ptr" -- and nine such arguments come out of two lines of C++.
 *
 * What the substitution means here is just the argument.  The body's
 * "value of p" IS "&x", so the address expression takes the access's
 * place whole rather than being folded into it.
 *
 * The loop is not decoration: without an access to x in the scop there
 * is nothing for the address to be an address OF, and the case does not
 * arise.
 */
void use(float **q);

static void helper(float **p)
{
	use(p);
}

void f(float *x, float *o, int n)
{
#pragma scop
	for (int i = 0; i < n; i++)
		o[i] = x[i];
	helper(&x);
#pragma endscop
}
