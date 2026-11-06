/* SPDX-License-Identifier: Apache-2.0 */
#include "care_policy.h"
#include "safety_runtime.h"
#include "kv_store.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct { int enabled, sleep, start, end, quiet, revision; } policy_t;
static uint64_t paused_until;
static policy_t policy = {1,0,1320,420,0,1};
static int number(cJSON *j,int lo,int hi) {
 return cJSON_IsNumber(j) && isfinite(j->valuedouble) && j->valuedouble>=lo &&
 j->valuedouble<=hi && j->valuedouble==(int)j->valuedouble;
}
static cJSON *get(cJSON *j,const char *key){return cJSON_GetObjectItemCaseSensitive(j,key);}
static int parse(const char *text,policy_t *p,int stored) {
 cJSON *j=cJSON_ParseWithOpts(text,NULL,1);int ok=0;
 if(!cJSON_IsObject(j)||cJSON_GetArraySize(j)!=6)goto end;
 cJSON *e=get(j,"enabled"),*s=get(j,"sleep_enabled"),*a=get(j,"start"),*b=get(j,"end"),*q=get(j,"quiet"),*r=get(j,stored?"revision":"expected_revision");
 if(!cJSON_IsBool(e)||!cJSON_IsBool(s)||!number(a,0,1439)||!number(b,0,1439)||!number(q,0,1)||!number(r,1,999999998))goto end;
 p->enabled=cJSON_IsTrue(e);p->sleep=cJSON_IsTrue(s);p->start=a->valueint;p->end=b->valueint;p->quiet=q->valueint;p->revision=r->valueint;
 if(p->sleep && p->start==p->end)goto end;
 ok=1;
 end:cJSON_Delete(j);return ok;
}
static int encode(char *out,int n,const policy_t *p) {
 return snprintf(out,n,"{\"enabled\":%s,\"sleep_enabled\":%s,\"start\":%d,\"end\":%d,\"quiet\":%d,\"revision\":%d}",p->enabled?"true":"false",p->sleep?"true":"false",p->start,p->end,p->quiet,p->revision);
}
void care_policy_init(void) {
 char b[128]={0};policy_t p={1,0,1320,420,0,1};
 safety_storage_lock();
 if(UtilsGetValue("care_policy",b,sizeof(b))>=0){policy_t saved;if(parse(b,&saved,1))p=saved;}
 safety_lock();policy=p;paused_until=0;safety_unlock();safety_storage_unlock();
}
int care_policy_apply(const char *text) {
 policy_t p;char b[128];if(!parse(text,&p,0))return 1;
 safety_storage_lock();safety_lock();int same=p.revision==policy.revision;safety_unlock();
 if(!same){safety_storage_unlock();return 4;}
 p.revision++;
 int n=encode(b,sizeof(b),&p);
 if(n<0||n>=(int)sizeof(b)||UtilsSetValue("care_policy",b)!=0){safety_storage_unlock();return 2;}
 safety_lock();policy=p;safety_unlock();safety_storage_unlock();return 0;
}
static int mode(const policy_t *p,unsigned epoch) {
 if(!p->enabled)return 0;
 /* An unsynchronised clock must never silently activate a sleep window. */
 if(!p->sleep||epoch<1609459200U)return 1;
 unsigned m=(epoch%86400U/60U+480U)%1440U;
 int asleep=p->start<p->end ? (m>=(unsigned)p->start&&m<(unsigned)p->end) : (m>=(unsigned)p->start||m<(unsigned)p->end);
 return asleep?(p->quiet?2:0):1;
}
int care_policy_mode(unsigned epoch){return care_policy_mode_at(epoch,0);}
int care_policy_mode_at(unsigned epoch,uint64_t now){safety_lock();policy_t p=policy;uint64_t until=paused_until;safety_unlock();return now<until?0:mode(&p,epoch);}
int care_policy_pause(unsigned minutes,uint64_t now){
 if(minutes>1440)return 1;
 safety_lock();paused_until=minutes?now+(uint64_t)minutes*60000:0;safety_unlock();return 0;
}
int care_policy_json(char *out,int size,unsigned epoch,uint64_t now) {
 char b[128];safety_lock();policy_t p=policy;uint64_t until=paused_until;safety_unlock();
 int n=encode(b,sizeof(b),&p);if(n<=0||n>=(int)sizeof(b))return -1;b[n-1]=0;
 n=snprintf(out,size,"%s,\"mode\":%d,\"time_synced\":%s,\"pause_seconds\":%u}",b,now<until?0:mode(&p,epoch),epoch>=1609459200U?"true":"false",now<until?(unsigned)((until-now+999)/1000):0);
 return n<0||n>=size?-1:0;
}
