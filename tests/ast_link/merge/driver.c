#include <stdio.h>
#include "common.h"
int main(void)
{
	float d[8];
	Matrix m = { 8, d };
	int i;

	for (i = 0; i < 8; ++i)
		d[i] = (float) (i - 3) / 4.0f;
	printf("%.6f\n", total(&m, 1.5f));
	for (i = 0; i < 8; ++i)
		printf("%.6f\n", d[i]);
	return 0;
}
