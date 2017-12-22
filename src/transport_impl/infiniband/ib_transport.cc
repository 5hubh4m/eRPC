#include <iomanip>
#include <stdexcept>

#include "ib_transport.h"
#include "util/huge_alloc.h"

namespace erpc {

constexpr size_t IBTransport::kMaxDataPerPkt;

// Initialize the protection domain, queue pair, and memory registration and
// deregistration functions. RECVs will be initialized later when the hugepage
// allocator is provided.
IBTransport::IBTransport(uint8_t rpc_id, uint8_t phy_port)
    : Transport(TransportType::kInfiniBand, rpc_id, phy_port) {
  rt_assert(kHeadroom == 0, "Invalid packet header headroom for InfiniBand");
  resolve_phy_port();
  init_verbs_structs();
  init_mem_reg_funcs();

  LOG_INFO("eRPC IBTransport: Created for ID %u. Device %s, port %d.\n", rpc_id,
           resolve.ib_ctx->device->name, resolve.dev_port_id);
}

void IBTransport::init_hugepage_structures(HugeAlloc *huge_alloc,
                                           uint8_t **rx_ring) {
  this->huge_alloc = huge_alloc;
  this->numa_node = huge_alloc->get_numa_node();

  init_recvs(rx_ring);
  init_sends();
}

// The transport destructor is called after \p huge_alloc has already been
// destroyed by \p Rpc. Deleting \p huge_alloc deregisters and frees all SHM
// memory regions.
//
// We only need to clean up non-hugepage structures.
IBTransport::~IBTransport() {
  LOG_INFO("eRPC IBTransport: Destroying transport for ID %u\n", rpc_id);

  // Destroy QPs and CQs. QPs must be destroyed before CQs.
  exit_assert(ibv_destroy_qp(qp) == 0,
              "eRPC IBTransport: Failed to destroy SEND QP.");

  exit_assert(ibv_destroy_cq(send_cq) == 0,
              "eRPC IBTransport: Failed to destroy send CQ.");

  exit_assert(ibv_destroy_cq(recv_cq) == 0,
              "eRPC IBTransport: Failed to destroy RECV CQ.");

  exit_assert(ibv_destroy_ah(self_ah) == 0,
              "eRPC IBTransport: Failed to destroy self address handle.");

  // Destroy protection domain and device context
  exit_assert(ibv_dealloc_pd(pd) == 0,
              "eRPC IBTransport: Failed to destroy protection domain.");

  exit_assert(ibv_close_device(resolve.ib_ctx) == 0,
              "eRPC IBTransport: Failed to close device.");
}

struct ibv_ah *IBTransport::create_ah(const ib_routing_info_t *ib_rinfo) const {
  struct ibv_ah_attr ah_attr;
  memset(&ah_attr, 0, sizeof(struct ibv_ah_attr));
  ah_attr.is_global = is_roce() ? 1 : 0;
  ah_attr.dlid = is_roce() ? 0 : ib_rinfo->port_lid;
  ah_attr.sl = 0;
  ah_attr.src_path_bits = 0;
  ah_attr.port_num = resolve.dev_port_id;  // Local port

  if (is_roce()) {
    ah_attr.grh.dgid.global.interface_id = ib_rinfo->gid.global.interface_id;
    ah_attr.grh.dgid.global.subnet_prefix = ib_rinfo->gid.global.subnet_prefix;
    ah_attr.grh.sgid_index = 0;
    ah_attr.grh.hop_limit = 1;
  }

