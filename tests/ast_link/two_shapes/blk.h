/* One header describing two records of one name.
 *
 * The aggregate is anonymous where the unit is read as C and called
 * data where it is read as C++, which is how ggml-common.h describes
 * the quantised blocks of ggml: written for compilers that warn about
 * anonymous aggregates in C++.  Each unit on its own is fine.  Linked
 * together they are two records under one name whose members do not
 * answer to the same names, and a member access written in one language
 * would read a field the other has not got.
 */
#ifdef __cplusplus
#define AGGR_U data
#else
#define AGGR_U
#endif

typedef struct {
	union {
		struct { unsigned short d; unsigned short dmin; } s;
		unsigned int dm;
	} AGGR_U;
	unsigned char qs[8];
} blk;
