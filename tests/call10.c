void set_odd(int n, int A[static n])
{
	goto next;
next:
	for (int i = 1; i < n; i += 2)
		A[i] = i;
}

void foo(int n, int A[static n][n])
{
#pragma scop
	for (int i = 0; i < n; ++i)
		set_odd(n, A[i]);
#pragma endscop
}