  return ibv_create_ah(pd, &ah_attr);
}

void IBTransport::fill_local_routing_info(RoutingInfo *routing_info) const {
  memset(static_cast<void *>(routing_info), 0, kMaxRoutingInfoSize);
  auto *ib_routing_info = reinterpret_cast<ib_routing_info_t *>(routing_info);
  ib_routing_info->port_lid = resolve.port_lid;
  ib_routing_info->qpn = qp->qp_num;
  if (is_roce()) ib_routing_info->gid = resolve.gid;
}

bool IBTransport::resolve_remote_routing_info(RoutingInfo *routing_info) const {
  auto *ib_rinfo = reinterpret_cast<ib_routing_info_t *>(routing_info);
  ib_rinfo->ah = create_ah(ib_rinfo);
  return (ib_rinfo->ah != nullptr);
}

void IBTransport::resolve_phy_port() {
  std::ostringstream xmsg;  // The exception message

  // Get the device list
  int num_devices = 0;
  struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
  rt_assert(dev_list != nullptr,
            "eRPC IBTransport: Failed to get InfiniBand device list");

  // Traverse the device list
  int ports_to_discover = phy_port;

  for (int dev_i = 0; dev_i < num_devices; dev_i++) {
    struct ibv_context *ib_ctx = ibv_open_device(dev_list[dev_i]);
    rt_assert(ib_ctx != nullptr,
              "eRPC IBTransport: Failed to open dev " + std::to_string(dev_i));

    struct ibv_device_attr device_attr;
    memset(&device_attr, 0, sizeof(device_attr));
    if (ibv_query_device(ib_ctx, &device_attr) != 0) {
      xmsg << "eRPC IBTransport: Failed to query InfiniBand device "
           << std::to_string(dev_i);
      throw std::runtime_error(xmsg.str());
    }

    for (uint8_t port_i = 1; port_i <= device_attr.phys_port_cnt; port_i++) {
      // Count this port only if it is enabled
      struct ibv_port_attr port_attr;
      if (ibv_query_port(ib_ctx, port_i, &port_attr) != 0) {
        xmsg << "eRPC IBTransport: Failed to query port "
             << std::to_string(port_i) << " on device " << ib_ctx->device->name;
        throw std::runtime_error(xmsg.str());
      }

      if (port_attr.phys_state != IBV_PORT_ACTIVE &&
          port_attr.phys_state != IBV_PORT_ACTIVE_DEFER) {
        continue;
      }

      if (ports_to_discover == 0) {
        // Resolution succeeded. Check if the link layer matches.
        if (is_infiniband() &&
            port_attr.link_layer != IBV_LINK_LAYER_INFINIBAND) {
          throw std::runtime_error(
              "Transport type required is InfiniBand but port link layer is " +
              link_layer_str(port_attr.link_layer));
        }

        if (is_roce() && port_attr.link_layer != IBV_LINK_LAYER_ETHERNET) {
          throw std::runtime_error(
              "Transport type required is RoCE but port link layer is " +
              link_layer_str(port_attr.link_layer));
        }

        // Check the class's constant MTU
        size_t active_mtu = enum_to_mtu(port_attr.active_mtu);
        if (kMTU > active_mtu) {
          throw std::runtime_error("Transport's required MTU is " +
                                   std::to_string(kMTU) + ", active_mtu is " +
                                   std::to_string(active_mtu));
        }

        resolve.device_id = dev_i;
        resolve.ib_ctx = ib_ctx;
        resolve.dev_port_id = port_i;
        resolve.port_lid = port_attr.lid;

        // Resolve and cache the ibv_gid struct for RoCE
        if (is_roce()) {
          int ret = ibv_query_gid(ib_ctx, resolve.dev_port_id, 0, &resolve.gid);
          rt_assert(ret == 0, "Failed to query GID");
        }

        return;
      }

      ports_to_discover--;
    }

    // Thank you Mario, but our port is in another device
    if (ibv_close_device(ib_ctx) != 0) {
      xmsg << "eRPC IBTransport: Failed to close InfiniBand device "
           << ib_ctx->device->name;
      throw std::runtime_error(xmsg.str());
    }
  }

  // If we are here, port resolution has failed
  assert(resolve.ib_ctx == nullptr);
  xmsg << "eRPC IBTransport: Failed to resolve InfiniBand port index "
       << std::to_string(phy_port);
  throw std::runtime_error(xmsg.str());
}

void IBTransport::init_verbs_structs() {
  assert(resolve.ib_ctx != nullptr && resolve.device_id != -1);

  // Create protection domain, send CQ, and recv CQ
  pd = ibv_alloc_pd(resolve.ib_ctx);
  rt_assert(pd != nullptr, "eRPC IBTransport: Failed to allocate PD");

  send_cq = ibv_create_cq(resolve.ib_ctx, kSQDepth, nullptr, nullptr, 0);
  rt_assert(send_cq != nullptr, "eRPC IBTransport: Failed to create SEND CQ");

  recv_cq = ibv_create_cq(resolve.ib_ctx, kRQDepth, nullptr, nullptr, 0);
  rt_assert(recv_cq != nullptr, "eRPC IBTransport: Failed to create SEND CQ");

  // Initialize QP creation attributes
  struct ibv_qp_init_attr create_attr;
  memset(static_cast<void *>(&create_attr), 0, sizeof(struct ibv_qp_init_attr));
  create_attr.send_cq = send_cq;
  create_attr.recv_cq = recv_cq;
  create_attr.qp_type = IBV_QPT_UD;

  create_attr.cap.max_send_wr = kSQDepth;
  create_attr.cap.max_recv_wr = kRQDepth;
  create_attr.cap.max_send_sge = 1;
  create_attr.cap.max_recv_sge = 1;
  create_attr.cap.max_inline_data = kMaxInline;

  qp = ibv_create_qp(pd, &create_attr);
  rt_assert(qp != nullptr, "eRPC IBTransport: Failed to create QP");

  // Transition QP to INIT state
  struct ibv_qp_attr init_attr;
  memset(static_cast<void *>(&init_attr), 0, sizeof(struct ibv_qp_attr));
  init_attr.qp_state = IBV_QPS_INIT;
  init_attr.pkey_index = 0;
  init_attr.port_num = static_cast<uint8_t>(resolve.dev_port_id);
  init_attr.qkey = kQKey;

  int attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
  if (ibv_modify_qp(qp, &init_attr, attr_mask) != 0) {
    throw std::runtime_error("eRPC IBTransport: Failed to modify QP to init");
  }

  // RTR state
  struct ibv_qp_attr rtr_attr;
  memset(static_cast<void *>(&rtr_attr), 0, sizeof(struct ibv_qp_attr));
  rtr_attr.qp_state = IBV_QPS_RTR;

  if (ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE)) {
    throw std::runtime_error("eRPC IBTransport: Failed to modify QP to RTR");
  }

