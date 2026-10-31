#include <stdint.h>
#define GPIO_DIRECTION_OUTPUT 1
uint32_t uapi_gpio_set_val(int pin, int val);
uint32_t uapi_gpio_set_dir(int pin, int dir);
