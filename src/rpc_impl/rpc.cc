#include "rpc.h"

namespace ERpc {

template <class Transport_>
Rpc<Transport_>::Rpc(Nexus &nexus) : nexus(nexus) {
  Transport_ *transport = new Transport_();
  _unused(transport);
  nexus.register_hook((SessionManagementHook *)&sm_hook);
}

template <class Transport_>
Rpc<Transport_>::~Rpc() {}

template <class Transport_>
void Rpc<Transport_>::resolve_session(Session &session) {
  transport->send_resolve_session_msg(session);
}

template <class Transport_>
void Rpc<Transport_>::send_request(const Session &session,
                                   const Buffer &buffer) {
  _unused(session);
  _unused(buffer);
}

template <class Transport_>
void Rpc<Transport_>::send_response(const Session &session,
                                    const Buffer &buffer) {
  _unused(session);
  _unused(buffer);
};

template <class Transport_>
void Rpc<Transport_>::run_event_loop(){};

}  // End ERpc
