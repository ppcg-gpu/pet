#include <stdio.h>

#include "table.h"

float reads_table(void);

int main(void)
{
	int i;

	tentative[0] = 1.5f;
	printf("%g\n", reads_table());
	for (i = 0; i < 4; ++i)
		printf("%g\n", seeded[i]);
	return 0;
}
