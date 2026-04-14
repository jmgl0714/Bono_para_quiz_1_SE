#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/adc.h"

// PINES
#define PIN_BTN_DER      25
#define PIN_BTN_IZQ      33
#define PIN_LED_VERDE    15
#define PIN_LED_ROJO     14
#define PIN_SEG_A        23
#define PIN_SEG_B        22
#define PIN_SEG_C        21
#define PIN_SEG_D        19
#define PIN_SEG_E        18
#define PIN_SEG_F        5
#define PIN_SEG_G        17
#define PIN_DIG_CEN      16
#define PIN_DIG_DEC      4
#define PIN_DIG_UNI      32
#define PIN_HS_LEFT      12
#define PIN_LS_LEFT      26
#define PIN_HS_RIGHT     13
#define PIN_LS_RIGHT     27
#define ADC_CHANNEL      ADC1_CHANNEL_6

// PWM
#define PWM_FREQ_HZ      5000
#define PWM_RESOLUTION   LEDC_TIMER_8_BIT
#define PWM_MODE         LEDC_HIGH_SPEED_MODE
#define PWM_TIMER        LEDC_TIMER_0
#define PWM_CHANNEL_L    LEDC_CHANNEL_0
#define PWM_CHANNEL_R    LEDC_CHANNEL_1
#define PWM_DUTY_MAX     255

// Tiempos
#define SAFE_REVERSE_MS  300
#define DISPLAY_DELAY_MS 2
#define BUTTON_DELAY_MS  20

typedef enum
{
    DIR_RIGHT = 0,
    DIR_LEFT
} motor_dir_t;

// Variables globales de potencia y dirección
static volatile int percent_power = 0;
static volatile int pwm_value     = 0;
static volatile motor_dir_t current_dir   = DIR_RIGHT;
static volatile motor_dir_t requested_dir = DIR_RIGHT;

// Tabla de segmentos para dígitos 0-9, ánodo común (0=encendido, 1=apagado)
static const uint8_t digits_map[10][7] = {
    {0,0,0,0,0,0,1}, // 0
    {1,0,0,1,1,1,1}, // 1
    {0,0,1,0,0,1,0}, // 2
    {0,0,0,0,1,1,0}, // 3
    {1,0,0,1,1,0,0}, // 4
    {0,1,0,0,1,0,0}, // 5
    {0,1,0,0,0,0,0}, // 6
    {0,0,0,1,1,1,1}, // 7
    {0,0,0,0,0,0,0}, // 8
    {0,0,0,0,1,0,0}  // 9
};

// Inicializa todos los GPIO de salida y entrada
static void gpio_init_all(void)
{
    gpio_config_t out_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask =
            (1ULL << PIN_LED_VERDE) |
            (1ULL << PIN_LED_ROJO)  |
            (1ULL << PIN_SEG_A)     |
            (1ULL << PIN_SEG_B)     |
            (1ULL << PIN_SEG_C)     |
            (1ULL << PIN_SEG_D)     |
            (1ULL << PIN_SEG_E)     |
            (1ULL << PIN_SEG_F)     |
            (1ULL << PIN_SEG_G)     |
            (1ULL << PIN_DIG_CEN)   |
            (1ULL << PIN_DIG_DEC)   |
            (1ULL << PIN_DIG_UNI)   |
            (1ULL << PIN_HS_LEFT)   |
            (1ULL << PIN_HS_RIGHT),
        .pull_down_en = 0,
        .pull_up_en   = 0,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&out_conf);

    gpio_config_t in_conf = {
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_BTN_DER) | (1ULL << PIN_BTN_IZQ),
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .intr_type    = GPIO_INTR_DISABLE
    };
    gpio_config(&in_conf);
}

// Configura el ADC a 12 bits con atenuación 11dB
static void adc_init_all(void)
{
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC_CHANNEL, ADC_ATTEN_DB_11);
}

// Configura el timer y canales PWM para los MOSFET de low-side
static void pwm_init_all(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = PWM_MODE,
        .timer_num       = PWM_TIMER,
        .duty_resolution = PWM_RESOLUTION,
        .freq_hz         = PWM_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer_conf);

    ledc_channel_config_t ch_left = {
        .gpio_num   = PIN_LS_LEFT,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL_L,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch_left);

    ledc_channel_config_t ch_right = {
        .gpio_num   = PIN_LS_RIGHT,
        .speed_mode = PWM_MODE,
        .channel    = PWM_CHANNEL_R,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = PWM_TIMER,
        .duty       = 0,
        .hpoint     = 0
    };
    ledc_channel_config(&ch_right);
}

// Apaga los tres dígitos del display
static void all_digits_off(void)
{
    gpio_set_level(PIN_DIG_CEN, 1);
    gpio_set_level(PIN_DIG_DEC, 1);
    gpio_set_level(PIN_DIG_UNI, 1);
}

// Activa solo el dígito indicado (0=centenas, 1=decenas, 2=unidades)
static void enable_digit(int digit)
{
    all_digits_off();
    if (digit == 0) gpio_set_level(PIN_DIG_CEN, 0);
    if (digit == 1) gpio_set_level(PIN_DIG_DEC, 0);
    if (digit == 2) gpio_set_level(PIN_DIG_UNI, 0);
}

// Escribe los segmentos correspondientes al número dado
static void set_segments(int number)
{
    if (number < 0 || number > 9) number = 0;
    gpio_set_level(PIN_SEG_A, digits_map[number][0]);
    gpio_set_level(PIN_SEG_B, digits_map[number][1]);
    gpio_set_level(PIN_SEG_C, digits_map[number][2]);
    gpio_set_level(PIN_SEG_D, digits_map[number][3]);
    gpio_set_level(PIN_SEG_E, digits_map[number][4]);
    gpio_set_level(PIN_SEG_F, digits_map[number][5]);
    gpio_set_level(PIN_SEG_G, digits_map[number][6]);
}

