#include <vector>

int first(std::vector<int> &v)
{
	return *v.begin() + (int) v.capacity();
}
