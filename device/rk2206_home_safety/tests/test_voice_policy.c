#include <assert.h>
#include <stdio.h>
#include "voice_policy.h"
int main(void){
 voice_policy_t p={0};
 assert(voice_policy_next(&p,16,0,0)==5);
 assert(!voice_policy_next(&p,16,0,10000));
 assert(voice_policy_next(&p,17,0,10001)==1);
 assert(!voice_policy_next(&p,49,0,16000));
 assert(voice_policy_next(&p,48,0,17000)==0); /* environment already announced */
 assert(voice_policy_next(&p,32,0,18000)==6); /* deferred medicine */
 assert(!voice_policy_next(&p,0,0,19000));
 assert(!voice_policy_next(&p,1,1,20000));
 assert(!voice_policy_next(&p,1,0,21000)); /* repeat episode cooldown */
 assert(voice_policy_next(&p,1,0,41000)==1);
 assert(!voice_policy_next(&p,1,0,80000));
 puts("PASS voice priority, deferred dose, mute, episode deduplication, anti-flap cooldown");
}