// Refresca el display multiplexado mostrando percent_power
static void display_task(void *pvParameters)
{
    int digit = 0;

    while (1)
    {
        int centenas = percent_power / 100;
        int decenas  = (percent_power / 10) % 10;
        int unidades = percent_power % 10;

        switch (digit)
        {
            case 0:
                if (percent_power >= 100)
                {
                    set_segments(centenas);
                    enable_digit(0);
                }
                else
                {
                    all_digits_off();
                }
                break;

            case 1:
                if (percent_power >= 10)
                {
                    set_segments(decenas);
                    enable_digit(1);
                }
                else
                {
                    all_digits_off();
                }
                break;

            case 2:
                set_segments(unidades);
                enable_digit(2);
                break;
        }

        digit++;
        if (digit > 2) digit = 0;

        vTaskDelay(pdMS_TO_TICKS(DISPLAY_DELAY_MS));
    }
}

// Apaga todos los MOSFET del puente H
static void motor_off(void)
{
    // P-MOS high-side: GPIO=0 -> 4N25 no conduce -> gate a 12V -> OFF
    gpio_set_level(PIN_HS_LEFT,  0);
    gpio_set_level(PIN_HS_RIGHT, 0);

    // N-MOS low-side: duty=0 -> 4N25 no conduce -> gate a GND -> OFF
    ledc_set_duty(PWM_MODE, PWM_CHANNEL_L, 0);
    ledc_update_duty(PWM_MODE, PWM_CHANNEL_L);
    ledc_set_duty(PWM_MODE, PWM_CHANNEL_R, 0);
    ledc_update_duty(PWM_MODE, PWM_CHANNEL_R);
}

// Actualiza los LEDs según la dirección actual
static void update_leds(motor_dir_t dir)
{
    if (dir == DIR_RIGHT)
    {
        gpio_set_level(PIN_LED_VERDE, 1);
        gpio_set_level(PIN_LED_ROJO,  0);
    }
    else
    {
        gpio_set_level(PIN_LED_VERDE, 0);
        gpio_set_level(PIN_LED_ROJO,  1);
    }
}

// Aplica dirección y duty al puente H
static void apply_motor(motor_dir_t dir, int pwm)
{
    motor_off();

    if (pwm <= 0) return;

    // GPIO=1 -> 4N25 conduce -> gate P-MOS a GND -> P-MOS ON
    if (dir == DIR_RIGHT)
    {
        gpio_set_level(PIN_HS_LEFT,  1);
        gpio_set_level(PIN_HS_RIGHT, 0);

        ledc_set_duty(PWM_MODE, PWM_CHANNEL_L, 0);
        ledc_update_duty(PWM_MODE, PWM_CHANNEL_L);
        ledc_set_duty(PWM_MODE, PWM_CHANNEL_R, pwm);
        ledc_update_duty(PWM_MODE, PWM_CHANNEL_R);
    }
    else // DIR_LEFT
    {
        gpio_set_level(PIN_HS_LEFT,  0);
        gpio_set_level(PIN_HS_RIGHT, 1);

        ledc_set_duty(PWM_MODE, PWM_CHANNEL_L, pwm);
        ledc_update_duty(PWM_MODE, PWM_CHANNEL_L);
        ledc_set_duty(PWM_MODE, PWM_CHANNEL_R, 0);
        ledc_update_duty(PWM_MODE, PWM_CHANNEL_R);
    }
}

// Detiene el motor, espera y luego aplica la nueva dirección
static void safe_change_direction(motor_dir_t new_dir)
{
    if (new_dir == current_dir) return;

    motor_off();
    vTaskDelay(pdMS_TO_TICKS(SAFE_REVERSE_MS));

    current_dir = new_dir;
    update_leds(current_dir);
    apply_motor(current_dir, pwm_value);
}

// Lee el ADC, calcula porcentaje y duty, y actualiza el motor
static void adc_task(void *pvParameters)
{
    while (1)
    {
        int raw = adc1_get_raw(ADC_CHANNEL);

        if (raw < 0)    raw = 0;
        if (raw > 4095) raw = 4095;

        percent_power = (raw * 100) / 4095;
        pwm_value     = (percent_power * PWM_DUTY_MAX) / 100;

        apply_motor(current_dir, pwm_value);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// Detecta flancos en los botones y solicita cambio de dirección
static void button_task(void *pvParameters)
{
    bool last_der = true;
    bool last_izq = true;

    while (1)
    {
        bool der = gpio_get_level(PIN_BTN_DER);
        bool izq = gpio_get_level(PIN_BTN_IZQ);

        if (last_der == 1 && der == 0)
        {
            requested_dir = DIR_RIGHT;
        }

        if (last_izq == 1 && izq == 0)
        {
            requested_dir = DIR_LEFT;
        }

        if (requested_dir != current_dir)
        {
            safe_change_direction(requested_dir);
        }

        last_der = der;
        last_izq = izq;

        vTaskDelay(pdMS_TO_TICKS(BUTTON_DELAY_MS));
    }
}

// Inicializa periféricos y lanza las tareas
void app_main(void)
{
    gpio_init_all();
    adc_init_all();
    pwm_init_all();

    current_dir   = DIR_RIGHT;
    requested_dir = DIR_RIGHT;

    update_leds(current_dir);
    motor_off();
    all_digits_off();

    xTaskCreate(display_task, "display_task", 2048, NULL, 1, NULL);
    xTaskCreate(adc_task,     "adc_task",     2048, NULL, 1, NULL);
    xTaskCreate(button_task,  "button_task",  2048, NULL, 1, NULL);
}