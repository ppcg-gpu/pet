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

#ifdef __cplusplus
}
#endif

#endif /* PET_DEBUG_HOOKS_H */
