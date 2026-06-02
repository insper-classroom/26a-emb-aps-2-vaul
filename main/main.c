#include <FreeRTOS.h>
#include <queue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <task.h>

#include "hardware/adc.h"
#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/uart.h"
#include "hc06.h"
#include "pico/stdlib.h"
#include "ssd1306.h"

#define HC06_NAME "ENZO-BLUETOOTH"
#define HC06_PIN "1234"

#define BUTTON_PIN 10
#define PAUSE_BTN_PIN 14
#define MACRO_BTN_PIN 12
#define MACRO_LED_PIN 13
#define MACRO_MAX_EVENTS 4000   // ~20s @ 200 ev/s (.bss ~64KB; RP2350 tem 520KB)
#define OLED_SDA_PIN 2
#define OLED_SCL_PIN 3

#define JOYSTICK_VRX_PIN 26
#define JOYSTICK_VRY_PIN 27
#define JOYSTICK_SW_PIN 15
#define JOYSTICK_DEADZONE 60
#define JOYSTICK_OUT_MAX 127
#define JOYSTICK_FILTER_N 5

// Mensagem atomica para o stream Bluetooth: cada produtor (joystick, botoes,
// IA, etc.) enfileira um pacote inteiro de uma vez, evitando que os bytes de
// um quadro se intercalem com os de outro na xQueueTX.
typedef struct {
    uint8_t data[24];
    uint8_t len;
} tx_msg_t;

QueueHandle_t xQueueRX;
QueueHandle_t xQueueTX;
QueueHandle_t xQueuePin;

static ssd1306_t g_disp;

// ---------------------------------------------------------------------------
// Macro: grava o stream de saida (tap no tx_task, consumidor unico -> sem
// corrida) e o re-injeta na xQueueTX com o mesmo timing (replay). RECORDING e
// PLAYING sao mutuamente exclusivos, entao o buffer e escrito (tx_task) e lido
// (macro_task) sem sobreposicao. Tudo em RAM -> some no reset (boot = EMPTY).
// ---------------------------------------------------------------------------
typedef enum { MACRO_EMPTY, MACRO_RECORDING, MACRO_READY, MACRO_PLAYING } macro_state_t;

typedef struct {
    uint8_t data[8];
    uint8_t len;
    TickType_t t_tick;   // ticks desde o inicio da gravacao
} macro_event_t;

static macro_event_t g_macro_buf[MACRO_MAX_EVENTS];
static volatile int g_macro_count = 0;
static volatile bool g_macro_recording = false;
static volatile bool g_macro_playing = false;
static volatile TickType_t g_macro_rec_start = 0;

void uart_rx_handler() {
        uint8_t ch = uart_getc(HC06_UART_ID);
        xQueueSendFromISR(xQueueRX, &ch, 0);
}

void init_uart_hc06(void) {
    uart_init(HC06_UART_ID, HC06_BAUD_RATE);

    // Set the TX and RX pins by using the function select on the GPIO
    // Set datasheet for more information on function select
    gpio_set_function(HC06_TX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_TX_PIN));
    gpio_set_function(HC06_RX_PIN, UART_FUNCSEL_NUM(HC06_UART_ID, HC06_RX_PIN));

    int __unused actual = uart_set_baudrate(HC06_UART_ID, HC06_BAUD_RATE);

    // Set UART flow control CTS/RTS, we don't want these, so turn them off
    uart_set_hw_flow(HC06_UART_ID, false, false);

    // Set our data format
    uart_set_format(HC06_UART_ID, 8, 1, UART_PARITY_NONE);
}

void init_uart_irq() {
     // Turn off FIFO's - we want to do this character by character
    uart_set_fifo_enabled(HC06_UART_ID, false);

    // Set up a RX interrupt
    // We need to set up the handler first
    // Select correct interrupt for the UART we are using
    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    // And set up and enable the interrupt handlers
    irq_set_exclusive_handler(UART_IRQ, uart_rx_handler);
    irq_set_enabled(UART_IRQ, true);

    // Now enable the UART to send interrupts - RX only
    uart_set_irq_enables(HC06_UART_ID, true, false);
}

