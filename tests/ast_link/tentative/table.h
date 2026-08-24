/* Three things a link has to bring over from the unit that holds them.
 *
 * "tentative" is written without an initialiser at the outermost level
 * of a C unit, which makes it a tentative definition: it is the
 * definition unless something else claims the name.  The linked context
 * is C++, which has no such thing, so a link that says nothing about it
 * leaves a mention and defines it nowhere.
 *
 * "seeded" is written with an initialiser, so that what is checked is
 * the value and not merely that something was defined.
 *
 * first_of_table is the control: a function's body is carried over by a
 * step of its own, which is why a link could lose every variable of an
 * imported unit and no function at all.
 */
extern float tentative[4];
extern float seeded[4];
float first_of_table(void);
