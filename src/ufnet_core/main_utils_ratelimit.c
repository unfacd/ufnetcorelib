/**
 * @file main_utils_ratelimit.c
 * @brief Application rate-limit policy wiring.
 *
 * The ratelimit_policy config field is generated from ufnetcorelib's catalogue,
 * so this helper stays in ufnetcorelib: moving it into ufsrvcorelib would make
 * the base library depend on a field it does not own.
 */

#include <ufsrvcorelib/ufsrv_core/ratelimit_v2/ratelimit.h>
#include <ufsrvcorelib/ufsrv_core/ufsrv_config/generated/ufsrv_config_accessor_decls_generated.h>

#include <ufnetcorelib/ratelimit_policy.h>

void
ConfigPrepareApplicationRateLimitPolicy(void)
{
  RateLimitApplicationPolicy *ratelimit_application_policy = UfsrvConfigRatelimitPolicyAsPtr();

  if (!ratelimit_application_policy) return;

  ratelimit_application_policy->is_enabled = UfsrvConfigRatelimitIsEnabled();
  ratelimit_application_policy->is_bypass_private_ip = UfsrvConfigRatelimitIsBypassPrivateIp();

  RateLimitSetApplicationPolicy(ratelimit_application_policy);
}
