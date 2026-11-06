/* SPDX-License-Identifier: Apache-2.0 */
#include "care_policy.h"
#include "safety_runtime.h"
#include "kv_store.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
typedef struct {int enabled,sleep,start,end,quiet,revision;unsigned noact_ms;} policy_t;
static policy_t policy={1,0,1320,420,0,1,0};
static uint64_t paused_until;
static unsigned pause_revision=1,serial=1;
static int number(cJSON *j,int lo,int hi){return cJSON_IsNumber(j)&&isfinite(j->valuedouble)&&j->valuedouble>=lo&&j->valuedouble<=hi&&j->valuedouble==(int)j->valuedouble;}
static cJSON *get(cJSON *j,const char *key){return cJSON_GetObjectItemCaseSensitive(j,key);}
static int parse(cJSON *j,policy_t *p,int stored){
 if(!cJSON_IsObject(j))return 0;
 int n=cJSON_GetArraySize(j);
 cJSON *e=get(j,"enabled"),*s=get(j,"sleep_enabled"),*a=get(j,"start"),*b=get(j,"end"),*q=get(j,"quiet"),*r=get(j,stored?"revision":"expected_revision"),*t=get(j,"noact_ms");
 if((n!=6&&n!=7)||!cJSON_IsBool(e)||!cJSON_IsBool(s)||!number(a,0,1439)||!number(b,0,1439)||!number(q,0,1)||!number(r,1,999999998)||(t&&!number(t,10000,86400000)))return 0;
 p->enabled=cJSON_IsTrue(e);p->sleep=cJSON_IsTrue(s);p->start=a->valueint;p->end=b->valueint;p->quiet=q->valueint;p->revision=r->valueint;p->noact_ms=t?(unsigned)t->valueint:0;
 return !(p->sleep&&p->start==p->end);
}
static int compact(cJSON *j,policy_t *p){
 if(!cJSON_IsObject(j)||cJSON_GetArraySize(j)!=7)return 0;
 cJSON *e=get(j,"e"),*s=get(j,"s"),*a=get(j,"a"),*b=get(j,"b"),*q=get(j,"q"),*r=get(j,"r"),*t=get(j,"t");
 if(!number(e,0,1)||!number(s,0,1)||!number(a,0,1439)||!number(b,0,1439)||!number(q,0,1)||!number(r,1,999999999)||!number(t,0,86400000)||(t->valueint&&t->valueint<10000))return 0;
 p->enabled=e->valueint;p->sleep=s->valueint;p->start=a->valueint;p->end=b->valueint;p->quiet=q->valueint;p->revision=r->valueint;p->noact_ms=t->valueint;return !(p->sleep&&p->start==p->end);
}
void care_policy_init(void){
 char b[128]={0};policy_t p={1,0,1320,420,0,1,0};safety_storage_lock();
 if(UtilsGetValue("care_policy",b,sizeof(b))>=0){cJSON *j=cJSON_Parse(b);policy_t saved;if(compact(j,&saved)||parse(j,&saved,1))p=saved;cJSON_Delete(j);}
 safety_lock();policy=p;paused_until=0;pause_revision=serial=1;safety_unlock();safety_storage_unlock();
}
static int apply(policy_t p){
 char b[128];safety_storage_lock();safety_lock();int same=p.revision==policy.revision;if(!p.noact_ms)p.noact_ms=policy.noact_ms;safety_unlock();
 if(!same){safety_storage_unlock();return 4;}p.revision++;
 int n=snprintf(b,sizeof(b),"{\"e\":%d,\"s\":%d,\"a\":%d,\"b\":%d,\"q\":%d,\"r\":%d,\"t\":%u}",p.enabled,p.sleep,p.start,p.end,p.quiet,p.revision,p.noact_ms);
 if(n<0||n>=(int)sizeof(b)||UtilsSetValue("care_policy",b)!=0){safety_storage_unlock();return 2;}
 safety_lock();policy=p;serial++;safety_unlock();safety_storage_unlock();return 0;
}
int care_policy_apply(const char *text){policy_t p;cJSON *j=cJSON_ParseWithOpts(text,NULL,1);int valid=parse(j,&p,0);cJSON_Delete(j);return valid?apply(p):1;}
static int mode(const policy_t *p,unsigned epoch){
 if(!p->enabled)return 0;
 if(!p->sleep||epoch<1609459200U)return 1;
 unsigned m=(epoch%86400U/60U+480U)%1440U;
 int asleep=p->start<p->end?(m>=(unsigned)p->start&&m<(unsigned)p->end):(m>=(unsigned)p->start||m<(unsigned)p->end);
 return asleep?(p->quiet?2:0):1;
}
static void expire(uint64_t now){if(paused_until&&now>=paused_until){paused_until=0;pause_revision++;serial++;}}
unsigned care_policy_timeout(unsigned fallback){safety_lock();unsigned t=policy.noact_ms;safety_unlock();return t?t:fallback;}
unsigned care_policy_serial(void){safety_lock();unsigned s=serial;safety_unlock();return s;}
int care_policy_mode(unsigned epoch){return care_policy_mode_at(epoch,0);}
int care_policy_mode_at(unsigned epoch,uint64_t now){safety_lock();expire(now);policy_t p=policy;uint64_t until=paused_until;safety_unlock();return now<until?0:mode(&p,epoch);}
int care_policy_pause(unsigned minutes,uint64_t now){if(minutes>1440)return 1;safety_lock();paused_until=minutes?now+(uint64_t)minutes*60000:0;pause_revision++;serial++;safety_unlock();return 0;}
int care_policy_json(char *out,int size,unsigned epoch,uint64_t now){
 safety_config_t cfg;safety_config_get(&cfg);safety_lock();expire(now);policy_t p=policy;uint64_t until=paused_until;unsigned pr=pause_revision;safety_unlock();
 int n=snprintf(out,size,"{\"enabled\":%s,\"sleep_enabled\":%s,\"start\":%d,\"end\":%d,\"quiet\":%d,\"revision\":%d,\"noact_ms\":%u,\"mode\":%d,\"time_synced\":%s,\"pause_seconds\":%u,\"pause_revision\":%u}",p.enabled?"true":"false",p.sleep?"true":"false",p.start,p.end,p.quiet,p.revision,p.noact_ms?p.noact_ms:cfg.noact_ms,now<until?0:mode(&p,epoch),epoch>=1609459200U?"true":"false",now<until?(unsigned)((until-now+999)/1000):0,pr);
 return n<0||n>=size?-1:0;
}
static int cloud_parse(int code,const char *text,policy_t *p,unsigned *minutes,unsigned *revision){
 cJSON *j=cJSON_ParseWithOpts(text,NULL,1),*boot=get(j,"boot_id");int result=1;
 if(!cJSON_IsObject(j)||!number(boot,1,2147483647))goto done;
 if((unsigned)boot->valueint!=safety_boot_id()){result=4;goto done;}
 if(code==SAFETY_CMD_CARE_POLICY){if(cJSON_GetArraySize(j)!=8)goto done;cJSON_DeleteItemFromObjectCaseSensitive(j,"boot_id");if(parse(j,p,0)&&p->noact_ms)result=0;}
 else if(code==SAFETY_CMD_CARE_PAUSE){cJSON *m=get(j,"minutes"),*r=get(j,"expected_pause_revision");if(cJSON_GetArraySize(j)==3&&number(m,0,1440)&&number(r,1,999999998)){*minutes=m->valueint;*revision=r->valueint;result=0;}}
 done:cJSON_Delete(j);return result;
}
int care_cloud_validate(int code,const char *text){policy_t p;unsigned m,r;return cloud_parse(code,text,&p,&m,&r);}
int care_cloud_execute(int code,const char *text,uint64_t now){
 policy_t p;unsigned minutes,expected;int result=cloud_parse(code,text,&p,&minutes,&expected);if(result)return result;
 if(code==SAFETY_CMD_CARE_POLICY)return apply(p);
 safety_lock();expire(now);if(pause_revision!=expected){safety_unlock();return 4;}
 paused_until=minutes?now+(uint64_t)minutes*60000:0;pause_revision++;serial++;safety_unlock();return 0;
}
