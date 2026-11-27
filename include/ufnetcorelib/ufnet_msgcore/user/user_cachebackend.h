//
// Created by ayman on 23/07/2026.
//

#ifndef UFNETCORELIB_USER_CACHEBACKEND_H
#define UFNETCORELIB_USER_CACHEBACKEND_H

#include <ufsrv_core/ufsrvresult_type.h>
#include <ufnetcorelib/ufnet_msgcore/fence/fence_type.h>
#include <ufnetcorelib/ufnet_core/session/session_type.h>

UFSRVResult *ClearBackendCacheForInvalidUserId(Session *sesn_ptr_carrier, Session *sesn_ptr_invalid, Fence *f_ptr, unsigned long call_flags);

#endif //UFNETCORELIB_USER_CACHEBACKEND_H
