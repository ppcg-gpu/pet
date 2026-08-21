/* Check that an if whose only business is to trap is not a branch.
 *
 * The condition is not looked at at all -- an assertion is usually
 * written about something a scop cannot describe, a pointer most often,
 * and reading it is what used to end the scop at the assertion and take
 * the loop with it.  The one below is written so that this test can see
 * whether it was read: pet turns it into a statement of its own, so if
 * the trap were read as a branch the loop would hold two statements
 * rather than one.
 */
void die(const char *msg) __attribute__((noreturn));

void f(int n, int a[n], int b[n])
{
	for (int i = 0; i < n; ++i) {
		b[i] = a[i];
		if (a[i] & 1)
			die("odd");
	}
}
