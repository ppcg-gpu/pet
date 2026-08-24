/* A variable written without an initialiser at the outermost level of a
 * C unit is a tentative definition, and what makes it the definition is
 * the end of the unit arriving with nothing else claiming the name.
 */
extern float table[16];
float first_of_table(void);
