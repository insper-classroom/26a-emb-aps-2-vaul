// gesture.cpp - Ponte C++ entre o firmware (main.c) e a IA do Edge Impulse.
//
// Le o acelerometro do MPU6050 (i2c0), monta a janela de amostras, roda o
// classificador treinado (idle / updown / wave) e, quando detecta "updown",
// envia "MINE\n" pela Bluetooth -> clique esquerdo no PC -> quebrar bloco.
//
// Adaptado de reference/ei_reference_main.cpp, com 3 simplificacoes:
//   - Sem LED RGB: a acao e o proprio clique (ignoramos rgb_led).
//   - So o gesto "updown" (indice 1) importa; "idle"/"wave" sao ignorados.
//   - Disparo por borda: 1 clique por gesto, rearma so ao sair do estado.
//
// O Edge Impulse SDK e C++ (usa std::function, templates, namespaces) e nao
// pode virar C. Por isso a inferencia mora aqui, e expomos so gesture_task()
// com linkagem C (via gesture.h) para o main.c criar a task.

#include <hardware/gpio.h>
#include <hardware/i2c.h>
#include <stdio.h>
#include <string.h>

#include <FreeRTOS.h>
#include <task.h>

// ei_run_classifier.h DEFINE run_classifier() (corpo, nao-inline). O SDK espera
// que exatamente UM arquivo do usuario o inclua: incluir aqui faz o gesture.cpp
// emitir a definicao. Neste projeto EI_C_LINKAGE nao esta definido, entao nenhum
// .cpp do SDK provê o simbolo -> esta e a unica unidade que o define.
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"
#include "model-parameters/model_metadata.h"

#include "gesture.h"
#include "mpu6050.h"

using namespace ei;

// --- Alocador do Edge Impulse: sobrescreve as versoes weak do SDK ---------------
// Por padrao (sem FREERTOS_ENABLED) o EI usa o malloc da libc. No Pico+FreeRTOS,
// esse malloc passa por um mutex que le a task atual (pxCurrentTCB) -- NULL antes
// do escalonador. O construtor estatico de ei::spectral chama ei_malloc ANTES do
// main() -> bate nesse caminho e trava no boot. pvPortMalloc (heap_4) nao tem essa
// dependencia (o xTaskCreate no main() ja o usa pre-escalonador) e e thread-safe.
// Definicoes fortes (linkagem C++, igual ao header do EI) vencem as weak do porting,
// sem precisar de FREERTOS_ENABLED.
void *ei_malloc(size_t size) {
    return pvPortMalloc(size);
}
void *ei_calloc(size_t nitems, size_t size) {
    size_t total = nitems * size;
    void *p = pvPortMalloc(total);
    if (p) memset(p, 0, total);
    return p;
}
void ei_free(void *ptr) {
    vPortFree(ptr);
}

static bool debug_nn = false;

// --- MPU6050 no barramento i2c0 (GP8 = SDA, GP9 = SCL) ---
// A referencia usava i2c_default em GP4/GP5, mas aqui esses pinos sao a UART1
// do HC-06. O OLED ja ocupa o i2c1 (GP2/GP3); o i2c0 estava livre.
static i2c_inst_t *const MPU_I2C = i2c0;
static const uint MPU_SDA_GPIO = 8;
static const uint MPU_SCL_GPIO = 9;

// Indice do gesto que dispara o clique (ver model_variables.h:
// {"idle", "updown", "wave"} -> 0, 1, 2).
static const size_t LABEL_UPDOWN = 1;

// Confianca minima para aceitar o gesto (alinhado ao EI_CLASSIFIER_THRESHOLD).
static const float UPDOWN_CONFIDENCE = 0.6f;

// Periodo de amostragem: modelo treinado a 85 Hz (~11.76 ms). Com tick do
// FreeRTOS a 1 kHz a granularidade minima e 1 ms -> 12 ms ~= 83.3 Hz.
static const TickType_t SAMPLE_PERIOD_TICKS = pdMS_TO_TICKS(12);

