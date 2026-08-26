#ifndef PET_EXPR_ACCESS_TYPE_H
#define PET_EXPR_ACCESS_TYPE_H

#if defined(__cplusplus)
extern "C" {
#endif

enum pet_expr_access_type {
	pet_expr_access_may_read,
	pet_expr_access_begin = pet_expr_access_may_read,
	pet_expr_access_fake_killed = pet_expr_access_may_read,
	pet_expr_access_may_write,
	pet_expr_access_must_write,
	pet_expr_access_end,
	/* LIVENESS SEES THE ARRAY THE SOURCE NAMES, NOT THE REPRESENTATIVE.
	 *
	 * When an annotation composes a member's accesses onto the storage's
	 * representative, the may/must_write slots hold the COMPOSED relation
	 * and the dependence analysis is right to read it: writes through
	 * different arrays that share bytes really do conflict.  But liveness
	 * must not read it.  A write through `b` composed onto representative
	 * `a` looks, to a kill analysis over the composed relations, like a
	 * second write to `a` that covers the first -- and the first write,
	 * through `a` itself, is classified dead even though the caller can
	 * still reach it through `a`.  Measured on the 402-node scop: nine of
	 * eleven parameters losing writes were the representatives, and the
	 * scheduler then died in "unable to carry dependences" with 0 bands.
	 *
	 * These two slots hold the relation over the array the index names,
	 * exactly what pet would have built with no annotation, and only the
	 * liveness computations read them.  They sit past _end so that no
	 * existing loop over the access types -- argument modification, nest
	 * rewriting, summaries -- carries them by accident.
	 */
	pet_expr_access_plain_may_write = pet_expr_access_end,
	pet_expr_access_plain_must_write,
	pet_expr_access_plain_end,
	pet_expr_access_killed
};

#if defined(__cplusplus)
}
#endif

#endif
