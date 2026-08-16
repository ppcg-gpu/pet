#ifndef PET_AST_LINK_COMMON_H
#define PET_AST_LINK_COMMON_H
typedef struct Matrix { int n; float *data; } Matrix;
void scale(Matrix *m, float f);
float sum(Matrix *m);
float total(Matrix *m, float f);
#endif
