/* Reads the table through both units, so that a link which lost the
 * definition fails to build rather than printing something else.
 */
#include <stdio.h>

#include "table.h"

float reads_table(void);

int main(void)
{
	table[0] = 2.5f;
	table[1] = 4.0f;
	printf("%g\n", reads_table());
	return 0;
}
