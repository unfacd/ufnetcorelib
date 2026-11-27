/**
 * Copyright (C) 2015-2025 unfacd works
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

#ifdef HAVE_CONFIG_UFNETCORELIB_H
#include <config_ufnetcorelib.h>
#endif

#include <main_ufnetcorelib.h>

#include <ufnet_msgcore/session_service.h>
#include <ufnet_msgcore/utils_location.h>
#include <ufnet_core/user_prefs/user_prefs.h>

unsigned
CreateSessionService(Session *sesn_ptr)
{
	LoadDefaultUserPreferences(sesn_ptr);

	return 1;

}	  /**/

/**
 * 	@brief:
 * 	@worker: ufsrv and session
 * 	@access_context sesn_ptr_target: must be fully loaded
 */
void
DestructSessionService(InstanceHolderForSession *instance_sesn_ptr_target, unsigned call_flags)
{
  SessionService *ss_ptr = NULL;
  Session *sesn_ptr_target = SessionOffInstanceHolder(instance_sesn_ptr_target);

  ss_ptr = &(sesn_ptr_target->ufnet_session.sservice);

  ResetUser(instance_sesn_ptr_target, call_flags);
  memset(ss_ptr, 0, sizeof(SessionService));

}

/**
 * 	@sesn_ptr_target: must have full access context loaded already
 */
void
ResetSessionService(InstanceHolderForSession *instance_sesn_ptr_target, unsigned call_flags)
{
	ResetSessionGeoFenceData(instance_sesn_ptr_target);
	ResetUser(instance_sesn_ptr_target, call_flags);

}