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
	 * representative, the may/must_write relations the collection hands
	 * out hold the COMPOSED relation and the dependence analysis is right
	 * to read it: writes through different arrays that share bytes really
	 * do conflict.  But liveness must not read it.  A write through `b`
	 * composed onto representative `a` looks, to a kill analysis over the
	 * composed relations, like a second write to `a` that covers the
	 * first -- and the first write, through `a` itself, is classified
	 * dead even though the caller can still reach it through `a`.
	 * Measured on the 402-node scop: nine of eleven parameters losing
	 * writes were the representatives, and the scheduler then died in
	 * "unable to carry dependences" with 0 bands.
	 *
	 * These two are COLLECTION TYPES, not stored relations: they ask
	 * expr_collect_access for the very same write relation with the
	 * arena composition skipped.  There is nothing extra to store, so
	 * nothing can go stale, and every narrowing pet applies to a write --
	 * a called function's summary above all -- is in them.  They sit
	 * past _end so that no loop over the stored types can reach them.
	 */
	pet_expr_access_plain_may_write = pet_expr_access_end,
	pet_expr_access_plain_must_write,
	pet_expr_access_killed
};

#if defined(__cplusplus)
}
#endif

#endif
