#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include "../src/key.c"
#include "key_actions.h"
static jmp_buf finished;
static unsigned samples,held_samples,shorts,longs,short_sample;
static uint64_t tick;
static key_id_t tested;
static bool jitter;
uint64_t LOS_TickCountGet(void){return tick;}
unsigned int IoTAdcInit(unsigned int c){(void)c;return 0;}
unsigned int IoTAdcGetVal(unsigned int c,unsigned int *v){
 (void)c;*v=samples>=10 && samples<10+held_samples?(tested==KEY_RIGHT?111:tested==KEY_DOWN?(jitter?(samples%2?211:219):215):tested==KEY_LEFT?338:1):680;return 0;
}
int LOS_TaskCreate(unsigned int *id,TSK_INIT_PARAM_S *task){(void)id;(void)task;return 0;}
void LOS_Msleep(unsigned int ms){
 key_id_t e=key_take_event();if(e!=KEY_NONE){assert(e==tested);shorts++;short_sample=samples;}
 e=key_take_long_event();if(e!=KEY_NONE){assert(e==tested);longs++;}
 tick+=ms;samples++;
 if(samples>held_samples+25)longjmp(finished,1);
}
static void run(key_id_t key,unsigned duration){
 tested=key;held_samples=duration;samples=shorts=longs=short_sample=0;tick=0;g_event=g_long_event=KEY_NONE;
 if(!setjmp(finished))key_thread(NULL);
}
int main(void){
 for(int page=0;page<5;page++) {
    assert(k6_action(page,-1,true,true,false)==K6_ACK);
    assert(k6_action(page,-1,true,false,true)==K6_NONE);
    assert(k6_action(page,-1,false,false,false)!=K6_CLEAR_EVENTS);
    assert(k6_action(page,-1,false,true,false)==K6_MEDICINE);
 }
 for(int page=1;page<=2;page++) {
    assert(k6_action(page,-1,false,false,false)==K6_NONE);
    assert(k6_action(page,-1,false,false,true)==K6_CLEAR_EVENTS);
 }
 assert(k6_action(4,-1,false,false,false)==K6_EDIT);
 assert(k6_action(4,0,false,true,false)==K6_EDIT);
 assert(k6_action(4,-1,false,false,true)==K6_EDIT);
 assert(k6_action(4,9,false,false,true)==K6_SAVE);
 assert(k6_action(4,10,false,false,true)==K6_NONE);
 puts("PASS K6 policy: alarm ACK on all pages, short never clears, long history clear only without alarm, living edit/save preserved");

 assert(decode(0.003f)==KEY_UP);
 assert(decode(0.358f)==KEY_RIGHT);
 assert(decode(0.68f)==KEY_DOWN && decode(0.71f)==KEY_DOWN);
 assert(decode(1.089f)==KEY_LEFT);
 for(float idle=2.0f;idle<2.4f;idle+=0.01f)assert(decode(idle)==KEY_NONE);
 for(float v=1.0f;v<1.2f;v+=0.005f)assert(decode(v)==KEY_LEFT);
 assert(decode(3.29f)==KEY_NONE);
 run(KEY_UP,10);assert(shorts==1&&longs==0);
 run(KEY_LEFT,10);assert(shorts==1&&longs==0);
 run(KEY_LEFT,60);assert(shorts==1&&longs==1); /* Scanner long event preserved; monitor disables mode reboot. */
 jitter=true;run(KEY_DOWN,60);assert(shorts==1&&longs==1);jitter=false;

 run(KEY_RIGHT,10);assert(shorts==1&&longs==0&&short_sample>=20);
 run(KEY_RIGHT,60);assert(shorts==0&&longs==1);
 run(KEY_RIGHT,2);assert(shorts==0&&longs==0); /* Contact bounce never becomes a press. */
 run(KEY_DOWN,10);assert(shorts==1&&short_sample<20); /* K4 keeps immediate mute. */
 puts("PASS calibrated real key scan: K4 0.68-0.71V stable; idle 2.0-2.4V never becomes a key, four keys separated; K6 release confirmation, long press suppresses short, bounce rejected, K4 immediate");
}
