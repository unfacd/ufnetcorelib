/**
 * @file ratelimit_policy.h
 * @brief Application rate-limit policy helper backed by the netcore-owned
 *        ratelimit_policy config field.
 */

#ifndef UFNETCORE_RATELIMIT_POLICY_H
#define UFNETCORE_RATELIMIT_POLICY_H

void ConfigPrepareApplicationRateLimitPolicy(void);

#endif /* UFNETCORE_RATELIMIT_POLICY_H */
