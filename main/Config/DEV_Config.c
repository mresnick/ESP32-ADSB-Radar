/*****************************************************************************
* | File      	:   DEV_Config.c
* | Author      :   Waveshare team
* | Function    :   Hardware underlying interface (ESP32 / ESP-IDF port)
* | Info        :
*                Used to shield the underlying layers of each master
*                and enhance portability. Ported from the RaspberryPi/c
*                DEV_Config to use ESP-IDF's gpio, spi_master and ledc
*                drivers instead of bcm2835/wiringPi/lgpio.
*----------------
* |	This version:   V1.0 (ESP32 port)
*
******************************************************************************/
#include "DEV_Config.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_10_BIT   // duty range 0-1023, matches LCD_SetBacklight(1023) call in the demo
#define LEDC_FREQUENCY          5000

static spi_device_handle_t lcd_spi;

/*****************************************
                GPIO
*****************************************/
void DEV_GPIO_Mode(UWORD Pin, UWORD Mode)
{
    gpio_set_direction((gpio_num_t)Pin, Mode ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
}

void DEV_Digital_Write(UWORD Pin, UBYTE Value)
{
    // GPIO25 (LCD_BL) is owned by the LEDC peripheral once DEV_ModuleInit runs;
    // a raw gpio_set_level here would fight the PWM output, so route it through
    // DEV_SetBacklight instead. LCD_1IN28_Init() calls LCD_1IN28_BL_1 (this path)
    // before the demo later calls LCD_SetBacklight(1023) directly.
    if (Pin == LCD_BL) {
        DEV_SetBacklight(Value ? 1023 : 0);
        return;
    }
    gpio_set_level((gpio_num_t)Pin, Value);
}

UBYTE DEV_Digital_Read(UWORD Pin)
{
    return gpio_get_level((gpio_num_t)Pin);
}

/**
 * delay x ms
**/
void DEV_Delay_ms(UDOUBLE xms)
{
    if (xms == 0) {
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(xms));
}

/**
 * PWM backlight
**/
void DEV_SetBacklight(UWORD Value)
{
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, Value);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

/*****************************************
                SPI
*****************************************/
void DEV_SPI_WriteByte(UBYTE Value)
{
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &Value;
    spi_device_polling_transmit(lcd_spi, &t);
}

void DEV_SPI_Write_nByte(uint8_t *pData, uint32_t Len)
{
    if (Len == 0) {
        return;
    }
    spi_transaction_t t = {0};
    t.length = Len * 8;
    t.tx_buffer = pData;
    spi_device_polling_transmit(lcd_spi, &t);
}

static void DEV_GPIO_Init(void)
{
    DEV_GPIO_Mode(LCD_DC, 1);
    DEV_GPIO_Mode(LCD_RST, 1);
    gpio_set_level((gpio_num_t)LCD_DC, 1);
    gpio_set_level((gpio_num_t)LCD_RST, 1);
}

static void DEV_LEDC_Init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LCD_BL,
        .duty           = 0,
        .hpoint         = 0,
    };
    ledc_channel_config(&ledc_channel);
}

static UBYTE DEV_SPI_Init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = LCD_MOSI,
        .miso_io_num = -1,   // LCD is write-only, no MISO connection
        .sclk_io_num = LCD_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        DEBUG("spi_bus_initialize failed: %d\r\n", ret);
        return 1;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 40 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = LCD_CS,
        .queue_size = 1,
    };
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &lcd_spi);
    if (ret != ESP_OK) {
        DEBUG("spi_bus_add_device failed: %d\r\n", ret);
        return 1;
    }

    return 0;
}

/******************************************************************************
function:	Module Initialize, initialize GPIO, SPI bus and backlight PWM
parameter:
Info:
******************************************************************************/
UBYTE DEV_ModuleInit(void)
{
    DEV_GPIO_Init();
    DEV_LEDC_Init();
    if (DEV_SPI_Init() != 0) {
        return 1;
    }
    return 0;
}

/******************************************************************************
function:	Module exits, frees the SPI device/bus
parameter:
Info:
******************************************************************************/
void DEV_ModuleExit(void)
{
    spi_bus_remove_device(lcd_spi);
    spi_bus_free(SPI3_HOST);
}