static void tx_task(void* p) {
    tx_msg_t msg;
    while (true) {
        if (xQueueReceive(xQueueTX, &msg, portMAX_DELAY) == pdTRUE) {
            for (int i = 0; i < msg.len; i++) {
                uart_putc_raw(HC06_UART_ID, msg.data[i]);
            }
            // Tap de gravacao do macro: consumidor unico do stream -> sem corrida.
            if (g_macro_recording && g_macro_count < MACRO_MAX_EVENTS && msg.len <= 8) {
                macro_event_t *e = &g_macro_buf[g_macro_count];
                memcpy(e->data, msg.data, msg.len);
                e->len = msg.len;
                e->t_tick = xTaskGetTickCount() - g_macro_rec_start;
                g_macro_count++;
            }
        }
    }
}

static void button_task(void* p) {
    gpio_init(BUTTON_PIN);
    gpio_set_dir(BUTTON_PIN, GPIO_IN);
    gpio_pull_up(BUTTON_PIN);

    srand(to_us_since_boot(get_absolute_time()));

    int UART_IRQ = HC06_UART_ID == uart0 ? UART0_IRQ : UART1_IRQ;

    while (true) {
        if (gpio_get(BUTTON_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(30));
            if (gpio_get(BUTTON_PIN) == 0) {
                char pin[5];
                snprintf(pin, sizeof(pin), "%04d", rand() % 10000);

                irq_set_enabled(UART_IRQ, false);
                hc06_set_at_mode(1);
                hc06_set_pin(pin);
                hc06_set_at_mode(0);
                irq_set_enabled(UART_IRQ, true);

                xQueueOverwrite(xQueuePin, pin);

                while (gpio_get(BUTTON_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void joystick_send_axis(uint8_t axis, int16_t value) {
    if (g_macro_playing) return;  // durante replay, nao mistura input ao vivo
    tx_msg_t msg;
    msg.data[0] = 0xFF;
    msg.data[1] = axis;
    msg.data[2] = (uint8_t)(value & 0xFF);
    msg.data[3] = (uint8_t)((value >> 8) & 0xFF);
    msg.len = 4;
    xQueueSend(xQueueTX, &msg, 0);
}

// Enfileira uma mensagem ASCII (comando) inteira no stream Bluetooth.
static void send_command(const char *s) {
    if (g_macro_playing) return;  // durante replay, nao mistura input ao vivo
    tx_msg_t msg;
    size_t n = strlen(s);
    if (n > sizeof(msg.data)) n = sizeof(msg.data);
    memcpy(msg.data, s, n);
    msg.len = (uint8_t)n;
    xQueueSend(xQueueTX, &msg, 0);
}

// Botao embutido no joystick (pino SW). Solto = HIGH (pull-up interno);
// "afundar" o joystick fecha o switch para o GND => leitura 0.
// Detecta a borda de pressao (com debounce) e envia "JUMP\n" uma vez por
// pressao; rearma so quando o botao volta a ser solto.
static void jump_task(void* p) {
    gpio_init(JOYSTICK_SW_PIN);
    gpio_set_dir(JOYSTICK_SW_PIN, GPIO_IN);
    gpio_pull_up(JOYSTICK_SW_PIN);

    bool was_pressed = false;
    while (true) {
        bool pressed = (gpio_get(JOYSTICK_SW_PIN) == 0);

        if (pressed && !was_pressed) {
            vTaskDelay(pdMS_TO_TICKS(20));  // debounce
            if (gpio_get(JOYSTICK_SW_PIN) == 0) {
                send_command("JUMP\n");
                was_pressed = true;
            }
        } else if (!pressed) {
            was_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Botao de Pause (novo, num pino antes usado pelo LED RGB). Clique simples ->
// "PAUSE\n" (o PC mapeia para ESC no Minecraft). O "power" (corte de energia)
// fica para o hardware final com bateria + circuito de latch -- alimentado por
// USB nao ha o que cortar, entao por enquanto este botao e so pause.
static void pause_task(void* p) {
    gpio_init(PAUSE_BTN_PIN);
    gpio_set_dir(PAUSE_BTN_PIN, GPIO_IN);
    gpio_pull_up(PAUSE_BTN_PIN);

    bool was_pressed = false;
    while (true) {
        bool pressed = (gpio_get(PAUSE_BTN_PIN) == 0);

        if (pressed && !was_pressed) {
            vTaskDelay(pdMS_TO_TICKS(20));  // debounce
            if (gpio_get(PAUSE_BTN_PIN) == 0) {
                send_command("PAUSE\n");
                was_pressed = true;
            }
        } else if (!pressed) {
            was_pressed = false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static int16_t joystick_process(uint16_t raw) {
    int delta = (int)raw - 2048;
    if (delta > -JOYSTICK_DEADZONE && delta < JOYSTICK_DEADZONE) {
        return 0;
    }
    int scaled;
    if (delta > 0) {
        scaled = ((delta - JOYSTICK_DEADZONE) * JOYSTICK_OUT_MAX) / (2047 - JOYSTICK_DEADZONE);
    } else {
        scaled = ((delta + JOYSTICK_DEADZONE) * JOYSTICK_OUT_MAX) / (2048 - JOYSTICK_DEADZONE);
    }
    if (scaled > JOYSTICK_OUT_MAX) scaled = JOYSTICK_OUT_MAX;
    if (scaled < -JOYSTICK_OUT_MAX) scaled = -JOYSTICK_OUT_MAX;
    return (int16_t)scaled;
}

static void joystick_task(void* p) {
    adc_init();
    adc_gpio_init(JOYSTICK_VRX_PIN);
    adc_gpio_init(JOYSTICK_VRY_PIN);

    uint16_t buf_x[JOYSTICK_FILTER_N] = {0};
    uint16_t buf_y[JOYSTICK_FILTER_N] = {0};
    int idx = 0;

    while (true) {
        adc_select_input(0);
        buf_x[idx] = adc_read();
        adc_select_input(1);
        buf_y[idx] = adc_read();
        idx = (idx + 1) % JOYSTICK_FILTER_N;

        uint32_t sum_x = 0, sum_y = 0;
        for (int i = 0; i < JOYSTICK_FILTER_N; i++) {
            sum_x += buf_x[i];
            sum_y += buf_y[i];
        }
        uint16_t avg_x = sum_x / JOYSTICK_FILTER_N;
        uint16_t avg_y = sum_y / JOYSTICK_FILTER_N;

        int16_t vx = joystick_process(avg_x);
        int16_t vy = joystick_process(avg_y);

        // Modelo de VELOCIDADE: envia os dois eixos todo ciclo (inclusive 0),
        // para o PC sempre ter a velocidade atual e parar ao centrar. A 100 Hz
        // sao ~800 B/s @ 115200 baud (~7% do link) -- UART nao e gargalo.
        joystick_send_axis(0, vx);
        joystick_send_axis(1, vy);

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// Zera a velocidade da camera nos dois eixos. Como o PC segura a ultima
// velocidade entre pacotes, sem isso a camera continuaria girando apos o
// replay terminar/ser interrompido no meio de um movimento.
static void macro_emit_stop(void) {
    for (uint8_t axis = 0; axis < 2; axis++) {
        tx_msg_t m;
        m.data[0] = 0xFF;
        m.data[1] = axis;
        m.data[2] = 0;
        m.data[3] = 0;
        m.len = 4;
        xQueueSend(xQueueTX, &m, 0);
    }
}

// FSM do macro (1 botao) + LED de status (PWM) + replay por tempo.
// EMPTY (off) -> [click] -> RECORDING (pisca rapido) -> [click] -> READY
// (fade lento) -> [click] -> PLAYING (aceso). PLAYING + [click] = interrompe
// (mantem gravacao, volta a READY). PLAYING ate o fim = limpa e volta a EMPTY.
// Clique-vence: na mesma iteracao, o clique (interrupcao) tem prioridade.
static void macro_task(void* p) {
    gpio_init(MACRO_BTN_PIN);
    gpio_set_dir(MACRO_BTN_PIN, GPIO_IN);
    gpio_pull_up(MACRO_BTN_PIN);

    gpio_set_function(MACRO_LED_PIN, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(MACRO_LED_PIN);
    uint chan  = pwm_gpio_to_channel(MACRO_LED_PIN);
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv(&cfg, 4.0f);
    pwm_config_set_wrap(&cfg, 255);
    pwm_init(slice, &cfg, true);
    pwm_set_chan_level(slice, chan, 0);

    macro_state_t state = MACRO_EMPTY;
    bool was_pressed = false;
    int play_idx = 0;
    TickType_t play_start = 0;
    int fade = 0, fade_dir = 6;
    uint32_t phase = 0;

    while (true) {
        // --- borda de clique (com debounce) ---
        bool clicked = false;
        bool pressed = (gpio_get(MACRO_BTN_PIN) == 0);
        if (pressed && !was_pressed) {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (gpio_get(MACRO_BTN_PIN) == 0) {
                clicked = true;
                was_pressed = true;
            }
        } else if (!pressed) {
            was_pressed = false;
        }

        // --- FSM (clique tem prioridade sobre o avanco do replay) ---
        if (clicked) {
            switch (state) {
                case MACRO_EMPTY:
                    g_macro_count = 0;
                    g_macro_rec_start = xTaskGetTickCount();
                    g_macro_recording = true;
                    state = MACRO_RECORDING;
                    break;
                case MACRO_RECORDING:
                    g_macro_recording = false;  // para de gravar ANTES de ler count
                    state = (g_macro_count > 0) ? MACRO_READY : MACRO_EMPTY;
                    break;
                case MACRO_READY:
                    play_idx = 0;
                    play_start = xTaskGetTickCount();
                    g_macro_playing = true;     // suprime input ao vivo
                    state = MACRO_PLAYING;
                    break;
                case MACRO_PLAYING:
                    g_macro_playing = false;    // interrompe, MANTEM a gravacao
                    macro_emit_stop();
                    state = MACRO_READY;
                    break;
            }
        } else if (state == MACRO_PLAYING) {
            // emite todos os eventos cujo tempo ja chegou
            TickType_t elapsed = xTaskGetTickCount() - play_start;
            while (play_idx < g_macro_count && g_macro_buf[play_idx].t_tick <= elapsed) {
                tx_msg_t m;
                memcpy(m.data, g_macro_buf[play_idx].data, g_macro_buf[play_idx].len);
                m.len = g_macro_buf[play_idx].len;
                xQueueSend(xQueueTX, &m, 0);
                play_idx++;
            }
            if (play_idx >= g_macro_count) {    // terminou naturalmente -> limpa
                g_macro_playing = false;
                macro_emit_stop();
                g_macro_count = 0;
                state = MACRO_EMPTY;
            }
        }

        // --- LED por estado ---
        uint16_t duty = 0;
        switch (state) {
            case MACRO_EMPTY:
                duty = 0;
                break;
            case MACRO_RECORDING:
                duty = (phase % 12 < 6) ? 255 : 0;   // pisca rapido (~8 Hz)
                break;
            case MACRO_READY:
                fade += fade_dir;                    // fade lento (respirando)
                if (fade >= 255) { fade = 255; fade_dir = -fade_dir; }
                else if (fade <= 0) { fade = 0; fade_dir = -fade_dir; }
                duty = (uint16_t)fade;
                break;
            case MACRO_PLAYING:
                duty = 255;                          // aceso fixo
                break;
        }
        pwm_set_chan_level(slice, chan, duty);

        phase++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void oled_task(void* p) {
    i2c_init(i2c1, 400000);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);

    g_disp.external_vcc = false;
    ssd1306_init(&g_disp, 128, 32, 0x3C, i2c1);
    ssd1306_clear(&g_disp);

    char pin[5];
    snprintf(pin, sizeof(pin), "%s", HC06_PIN);

    while (true) {
        ssd1306_clear(&g_disp);
        ssd1306_draw_string(&g_disp, 0, 0, 1, "PIN atual:");
        ssd1306_draw_string(&g_disp, 0, 16, 2, pin);
        ssd1306_show(&g_disp);

        if (xQueueReceive(xQueuePin, pin, portMAX_DELAY) == pdTRUE) {
            // novo PIN recebido, loop redesenha
        }
    }
}

int main(void) {
    stdio_init_all();

    // inicializa uart e hc06
    init_uart_hc06();
    hc06_config(HC06_NAME, HC06_PIN);
    init_uart_irq();

    xQueueRX = xQueueCreate(256, sizeof(uint8_t));
    xQueueTX = xQueueCreate(32, sizeof(tx_msg_t));
    xQueuePin = xQueueCreate(1, 5 * sizeof(char));

    gpio_put(MACRO_LED_PIN, 1);
    sleep_ms(1000);
    gpio_put(MACRO_LED_PIN, 0);

    xTaskCreate(tx_task, "TX", 512, NULL, 2, NULL);
    xTaskCreate(button_task, "BTN", 512, NULL, 1, NULL);
    xTaskCreate(jump_task, "JUMP", 256, NULL, 1, NULL);
    xTaskCreate(pause_task, "PAUSE", 256, NULL, 1, NULL);
    xTaskCreate(macro_task, "MACRO", 512, NULL, 1, NULL);
    xTaskCreate(oled_task, "OLED", 1024, NULL, 1, NULL);
    xTaskCreate(joystick_task, "JOY", 512, NULL, 1, NULL);

    vTaskStartScheduler();
    while (true);
}
