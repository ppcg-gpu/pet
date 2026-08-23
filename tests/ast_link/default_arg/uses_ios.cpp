/* A call that hands over fewer arguments than the body declares parameters.
 *
 * std::basic_ios::clear takes one parameter with a default value, and
 * basic_ios::rdbuf calls it as clear() -- writing nothing for that
 * parameter.  Read back out of a linked AST the call carries no argument
 * for it, so a scan that goes over the body's parameters and reaches into
 * the call for each one reads past the end of the call.  It did, and the
 * mapper's worker died with SIGSEGV on every unit of a real program that
 * includes a stream header.
 *
 * Two lines of C++ are enough because the headers behind them are not two
 * lines.  Which functions libstdc++ offers varies between versions, so
 * this asks only that the map comes back whole and says nothing about
 * what is in it.
 */
#include <sstream>

void f(std::ostringstream &o)
{
	o.clear();
}
