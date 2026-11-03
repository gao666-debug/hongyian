#include <assert.h>
#include <stdio.h>
#include "door.h"
#include "lz_hardware/pinctrl.h"
#ifndef DOOR_OPEN_LEVEL
#define DOOR_OPEN_LEVEL 1
#endif
static unsigned int init_rc, mux_rc, dir_rc, read_rc;
static int level;
unsigned int LzGpioInit(GpioID id) { assert(id == GPIO0_PB6); return init_rc; }
unsigned int LzGpioSetDir(GpioID id, LzGpioDir dir) { assert(id == GPIO0_PB6); assert(dir == LZGPIO_DIR_IN); return dir_rc; }
unsigned int LzGpioGetVal(GpioID id, LzGpioValue *v) { assert(id == GPIO0_PB6); *v = level; return read_rc; }
unsigned int PinctrlSet(GpioID id, MuxFunc func, PullType pull, DriveLevel drive) {
 assert(id == GPIO0_PB6 && func == MUX_GPIO && pull == PULL_KEEP && drive == DRIVE_KEEP);
 return mux_rc;
}
int main(void) {
 bool open = false;
 init_rc = 1; door_dev_init(); assert(!door_read(0, &open));
 init_rc = 0; mux_rc = 1; door_dev_init(); assert(!door_read(0, &open));
 mux_rc = 0; dir_rc = 1; door_dev_init(); assert(!door_read(0, &open));
 dir_rc = 0; door_dev_init(); level = DOOR_OPEN_LEVEL;
 assert(!door_read(0, &open)); assert(!door_read(99, &open));
 assert(door_read(100, &open) && open);
 level = !DOOR_OPEN_LEVEL; assert(door_read(110, &open) && open);
 level = DOOR_OPEN_LEVEL; assert(door_read(150, &open) && open);
 level = !DOOR_OPEN_LEVEL; assert(door_read(200, &open) && open);
 assert(door_read(299, &open) && open);
 assert(door_read(300, &open) && !open);
 level = DOOR_OPEN_LEVEL; assert(door_read(400, &open) && !open);
 assert(door_read(500, &open) && open);
 assert(door_read(3600000, &open) && open);
 read_rc = 1; assert(!door_read(3600001, &open));
 read_rc = 0; level = !DOOR_OPEN_LEVEL;
 assert(!door_read(3600002, &open));
 assert(door_read(3600102, &open) && !open);
 assert(!door_read(3600200, 0));
 puts("door tests passed");
}