static void mpu6050_init(void) {
    i2c_init(MPU_I2C, 400 * 1000);
    gpio_set_function(MPU_SDA_GPIO, GPIO_FUNC_I2C);
    gpio_set_function(MPU_SCL_GPIO, GPIO_FUNC_I2C);
    gpio_pull_up(MPU_SDA_GPIO);
    gpio_pull_up(MPU_SCL_GPIO);

    // Tira o sensor do sleep: PWR_MGMT_1 = 0x00.
    uint8_t buf[] = { MPUREG_PWR_MGMT_1, 0x00 };
    i2c_write_blocking(MPU_I2C, MPU6050_I2C_DEFAULT, buf, 2, false);
}

// Le os 3 eixos do acelerometro (registros 0x3B..0x40), raw int16.
static void mpu6050_read_accel(int16_t accel[3]) {
    uint8_t buffer[6];
    uint8_t reg = MPUREG_ACCEL_XOUT_H;
    i2c_write_blocking(MPU_I2C, MPU6050_I2C_DEFAULT, &reg, 1, true);
    i2c_read_blocking(MPU_I2C, MPU6050_I2C_DEFAULT, buffer, 6, false);

    for (int i = 0; i < 3; i++) {
        accel[i] = (int16_t)((buffer[i * 2] << 8) | buffer[(i * 2) + 1]);
    }
}

extern "C" void gesture_task(void *p) {
    (void)p;
    mpu6050_init();

    int16_t accel[3];
    bool prev_updown = false;  // estado anterior, para disparo por borda

    while (true) {
        // 1) Coleta uma janela cheia (~2 s) de amostras de aceleracao.
        float buffer[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE] = { 0 };

        // DIAG: rastreia min/max de cada eixo na janela para medir o pico-a-pico
        // (quanto o sinal varia). pp ~0 mexendo => MPU nao entrega dado (infra);
        // pp grande mexendo => dado vivo (entao e questao de modelo/gesto).
        int16_t mn[3] = { 32767, 32767, 32767 };
        int16_t mx[3] = { -32768, -32768, -32768 };

        TickType_t last_wake = xTaskGetTickCount();
        for (size_t ix = 0; ix < EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; ix += 3) {
            mpu6050_read_accel(accel);
            buffer[ix + 0] = (float)accel[0];
            buffer[ix + 1] = (float)accel[1];
            buffer[ix + 2] = (float)accel[2];
            for (int a = 0; a < 3; a++) {
                if (accel[a] < mn[a]) mn[a] = accel[a];
                if (accel[a] > mx[a]) mx[a] = accel[a];
            }
            vTaskDelayUntil(&last_wake, SAMPLE_PERIOD_TICKS);
        }
        int pp = 0;  // maior pico-a-pico entre os 3 eixos
        for (int a = 0; a < 3; a++) {
            int r = (int)mx[a] - (int)mn[a];
            if (r > pp) pp = r;
        }

        // 2) Roda o classificador sobre a janela.
        signal_t signal;
        int err = numpy::signal_from_buffer(buffer, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
        if (err != 0) {
            printf("[gesture] signal_from_buffer falhou (%d)\n", err);
            continue;
        }

        ei_impulse_result_t result = { 0 };
        EI_IMPULSE_ERROR ei_err = run_classifier(&signal, &result, debug_nn);
        if (ei_err != EI_IMPULSE_OK) {
            printf("[gesture] run_classifier falhou (%d)\n", ei_err);
            continue;
        }

        // 3) Acha o gesto de maior confianca.
        size_t best = 0;
        for (size_t ix = 1; ix < EI_CLASSIFIER_LABEL_COUNT; ix++) {
            if (result.classification[ix].value > result.classification[best].value) {
                best = ix;
            }
        }

        // DIAG (remover depois): transmite predicao + pico-a-pico do acelerometro
        // pela Bluetooth para validar a IA ao vivo no terminal.py.
        // Formato "AI:<label> <conf%> pp<pico-a-pico>".
        char dbg[32];
        snprintf(dbg, sizeof(dbg), "AI:%s %d pp%d\n",
                 result.classification[best].label,
                 (int)(result.classification[best].value * 100.0f),
                 pp);
        controller_send_command(dbg);

        // 4) "updown" ativo = e o vencedor E passou do limiar de confianca.
        bool updown = (best == LABEL_UPDOWN) &&
                      (result.classification[LABEL_UPDOWN].value >= UPDOWN_CONFIDENCE);

        // 5) Disparo por borda: 1 clique por gesto; rearma so ao sair do estado.
        if (updown && !prev_updown) {
            controller_send_command("MINE\n");
        }
        prev_updown = updown;
    }
}