  // Create self address handle. We use local routing info for convenience,
  // so this must be done after creating the QP.
  RoutingInfo self_routing_info;
  fill_local_routing_info(&self_routing_info);
  self_ah =
      create_ah(reinterpret_cast<ib_routing_info_t *>(&self_routing_info));
  rt_assert(self_ah != nullptr, "eRPC IBTransport: Failed to create self AH.");

  // Reuse rtr_attr for RTS
  rtr_attr.qp_state = IBV_QPS_RTS;
  rtr_attr.sq_psn = 0;  // PSN does not matter for UD QPs

  if (ibv_modify_qp(qp, &rtr_attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
    throw std::runtime_error("eRPC IBTransport: Failed to modify QP to RTS");
  }

  // Check if driver is modded for fast RECVs
  struct ibv_recv_wr mod_probe_wr;
  mod_probe_wr.wr_id = kModdedProbeWrID;
  struct ibv_recv_wr *bad_wr = &mod_probe_wr;

  int probe_ret = ibv_post_recv(qp, nullptr, &bad_wr);
  if (probe_ret != kModdedProbeRet) {
    LOG_WARN("eRPC IBTransport: Warning. No driver support for fast RECVs.\n");
    use_fast_recv = false;
  } else {
    LOG_INFO("eRPC IBTransport: Driver supports fast RECVs.\n");
    use_fast_recv = true;
  }
}

void IBTransport::init_mem_reg_funcs() {
  using namespace std::placeholders;
  assert(pd != nullptr);
  reg_mr_func = std::bind(ibv_reg_mr_wrapper, pd, _1, _2);
  dereg_mr_func = std::bind(ibv_dereg_mr_wrapper, _1);
}

void IBTransport::init_recvs(uint8_t **rx_ring) {
  std::ostringstream xmsg;  // The exception message

  // Initialize the memory region for RECVs
  size_t ring_extent_size = kNumRxRingEntries * kRecvSize;
  ring_extent = huge_alloc->alloc(ring_extent_size);
  if (ring_extent.buf == nullptr) {
    xmsg << "eRPC IBTransport: Failed to allocate " << std::setprecision(2)
         << 1.0 * ring_extent_size / MB(1) << "MB for ring buffers.";
    throw std::runtime_error(xmsg.str());
  }

  // Initialize constant fields of RECV descriptors
  for (size_t i = 0; i < kRQDepth; i++) {
    uint8_t *buf = ring_extent.buf;

    // From each slot of size kRecvSize = (kMTU + 64), we give up the first
    // (64 - kGRHBytes) bytes. Each slot is still large enough to receive the
    // GRH and kMTU payload bytes.
    size_t offset = (i * kRecvSize) + (64 - kGRHBytes);
    assert(offset + (kGRHBytes + kMTU) <= ring_extent_size);

    recv_sgl[i].length = kRecvSize;
    recv_sgl[i].lkey = ring_extent.lkey;
    recv_sgl[i].addr = reinterpret_cast<uint64_t>(&buf[offset]);

    recv_wr[i].wr_id = recv_sgl[i].addr + kGRHBytes;  // For quick prefetch
    recv_wr[i].sg_list = &recv_sgl[i];
    recv_wr[i].num_sge = 1;

    // Circular link
    recv_wr[i].next = (i < kRQDepth - 1) ? &recv_wr[i + 1] : &recv_wr[0];
    rx_ring[i] = &buf[offset + kGRHBytes];  // RX ring entry
  }

  // Fill the RECV queue. post_recv can use fast RECV, so it's not usable here.
  struct ibv_recv_wr *bad_wr;
  recv_wr[kRQDepth - 1].next = nullptr;  // Breaker of chains

  int ret = ibv_post_recv(qp, &recv_wr[0], &bad_wr);
  rt_assert(ret == 0, "eRPC IBTransport: Failed to fill RECV queue.");

  recv_wr[kRQDepth - 1].next = &recv_wr[0];  // Restore circularity
}

void IBTransport::init_sends() {
  for (size_t i = 0; i < kPostlist; i++) {
    send_wr[i].next = &send_wr[i + 1];
    send_wr[i].wr.ud.remote_qkey = kQKey;
    send_wr[i].opcode = IBV_WR_SEND_WITH_IMM;
    send_wr[i].sg_list = &send_sgl[i][0];
  }
}

}  // End erpc