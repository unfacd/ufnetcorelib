/**
 * @file http_callback_harness.c
 * @brief C side of http_callback_dispatch_tests: drives the real callbacks.
 *
 * The gtest driver is C++ and the session headers use Blocks, so the calls
 * into the HTTP dispatch machinery are made here where the real prototypes are
 * visible and -fblocks is active.
 */

#include <stdio.h>
#include <string.h>

#include <ufsrvcorelib/ufsrv_core/http/dictionary.h>
#include <ufsrvcorelib/ufsrv_core/http/h_handler.h>

#include "http_callback_harness.h"

static void collect_visitor(void *data, const char *key, const void *value, int flags) {
	struct visit_result *result = data;
	(void)flags;
	if (result->count == 0) {
		snprintf(result->first_key, sizeof(result->first_key), "%s", key);
		snprintf(result->first_value, sizeof(result->first_value), "%s", (const char *)value);
	}
	result->count++;
}

int HttpHarnessDictionaryPreorder(struct visit_result *result) {
	onion_dict *dict = onion_dict_new();
	onion_dict_add(dict, "b", "2", 0);
	onion_dict_add(dict, "a", "1", 0);
	onion_dict_add(dict, "c", "3", 0);

	result->first_key[0] = '\0';
	result->first_value[0] = '\0';
	result->count = 0;
	onion_dict_preorder(dict, collect_visitor, result);

	onion_dict_free(dict);
	return result->count;
}

int HttpHarnessDictionaryMerge(char *x, size_t xlen, char *y, size_t ylen) {
	onion_dict *dst = onion_dict_new();
	onion_dict *src = onion_dict_new();
	onion_dict_add(src, "x", "10", 0);
	onion_dict_add(src, "y", "20", 0);

	onion_dict_merge(dst, src);

	const char *xv = onion_dict_get(dst, "x");
	const char *yv = onion_dict_get(dst, "y");
	if (xv != NULL)
		snprintf(x, xlen, "%s", xv);
	if (yv != NULL)
		snprintf(y, ylen, "%s", yv);

	onion_dict_free(src);
	onion_dict_free(dst);
	return (xv != NULL && yv != NULL) ? 0 : -1;
}

int HttpHarnessDictionaryHardDup(char *value, size_t len) {
	onion_dict *src = onion_dict_new();
	onion_dict_add(src, "k", "v", 0);

	onion_dict *dup = onion_dict_hard_dup(src);
	const char *v = dup ? onion_dict_get(dup, "k") : NULL;
	if (v != NULL)
		snprintf(value, len, "%s", v);

	onion_dict_free(dup);
	onion_dict_free(src);
	return (v != NULL) ? 0 : -1;
}

static int handler_calls;

static onion_connection_status probe_handler(InstanceHolderForSession *instance_sesn_ptr,
                                             void *privdata, onion_request *request,
                                             onion_response *response) {
	(void)instance_sesn_ptr;
	(void)privdata;
	(void)request;
	(void)response;
	handler_calls++;
	return OCS_NOT_PROCESSED;
}

int HttpHarnessHandlerDispatch(int *calls) {
	onion_handler *handler = onion_handler_new(probe_handler, NULL, NULL);
	handler_calls = 0;

	/* OCS_NOT_PROCESSED walks to handler->next (NULL), so no response is
	 * touched and a NULL session holder is never dereferenced. */
	onion_connection_status status = onion_handler_handle(NULL, handler, NULL, NULL);

	*calls = handler_calls;
	onion_handler_free(handler);
	return (int)status;
}

static int priv_data_free_calls;

static void free_probe_priv_data(void *privdata) {
	(void)privdata;
	priv_data_free_calls++;
}

int HttpHarnessPrivDataFree(int *calls) {
	/* A non-NULL priv_data is required for onion_handler_free() to invoke the
	 * destructor through the function pointer. */
	static int priv_marker;
	onion_handler *handler = onion_handler_new(NULL, &priv_marker, free_probe_priv_data);
	priv_data_free_calls = 0;

	onion_handler_free(handler);

	*calls = priv_data_free_calls;
	return 0;
}
