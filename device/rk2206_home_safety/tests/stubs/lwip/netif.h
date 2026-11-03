#ifndef TEST_STUB_NETIF_H
#define TEST_STUB_NETIF_H
#include "ip_addr.h"
struct netif {ip4_addr_t ip_addr;};
extern struct netif *netif_default;

#endif
