/**
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef UFSRV_GPC_SERVICE_ACCOUNT_H
#define UFSRV_GPC_SERVICE_ACCOUNT_H

#include <ufnet_msgcore/gpc_service_request_descriptor.h>
#include <ufsrv_core/jwt/jwt.h>

/**
 * @brief Supplies the signing key of the named Google Cloud service account.
 *
 * The service-account document is held in the configuration as an enc:v1
 * envelope; it is decrypted once, on first use, and the key set it yields is
 * shared read-only by every caller and every thread thereafter.
 *
 * @param[in] service_account which service account to load
 * @param[out] keyset_out receives the loaded key set, or NULL
 * @param[out] key_id_out receives the key id the document was filed under at Google
 * @return 0 on success, -1 when the document is absent, undecryptable or malformed
 */
int ProvideGpcServiceAccountSigningKey(enum GpcServiceAccount service_account, JwtKeyset **keyset_out, const char **key_id_out);

#endif //UFSRV_GPC_SERVICE_ACCOUNT_H
