/* One of two units that use the same instantiation of a template and
 * reach for different members of it.
 *
 * A template is instantiated as it is used, so this unit materialises
 * what it called and the other one materialises what it called, and the
 * two sets differ.  Compared member by member the two instantiations
 * come out unequal, which is what clang's importer concludes and why it
 * refuses to link them; by the rules of the language they are one type,
 * and which members were materialised says nothing about that.
 *
 * The standard library is used rather than a template written here
 * because a template whose members are all defined in the class has all
 * of them declared in every unit, and there is nothing to differ.
 */
#include <vector>

int total(std::vector<int> &v)
{
	return (int) v.size();
}
