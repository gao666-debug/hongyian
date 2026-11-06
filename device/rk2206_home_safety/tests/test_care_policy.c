#include "care_policy.h"
#include "safety_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
void safety_config_get(safety_config_t *c){memset(c,0,sizeof(*c));c->noact_ms=60000;}
unsigned safety_boot_id(void){return 42;}
static char saved[128];static int fail_write;
void safety_lock(void){} void safety_unlock(void){}
void safety_storage_lock(void){} void safety_storage_unlock(void){}
int UtilsGetValue(const char *k,char *v,unsigned n){if(!*saved)return -1;snprintf(v,n,"%s",saved);return 0;}
int UtilsSetValue(const char *k,const char *v){if(fail_write)return -1;assert(strlen(v)<sizeof(saved));strcpy(saved,v);return 0;}
static int apply(int enabled,int sleep,int start,int end,int quiet,int revision){char b[200];snprintf(b,sizeof(b),"{\"enabled\":%s,\"sleep_enabled\":%s,\"start\":%d,\"end\":%d,\"quiet\":%d,\"expected_revision\":%d}",enabled?"true":"false",sleep?"true":"false",start,end,quiet,revision);return care_policy_apply(b);}
/* 2021-01-01 UTC midnight; UTC+8 local schedule. */
static unsigned at(int h,int m){return 1609459200U+(unsigned)((h+16)%24)*3600U+(unsigned)m*60U;}
int main(void){
 care_policy_init();assert(care_policy_mode(0)==1);
 assert(apply(1,1,1320,420,0,1)==0);
 assert(care_policy_mode(at(21,59))==1);assert(care_policy_mode(at(22,0))==0);
 assert(care_policy_mode(at(0,0))==0);assert(care_policy_mode(at(6,59))==0);assert(care_policy_mode(at(7,0))==1);
 assert(care_policy_mode(0)==1);assert(care_policy_mode(100)==1);
 assert(apply(1,1,1320,420,1,1)==4);assert(apply(1,1,1320,420,1,2)==0);
 assert(care_policy_mode(at(23,0))==2);care_policy_init();assert(care_policy_mode(at(23,0))==2);
 assert(apply(1,1,600,600,0,3)==1);assert(apply(1,1,-1,420,0,3)==1);
 assert(care_policy_apply("{\"enabled\":true}")==1);assert(care_policy_apply("null")==1);
 fail_write=1;assert(apply(0,0,600,660,0,3)==2);assert(care_policy_mode(at(23,0))==2);fail_write=0;
 assert(apply(1,1,600,660,0,3)==0);assert(care_policy_mode(at(9,59))==1);assert(care_policy_mode(at(10,0))==0);assert(care_policy_mode(at(11,0))==1);
 assert(apply(0,0,600,660,0,4)==0);assert(care_policy_mode(0)==0);
 char b[512];assert(care_policy_json(b,sizeof(b),0,0)==0);assert(strstr(b,"\"revision\":5"));assert(strstr(b,"\"time_synced\":false"));assert(care_policy_json(b,5,0,0)==-1);
 assert(care_policy_pause(1441,10)==1);assert(care_policy_pause(1,100)==0);
 assert(care_policy_mode_at(at(12,0),100)==0);assert(care_policy_pause(0,200)==0);
 assert(apply(1,0,600,660,0,5)==0);assert(care_policy_mode_at(at(12,0),200)==1);
 assert(care_policy_pause(1,100)==0);assert(care_policy_mode_at(at(12,0),60099)==0);assert(care_policy_mode_at(at(12,0),60100)==1);
 care_policy_init();assert(care_policy_mode_at(at(12,0),0)==1);
 strcpy(saved,"garbage");care_policy_init();assert(care_policy_mode(at(23,0))==1);
 assert(care_policy_apply("{\"enabled\":true,\"sleep_enabled\":false,\"start\":1320,\"end\":420,\"quiet\":0,\"noact_ms\":120000,\"expected_revision\":1}")==0);
 assert(care_policy_timeout(60000)==120000);care_policy_init();assert(care_policy_timeout(60000)==120000);
 const char *remote="{\"boot_id\":42,\"enabled\":true,\"sleep_enabled\":false,\"start\":1320,\"end\":420,\"quiet\":1,\"noact_ms\":180000,\"expected_revision\":2}";
 assert(care_cloud_validate(SAFETY_CMD_CARE_POLICY,remote)==0);assert(care_cloud_execute(SAFETY_CMD_CARE_POLICY,remote,0)==0);assert(care_cloud_execute(SAFETY_CMD_CARE_POLICY,remote,0)==4);assert(care_policy_timeout(60000)==180000);
 const char *pause="{\"boot_id\":42,\"minutes\":1,\"expected_pause_revision\":1}";
 assert(!care_cloud_execute(SAFETY_CMD_CARE_PAUSE,pause,100));assert(care_cloud_execute(SAFETY_CMD_CARE_PAUSE,pause,100)==4);
 assert(care_policy_mode_at(at(12,0),60099)==0);assert(care_policy_mode_at(at(12,0),60100)==1);assert(care_cloud_execute(SAFETY_CMD_CARE_PAUSE,pause,60100)==4);
 assert(care_cloud_validate(SAFETY_CMD_CARE_PAUSE,"{\"boot_id\":43,\"minutes\":1,\"expected_pause_revision\":1}")==4);
 puts("PASS remote care: atomic threshold/config storage, revision conflict, pause replay rejection, automatic recovery and boot guard");
 puts("PASS care policy: boundaries, overnight, unsynced, revision, persistence and write failure");
}
