/* This unit and the other one disagree about both a type and a function,
 * so they describe programs that cannot be linked into one.
 */
struct Point { int x; int y; };
int shape(int n);
int use_a(void) { struct Point p = {1, 2}; return shape(p.x); }
