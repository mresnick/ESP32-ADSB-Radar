/*****************************************************************************
* | File      	:   DEV_Config.h
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
#ifndef _DEV_CONFIG_H_
#define _DEV_CONFIG_H_

#include "Debug.h"

#include <stdint.h>
#include <stdio.h>

/**
 * data
**/
#define UBYTE   uint8_t
#define UWORD   uint16_t
#define UDOUBLE uint32_t

/**
 * GPIO config (generic ESP32-WROOM DevKitC)
**/
#define LCD_CS   5   // VSPI default CS0 - managed automatically by the SPI driver, never bit-banged
#define LCD_RST  26
#define LCD_DC   27
#define LCD_BL   25  // driven via LEDC PWM, not a plain GPIO

// SPI bus pins
#define LCD_SCLK 18
#define LCD_MOSI 23

//LCD
#define LCD_CS_0		DEV_Digital_Write(LCD_CS,0)
#define LCD_CS_1		DEV_Digital_Write(LCD_CS,1)

#define LCD_RST_0		DEV_Digital_Write(LCD_RST,0)
#define LCD_RST_1		DEV_Digital_Write(LCD_RST,1)

#define LCD_DC_0		DEV_Digital_Write(LCD_DC,0)
#define LCD_DC_1		DEV_Digital_Write(LCD_DC,1)

#define LCD_BL_0		DEV_Digital_Write(LCD_BL,0)
#define LCD_BL_1		DEV_Digital_Write(LCD_BL,1)

#define LCD_SetBacklight(Value) DEV_SetBacklight(Value)

/*------------------------------------------------------------------------------------------------------*/
UBYTE DEV_ModuleInit(void);
void DEV_ModuleExit(void);

void DEV_GPIO_Mode(UWORD Pin, UWORD Mode);
void DEV_Digital_Write(UWORD Pin, UBYTE Value);
UBYTE DEV_Digital_Read(UWORD Pin);
void DEV_Delay_ms(UDOUBLE xms);

void DEV_SPI_WriteByte(UBYTE Value);
void DEV_SPI_Write_nByte(uint8_t *pData, uint32_t Len);
void DEV_SetBacklight(UWORD Value);
#endif
