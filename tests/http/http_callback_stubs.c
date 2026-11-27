/**
 * @file http_callback_stubs.c
 * @brief Link stubs for the parts of the HTTP machine the dispatch tests do not
 *        exercise.
 *
 * These tests drive callback dispatch only: the probe handler returns
 * OCS_NOT_PROCESSED, so no response is ever written, no url is created and the
 * default error handler is never called.  The stubs exist so that the real
 * h_handler.c can be compiled into the test without dragging in the response,
 * url and request machinery (which in turn needs the consumer-supplied config
 * accessors).
 */

#include <stddef.h>

#include <ufsrvcorelib/ufsrv_core/http/h_handler.h>
#include <ufsrvcorelib/ufsrv_core/http/response.h>
#include <ufsrvcorelib/ufsrv_core/http/url.h>

void onion_response_set_length(onion_response *res, size_t length) {
	(void)res;
	(void)length;
}

int onion_response_network_flush(InstanceHolderForSession *instance_sesn_ptr, onion_response *res) {
	(void)instance_sesn_ptr;
	(void)res;
	return 0;
}

void onion_response_set_code(onion_response *res, int code) {
	(void)res;
	(void)code;
}

int onion_response_write_headers(InstanceHolderForSession *instance_sesn_ptr, onion_response *res) {
	(void)instance_sesn_ptr;
	(void)res;
	return 0;
}

ssize_t onion_response_write(InstanceHolderForSession *instance_sesn_ptr, onion_response *res,
                               const char *data, size_t length) {
	(void)instance_sesn_ptr;
	(void)res;
	(void)data;
	(void)length;
	return 0;
}

onion_url *onion_url_new(void) {
	return NULL;
}
