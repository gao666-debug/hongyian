#ifndef TEST_STUB_IP_ADDR_H
#define TEST_STUB_IP_ADDR_H
#ifndef TEST_IP_H
#define TEST_IP_H
#include <arpa/inet.h>
typedef struct {unsigned int addr;} ip4_addr_t;
static inline char *ip4addr_ntoa_r(const ip4_addr_t *a,char *out,int n){return (char*)inet_ntop(AF_INET,&a->addr,out,n);}
#define ip_addr_get_ip4_u32(x) ((x)->addr)
#endif

#endif
