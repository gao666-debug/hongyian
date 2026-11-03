#!/usr/bin/env python3
"""Compile the production PIR section with GPIO/clock mocks and real filter.

Other functions in home_safety.c drive physical PWM hardware and are excluded.
The extraction fails loudly if the section markers change.
"""
from pathlib import Path
import subprocess
import tempfile

project = Path(__file__).resolve().parents[1]
source = (project / 'src/home_safety.c').read_text()
begin = source.index('static body_diagnostics_t g_body_diag')
end = source.index('/***************************************************************\n * 函数名称: beep_dev_init', begin)
prefix = r'''
#include "home_safety.h"
#include "monitor_logic.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define IOT_SUCCESS 0
#define IOT_GPIO_VALUE0 0
#define IOT_GPIO_DIR_IN 0
#define GPIO_BODY_INDUCTION 3
typedef int IotGpioValue;
static unsigned int init_result, dir_result, read_result;
static int level = 1;
static uint64_t tick;
void safety_lock(void) {} void safety_unlock(void) {}
uint64_t LOS_TickCountGet(void) { return tick; }
unsigned int IoTGpioInit(int pin) { assert(pin==3); return init_result; }
unsigned int IoTGpioSetDir(int pin,int dir) { assert(pin==3 && dir==0); return dir_result; }
unsigned int IoTGpioGetInputVal(int pin,IotGpioValue *v) {
    assert(pin==3); if (!read_result) *v=level; return read_result;
}
'''
tests = r'''
static body_diagnostics_t sample(uint64_t now,int raw,int result,bool *active) {
    tick=now;level=raw;read_result=result;
    body_induction_get_state(active);
    body_diagnostics_t d;body_induction_diagnostics(&d);return d;
}
int main(void) {
    bool active; body_diagnostics_t d;
    tick=0;body_induction_dev_init();
    d=sample(0,1,0,&active);assert(!d.filtered_valid && !active);
    d=sample(2000,1,0,&active);assert(d.filtered_valid && !active);
    d=sample(2100,0,0,&active);assert(active && d.filtered_valid && d.raw==0);
    d=sample(2300,1,0,&active);assert(active); /* ignore brief release */
    d=sample(2400,0,0,&active);assert(active);
    d=sample(2800,0,0,&active);assert(active && d.filtered_valid);
    d=sample(2900,1,0,&active);assert(active);
    d=sample(3700,1,0,&active);assert(!active && d.filtered_valid);
    d=sample(3800,0,1,&active);assert(!active && !d.filtered_valid && d.raw==-1);
    d=sample(3900,0,0,&active);assert(active && d.filtered_valid);
    d=sample(4300,0,0,&active);assert(active && d.filtered_valid);
    /* Init may fail because GPIO is already allocated, but IN/read can work. */
    tick=5000;init_result=1;dir_result=0;body_induction_dev_init();
    d=sample(5000,0,0,&active);assert(!d.init_rc && d.gpio_rc==1 && !d.filtered_valid);
    d=sample(7000,0,0,&active);assert(d.filtered_valid && active);
    /* A direction failure must not be accepted just because raw read succeeds. */
    tick=8000;dir_result=1;body_induction_dev_init();
    d=sample(10000,0,0,&active);assert(!active && !d.filtered_valid && d.raw==-1 && d.init_rc);
    puts("PASS production PIR driver: PA3 calls, initialization, active-low debounce, GPIO errors, diagnostics and recovery");
}
'''
with tempfile.TemporaryDirectory(prefix='pir-driver-tests.') as build:
    program = Path(build) / 'test.c'
    binary = Path(build) / 'test'
    program.write_text(prefix + source[begin:end] + tests)
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    '-I' + str(project / 'include'), str(program),
                    str(project / 'src/monitor_logic.c'), '-lm', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
