#define getaddrinfo fake_getaddrinfo
#define freeaddrinfo fake_freeaddrinfo
#include "MQTTLiteOS.c"
#include <assert.h>
struct netif *netif_default;
static ip_addr_t servers[2];
static int queries,sets,core_fail,core_calls,always_fail,initial_ok;
const ip_addr_t *dns_getserver(unsigned char i){return &servers[i];}
void dns_setserver(unsigned char i,const ip_addr_t *p){servers[i]=*p;sets++;}
int ipaddr_aton(const char *s,ip_addr_t *p){snprintf(p->text,sizeof(p->text),"%s",s);return 1;}
char *ipaddr_ntoa_r(const ip_addr_t *p,char *out,int n){snprintf(out,n,"%s",p->text);return out;}
err_t tcpip_api_call(err_t (*fn)(struct tcpip_api_call_data *),struct tcpip_api_call_data *p){core_calls++;return core_fail?-1:fn(p);}
int fake_getaddrinfo(const char *host,const char *service,const struct addrinfo *hint,struct addrinfo **out){
 (void)service;(void)hint; assert(!strcmp(host,"example.invalid")); *out=NULL; queries++;
 return initial_ok?0:always_fail?-4:queries==1?-4:0;
}
void fake_freeaddrinfo(struct addrinfo *p){(void)p;}
static void reset(void){queries=sets=core_fail=core_calls=always_fail=initial_ok=0;}
int main(void){
 struct addrinfo hints={0},*all=NULL;
 reset();initial_ok=1;assert(!mqtt_resolve("example.invalid",&hints,&all));assert(queries==1&&sets==0&&core_calls==0);
 reset();assert(!mqtt_resolve("example.invalid",&hints,&all));assert(queries==2&&sets==2&&core_calls==1);
 assert(!strcmp(servers[0].text,"223.5.5.5")&&!strcmp(servers[1].text,"223.6.6.6"));
 reset();always_fail=1;assert(mqtt_resolve("example.invalid",&hints,&all)==-4);assert(queries==2&&core_calls==1);
 reset();core_fail=1;assert(mqtt_resolve("example.invalid",&hints,&all)==-4);assert(queries==1&&sets==0);
 puts("PASS real resolver fallback: initial success unchanged, core-thread apply, one retry, persistent failure, setup failure");
}
