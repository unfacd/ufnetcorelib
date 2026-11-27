/**
 * @file ufsrvcore_utils.h
 * @brief Domain-side helpers that sit next to ufsrvcorelib without joining it.
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFNETCORELIB_UFSRVCORE_UTILS_H
#define UFNETCORELIB_UFSRVCORE_UTILS_H

#include <ufnetcorelib/ufnetcorelib_defs.h>
#include <uflib/adt/adt_hashtable.h>
#include <ufnet_msgcore/msgqueue_backend/ufsrvmsgqueue_type.h>

/**
 * @brief Instantiate one delegator session table in caller-supplied storage.
 *
 * The caller owns the storage and describes the key it indexes; the table is
 * placed by HashTableLockingInstantiateWithSpecs(), so it takes its own
 * read/write lock. This is how the domain hands corelib a table that indexes
 * the composed Session layout without corelib knowing that layout.
 *
 * @param[in] hash_table Storage for the table; must not be NULL.
 * @param[in] spec Key configuration, name and extractor; applied unchanged.
 * @note Re-initialising the same storage destroys its previous lock and bucket array.
 */
PUBLIC_API void SessionsDelegatorInitialiseHashtable(HashTable *hash_table, HashtableSpecs *spec);

/**
 * @brief Instantiate the delegator's four session tables from descriptors.
 *
 * Sessions by id, cookies, user ids and user names, each described by its own
 * HashtableSpecs and placed with SessionsDelegatorInitialiseHashtable().
 */
PUBLIC_API void SessionDelegatorInitHashTables(void);

/**
 * @brief The delegator's message-queue publisher backend, if one was built.
 *
 * The backend is allocated by this library because it needs the domain
 * connection handle; the delegator only stores the opaque pointer, so callers
 * ask for it here instead of reaching into the delegator struct.
 *
 * @return The publisher backend, or NULL when the active protocol does not ask
 *         for publisher semantics or the connection could not be built.
 */
PUBLIC_API MessageQueueBackend *SessionsDelegatorMessageQueueBackend(void);

/**
 * @brief Register and schedule the periodic session idle-time check.
 * @code{.c}
 * LaunchTimerManagerThread(InitialiseScheduledJobTypeForSessionsTimeouts);
 * @endcode
 */
PUBLIC_API void InitialiseScheduledJobTypeForSessionsTimeouts(void);

#endif
