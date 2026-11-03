#include <assert.h>
#include <stdio.h>
#include "button.h"
#include "lz_hardware/pinctrl.h"
static int raw, fail, ack, mute, page;
void safety_lock(void) {}
void safety_unlock(void) {}
void button_scan(unsigned long long now);
static bool sample(unsigned long long now) { button_scan(now); return (button_take_events() & WIRELESS_SOS) != 0; }
unsigned int LzGpioInit(GpioID id) { assert(id==GPIO0_PB7); return 0; }
unsigned int PinctrlSet(GpioID id, MuxFunc m, PullType p, DriveLevel d) { assert((id==GPIO0_PB7 || id==GPIO0_PB0 || id==GPIO0_PA2 || id==GPIO0_PB1) && m==MUX_GPIO && p==PULL_DOWN); return 0; }
unsigned int LzGpioSetDir(GpioID id, LzGpioDir d) { assert((id==GPIO0_PB7 || id==GPIO0_PB0 || id==GPIO0_PA2 || id==GPIO0_PB1) && d==LZGPIO_DIR_IN); return 0; }
unsigned int LzGpioGetVal(GpioID id, LzGpioValue *v) { *v=(id==GPIO0_PB7?raw:id==GPIO0_PB0?ack:id==GPIO0_PA2?mute:page)?LZGPIO_LEVEL_HIGH:LZGPIO_LEVEL_LOW; return fail; }
int main(void) {
 button_dev_init(); raw=1;
 assert(!sample(0)); assert(!sample(100)); /* boot held: no SOS */
 raw=0; assert(!sample(110)); assert(!sample(160));
 raw=1; assert(!sample(170)); raw=0; assert(!sample(190));
 raw=1; assert(!sample(200)); assert(!sample(249));
 assert(sample(250)); assert(!sample(10000)); /* held once */
 raw=0; assert(!sample(10010)); raw=1; assert(!sample(10020));
 assert(!sample(10100)); /* brief release cannot rearm */
 raw=0; assert(!sample(10200)); assert(!sample(10250));
 raw=1; assert(!sample(10300)); assert(sample(10350));
 fail=1; assert(!sample(10400)); fail=0; assert(!sample(10500));
 assert(!sample(10600)); /* read fault requires a release */
 raw=0; assert(!sample(10700)); assert(!sample(10750));
 raw=1; assert(!sample(10800)); assert(sample(10850));
 raw=0; button_scan(11000); button_scan(11050);
 raw=1; button_scan(11100); button_scan(11150);
 button_scan(11200); button_scan(13000);
 assert(button_take_events()==WIRELESS_SOS); assert(!button_take_events()); /* event survives until monitor */
 ack=mute=page=1; raw=0; button_scan(14000);button_scan(16000);
 assert(!button_take_events());
 puts("PASS single B7: debounce, release before rearm, held once, fault recovery; spare outputs ignored");
}
