#ifndef UFNETCORELIB_SESSION_RESULT_MACROS_H
#define UFNETCORELIB_SESSION_RESULT_MACROS_H

/* Result helpers for call sites that hold a composed Session pointer. The
 * carrier itself — UFSRVResult — is a member of UfsrvSession, so these are
 * only shorthands for w->ufsrv_session.result. */
#include <ufsrv_core/ufsrvresult_type.h>

//TODO: delete this
//w is Session *
#define _RETURN_RESULT_SESN(w,x, y, z)\
{\
	w->ufsrv_session.result.result_user_data=(void *)x;\
	w->ufsrv_session.result.result_type=y;\
	w->ufsrv_session.result.result_code=z;\
	return (void *)&(w->ufsrv_session.result);\
}

#define SESSION_RETURN_RESULT(w,x, y, z)\
{\
	w->ufsrv_session.result.result_user_data=(void *)x;\
	w->ufsrv_session.result.result_type=y;\
	w->ufsrv_session.result.result_code=z;\
	return (void *)&(w->ufsrv_session.result);\
}

//Session *
#define SESSION_RESULT_TYPE_SUCCESS(x) ((x->ufsrv_session.result.result_type==RESULT_TYPE_SUCCESS) || (x->ufsrv_session.result.result_type==RESULT_TYPE_SUCCESS_INFORM_USER) || (x->ufsrv_session.result.result_type==RESULT_TYPE_SUCCESS_NOOP))
#define SESSION_RESULT_TYPE_ERROR(x) ((x->ufsrv_session.result.result_type==RESULT_TYPE_ERR) || (x->ufsrv_session.result.result_type==RESULT_TYPE_BACKENDERR) || (x->ufsrv_session.result.result_type==RESULT_TYPE_PROTOCOLERR) || (x->ufsrv_session.result.result_type==RESULT_TYPE_IOERR))
#define SESSION_RESULT_TYPE_WARNING(x) ((x->ufsrv_session.result.result_type==RESULT_TYPE_WARN_NOOP) || (x->ufsrv_session.result.result_type==RESULT_TYPE_WARN_INFORM_USER))

#define SESSION_RESULT_CODE_EQUAL(x, y) (x->ufsrv_session.result.result_code==y)
#define SESSION_RESULT_TYPE_EQUAL(x, y) (x->ufsrv_session.result.result_type==y)
#define SESSION_RESULT_USERDATA(x) (x->ufsrv_session.result.result_user_data)

#define SESSION_RESULT_CODE(x) (x->ufsrv_session.result.result_code)
#define SESSION_RESULT_TYPE(x) (x->ufsrv_session.result.result_type)

#endif
