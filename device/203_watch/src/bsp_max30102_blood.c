#include "bsp_max30102_blood.h"

#include <stdio.h>
#include <string.h> // For memset

#define BLOCK_SIZE           1     /* 每次处理的采样点个数 */
#define NUM_TAPS             29    /* 滤波器系数个数 */

typedef struct {
    float *coeffs;          /* 滤波器系数 */
    float *state;           /* 状态缓存 */
    unsigned int blockSize;     /* 处理的块大小 */
    unsigned int numTaps;       /* 滤波器阶数 */
} fir_instance_f32;

static float firCoeffs32LP[NUM_TAPS] = {
    -0.001542701735,-0.002211477375,-0.003286228748, -0.00442651147,-0.004758632276,
    -0.003007677384, 0.002192312852,  0.01188309677,  0.02637642808,  0.04498152807,
    0.06596207619,   0.0867607221,   0.1044560149,   0.1163498312,   0.1205424443,
    0.1163498312,   0.1044560149,   0.0867607221,  0.06596207619,  0.04498152807,
    0.02637642808,  0.01188309677, 0.002192312852,-0.003007677384,-0.004758632276,
    -0.00442651147,-0.003286228748,-0.002211477375,-0.001542701735
};

static float firStateF32_ir[BLOCK_SIZE + NUM_TAPS - 1];        /* 状态缓存 */
static float firStateF32_red[BLOCK_SIZE + NUM_TAPS - 1];        /* 状态缓存 */

fir_instance_f32 S_ir = {firCoeffs32LP, firStateF32_ir, BLOCK_SIZE, NUM_TAPS};
fir_instance_f32 S_red = {firCoeffs32LP, firStateF32_red, BLOCK_SIZE, NUM_TAPS};

void max30102_fir_init(void)
{
    // 初始化状态缓存为0
    memset(firStateF32_ir, 0, sizeof(firStateF32_ir));
    memset(firStateF32_red, 0, sizeof(firStateF32_red));
}

void fir_f32(fir_instance_f32 *S, float *input, float *output)
{
    float *state = S->state;
    float *coeffs = S->coeffs;
    unsigned int i, j;

    for (i = 0; i < S->blockSize; i++) {
        float acc = 0.0f;

        // 将新的输入样本添加到状态缓存的末尾，并将旧样本左移
        memmove(&state[0], &state[1], (S->numTaps - 1) * sizeof(float));
        state[S->numTaps - 1] = input[i];

        // 执行卷积操作：y[n] = sum(x[n-i] * h[i])
        for (j = 0; j < S->numTaps; j++) {
            acc += state[j] * coeffs[j];
        }

        output[i] = acc;
    }
}

void ir_max30102_fir(float *input, float *output)
{
    fir_f32(&S_ir, input, output);
}

void red_max30102_fir(float *input, float *output)
{
    fir_f32(&S_red, input, output);
}

