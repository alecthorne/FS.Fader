# ETC open-source libraries (Apache-2.0) providing the ANSI E1.33 (RDMnet) implementation:
#   EtcPal  https://github.com/ETCLabs/EtcPal   (platform abstraction, FreeRTOS + lwIP targets)
#   RDM     https://github.com/ETCLabs/RDM      (ANSI E1.20 message helpers)
#   RDMnet  https://github.com/ETCLabs/RDMnet   (ANSI E1.33 device + LLRP target, lightweight mDNS)
# See VERSIONS.txt for the upstream commits vendored here.

set(ETCPAL EtcPal)
set(RDM RDM)
set(RDMNET RDMnet)

set(srcs
  # EtcPal core
  ${ETCPAL}/src/etcpal/acn_pdu.c
  ${ETCPAL}/src/etcpal/acn_rlp.c
  ${ETCPAL}/src/etcpal/common.c
  ${ETCPAL}/src/etcpal/error.c
  ${ETCPAL}/src/etcpal/handle_manager.c
  ${ETCPAL}/src/etcpal/log.c
  ${ETCPAL}/src/etcpal/mempool.c
  ${ETCPAL}/src/etcpal/pack.c
  ${ETCPAL}/src/etcpal/rbtree.c
  ${ETCPAL}/src/etcpal/timer.c
  ${ETCPAL}/src/etcpal/uuid.c
  ${ETCPAL}/src/etcpal/inet.c
  ${ETCPAL}/src/etcpal/netint.c
  ${ETCPAL}/src/etcpal/thirdparty/md5.c
  ${ETCPAL}/src/etcpal/thirdparty/sha1.c
  # EtcPal FreeRTOS
  ${ETCPAL}/src/os/freertos/etcpal/os_event_group.c
  ${ETCPAL}/src/os/freertos/etcpal/os_mutex.c
  ${ETCPAL}/src/os/freertos/etcpal/os_recursive_mutex.c
  ${ETCPAL}/src/os/freertos/etcpal/os_queue.c
  ${ETCPAL}/src/os/freertos/etcpal/os_rwlock.c
  ${ETCPAL}/src/os/freertos/etcpal/os_sem.c
  ${ETCPAL}/src/os/freertos/etcpal/os_signal.c
  ${ETCPAL}/src/os/freertos/etcpal/os_thread.c
  ${ETCPAL}/src/os/freertos/etcpal/os_timer.c
  ${ETCPAL}/src/os/freertos/etcpal/os_uuid.c
  # EtcPal lwIP
  ${ETCPAL}/src/os/lwip/etcpal/os_error.c
  ${ETCPAL}/src/os/lwip/etcpal/os_inet.c
  ${ETCPAL}/src/os/lwip/etcpal/os_netint.c
  ${ETCPAL}/src/os/lwip/etcpal/os_socket.c
  # RDM
  ${RDM}/src/rdm/controller.c
  ${RDM}/src/rdm/message.c
  ${RDM}/src/rdm/responder.c
  ${RDM}/src/rdm/uid.c
  # RDMnet API + core
  ${RDMNET}/src/rdmnet/common.c
  ${RDMNET}/src/rdmnet/controller.c
  ${RDMNET}/src/rdmnet/device.c
  ${RDMNET}/src/rdmnet/ept_client.c
  ${RDMNET}/src/rdmnet/llrp.c
  ${RDMNET}/src/rdmnet/llrp_manager.c
  ${RDMNET}/src/rdmnet/llrp_target.c
  ${RDMNET}/src/rdmnet/message.c
  ${RDMNET}/src/rdmnet/core/broker_prot.c
  ${RDMNET}/src/rdmnet/core/client.c
  ${RDMNET}/src/rdmnet/core/client_entry.c
  ${RDMNET}/src/rdmnet/core/common.c
  ${RDMNET}/src/rdmnet/core/connection.c
  ${RDMNET}/src/rdmnet/core/llrp.c
  ${RDMNET}/src/rdmnet/core/llrp_manager.c
  ${RDMNET}/src/rdmnet/core/llrp_prot.c
  ${RDMNET}/src/rdmnet/core/llrp_target.c
  ${RDMNET}/src/rdmnet/core/mcast.c
  ${RDMNET}/src/rdmnet/core/message.c
  ${RDMNET}/src/rdmnet/core/msg_buf.c
  ${RDMNET}/src/rdmnet/core/rpt_prot.c
  ${RDMNET}/src/rdmnet/core/util.c
  # RDMnet discovery (lightweight built-in mDNS querier)
  ${RDMNET}/src/rdmnet/disc/common.c
  ${RDMNET}/src/rdmnet/disc/discovered_broker.c
  ${RDMNET}/src/rdmnet/disc/monitored_scope.c
  ${RDMNET}/src/rdmnet/disc/registered_broker.c
  ${RDMNET}/src/rdmnet/disc/lightweight/rdmnet_disc_lightweight.c
  ${RDMNET}/src/rdmnet/disc/lightweight/lwmdns_common.c
  ${RDMNET}/src/rdmnet/disc/lightweight/lwmdns_recv.c
  ${RDMNET}/src/rdmnet/disc/lightweight/lwmdns_send.c
)

idf_component_register(
  SRCS ${srcs}
  INCLUDE_DIRS
    config
    ${ETCPAL}/include
    ${ETCPAL}/include/os/freertos
    ${ETCPAL}/include/os/lwip
    ${RDM}/include
    ${RDMNET}/include
  PRIV_INCLUDE_DIRS
    ${ETCPAL}/src
    ${RDM}/src
    ${RDMNET}/src
    ${RDMNET}/src/rdmnet/disc/lightweight
  REQUIRES freertos lwip
  PRIV_REQUIRES esp_system
)

# EtcPal includes "FreeRTOS.h" / "semphr.h" directly; ESP-IDF keeps them under freertos/.
idf_component_get_property(freertos_dir freertos COMPONENT_DIR)
target_include_directories(${COMPONENT_LIB} PUBLIC ${freertos_dir}/FreeRTOS-Kernel/include/freertos)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
  ETCPAL_HAVE_CONFIG_H=1
  RDMNET_HAVE_CONFIG_H=1
  ETCPAL_TARGETING_FREERTOS=1
  ETCPAL_NO_EXCEPTIONS
)
# Upstream code is warning-clean on desktop compilers; keep vendored code from failing -Werror here.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-error=all -Wno-address -Wno-unused-variable -Wno-unused-function
  -Wno-format -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-char-subscripts)
