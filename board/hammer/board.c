/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/* Hammer board configuration */

#include "common.h"
#include "ec_version.h"
#include "touchpad_elan.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "keyboard_raw.h"
#include "pwm.h"
#include "pwm_chip.h"
#include "registers.h"
#include "task.h"
#include "timer.h"
#include "update_fw.h"
#include "usart-stm32f0.h"
#include "usart_tx_dma.h"
#include "usart_rx_dma.h"
#include "usb_descriptor.h"
#include "usb_i2c.h"
#include "util.h"

#include "gpio_list.h"

/******************************************************************************
 * Define the strings used in our USB descriptors.
 */
const void *const usb_strings[] = {
	[USB_STR_DESC]         = usb_string_desc,
	[USB_STR_VENDOR]       = USB_STRING_DESC("Google Inc."),
	[USB_STR_PRODUCT]      = USB_STRING_DESC("Hammer"),
	[USB_STR_SERIALNO]     = 0,
	[USB_STR_VERSION]      = USB_STRING_DESC(CROS_EC_VERSION32),
	[USB_STR_I2C_NAME]     = USB_STRING_DESC("I2C"),
	[USB_STR_UPDATE_NAME]  = USB_STRING_DESC("Firmware update"),
};

BUILD_ASSERT(ARRAY_SIZE(usb_strings) == USB_STR_COUNT);

/******************************************************************************
 * Support I2C bridging over USB, this requires usb_i2c_board_enable and
 * usb_i2c_board_disable to be defined to enable and disable the I2C bridge.
 */

#ifdef SECTION_IS_RW
/* I2C ports */
const struct i2c_port_t i2c_ports[] = {
	{"master", I2C_PORT_MASTER, 400,
		GPIO_MASTER_I2C_SCL, GPIO_MASTER_I2C_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

#ifdef BOARD_STAFF
#define KBLIGHT_PWM_FREQ 100 /* Hz */
#else
#define KBLIGHT_PWM_FREQ 10000 /* Hz */
#endif

/* PWM channels. Must be in the exactly same order as in enum pwm_channel. */
const struct pwm_t pwm_channels[] = {
	{STM32_TIM(TIM_KBLIGHT), STM32_TIM_CH(1), 0, KBLIGHT_PWM_FREQ},
};
BUILD_ASSERT(ARRAY_SIZE(pwm_channels) == PWM_CH_COUNT);

int usb_i2c_board_enable(void) { return EC_SUCCESS; }
void usb_i2c_board_disable(void) {}
#endif

/******************************************************************************
 * Initialize board.
 */
static void board_init(void)
{

}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

void board_config_pre_init(void)
{
	/* enable SYSCFG clock */
	STM32_RCC_APB2ENR |= 1 << 0;

	/* Remap USART DMA to match the USART driver */
	/*
	 * the DMA mapping is :
	 *  Chan 4 : USART1_TX
	 *  Chan 5 : USART1_RX
	 */
	STM32_SYSCFG_CFGR1 |= (1 << 9) | (1 << 10); /* Remap USART1 RX/TX DMA */
}

/*
 * Side-band USB wake, to be able to wake lid even in deep S3, when USB
 * controller is off.
 */
void board_usb_wake(void)
{
	/*
	 * Poke detection pin for less than 100us, we disable interrupts
	 * to make sure that we do not get preempted (setting GPIO high
	 * for too long would prevent pulse detection on lid EC side from
	 * working properly, or even kill hammer power if it is held for
	 * longer than debounce time).
	 */
	interrupt_disable();
	gpio_set_flags(GPIO_BASE_DET, GPIO_OUT_HIGH);
	udelay(100);
	gpio_set_flags(GPIO_BASE_DET, GPIO_INPUT);
	interrupt_enable();
}
