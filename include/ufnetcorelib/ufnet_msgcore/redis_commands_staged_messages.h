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


#ifndef UFSRV_REDIS_COMMANDS_STAGED_MESSAGES_H
#define UFSRV_REDIS_COMMANDS_STAGED_MESSAGES_H

// MSGS_FOR:<%uid>
#define REDIS_CMD_INTRAMESSAGE_LIST_GETALL 				"ZRANGE STAGED_INTRAMSGS_%s_%d 0 -1"
#define REDIS_CMD_INTRAMESSAGE_LIST_GET_EARLIEST 	"ZRANGE STAGED_INTRAMSGS_%s_%d 0 0"
#define REDIS_CMD_INTRAMESSAGE_LIST_GET_LAST 			"ZRANGE STAGED_INTRAMSGS_%s_%d -1 -1"
#define REDIS_CMD_INTRAMESSAGE_LIST_GET_NAMED 		"ZRANGE STAGED_INTRAMSGS_%s_%d %s"
#define REDIS_CMD_INTRAMESSAGE_LIST_REM_EARLIEST	"ZREMRANGEBYRANK STAGED_INTRAMSGS_%s_%d 0 0"
#define REDIS_CMD_INTRAMESSAGE_LIST_REM_LAST			"ZREMRANGEBYRANK STAGED_INTRAMSGS_%s_%d -1 -1"
#define REDIS_CMD_INTRAMESSAGE_LIST_REM_ALL				"ZREMRANGEBYRANK STAGED_INTRAMSGS_%s_%d 0 -1"


//OLD  ZADD INTRAMSGS__class>_<geogroup> <%now> <type>:<%uid_from>:<%fid>:<rawb64msg>
//#define REDIS_CMD_INTRAMESSAGE_RECORD_ADD "ZADD STAGED_INTRAMSGS_%s_%d %lu %d:%lu:%lu:%s"

//ZADD INTRAMSGS_<%class>_<%geogroup> <%reqid> <%reqid>:<%type>:<raw binarymsg>
#define REDIS_CMD_INTRAMESSAGE_RECORD_ADD "ZADD STAGED_INTRAMSGS_%s_%d %lu %lu:%d:%b"

//ZADD MSGS_FOR:<%uid> <%now> <type>:<%fid:%uidfrom>:<rawb64msg>
//#define REDIS_CMD_INMESSAGE_RECORD_ADD "ZADD MSGS_FOR:%lu %lu %d:%lu:%lu:%s"
#define REDIS_CMD_INTRAMESSAGE_RECORD_REM	"ZREM STAGED_INTRAMSGS_%s_%d %s"

//<%uid> <%current unix time>
#define REDIS_CMD_INTRAMESSAGE_RECORDE_EXPIRE	"ZREMRANGEBYSCORE STAGED_INTRAMSGS_%s_%d 0 %lu"


//ZADD INMSGS:<%uid_to> <%now_inmillis> <%fid>:<eid>
#define REDIS_CMD_STAGED_OUTMSG_EVENT_RECORD_ADD 		"ZADD STAGED_OUTMSG_EVENTS:%lu %lu %lu:%lu"
#define REDIS_CMD_STAGED_OUTMSG_EVENT_RECORD_GETALL "ZRANGE STAGED_OUTMSG_EVENTS:%lu 0 -1"
#define REDIS_CMD_STAGED_OUTMSG_EVENT_RECORD_EXPIRE	"ZREMRANGEBYSCORE STAGED_OUTMSG_EVENTS:%lu 0 %lu"
#define REDIS_CMD_STAGED_OUTMSG_EVENT_RECORD_REM		"ZREM STAGED_OUTMSG_EVENTS:%lu %lu:%lu"
#define REDIS_CMD_STAGED_OUTMSG_EVENT_COUNT					"ZCARD STAGED_OUTMSG_EVENTS:%lu"

//"HSET STAGED_INMSGS:<%uid> %<fid>:%<eid> %<msg_sz>:%<msg>"
#define REDIS_CMD_STAGED_OUTMSG_MSG_COMMAND_HEADER "HDEL STAGED_OUTMSG:"
#define REDIS_CMD_STAGED_OUTMSG_MSG_RECORD_GET "HGET STAGED_OUTMSG:%lu %s"//when we get the message willbe already formatted as <fid:eid>
#define REDIS_CMD_STAGED_OUTMSG_MSG_RECORD_ADD "HSET STAGED_OUTMSG:%lu %lu:%lu %lu:%b"
#define REDIS_CMD_STAGED_OUTMSG_MSG_RECORD_DEL "HDEL STAGED_OUTMSG:%lu %lu:%lu"

#define REDIS_CMD_FENCE_OUT_QUEUE_ADD "RPUSH FENCE_OUT_QUEUE:%lu:%lu %b"
#define REDIS_CMD_FENCE_OUT_QUEUE_GET "LPOP FENCE_OUT_QUEUE:%lu:%lu"

//<uid> is the target user the queue belongs to; <eid> is that user's event id
#define REDIS_CMD_USER_OUT_QUEUE_ADD "RPUSH USER_OUT_QUEUE:%lu:%lu %b"
#define REDIS_CMD_USER_OUT_QUEUE_GET "LPOP USER_OUT_QUEUE:%lu:%lu"

//<uid> is the user whose event the sync copy belongs to; <eid> is that user's event id
#define REDIS_CMD_SYNC_OUT_QUEUE_ADD "RPUSH SYNC_OUT_QUEUE:%lu:%lu %b"
#define REDIS_CMD_SYNC_OUT_QUEUE_GET "LPOP SYNC_OUT_QUEUE:%lu:%lu"

//<uid> is the user whose event the state belongs to; <eid> is that user's event id
#define REDIS_CMD_STATE_OUT_QUEUE_ADD "RPUSH STATE_OUT_QUEUE:%lu:%lu %b"
#define REDIS_CMD_STATE_OUT_QUEUE_GET "LPOP STATE_OUT_QUEUE:%lu:%lu"

//<%uid> <%identifier> <%ttl time in socends>
//set processing lock
#define REDIS_CMD_STAGEDINMSG_PLOCK				"SET STAGED_INMSGS:PL:%lu %s NX EX %lu"
//<%sha1id>  <%uid> 1 is number of keys given as argument into the scrit
#define REDIS_CMD_STAGEDINMSG_DEL_PLOCK		"EVALSHA %s 1 STAGED_INMSGS:PL:%lu L"

#endif //UFSRV_REDIS_COMMANDS_STAGED_MESSAGES_H