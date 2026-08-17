/* Whether this unit defines a helper at all depends on a macro, so a unit
 * serialised without pet's options describes a different program.
 */
#ifdef WITH_HELPER
static int helper(int x) { return x + 1; }
int use(void) { return helper(1); }
#else
int use(void) { return 0; }
#endif
