/* SPDX-License-Identifier: Apache-2.0 */
#include "living.h"
#include "safety_runtime.h"
#include "kv_store.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static living_status_t state;
static unsigned serial, taken[3];
static int light_override=-1;
void living_light_override(int mode){if(mode < -1 || mode > 1)return;safety_lock();light_override=mode;serial++;safety_unlock();}
static int record_day=-1, dark, candidate=-1, silenced, pending_mask;
static uint64_t candidate_since, pending_since;
int living_slot_enabled(const living_config_t *c,int i){return i==0?c->medicine_enabled:c->extra_enabled[i-1];}
int living_slot_minute(const living_config_t *c,int i){return i==0?c->medicine_minute:c->extra_minute[i-1];}
void living_slot_set(living_config_t *c,int i,int enabled,int minute){
 if(i==0){c->medicine_enabled=enabled;c->medicine_minute=minute;}
 else if(i>0&&i<3){c->extra_enabled[i-1]=enabled;c->extra_minute[i-1]=minute;}
}
int living_pending_slot(const living_status_t *s){
 int selected=-1;
 for(int i=0;i<3;i++)if(s->slot_state[i]==3 &&
    (selected<0||living_slot_minute(&s->config,i)<living_slot_minute(&s->config,selected)))selected=i;
 return selected;
}
int living_confirm_next(unsigned epoch,int *slot){
 living_status_t s;living_get(&s);int next=living_pending_slot(&s);
 if(slot)*slot=-1;
 if(next<0)return 1;
 char payload[144];
 snprintf(payload,sizeof(payload),"{\"dose_day\":%d,\"slot\":%d,\"taken\":true,\"expected_medicine_revision\":%u}",s.dose_day,next,s.medicine_revision);
 int rc=living_execute(SAFETY_CMD_MEDICINE_STATE,payload,epoch);
 if(!rc && slot)*slot=next;
 return rc;
}
static int integer(cJSON *j,int lo,int hi){return cJSON_IsNumber(j)&&isfinite(j->valuedouble)&&j->valuedouble>=lo&&j->valuedouble<=hi&&j->valuedouble==(int)j->valuedouble;}
static cJSON *field(cJSON *j,const char *key){return cJSON_GetObjectItemCaseSensitive(j,key);}
typedef struct {living_config_t config;int day,slot,taken;unsigned revision;} request_t;
static int parse(int code,const char *json,request_t *out){
 cJSON *j=cJSON_ParseWithOpts(json,NULL,1);int ok=0;
 memset(out,0,sizeof(*out));
 if(!cJSON_IsObject(j))goto done;
 if(code==SAFETY_CMD_LIVING||code==SAFETY_CMD_MEDICINE_PLAN){
  cJSON *n=field(j,"night_auto"),*m=field(j,"medicine_enabled"),*t=field(j,"medicine_minute"),*r=field(j,"expected_revision");
  int count=code==SAFETY_CMD_LIVING?4:8;
  if(cJSON_GetArraySize(j)!=count||!cJSON_IsBool(n)||!cJSON_IsBool(m)||!integer(t,0,1439)||!integer(r,1,999999999))goto done;
  out->config.night_auto=cJSON_IsTrue(n);out->config.revision=r->valueint;
  living_slot_set(&out->config,0,cJSON_IsTrue(m),t->valueint);
  if(code==SAFETY_CMD_MEDICINE_PLAN){
   for(int i=1;i<3;i++){
    char key[32];snprintf(key,sizeof(key),"medicine_enabled_%d",i+1);m=field(j,key);
    snprintf(key,sizeof(key),"medicine_minute_%d",i+1);t=field(j,key);
    if(!cJSON_IsBool(m)||!integer(t,0,1439))goto done;
    living_slot_set(&out->config,i,cJSON_IsTrue(m),t->valueint);
   }
  }
  ok=1;
 }else if(code==SAFETY_CMD_MEDICINE||code==SAFETY_CMD_MEDICINE_STATE){
  cJSON *d=field(j,"dose_day");if(!integer(d,0,1000000))goto done;
  out->day=d->valueint;out->taken=1;
  if(code==SAFETY_CMD_MEDICINE){if(cJSON_GetArraySize(j)!=1)goto done;}
  else{
   cJSON *slot=field(j,"slot"),*t=field(j,"taken"),*r=field(j,"expected_medicine_revision");
   if(cJSON_GetArraySize(j)!=4||!integer(slot,0,2)||!cJSON_IsBool(t)||!integer(r,1,999999999))goto done;
   out->slot=slot->valueint;out->taken=cJSON_IsTrue(t);out->revision=r->valueint;
  }
  ok=1;
 }
 done:cJSON_Delete(j);return ok?0:1;
}
int living_validate(int code,const char *json){request_t r;return parse(code,json,&r);}
void living_init(void){
 char b[128]={0};unsigned epoch=0;int day=-1;
 memset(&state,0,sizeof(state));state.config.night_auto=1;state.config.medicine_minute=480;
 state.config.extra_minute[0]=720;state.config.extra_minute[1]=1200;state.config.revision=1;
 state.dose_day=-1;state.medicine_revision=1;
 light_override=-1;serial=1;record_day=-1;memset(taken,0,sizeof(taken));dark=0;candidate=-1;silenced=0;pending_mask=0;pending_since=0;
 safety_storage_lock();
 if(UtilsGetValue("living_cfg",b,sizeof(b))>=0){
  cJSON *j=cJSON_Parse(b),*v=field(j,"v"),*r=field(j,"r"),*n=field(j,"n"),*m=field(j,"m"),*t=field(j,"t");
  if(integer(v,1,2)&&integer(r,1,1000000000)&&integer(n,0,1)&&integer(m,0,1)&&integer(t,0,1439)){
   living_config_t c=state.config;c.night_auto=n->valueint;c.revision=r->valueint;living_slot_set(&c,0,m->valueint,t->valueint);
   if(v->valueint==1)state.config=c;
   else if(integer(field(j,"a"),0,1)&&integer(field(j,"b"),0,1439)&&integer(field(j,"c"),0,1)&&integer(field(j,"d"),0,1439)){
    living_slot_set(&c,1,field(j,"a")->valueint,field(j,"b")->valueint);living_slot_set(&c,2,field(j,"c")->valueint,field(j,"d")->valueint);state.config=c;
   }
  }
  cJSON_Delete(j);
 }
 if(UtilsGetValue("med_taken",b,sizeof(b))>=0){
  cJSON *j=cJSON_Parse(b),*d=field(j,"d"),*r=field(j,"r"),*e=field(j,"e");
  if(integer(d,0,1000000)&&integer(r,1,1000000000)&&cJSON_IsArray(e)&&cJSON_GetArraySize(e)==3){
   int valid=1;for(int i=0;i<3;i++){cJSON *a=cJSON_GetArrayItem(e,i);if(!cJSON_IsNumber(a)||a->valuedouble<0||a->valuedouble>4294967295.0||a->valuedouble!=(unsigned)a->valuedouble||(a->valuedouble>0&&((unsigned)a->valuedouble+28800U)/86400U!=(unsigned)d->valueint))valid=0;}
   if(valid){record_day=d->valueint;state.medicine_revision=r->valueint;for(int i=0;i<3;i++)taken[i]=(unsigned)cJSON_GetArrayItem(e,i)->valuedouble;}
  }else if(sscanf(b,"%d:%u",&day,&epoch)==2&&day>=0&&epoch>0&&(epoch+28800U)/86400U==(unsigned)day){record_day=day;taken[0]=epoch;}
  cJSON_Delete(j);
 }
 safety_storage_unlock();
 for(int i=0;i<3;i++)state.slot_state[i]=living_slot_enabled(&state.config,i)?1:0;
 state.medicine_state=state.slot_state[0];state.confirmed_epoch=taken[0];
}
void living_get(living_status_t *out){safety_lock();*out=state;safety_unlock();}
unsigned living_serial(void){safety_lock();unsigned n=serial;safety_unlock();return n;}
int living_save(const living_config_t *cfg){
 if(!cfg||cfg->night_auto<0||cfg->night_auto>1||cfg->revision<1||cfg->revision>=1000000000)return 1;
 for(int i=0;i<3;i++)if(living_slot_enabled(cfg,i)<0||living_slot_enabled(cfg,i)>1||living_slot_minute(cfg,i)<0||living_slot_minute(cfg,i)>1439)return 1;
 char b[128];int rc=2;
 safety_storage_lock();safety_lock();
 if(cfg->revision!=state.config.revision)rc=4;
 else{
  int n=snprintf(b,sizeof(b),"{\"v\":2,\"r\":%u,\"n\":%d,\"m\":%d,\"t\":%d,\"a\":%d,\"b\":%d,\"c\":%d,\"d\":%d}",cfg->revision+1,cfg->night_auto,cfg->medicine_enabled,cfg->medicine_minute,cfg->extra_enabled[0],cfg->extra_minute[0],cfg->extra_enabled[1],cfg->extra_minute[1]);
  safety_unlock();int saved=n>0&&n<(int)sizeof(b)?UtilsSetValue("living_cfg",b):-1;safety_lock();
  if(saved>=0){state.config=*cfg;state.config.revision++;light_override=-1;serial++;rc=0;}
 }
 safety_unlock();safety_storage_unlock();return rc;
}
int living_execute(int code,const char *json,unsigned epoch){
 request_t r;int rc=parse(code,json,&r);if(rc)return rc;
 if(code==SAFETY_CMD_LIVING||code==SAFETY_CMD_MEDICINE_PLAN){
  if(code==SAFETY_CMD_LIVING){living_status_t old;living_get(&old);memcpy(r.config.extra_enabled,old.config.extra_enabled,sizeof(r.config.extra_enabled));memcpy(r.config.extra_minute,old.config.extra_minute,sizeof(r.config.extra_minute));}
  return living_save(&r.config);
 }
 char b[128];rc=1;safety_storage_lock();safety_lock();
 int today=epoch?(int)((epoch+28800U)/86400U):-1,minute=epoch?(int)(((epoch+28800U)%86400U)/60U):-1;
 if(code==SAFETY_CMD_MEDICINE_STATE&&r.revision!=state.medicine_revision)rc=4;
 else if(epoch&&living_slot_enabled(&state.config,r.slot)&&r.day==today&&(!r.taken||minute>=living_slot_minute(&state.config,r.slot))&&record_day<=today){
  unsigned next[3]={0};if(record_day==today)memcpy(next,taken,sizeof(next));
  if((next[r.slot]!=0)==r.taken)rc=0;
  else if(state.medicine_revision>=1000000000)rc=4;
  else{
   next[r.slot]=r.taken?epoch:0;
   snprintf(b,sizeof(b),"{\"d\":%d,\"r\":%u,\"e\":[%u,%u,%u]}",today,state.medicine_revision+1,next[0],next[1],next[2]);
   safety_unlock();int saved=UtilsSetValue("med_taken",b);safety_lock();
   if(saved>=0){record_day=today;memcpy(taken,next,sizeof(taken));state.confirmed_epoch=taken[0];state.medicine_revision++;serial++;rc=0;}else rc=2;
  }
 }
 safety_unlock();safety_storage_unlock();return rc;
}
void living_tick(unsigned epoch,uint64_t now,double lux,int valid,int alarm){
 safety_lock();living_status_t previous=state;
 state.light_valid=valid&&isfinite(lux)&&lux>=0;
 if(!state.light_valid||!state.config.night_auto){dark=0;candidate=-1;}
 else{int want=dark;if(lux<30)want=1;else if(lux>60)want=0;
  if(want==dark)candidate=-1;else if(candidate!=want){candidate=want;candidate_since=now;}else if(now-candidate_since>=2000){dark=want;candidate=-1;}}
 state.time_synced=epoch!=0;state.dose_day=epoch?(int)((epoch+28800U)/86400U):-1;
 int minute=epoch?(int)(((epoch+28800U)%86400U)/60U):0,mask=0;
 for(int i=0;i<3;i++){
  state.slot_epoch[i]=record_day==state.dose_day?taken[i]:0;
  state.slot_state[i]=!living_slot_enabled(&state.config,i)?0:!epoch||state.dose_day<record_day?1:state.slot_epoch[i]?4:minute>=living_slot_minute(&state.config,i)?3:2;
  if(state.slot_state[i]==3)mask|=1<<i;
 }
 state.medicine_state=state.slot_state[0];state.confirmed_epoch=state.slot_epoch[0];
 if((mask&~pending_mask)|| (mask&&previous.dose_day!=state.dose_day)){pending_since=now;silenced=0;}
 pending_mask=mask;int night=light_override>=0?light_override:dark;state.light_owner=alarm?3:mask?2:night?1:0;state.night_on=state.light_owner==1;
 if(memcmp(&previous,&state,sizeof(state)))serial++;
 safety_unlock();
}
int living_beep(uint64_t now){safety_lock();uint64_t age=now-pending_since;int on=pending_mask&&!silenced&&age<1800000&&age%300000<5000&&(age/600)%2==0;safety_unlock();return on;}
void living_silence(void){safety_lock();silenced=1;safety_unlock();}
int living_json(char *out,int size){
 living_status_t s;living_get(&s);const char *owners[]={"off","night","medicine","alarm"};
 int n=snprintf(out,size,"{\"schema\":2,\"config_revision\":%u,\"night_auto\":%s,\"night_on\":%s,\"light_valid\":%s,\"light_owner\":\"%s\",\"medicine_enabled\":%s,\"medicine_minute\":%d,\"medicine_state\":%d,\"dose_day\":%d,\"confirmed_epoch\":%u,\"time_synced\":%s,\"medicine_revision\":%u,\"slots\":[",s.config.revision,s.config.night_auto?"true":"false",s.night_on?"true":"false",s.light_valid?"true":"false",owners[s.light_owner],s.config.medicine_enabled?"true":"false",s.config.medicine_minute,s.medicine_state,s.dose_day,s.confirmed_epoch,s.time_synced?"true":"false",s.medicine_revision);
 if(n<0||n>=size)return -1;
 for(int i=0;i<3;i++){
  int k=snprintf(out+n,size-n,"%s{\"slot\":%d,\"enabled\":%s,\"minute\":%d,\"state\":%d,\"confirmed_epoch\":%u}",i?",":"",i,living_slot_enabled(&s.config,i)?"true":"false",living_slot_minute(&s.config,i),s.slot_state[i],s.slot_epoch[i]);
  if(k<0||k>=size-n)return -1;
  n+=k;
 }
 int k=snprintf(out+n,size-n,"]}");return k>=0&&k<size-n?0:-1;
}
const char *living_medicine_text(int value){const char *t[]={"未开启","待对时","待提醒","未服药","已服药"};return value>=0&&value<=4?t[value]:"待更新";}
