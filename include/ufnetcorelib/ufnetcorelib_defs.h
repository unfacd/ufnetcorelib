/**
 * @file ufnetcorelib_defs.h
 * @brief Project‑wide preprocessor definitions shared across all public headers.
 */

#ifndef UFNETCORELIB_DEFS_H
#define UFNETCORELIB_DEFS_H

#include <pthread.h>
#include <syslog.h>

/*!
 * Marker for public API functions.
 *
 * Place before the return type of any function that is part of a module's
 * public contract.  Expands to nothing in the default build; can be redefined
 * to \c __attribute__((visibility("default"))) for shared‑library builds.
 *
 * @code{.c}
 * PUBLIC_API void UfsrvConfigInit(void *server);
 * PUBLIC_API int  UfsrvConfigServerId(void);
 * @endcode
 */
#ifndef PUBLIC_API
  #if defined(__GNUC__) || defined(__clang__)
    #define PUBLIC_API __attribute__((visibility("default")))
  #else
    #define PUBLIC_API
  #endif
#endif

/**
 * Trace macros. Each names the emitting function and its thread, matching the
 * rest of the library's syslog output, and compiles out unless the build
 * selects its switch. UFNET_LOG() carries no level and follows the base debug
 * switch, which either level implies.
 */
#if UFNET_DEBUG_BUILD
# define UFNET_LOG(fmt, ...) \
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: " fmt, __func__, pthread_self(), ##__VA_ARGS__)
#else
# define UFNET_LOG(fmt, ...) ((void)0)
#endif

#if UFNET_DEBUG_BUILD_L2
# define UFNET_LOG_L2(fmt, ...) \
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: " fmt, __func__, pthread_self(), ##__VA_ARGS__)
#else
# define UFNET_LOG_L2(fmt, ...) ((void)0)
#endif

#if UFNET_DEBUG_BUILD_L3
# define UFNET_LOG_L3(fmt, ...) \
  syslog(LOG_DEBUG, "%s {pid:'%lu'}: " fmt, __func__, pthread_self(), ##__VA_ARGS__)
#else
# define UFNET_LOG_L3(fmt, ...) ((void)0)
#endif

#endif /* UFNETCORELIB_DEFS_H */
