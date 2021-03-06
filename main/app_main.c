/*
 * MIT License
 *
 * Copyright (c) 2017 David Antliff
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "driver/pcnt.h"
#include "soc/rtc.h"
#include "driver/rmt.h"

#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "freqcount"

#define GPIO_LED             (GPIO_NUM_2)
#define GPIO_FREQ_SIGNAL     (CONFIG_FREQ_SIGNAL_GPIO)
#define GPIO_RMT             (GPIO_NUM_5)  // also used as PCNT control

#define PCNT_UNIT 0
#define RMT_TX_CHANNEL RMT_CHANNEL_0
//#define RMT_CLK_DIV          20   // results in 0.25us steps (80MHz / 20 = 4 MHz
#define RMT_CLK_DIV            160   // results in 2us steps (80MHz / 160 = 0.5 MHz
//#define RMT_CLK_DIV          1  // results in 25ns steps (80MHz / 2 / 1 = 40 MHz)

// The counter is signed 16-bit, so maximum positive value is 32767
// The filter is unsigned 10-bit, maximum value is 1023. Use full period of maximum frequency.
// For higher expected frequencies, the sample period and filter must be reduced.

// suitable up to 16,383.5 kHz
#define SAMPLE_PERIOD 1.0  // seconds
#define FILTER_LENGTH 1023  // APB @ 80MHz, limits to < 39,100 Hz

// suitable up to 163,835 Hz
//#define SAMPLE_PERIOD 0.1  // seconds
//#define FILTER_LENGTH 122  // APB @ 80MHz, limits to < 655,738 Hz

// suitable up to 1,638,350 Hz
//#define SAMPLE_PERIOD 0.01  // seconds
//#define FILTER_LENGTH 12  // APB @ 80MHz, limits to < 3,333,333 Hz

// suitable up to 16,383,500 Hz - no filter
//#define SAMPLE_PERIOD 0.001  // seconds
//#define FILTER_LENGTH 0  // APB @ 80MHz, limits to < 40 MHz


//#define ESP_INTR_FLAG_DEFAULT 0
//
//SemaphoreHandle_t xSemaphore = NULL;
//
//// interrupt service routine
//void IRAM_ATTR isr_handler(void * arg)
//{
//    // notify
//    xSemaphoreGiveFromISR(xSemaphore, NULL);
//}

void led_init(void)
{
    gpio_pad_select_gpio(GPIO_LED);
    gpio_set_direction(GPIO_LED, GPIO_MODE_OUTPUT);
}

void led_set(bool state)
{
    gpio_set_level(GPIO_LED, state ? 1 : 0);
}

void led_on(void)
{
    gpio_set_level(GPIO_LED, 1);
}

void led_off(void)
{
    gpio_set_level(GPIO_LED, 0);
}

//void task(void * arg)
//{
//    bool led_status = false;
//
//    for (;;) {
//        // wait for notification from ISR
//        if (xSemaphoreTake(xSemaphore, portMAX_DELAY) == pdTRUE)
//        {
//            ESP_LOGI(TAG, "ISR fired!");
//            led_status = !led_status;
//            led_set(led_status);
//        }
//    }
//}

static int16_t read_and_clear_counter(pcnt_unit_t pcnt_unit)
{
    int16_t count = 0;
    pcnt_get_counter_value(PCNT_UNIT, &count);
    pcnt_counter_clear(PCNT_UNIT);
    return count;
}

void read_counter_task(void * arg)
{
    while (1)
    {
        // read counter
        int16_t count = read_and_clear_counter(PCNT_UNIT);
        if (count > 0)
        {
            led_on();
        }
        else
        {
            led_off();
        }
        ESP_LOGI(TAG, "counter = %d", count);
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}



typedef struct
{
    double period;
} rmt_tx_task_args_t;

void rmt_tx_task(void * arg)
{
    rmt_tx_task_args_t * rmt_tx_task_args = arg;

    while (1)
    {
        rmt_item32_t items[100] = { 0 };
        int num_items = 0;

//        // toggle:
//        for (num_items = 0; num_items < 16; ++num_items)
//        {
//            items[num_items].level0 = 0x1 & num_items;
//            items[num_items].duration0 = 1;
//            items[num_items].level1 = ~items[num_items].level0;
//            items[num_items].duration1 = 1;
//        }

        // enable counter for exactly x seconds:
        double sample_period = SAMPLE_PERIOD;
        int32_t total_duration = (uint32_t)(sample_period / rmt_tx_task_args->period);
        //ESP_LOGI(TAG, "total_duration %d periods", total_duration);

        // max duration per item is 2^15-1 = 32767
        while (total_duration > 0)
        {
            uint32_t duration = total_duration > 32767 ? 32767 : total_duration;
            items[num_items].level0 = 1;
            items[num_items].duration0 = duration;
            total_duration -= duration;
            //ESP_LOGI(TAG, "duration %d", duration);

            if (total_duration > 0)
            {
                uint32_t duration = total_duration > 32767 ? 32767 : total_duration;
                items[num_items].level1 = 1;
                items[num_items].duration1 = duration;
                total_duration -= duration;
            }
            else
            {
                items[num_items].level1 = 0;
                items[num_items].duration1 = 0;
            }
            //ESP_LOGI(TAG, "[%d].level0 %d", num_items, items[num_items].level0);
            //ESP_LOGI(TAG, "[%d].duration0 %d", num_items, items[num_items].duration0);
            //ESP_LOGI(TAG, "[%d].level1 %d", num_items, items[num_items].level1);
            //ESP_LOGI(TAG, "[%d].duration1 %d", num_items, items[num_items].duration1);

            ++num_items;
        }
        //ESP_LOGI(TAG, "num_items %d", num_items);
        //ESP_LOGI(TAG, "RMT TX");

        // clear counter
        pcnt_counter_clear(PCNT_UNIT);
        led_set(1);
        rmt_write_items(RMT_TX_CHANNEL, items, num_items, false);
        rmt_wait_tx_done(RMT_TX_CHANNEL);
        led_set(0);

        // read counter
        int16_t count = 0;
        pcnt_get_counter_value(PCNT_UNIT, &count);
        pcnt_counter_clear(PCNT_UNIT);
        ESP_LOGI(TAG, "counter = %d", count);

        // TODO: check for overflow?

        double frequency = count / 2.0 / sample_period;
        ESP_LOGI(TAG, "frequency %f", frequency);

        vTaskDelay(1000 / portTICK_PERIOD_MS);
        //pcnt_get_counter_value(PCNT_UNIT, &count);
        //pcnt_counter_clear(PCNT_UNIT);
        //ESP_LOGI(TAG, "counter = %d", count);
    }
}

void app_main()
{
//    xSemaphore = xSemaphoreCreateBinary();

    esp_log_level_set("*", ESP_LOG_INFO);

    ESP_LOGI(TAG, "[APP] Startup..");
    led_init();
    led_off();

    // round to nearest MHz (stored value is only precise to MHz)
    uint32_t apb_freq = (rtc_clk_apb_freq_get() + 500000) / 1000000 * 1000000;
    ESP_LOGI(TAG, "APB CLK %u Hz", apb_freq);

    // set Freq signal GPIO to be input only
//    gpio_pad_select_gpio(GPIO_FREQ_SIGNAL);
//    gpio_set_direction(GPIO_FREQ_SIGNAL, GPIO_MODE_INPUT);

//    // Basic GPIO interrupt:
//    // enable interrupt on falling edge for pin
//    gpio_set_intr_type(GPIO_FREQ_SIGNAL, GPIO_INTR_NEGEDGE);
//
//    xTaskCreate(task, "task", 2048, NULL, 10, NULL);
//
//    // install ISR service with default configuration
//    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
//
//    // attach the interrupt service routine
//    gpio_isr_handler_add(GPIO_FREQ_SIGNAL, isr_handler, NULL);




    // RMT

    rmt_config_t rmt_tx = {
        .rmt_mode = RMT_MODE_TX,
        .channel = RMT_TX_CHANNEL,
        .gpio_num = GPIO_RMT,
        .mem_block_num = 1,  // single block
        .clk_div = RMT_CLK_DIV,
        .tx_config.loop_en = false,
        .tx_config.carrier_en = false,
        .tx_config.idle_level = RMT_IDLE_LEVEL_LOW,
        .tx_config.idle_output_en = true,
    };
    rmt_config(&rmt_tx);
    //rmt_set_source_clk(RMT_TX_CHANNEL, RMT_BASECLK_APB);
    rmt_driver_install(rmt_tx.channel, 0, 0);

    const double rmt_period = (double)(RMT_CLK_DIV) / 80000000.0;
    ESP_LOGI(TAG, "rmt_period %.4e s", rmt_period);
    rmt_tx_task_args_t rmt_tx_task_args = {
        .period = rmt_period,
    };

    // route RMT signal to PCNT control
    // https://github.com/espressif/esp-idf/blob/master/tools/unit-test-app/components/unity/ref_clock.c
//    int pcnt_sig_idx = (PCNT_UNIT < 5) ?
//            PCNT_SIG_CH0_IN0_IDX + 4 * PCNT_UNIT :
//            PCNT_SIG_CH0_IN5_IDX + 4 * (PCNT_UNIT - 5);
//    gpio_matrix_in(GPIO_RMT, pcnt_sig_idx, false);
//    if (GPIO_RMT != 20) {
//        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[GPIO_RMT]);
//    } else {
//        PIN_INPUT_ENABLE(PERIPHS_IO_MUX_GPIO20_U);
//    }


    xTaskCreate(rmt_tx_task, "rmt_tx_task", 4096, &rmt_tx_task_args, 10, NULL);


    // set up counter
    pcnt_config_t pcnt_config = {
        .pulse_gpio_num = GPIO_FREQ_SIGNAL,
        .ctrl_gpio_num = GPIO_RMT,
        .channel = PCNT_CHANNEL_0,
        .unit = PCNT_UNIT,
        .pos_mode = PCNT_COUNT_INC,  // count both rising and falling edges
        .neg_mode = PCNT_COUNT_INC,
        .lctrl_mode = PCNT_MODE_DISABLE,
        .hctrl_mode = PCNT_MODE_KEEP,
        .counter_h_lim = 0,
        .counter_l_lim = 0,
    };

    pcnt_unit_config(&pcnt_config);

    // set the GPIO back to high-impedance, as pcnt_unit_config sets it as pull-up
    gpio_set_pull_mode(GPIO_FREQ_SIGNAL, GPIO_FLOATING);

    // enable counter filter - at 80MHz APB CLK, 1000 pulses is max 80,000 Hz, so ignore pulses less than 12.5 us.
    pcnt_set_filter_value(PCNT_UNIT, FILTER_LENGTH);
    pcnt_filter_enable(PCNT_UNIT);

    pcnt_counter_pause(PCNT_UNIT);
    pcnt_counter_clear(PCNT_UNIT);

    //xTaskCreate(read_counter_task, "read_counter_task", 2048, NULL, 10, NULL);

    pcnt_counter_resume(PCNT_UNIT);


    ESP_LOGI(TAG, "[APP] Idle.");
    while(1) ;
}
