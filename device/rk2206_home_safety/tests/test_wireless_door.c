#include "button.h"
#include "door.h"
#include "lz_hardware/pinctrl.h"
#include <assert.h>
#include <stdio.h>

static int allocated[16], levels[16];
void button_scan(unsigned long long now);
void safety_lock(void) {} void safety_unlock(void) {}
unsigned int LzGpioInit(GpioID id) {
    assert(id==GPIO0_PB7 || id==GPIO0_PB0 || id==GPIO0_PA2 || id==GPIO0_PB1 || id==GPIO0_PB6);
    if (allocated[id]) return 1;
    allocated[id]=1; return 0;
}
unsigned int PinctrlSet(GpioID id,MuxFunc mode,PullType pull,DriveLevel drive) {
    assert(allocated[id] && mode==MUX_GPIO && drive==DRIVE_KEEP);
    assert(pull==(id==GPIO0_PB6 ? PULL_KEEP : PULL_DOWN)); return 0;
}
unsigned int LzGpioSetDir(GpioID id,LzGpioDir dir) {
    assert(allocated[id] && dir==LZGPIO_DIR_IN); return 0;
}
unsigned int LzGpioGetVal(GpioID id,LzGpioValue *value) {
    assert(allocated[id]); *value=levels[id]?LZGPIO_LEVEL_HIGH:LZGPIO_LEVEL_LOW; return 0;
}
int main(void) {
    bool open=false;
    levels[GPIO0_PB6]=1;
    door_dev_init(); button_dev_init();
    assert(!allocated[2] && !allocated[8] && !allocated[9] && allocated[14] && allocated[15]);
    button_scan(0); button_scan(50); assert(!button_take_events());
    assert(!door_read(0,&open)); assert(door_read(100,&open) && !open);
    levels[GPIO0_PB6]=0; levels[GPIO0_PB7]=1;
    assert(door_read(200,&open) && !open); button_scan(200);
    button_scan(250); assert(button_take_events()==WIRELESS_SOS);
    assert(door_read(300,&open) && open);
    levels[GPIO0_PA2]=levels[GPIO0_PB1]=levels[GPIO0_PB0]=1;
    button_scan(400); button_scan(450);
    assert(!button_take_events());
    assert(door_read(500,&open) && open);
    levels[GPIO0_PB6]=1; assert(door_read(600,&open) && open);
    assert(door_read(700,&open) && !open);
    button_scan(1400); assert(!button_take_events());
    puts("PASS production door + single wireless input: unique GPIO allocation, independent signals, debounce and concurrent events");
}
