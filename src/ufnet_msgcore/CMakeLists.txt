# src/ufnet_msgcore/CMakeLists.txt
# Contributes the ufnet_msgcore sources to the ufnetcorelib OBJECT target.
#
# Explicit list (no globbing) — see src/ufnet_core/CMakeLists.txt for rationale.
# Includes the committed, in-tree generated protobuf sources (*.pb-c.c). They are
# produced from protobuf/*.proto by the protobuf_uf / protobuf_ufsrv targets
# (cmake/protobuf_schemas.cmake) and compiled here as ordinary sources. Paths are
# relative to this directory (CMP0076).

target_sources(${_PROJECT_NAME}
    PRIVATE
        # top level
        attachments.c
        gpc_http_utils.c
        gpc_service_account.c
        gpc_utils.c
        guardian.c
        integrity.c
        misc.c
        session.c
        session_cachebackend.c
        session_service.c
        share_list.c
        staged_messages_ufsrv.c
        system_roles.c
        utils_crypto.c
        utils_curve.c
        utils_db_account.c
        utils_location.c
        utils_nonce.c
        # generated protobuf (committed in-tree)
        Groups.pb-c.c
        UnfacdService.pb-c.c
        crypto_certificates.pb-c.c
        # donations
        donations/donations_db_utils.c
        donations/donations_http_utils.c
        donations/utils_donations.c
        # fence
        fence/fence.c
        fence/fence_events.c
        fence/fence_permission.c
        fence/fence_proto.c
        fence/fence_protobuf.c
        fence/fence_redis_backend.c
        fence/fence_utils.c
        fence/fence_utils_db.c
        # staged_out_queue
        staged_out_queue/utils_staged_out_queue.c
        staged_out_queue/fence_staged_out_queue.c
        staged_out_queue/user_staged_out_queue.c
        staged_out_queue/sync_staged_out_queue.c
        staged_out_queue/state_staged_out_queue.c
        # msgqueue_backend
        msgqueue_backend/UfsrvMessageQueue.pb-c.c
        msgqueue_backend/delegator_msgqueue_subscriber_thread.c
        msgqueue_backend/ufsrvmsgqueue.c
        # type_providers
        type_providers/attachment_descriptor_provider.c
        type_providers/mpsc_queue_node_provider.c
        type_providers/session_provider.c
        type_providers/type_provider_fence.c
        # user
        user/user_backend.c
        user/user_cachebackend.c
        user/user_redis_backend.c
        user/user_profile.c
        user/users_protobuf.c
        # zkgroup_utils
        crypto_zk.c
        zkgroup_utils/utils_zkgroup.c
        WebSocketMessage.pb-c.c
)
