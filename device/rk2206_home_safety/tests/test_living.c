#include "living.h"
#include "monitor_logic.h"
#include "safety_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
static char config[128],taken[128];static int fail;
void safety_lock(void){}void safety_unlock(void){}void safety_storage_lock(void){}void safety_storage_unlock(void){}
int UtilsGetValue(const char*k,char*v,unsigned n){const char*s=!strcmp(k,"living_cfg")?config:taken;if(!*s)return -1;snprintf(v,n,"%s",s);return strlen(s);}
int UtilsSetValue(const char*k,const char*v){if(fail)return -1;snprintf(!strcmp(k,"living_cfg")?config:taken,128,"%s",v);return 0;}
static living_status_t get(void){living_status_t s;living_get(&s);return s;}
static unsigned at(int day,int minute){return day*86400U+minute*60U-28800U;}
static int save(int enabled,int minute){living_status_t s=get();s.config.medicine_enabled=enabled;s.config.medicine_minute=minute;return living_save(&s.config);}
static int confirm(int day,unsigned epoch){char b[64];snprintf(b,sizeof(b),"{\"dose_day\":%d}",day);return living_execute(SAFETY_CMD_MEDICINE,b,epoch);}
int main(void){
 living_init();living_light_override(1);living_tick(0,0,500,1,0);assert(get().night_on);
 living_light_override(0);living_tick(0,1,0,1,0);assert(!get().night_on);
 living_light_override(1);living_tick(0,2,0,1,1);assert(!get().night_on&&get().light_owner==3);
 living_light_override(-1);
 living_init();assert(get().config.night_auto==1&&get().medicine_state==0);
 living_tick(0,0,10,1,0);living_tick(0,2000,10,1,0);assert(get().night_on);
 living_tick(0,3000,45,1,0);assert(get().night_on);
 living_tick(0,4000,65,1,0);living_tick(0,6000,65,1,0);assert(!get().night_on);
 living_tick(0,7000,10,1,0);living_tick(0,9000,10,1,0);assert(get().night_on);
 living_tick(0,10000,NAN,1,0);assert(!get().night_on&&!get().light_valid);
 living_tick(0,11000,5,1,0);living_tick(0,13000,5,1,1);assert(!get().night_on&&get().light_owner==3);
 assert(save(1,480)==0);living_tick(0,14000,5,1,0);assert(get().medicine_state==1);
 assert(confirm(21000,0)==1);living_tick(at(21000,479),15000,5,1,0);assert(get().medicine_state==2);
 assert(confirm(21000,at(21000,479))==1);living_tick(at(21000,480),16000,5,1,0);assert(get().medicine_state==3&&living_beep(16000));
 assert(get().light_owner==2&&!get().night_on);living_silence();assert(!living_beep(17000));
 fail=1;assert(confirm(21000,at(21000,480))==2);assert(!get().confirmed_epoch);
 fail=0;assert(confirm(21000,at(21000,480))==0);unsigned stamp=get().confirmed_epoch;
 assert(confirm(21000,at(21000,481))==0&&get().confirmed_epoch==stamp);
 living_tick(at(21000,481),18000,5,1,0);assert(get().medicine_state==4);
 living_init();living_tick(at(21000,490),19000,5,1,0);assert(get().medicine_state==4);
 assert(save(0,500)==0);assert(save(1,500)==0);living_tick(at(21000,501),20000,5,1,0);assert(get().medicine_state==4);
 living_tick(at(21001,0),21000,5,1,0);assert(get().medicine_state==2);
 living_tick(at(21001,500),22000,5,1,0);assert(get().medicine_state==3);assert(confirm(21000,at(21001,500))==1);
 assert(living_beep(22000));assert(!living_beep(22000+6000));assert(living_beep(22000+300000));assert(!living_beep(22000+1800000));
 living_tick(at(20999,501),23000,5,1,0);assert(get().medicine_state==1);
 living_config_t before=get().config;fail=1;assert(save(0,100)==2);assert(get().config.revision==before.revision&&get().config.medicine_enabled);fail=0;
 before.revision--;assert(living_save(&before)==4);
 assert(living_validate(3,"{\"night_auto\":1,\"medicine_enabled\":true,\"medicine_minute\":480,\"expected_revision\":1}"));
 assert(living_validate(3,"{\"night_auto\":true,\"medicine_enabled\":true,\"medicine_minute\":1440,\"expected_revision\":1}"));
 assert(living_validate(4,"{\"dose_day\":12.1}"));assert(living_validate(4,"{\"dose_day\":12,\"other\":1}"));
 char b[1024];assert(!living_json(b,sizeof(b)));assert(strstr(b,"\"config_revision\":"));assert(living_json(b,8)==-1);
 /* Three slots keep independent confirmations and persist undo across a reboot. */
 living_init();living_status_t v=get();
 living_config_t plan=v.config;
 for(int i=0;i<3;i++)living_slot_set(&plan,i,1,0);
 assert(living_save(&plan)==0);living_tick(at(21002,10),40000,5,1,0);
 char action[160];
 snprintf(action,sizeof(action),"{\"dose_day\":21002,\"slot\":1,\"taken\":true,\"expected_medicine_revision\":%u}",get().medicine_revision);
 assert(living_execute(SAFETY_CMD_MEDICINE_STATE,action,at(21002,10))==0);
 living_tick(at(21002,10),41000,5,1,0);v=get();assert(v.slot_state[0]==3&&v.slot_state[1]==4&&v.slot_state[2]==3);
 assert(living_execute(SAFETY_CMD_MEDICINE_STATE,action,at(21002,10))==4);
 snprintf(action,sizeof(action),"{\"dose_day\":21002,\"slot\":1,\"taken\":false,\"expected_medicine_revision\":%u}",v.medicine_revision);
 assert(living_execute(SAFETY_CMD_MEDICINE_STATE,action,at(21002,10))==0);
 living_init();living_tick(at(21002,11),42000,5,1,0);v=get();assert(v.slot_state[0]==3&&v.slot_state[1]==3&&v.slot_state[2]==3);
 assert(v.config.extra_enabled[0]&&v.config.extra_enabled[1]);
 /* Moving to tomorrow clears all three daily states without losing their settings. */
 living_tick(at(21003,0),43000,5,1,0);assert(get().slot_state[2]==3);
 assert(living_validate(SAFETY_CMD_MEDICINE_STATE,"{\"dose_day\":21003,\"slot\":3,\"taken\":true,\"expected_medicine_revision\":1}"));
 assert(!living_json(b,sizeof(b)));assert(strstr(b,"\"slots\":["));
 /* Quick-confirm sorts actual reminder times, not slot indices, and never clears all doses. */
 plan=get().config;living_slot_set(&plan,0,1,600);living_slot_set(&plan,1,1,480);living_slot_set(&plan,2,1,480);
 assert(living_save(&plan)==0);living_tick(at(21004,700),45000,5,1,0);
 int next=-1;fail=1;assert(living_confirm_next(at(21004,700),&next)==2&&next==-1);fail=0;
 assert(living_confirm_next(at(21004,700),&next)==0&&next==1);
 living_tick(at(21004,700),46000,5,1,0);v=get();assert(v.slot_state[0]==3&&v.slot_state[1]==4&&v.slot_state[2]==3);
 living_silence();assert(!living_beep(46000)); /* Muting never marks the remaining doses taken. */
 assert(living_confirm_next(at(21004,700),&next)==0&&next==2);
 living_tick(at(21004,700),47000,5,1,0);
 assert(living_confirm_next(at(21004,700),&next)==0&&next==0);
 living_tick(at(21004,700),48000,5,1,0);assert(!living_beep(48000));
 assert(living_confirm_next(at(21004,700),&next)==1&&next==-1);
 living_init();living_tick(at(21004,701),49000,5,1,0);v=get();assert(v.slot_state[0]==4&&v.slot_state[1]==4&&v.slot_state[2]==4);
 /* Exercise the same care->living->output sequence used by the monitor. */
 plan=get().config;
 for(int i=0;i<3;i++)living_slot_set(&plan,i,0,0);
 plan.night_auto=1;assert(living_save(&plan)==0);
 living_tick(at(21004,701),50000,5,1,0);
 living_tick(at(21004,701),53000,5,1,0);assert(get().night_on);
 monitor_care_t care={0};
 monitor_care_step(&care,54000,true,true,false,false,60000);
 monitor_care_result_t cr=monitor_care_step(&care,114000,true,false,false,false,60000);
 assert(cr.warning&&!cr.alarm);
 living_tick(at(21004,701),114000,5,1,cr.alarm||cr.warning);v=get();
 assert(v.light_owner==3&&!v.night_on);
 monitor_outputs_t out=monitor_outputs(cr.alarm,false,false,cr.warning,false,true,v.light_owner==2,living_beep(114000),v.night_on);
 assert(out.owner==3&&out.alarm_light&&out.r==255&&out.g==160&&out.beep);
 cr=monitor_care_step(&care,144000,true,false,false,false,60000);assert(cr.alarm);
 living_tick(at(21004,701),144000,5,1,cr.alarm||cr.warning);v=get();assert(v.light_owner==3&&!v.night_on);
 out=monitor_outputs(cr.alarm,false,false,cr.warning,false,false,v.light_owner==2,true,v.night_on);
 assert(out.owner==3&&!out.beep&&out.alarm_light); /* mute leaves alarm light */
 cr=monitor_care_step(&care,145000,true,true,false,false,60000);assert(!cr.warning&&!cr.alarm);
 living_tick(at(21004,701),145000,5,1,0);assert(get().night_on);
 puts("PASS real living integration: dark night -> no-activity response -> alarm -> mute -> activity -> night restored");
 puts("PASS quick-confirm order, tie-breaking, one dose per press, persistence, mute and storage failure");
 puts("PASS three-dose independence, undo, replay conflict, persistence and midnight");
 puts("PASS living: hysteresis, sensor failures, alarm priority, disabled/no UTC, schedule, midnight, persistence, idempotency, stale-day rejection, mute, bounded beeps, flash failure and revision conflict");
}
