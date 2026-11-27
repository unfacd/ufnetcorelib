/*
 * ufsrv_config_stubs_as_string.c — GENERATED (one-off), DO NOT
 * EDIT.  Rename (drop _generated) once the stubs are hand-maintained.
 */
#include "ufsrv_config_stubs_as_string_priv.h"
#include <ufsrv_core/ratelimit_v2/ratelimit_application_policy_type.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

BufferDescriptor *
UfsrvConfigUfsrvmediaUploadUriAsString_const_char_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for const char *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigMsgqueueSubAsString_MessageQueueBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for MessageQueueBackend *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigPersistenceBackendAsString_PersistenceBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for PersistenceBackend *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigUsrmsgCachebackendAsString_UserMessageCacheBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for UserMessageCacheBackend *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigFenceCachebackendAsString_FenceCacheBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for FenceCacheBackend *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCloudAuthorizationFcmAsString_CloudAuthorizationToken_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for CloudAuthorizationToken *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCloudAuthorizationIntegrityApiAsString_CloudAuthorizationToken_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for CloudAuthorizationToken *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCryptoPrivateServerParamsAsString_uint8_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for uint8_t *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCryptoPublicServerParamsAsString_uint8_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for uint8_t *. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCryptoPrivateKeyServerAsString_ec_private_key(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for ec_private_key. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCryptoPublicKeyServerAsString_ec_public_key(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for ec_public_key. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigCryptoPublicKeyServerSerialisedAsString_ec_public_key(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  /* TODO: hand-author string formatting for ec_public_key. */
  (void)field_index;
  (void)value_ptr;
  (void)buffer_provided;
  return NULL;
}

BufferDescriptor *
UfsrvConfigRatelimitPolicyAsString_RateLimitApplicationPolicy(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided)
{
  (void)field_index;
  if (!buffer_provided || !value_ptr) return NULL;

  const RateLimitApplicationPolicy *policy =
      (const RateLimitApplicationPolicy *)value_ptr->stored_value.blob_val;
  if (!policy) {
    BufferDescriptorRelease(buffer_provided);
    return NULL;
  }

  if (BufferDescriptorAppendFormatted(buffer_provided, "is_enabled=%s,is_bypass_private_ip=%s",
                                      policy->is_enabled ? "true" : "false",
                                      policy->is_bypass_private_ip ? "true" : "false") != 0) {
    BufferDescriptorRelease(buffer_provided);
    return NULL;
  }

  return buffer_provided;
}
