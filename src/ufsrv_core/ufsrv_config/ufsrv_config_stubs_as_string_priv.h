/*
 * ufsrv_config_stubs_as_string_priv.h — hand-maintained.
 *
 * Declares the non-standard AsString marshallers for ufnetcorelib's own fields
 * plus those contributed by every library joined into the composition
 * (currently ufsrvcorelib).  The generated accessor definitions include THIS
 * file, so it must carry the UNION across all joined libraries — see G10.
 *
 * ufsrv_config_compose_host emits the merged union to
 * ufsrv_config_stubs_as_string_priv_generated.h and never overwrites this file;
 * after a compose that changes the symbol set, reconcile the two by hand
 * (ufnetcorelib 20 + ufsrvcorelib 25 = 45).
 */
#ifndef UFSRV_CONFIG_STUBS_AS_STRING_PRIV_GENERATED_H
#define UFSRV_CONFIG_STUBS_AS_STRING_PRIV_GENERATED_H

#include <ufsrvcorelib/ufsrv_core/ufsrv_config/ufsrv_config_value_type.h>
#include <uflib/main_types.h>

/* === ufnetcorelib's own fields === */
extern BufferDescriptor * UfsrvConfigUfsrvmediaUploadUriAsString_const_char_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigMsgqueueSubAsString_MessageQueueBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigPersistenceBackendAsString_PersistenceBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigUsrmsgCachebackendAsString_UserMessageCacheBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigFenceCachebackendAsString_FenceCacheBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCloudAuthorizationFcmAsString_CloudAuthorizationToken_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCloudAuthorizationIntegrityApiAsString_CloudAuthorizationToken_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCryptoPrivateServerParamsAsString_uint8_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCryptoPublicServerParamsAsString_uint8_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCryptoPrivateKeyServerAsString_ec_private_key(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCryptoPublicKeyServerAsString_ec_public_key(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigCryptoPublicKeyServerSerialisedAsString_ec_public_key(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigRatelimitPolicyAsString_RateLimitApplicationPolicy(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);

/* === joined from ufsrvcorelib (ufsrv_config_stubs_as_string_priv.h) === */
extern BufferDescriptor * UfsrvConfigConfFileStoreOperatingStateAsString_enum_UfsrvConfigStoreOperatingState(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigRuntimeStoreOperatingStateAsString_enum_UfsrvConfigStoreOperatingState(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigServerProcessModeAsString_enum_ServerProcessMode(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigServerRunModeAsString_enum_ServerRunMode(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigServerCpuAffinityAsString_enum_CpuAffinityPolicy(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigLuaHandleAsString_lua_State_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigInstrumentationBackendAsString_InstrumentationBackend_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigInstrumentationBackendServerAsString_struct_sockaddr_in(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSessionsDelegatorAsString_SessionsDelegator_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigWorkDelegatorPipeAsString_InstanceHolderForSession_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigPipefdsAsString_int_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslCommandConsoleAsString_SSL_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslCtxAsString_SSL_CTX_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslUserCtxAsString_SSL_CTX_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigThreadHandleTimerDelegatorAsString_pthread_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigFileLoaderServiceAsString_FileLoaderServiceDescriptor(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigOauthSettingsAsString_UfsrvConfigOauth(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigUfsrvResultAsString_UFSRVResult(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigThreadsSubsystemUfsrvOnceAsString_pthread_once_t(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslMutexesAsString_pthread_mutex_t_pointer(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslCommandConsoleAllowListGodAsString_DistinctArray(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslCommandConsoleAllowListBrowserAsString_DistinctArray(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigSslCommandConsoleDenyListAsString_DistinctArray(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);
extern BufferDescriptor * UfsrvConfigFileloaderBlackListedFilesRegistryAsString_CollectionDescriptor(int field_index, UfsrvConfigValue *value_ptr, BufferDescriptor *buffer_provided);

#endif /* UFSRV_CONFIG_STUBS_AS_STRING_PRIV_GENERATED_H */
