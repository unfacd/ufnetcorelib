/*

 Copyright (c) 2015-2026 unfacd works

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU Affero General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU Affero General Public License for more details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

 */

#ifndef SRC_INCLUDE_UFSRVCMD_BROADCAST_TYPE_H_
#define SRC_INCLUDE_UFSRVCMD_BROADCAST_TYPE_H_

#define _INTERCOMMAND_SESSION		"UFSRV:SESSION"
#define _INTERCOMMAND_FENCE			"UFSRV:FENCE"
#define _INTERCOMMAND_MSG				"UFSRV:MSG"
#define _INTERCOMMAND_LOC				"UFSRV:LOC"
#define _INTERCOMMAND_USER			"UFSRV:USER"
#define _INTERCOMMAND_CALL			"UFSRV:CALL"
#define _INTERCOMMAND_RECEIPT		"UFSRV:RECEIPT"
#define _INTERCOMMAND_STATE	    "UFSRV:STATE"
#define _INTERCOMMAND_SYNC			"UFSRV:SYNC"
#define _INTERCOMMAND_SYS				"UFSRV:SYS"

//>>IMPORTANT INCREASE 'UFSRVCMDID_INTER_INTRA_CONVERSION_FACTOR' BELOW IF YOU MODIFY ENUMS

#define _INTRACOMMAND_MSG				"UFSRV:INTRA:MSG"
#define _INTRACOMMAND_FENCE			"UFSRV:INTRA:FENCE"
#define _INTRACOMMAND_USER			"UFSRV:INTRA:USER"
#define _INTRACOMMAND_SESSION		"UFSRV:INTRA:SESSION"
#define _INTRACOMMAND_LOC				"UFSRV:INTRA:LOC"
#define _INTRACOMMAND_CALL			"UFSRV:INTRA:CALL"
#define _INTRACOMMAND_RECEIPT		"UFSRV:INTRA:RECEIPT"
#define _INTRACOMMAND_STATE	    "UFSRV:INTRA:STATE"
#define _INTRACOMMAND_SYNC			"UFSRV:INTRA:SYNC"
#define _INTRACOMMAND_SYS				"UFSRV:INTRA:SYS"

#define REDIS_CMD_FENCE_PUBLISH_INTRAMSG 		"PUBLISH UFSRV:INTRA:FENCE %b"//for protobuf payload

//>>>>>>IMPORTNT SET THIS TO THE SIZE OF THE UFSRV_XXX SET. Used to add/substract index to find the corresponding INTRA->INTER or INTER->INTRA
#define UFSRVCMDID_INTER_INTRA_CONVERSION_FACTOR 	10

//both elements below are aligned index wise
//IMPORTANT TO KEEP THE SYMMETRY and order across the symmetry line

//IMPORTANT THIS SHOULD BE KEPT IN SYNC with 'enum UfsrvType' in protobuf schema file and  'enum UfsrvMsgCommandType' and the static
//command table defined in ufsrvcmd_broadcast.c

enum enumUfsrvBroadcastTopicIds {
	UFSRV_SESSION=0,
	UFSRV_FENCE,
	UFSRV_MSG,
	UFSRV_LOC,
	UFSRV_USER,
	UFSRV_CALL,
	UFSRV_RECEIPT,
	UFSRV_SYNC,
  UFSRV_STATE,
	UFSRV_SYS,

	//------------------// symmetry line

	UFSRV_INTRA_SESSION,
	UFSRV_INTRA_FENCE,
	UFSRV_INTRA_MSG,
	UFSRV_INTRA_LOC,
	UFSRV_INTRA_USER,
	UFSRV_INTRA_CALL,
	UFSRV_INTRA_RECEIPT,
	UFSRV_INTRA_SYNC,
  UFSRV_INTRA_STATE,
	UFSRV_INTRA_SYS,

	UFSRV_MAX_BROADCAST_ID
};


#endif /* SRC_INCLUDE_UFSRVCMD_BROADCAST_TYPE_H_ */
