/**
 * @file fence_redis_backend.h
 * @brief fence_redis_backend
 *
 * Copyright (C) 2015-2026  unfacd works
 *
 * This file is part of ufnetcorelib source code.
 * Created by ayman on 16/11/2026.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef UFNETCORELIB_FENCE_REDIS_BACKEND_H
#define UFNETCORELIB_FENCE_REDIS_BACKEND_H

#include <uflib/collection/collection.h>
#include "fence_uids_collector_ctx_type.h"

PUBLIC_API bool FenceMembersCollectUids(const CollectionDescriptorV2 *collection_descriptor, unsigned long fid,
                             FenceUidsCollectorCtx *ctx, CollectionResult *out);
PUBLIC_API void FreeFenceMembersCollectUids(const CollectionDescriptorV2 *collection_descriptor, CollectionResult *out);
PUBLIC_API const CollectionDescriptorV2 * ProvideDefaultFenceMembersCollectionDescriptor(void);
PUBLIC_API const CollectionDescriptorV2 * ProvideArenaFenceMembersCollectionDescriptor(void);
PUBLIC_API const CollectionDescriptorV2 * ProvideUidListFenceMembersCollectionDescriptor(void);

#endif //UFNETCORELIB_FENCE_REDIS_BACKEND_H
