#ifndef BLUR_H
#define BLUR_H
#define N 64
/* Defined in blur_row.c.  Marked inline so that pet may take it into the
 * scop; whether it can see the body at all is what linking decides.
 */
inline void blur_row(float out[N], float in[N]);
void blur(float out[N][N], float in[N][N]);
#endif
