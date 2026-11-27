#ifndef UFNETCORELIB_MAIN_H
# define UFNETCORELIB_MAIN_H

#include <uflib/standard_c_includes.h>

#if defined(HAVE_VALGRIND_DRD)
#include <valgrind/valgrind.h>
#include <valgrind/drd.h>
#include <ufnet_core/valgrind_drd_inlines.h>
#endif
#if defined(HAVE_VALGRIND_MEMCHECK)
#include <valgrind/memcheck.h>
#endif

#include <systemd/sd-daemon.h>
#include <uflib/standard_net_includes.h>

#include <uflib/standard_ssl_includes.h>

#include <curl/curl.h>

#include <ufsrv_core/system/log_message_literals.h>
#include <uflib/utils.h>
#include <uflib/main_types.h>

#endif //UFNETCORELIB_MAIN_H
