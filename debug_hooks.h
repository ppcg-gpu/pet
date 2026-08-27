/*
 * Copyright 2025. All rights reserved.
 */

#ifndef PET_DEBUG_HOOKS_H
#define PET_DEBUG_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize debug hooks - installs signal handlers */
void pet_debug_hooks_init(void);

/* Cleanup debug hooks - restores original signal handlers */
void pet_debug_hooks_cleanup(void);

/* Should a debug dump about the array called "name" be printed?
 *
 * PET_DEBUG_ONLY narrows every arena dump to one array.  Unset, everything
 * that asks is printed; set, only the array it names.  On a scop of a few
 * statements the difference does not matter; on the 402-node one it is the
 * difference between a readable answer and gigabytes -- 1974 arrays lose a
 * write there and the question is about three of them.
 */
int pet_debug_only(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* PET_DEBUG_HOOKS_H */
