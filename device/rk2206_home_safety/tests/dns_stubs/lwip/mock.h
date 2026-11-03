#ifndef DNS_MOCK_H
#define DNS_MOCK_H
#include <stddef.h>
#define DNS_MAX_SERVERS 2
#define ERR_OK 0
#define ERR_ARG -16
typedef int err_t;
typedef struct {char text[48];} ip_addr_t;
struct netif {ip_addr_t ip_addr,gw,netmask;};
extern struct netif *netif_default;
struct tcpip_api_call_data {int unused;};
const ip_addr_t *dns_getserver(unsigned char i);
void dns_setserver(unsigned char i,const ip_addr_t *address);
int ipaddr_aton(const char *,ip_addr_t *);
char *ipaddr_ntoa_r(const ip_addr_t *,char *,int);
err_t tcpip_api_call(err_t (*fn)(struct tcpip_api_call_data *),struct tcpip_api_call_data *);
err_t tcpip_callback(void (*fn)(void *),void *);
#endif
