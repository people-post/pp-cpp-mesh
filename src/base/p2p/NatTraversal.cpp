#include "base/p2p/NatTraversal.h"

#include <mutex>
#include <string>

#if defined(PP_BROWSER_HAS_MINIUPNPC)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#endif

namespace pbr {

namespace {

#if defined(PP_BROWSER_HAS_MINIUPNPC)
std::mutex g_upnp_mutex;
int g_mapped_external_port = 0;
#endif

} // namespace

UpnpMappingResult TryUpnpTcpPortMapping(int internal_port, int external_port) {
  UpnpMappingResult out;
  if (internal_port <= 0 || internal_port > 65535) {
    out.error = "invalid internal port";
    return out;
  }
  if (external_port <= 0) {
    external_port = internal_port;
  }

#if defined(PP_BROWSER_HAS_MINIUPNPC)
  std::lock_guard lock(g_upnp_mutex);

  UPNPDev* devlist = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, nullptr);
  if (!devlist) {
    out.error = "UPnP discovery failed";
    return out;
  }

  UPNPUrls urls;
  IGDdatas data;
  const int status = UPNP_GetValidIGD(devlist, &urls, &data, nullptr, 0);
  if (status != 1 && status != 2) {
    freeUPNPDevlist(devlist);
    out.error = "no UPnP IGD found";
    return out;
  }

  char lanaddr[64] = {};
  if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, lanaddr) != UPNPCOMMAND_SUCCESS) {
    lanaddr[0] = '\0';
  }

  char int_client[64] = {};
  char int_port_str[16] = {};
  char ext_port_str[16] = {};
  snprintf(int_port_str, sizeof(int_port_str), "%d", internal_port);
  snprintf(ext_port_str, sizeof(ext_port_str), "%d", external_port);

  const int map_rc = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, ext_port_str, int_port_str,
                                       int_client, "pp-browser", "TCP", nullptr, "86400");
  FreeUPNPUrls(&urls);
  freeUPNPDevlist(devlist);

  if (map_rc != UPNPCOMMAND_SUCCESS) {
    out.error = "UPnP AddPortMapping failed";
    return out;
  }

  g_mapped_external_port = external_port;
  out.ok = true;
  out.external_ip = lanaddr;
  out.external_port = external_port;
  return out;
#else
  (void)internal_port;
  (void)external_port;
  out.error = "UPnP not available in this build";
  return out;
#endif
}

void ReleaseUpnpTcpPortMapping(int external_port) {
#if defined(PP_BROWSER_HAS_MINIUPNPC)
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
  const int igd_status = UPNP_GetValidIGD(devlist, &urls, &data, nullptr, 0);
  if (igd_status != 1 && igd_status != 2) {
    freeUPNPDevlist(devlist);
    return;
  }
  char ext_port_str[16] = {};
  snprintf(ext_port_str, sizeof(ext_port_str), "%d", external_port);
  UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, ext_port_str, "TCP", nullptr);
  FreeUPNPUrls(&urls);
  freeUPNPDevlist(devlist);
  if (g_mapped_external_port == external_port) {
    g_mapped_external_port = 0;
  }
#else
  (void)external_port;
#endif
}

} // namespace pbr
