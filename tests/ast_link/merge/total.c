#include "common.h"
/* Calls two functions that are only declared here. */
float total(Matrix *m, float f)
{
	scale(m, f);
	return sum(m);
}
