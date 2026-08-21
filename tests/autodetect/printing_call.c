/* Check that a call given nothing but values is left out of the scop.
 *
 * printf is handed a format and a number; it has been given no way of
 * reaching the memory the scop is about, so whatever it does happens
 * outside.  Leaving it in used to cost the loop it stands in: the scop
 * ended at the format and kept the one statement before it.
 *
 * snprintf below is given somewhere to write, so it is not this and
 * stays where it is -- which is why the loop ends where it does.
 */
int printf(const char *fmt, ...);
int snprintf(char *out, unsigned long n, const char *fmt, ...);

void f(int n, int a[n], char *out)
{
	for (int i = 0; i < n; ++i) {
		a[i] = i;
		printf("step %d\n", i);
	}

	snprintf(out, 8, "%d\n", n);
}
