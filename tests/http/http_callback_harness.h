/**
 * @file http_callback_harness.h
 * @brief C entry points shared by the gtest driver and http_callback_harness.c.
 *
 * Strings are copied into caller buffers: the dictionary owns its keys and
 * values and frees them before these functions return.
 */

#ifndef HTTP_CALLBACK_HARNESS_H
#define HTTP_CALLBACK_HARNESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct visit_result {
	char first_key[32];
	char first_value[32];
	int count;
};

int HttpHarnessDictionaryPreorder(struct visit_result *result);
int HttpHarnessDictionaryMerge(char *x, size_t xlen, char *y, size_t ylen);
int HttpHarnessDictionaryHardDup(char *value, size_t len);
int HttpHarnessHandlerDispatch(int *calls);
int HttpHarnessPrivDataFree(int *calls);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_CALLBACK_HARNESS_H */
