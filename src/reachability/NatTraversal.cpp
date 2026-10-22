#include "mesh/reachability/NatTraversal.h"

#include <cstdio>
#include <mutex>
#include <string>

#if defined(PP_MESH_HAS_MINIUPNPC)
#include <miniupnpc.h>
#include <upnpcommands.h>
#endif

namespace pp::mesh {

namespace {

#if defined(PP_MESH_HAS_MINIUPNPC)
std::mutex g_upnp_mutex;
int g_mapped_tcp_external_port = 0;
int g_mapped_udp_external_port = 0;
#endif

UpnpMappingResult TryUpnpPortMapping(int internal_port, int external_port, const char* protocol) {
  UpnpMappingResult out;
  if (internal_port <= 0 || internal_port > 65535) {
    out.error = "invalid internal port";
    return out;
  }
  if (external_port <= 0) {
    external_port = internal_port;
  }

#if defined(PP_MESH_HAS_MINIUPNPC)
  std::lock_guard lock(g_upnp_mutex);

  UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
  if (!devlist) {
    out.error = "UPnP discovery failed";
    return out;
  }

  UPNPUrls urls;
  IGDdatas data;
  // The host's LAN address toward the IGD: the mapping's internal client.
  char lanaddr[64] = {};
  char wanaddr[64] = {};
  const int status = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr), wanaddr, sizeof(wanaddr));
  if (status != UPNP_CONNECTED_IGD) {
    // A private WAN address (double NAT) would map a port nobody outside can reach.
    if (status != UPNP_NO_IGD) {
      FreeUPNPUrls(&urls);
    }
    freeUPNPDevlist(devlist);
    out.error = status == UPNP_PRIVATEIP_IGD ? "UPnP IGD has a private WAN address" : "no connected UPnP IGD found";
    return out;
  }

  char ext_ip[64] = {};
  if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, ext_ip) != UPNPCOMMAND_SUCCESS) {
    ext_ip[0] = '\0';
  }

  char int_port_str[16] = {};
  char ext_port_str[16] = {};
  snprintf(int_port_str, sizeof(int_port_str), "%d", internal_port);
  snprintf(ext_port_str, sizeof(ext_port_str), "%d", external_port);

  const int map_rc = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, ext_port_str, int_port_str,
                                         lanaddr, "pp-mesh", protocol, nullptr, "86400");
  FreeUPNPUrls(&urls);
  freeUPNPDevlist(devlist);

  if (map_rc != UPNPCOMMAND_SUCCESS) {
    out.error = "UPnP AddPortMapping failed";
    return out;
  }

  if (std::string(protocol) == "UDP") {
    g_mapped_udp_external_port = external_port;
  } else {
    g_mapped_tcp_external_port = external_port;
  }
  out.ok = true;
  out.external_ip = ext_ip;
  out.external_port = external_port;
  return out;
#else
  (void)protocol;
  out.error = "UPnP not available in this build";
  return out;
#endif
}

void ReleaseUpnpPortMapping(int external_port, const char* protocol) {
#if defined(PP_MESH_HAS_MINIUPNPC)
  if (external_port <= 0) {
    return;
  }
  std::lock_guard lock(g_upnp_mutex);

  UPNPDev* devlist = upnpDiscover(1000, nullptr, nullptr, 0, 0, 2, nullptr);
  if (!devlist) {
    return;
  }
  UPNPUrls urls;
  IGDdatas data;
  const int igd_status = UPNP_GetValidIGD(devlist, &urls, &data, nullptr, 0, nullptr, 0);
  if (igd_status == UPNP_NO_IGD) {
    freeUPNPDevlist(devlist);
    return;
  }
  char ext_port_str[16] = {};
  snprintf(ext_port_str, sizeof(ext_port_str), "%d", external_port);
  UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, ext_port_str, protocol, nullptr);
  FreeUPNPUrls(&urls);
  freeUPNPDevlist(devlist);
  if (std::string(protocol) == "UDP") {
    if (g_mapped_udp_external_port == external_port) {
      g_mapped_udp_external_port = 0;
    }
  } else if (g_mapped_tcp_external_port == external_port) {
    g_mapped_tcp_external_port = 0;
  }
#else
  (void)external_port;
  (void)protocol;
#endif
}

} // namespace

UpnpMappingResult TryUpnpUdpPortMapping(int internal_port, int external_port) {
  return TryUpnpPortMapping(internal_port, external_port, "UDP");
}

void ReleaseUpnpUdpPortMapping(int external_port) { ReleaseUpnpPortMapping(external_port, "UDP"); }

} // namespace pp::mesh
