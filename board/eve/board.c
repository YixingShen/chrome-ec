/* Copyright 2016 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Eve board-specific configuration */

#include "adc_chip.h"
#include "bd99992gw.h"
#include "board_config.h"
#include "button.h"
#include "charge_manager.h"
#include "charge_state.h"
#include "charge_ramp.h"
#include "charger.h"
#include "chipset.h"
#include "console.h"
#include "driver/accel_kionix.h"
#include "driver/accel_kxcj9.h"
#include "driver/accelgyro_bmi160.h"
#include "driver/als_si114x.h"
#include "driver/charger/bd9995x.h"
#include "driver/tcpm/anx74xx.h"
#include "driver/tcpm/tcpci.h"
#include "driver/tcpm/tcpm.h"
#include "driver/temp_sensor/bd99992gw.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "keyboard_scan.h"
#include "lid_angle.h"
#include "lid_switch.h"
#include "math_util.h"
#include "motion_lid.h"
#include "motion_sense.h"
#include "power.h"
#include "power_button.h"
#include "pwm.h"
#include "pwm_chip.h"
#include "spi.h"
#include "switch.h"
#include "system.h"
#include "tablet_mode.h"
#include "task.h"
#include "temp_sensor.h"
#include "timer.h"
#include "uart.h"
#include "usb_charge.h"
#include "usb_mux.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "util.h"
#include "espi.h"

#define CPRINTS(format, args...) cprints(CC_USBCHARGE, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_USBCHARGE, format, ## args)

static void tcpc_alert_event(enum gpio_signal signal)
{
	if ((signal == GPIO_USB_C0_PD_INT_ODL) &&
	    !gpio_get_level(GPIO_USB_C0_PD_RST_L))
		return;
	else if ((signal == GPIO_USB_C1_PD_INT_ODL) &&
		 !gpio_get_level(GPIO_USB_C1_PD_RST_L))
		return;

#ifdef HAS_TASK_PDCMD
	/* Exchange status with TCPCs */
	host_command_pd_send_status(PD_CHARGE_NO_CHANGE);
#endif
}

/*
 * enable_input_devices() is called by the tablet_mode ISR, but changes the
 * state of GPIOs, so its definition must reside after including gpio_list.
 */
static void enable_input_devices(void);
DECLARE_DEFERRED(enable_input_devices);

#define LID_DEBOUNCE_US	(30 * MSEC)
void tablet_mode_interrupt(enum gpio_signal signal)
{
	hook_call_deferred(&enable_input_devices_data, LID_DEBOUNCE_US);
}

/* Send event to wake AP based on trackpad input */
void trackpad_interrupt(enum gpio_signal signal)
{
	/* TODO(b/36024430): Use device specific wake event */
	host_set_single_event(EC_HOST_EVENT_KEY_PRESSED);
}

#ifdef CONFIG_USB_PD_TCPC_LOW_POWER
static void anx74xx_c0_cable_det_handler(void)
{
	int cable_det = gpio_get_level(GPIO_USB_C0_CABLE_DET);
	int reset_n = gpio_get_level(GPIO_USB_C0_PD_RST_L);

	/*
	 * A cable_det low->high transition was detected. If following the
	 * debounce time, cable_det is high, and reset_n is low, then ANX3429 is
	 * currently in standby mode and needs to be woken up. Set the
	 * TCPC_RESET event which will bring the ANX3429 out of standby
	 * mode. Setting this event is gated on reset_n being low because the
	 * ANX3429 will always set cable_det when transitioning to normal mode
	 * and if in normal mode, then there is no need to trigger a tcpc reset.
	 */
	if (cable_det && !reset_n)
		task_set_event(TASK_ID_PD_C0, PD_EVENT_TCPC_RESET, 0);
}
DECLARE_DEFERRED(anx74xx_c0_cable_det_handler);

static void anx74xx_c1_cable_det_handler(void)
{
	int cable_det = gpio_get_level(GPIO_USB_C1_CABLE_DET);
	int reset_n = gpio_get_level(GPIO_USB_C1_PD_RST_L);

	/*
	 * A cable_det low->high transition was detected. If following the
	 * debounce time, cable_det is high, and reset_n is low, then ANX3429 is
	 * currently in standby mode and needs to be woken up. Set the
	 * TCPC_RESET event which will bring the ANX3429 out of standby
	 * mode. Setting this event is gated on reset_n being low because the
	 * ANX3429 will always set cable_det when transitioning to normal mode
	 * and if in normal mode, then there is no need to trigger a tcpc reset.
	 */
	if (cable_det && !reset_n)
		task_set_event(TASK_ID_PD_C1, PD_EVENT_TCPC_RESET, 0);
}
DECLARE_DEFERRED(anx74xx_c1_cable_det_handler);

void anx74xx_cable_det_interrupt(enum gpio_signal signal)
{
	/* Check if it is port 0 or 1, and debounce for 2 msec. */
	if (signal == GPIO_USB_C0_CABLE_DET)
		hook_call_deferred(&anx74xx_c0_cable_det_handler_data,
				   (2 * MSEC));
	else
		hook_call_deferred(&anx74xx_c1_cable_det_handler_data,
				   (2 * MSEC));
}
#endif

#include "gpio_list.h"

/* power signal list.  Must match order of enum power_signal. */
const struct power_signal_info power_signal_list[] = {
	{GPIO_PCH_SLP_S0_L,	1, "SLP_S0_DEASSERTED"},
	{VW_SLP_S3_L,		1, "SLP_S3_DEASSERTED"},
	{VW_SLP_S4_L,		1, "SLP_S4_DEASSERTED"},
	{GPIO_PCH_SLP_SUS_L,	1, "SLP_SUS_DEASSERTED"},
	{GPIO_RSMRST_L_PGOOD,	1, "RSMRST_L_PGOOD"},
	{GPIO_PMIC_DPWROK,	1, "PMIC_DPWROK"},
};
BUILD_ASSERT(ARRAY_SIZE(power_signal_list) == POWER_SIGNAL_COUNT);

/* Keyboard scan. Increase output_settle_us to 80us from default 50us. */
struct keyboard_scan_config keyscan_config = {
	.output_settle_us = 80,
	.debounce_down_us = 9 * MSEC,
	.debounce_up_us = 30 * MSEC,
	.scan_period_us = 3 * MSEC,
	.min_post_scan_delay_us = 1000,
	.poll_timeout_us = 100 * MSEC,
	.actual_key_mask = {
		0x14, 0xff, 0xff, 0xff, 0xff, 0xf5, 0xff,
		0xa4, 0xff, 0xfe, 0x55, 0xfa, 0xca  /* full set */
	},
};

/* PWM channels. Must be in the exactly same order as in enum pwm_channel. */
const struct pwm_t pwm_channels[] = {
	[PWM_CH_KBLIGHT]     = { 5, 0, 10000 },
	[PWM_CH_LED_L_RED]   = { 2, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_LED_L_GREEN] = { 3, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_LED_L_BLUE]  = { 4, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_LED_R_RED]   = { 1, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_LED_R_GREEN] = { 0, PWM_CONFIG_DSLEEP, 100 },
	[PWM_CH_LED_R_BLUE]  = { 6, PWM_CONFIG_DSLEEP, 100 },
};
BUILD_ASSERT(ARRAY_SIZE(pwm_channels) == PWM_CH_COUNT);

/* Hibernate wake configuration */
const enum gpio_signal hibernate_wake_pins[] = {
	GPIO_AC_PRESENT,
	GPIO_LID_OPEN,
	GPIO_POWER_BUTTON_L,
};
const int hibernate_wake_pins_used = ARRAY_SIZE(hibernate_wake_pins);

/* I2C port map */
const struct i2c_port_t i2c_ports[]  = {
	{"tcpc0",     I2C_PORT_TCPC0,    400, GPIO_I2C0_0_SCL, GPIO_I2C0_0_SDA},
	{"tcpc1",     I2C_PORT_TCPC1,    400, GPIO_I2C0_1_SCL, GPIO_I2C0_1_SDA},
	{"accelgyro", I2C_PORT_GYRO,     400, GPIO_I2C1_SCL,   GPIO_I2C1_SDA},
	{"sensors",   I2C_PORT_LID_ACCEL, 400, GPIO_I2C2_SCL,  GPIO_I2C2_SDA},
	{"batt",      I2C_PORT_BATTERY,  100, GPIO_I2C3_SCL,   GPIO_I2C3_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

/* TCPC mux configuration */
const struct tcpc_config_t tcpc_config[CONFIG_USB_PD_PORT_COUNT] = {
	{I2C_PORT_TCPC0, 0x50, &anx74xx_tcpm_drv, TCPC_ALERT_ACTIVE_LOW},
	{I2C_PORT_TCPC1, 0x50, &anx74xx_tcpm_drv, TCPC_ALERT_ACTIVE_LOW},
};

struct usb_mux usb_muxes[CONFIG_USB_PD_PORT_COUNT] = {
	{
		.port_addr = 0,
		.driver = &anx74xx_tcpm_usb_mux_driver,
		.hpd_update = &anx74xx_tcpc_update_hpd_status,
	},
	{
		.port_addr = 1,
		.driver = &anx74xx_tcpm_usb_mux_driver,
		.hpd_update = &anx74xx_tcpc_update_hpd_status,
	},
};

/* called from anx74xx_set_power_mode() */
void board_set_tcpc_power_mode(int port, int mode)
{
	switch (port) {
	case 0:
		if (mode) {
			gpio_set_level(GPIO_USB_C0_TCPC_PWR, 1);
			msleep(10);
			gpio_set_level(GPIO_USB_C0_PD_RST_L, 1);
		} else {
			gpio_set_level(GPIO_USB_C0_PD_RST_L, 0);
			msleep(1);
			gpio_set_level(GPIO_USB_C0_TCPC_PWR, 0);
		}
		break;
	case 1:
		if (mode) {
			gpio_set_level(GPIO_USB_C1_TCPC_PWR, 1);
			msleep(10);
			gpio_set_level(GPIO_USB_C1_PD_RST_L, 1);
		} else {
			gpio_set_level(GPIO_USB_C1_PD_RST_L, 0);
			msleep(1);
			gpio_set_level(GPIO_USB_C1_TCPC_PWR, 0);
		}
		break;
	}
}

void board_reset_pd_mcu(void)
{
	/* Assert reset */
	gpio_set_level(GPIO_USB_C0_PD_RST_L, 0);
	gpio_set_level(GPIO_USB_C1_PD_RST_L, 0);
	msleep(1);
	/* Disable power */
	gpio_set_level(GPIO_USB_C0_TCPC_PWR, 0);
	gpio_set_level(GPIO_USB_C1_TCPC_PWR, 0);
	msleep(10);
	/* Enable power */
	gpio_set_level(GPIO_USB_C0_TCPC_PWR, 1);
	gpio_set_level(GPIO_USB_C1_TCPC_PWR, 1);
	msleep(10);
	/* Deassert reset */
	gpio_set_level(GPIO_USB_C0_PD_RST_L, 1);
	gpio_set_level(GPIO_USB_C1_PD_RST_L, 1);
}

void board_tcpc_init(void)
{
	int port;

	/* Only reset TCPC if not sysjump */
	if (!system_jumped_to_this_image())
		board_reset_pd_mcu();

	/* Enable TCPC interrupts */
	gpio_enable_interrupt(GPIO_USB_C0_PD_INT_ODL);
	gpio_enable_interrupt(GPIO_USB_C1_PD_INT_ODL);

#ifdef CONFIG_USB_PD_TCPC_LOW_POWER
	/* Enable CABLE_DET interrupt for ANX3429 wake from standby */
	gpio_enable_interrupt(GPIO_USB_C0_CABLE_DET);
	gpio_enable_interrupt(GPIO_USB_C1_CABLE_DET);
#endif

	/*
	 * Initialize HPD to low; after sysjump SOC needs to see
	 * HPD pulse to enable video path
	 */
	for (port = 0; port < CONFIG_USB_PD_PORT_COUNT; port++) {
		const struct usb_mux *mux = &usb_muxes[port];

		mux->hpd_update(port, 0, 0);
	}
}
DECLARE_HOOK(HOOK_INIT, board_tcpc_init, HOOK_PRIO_INIT_I2C+1);

uint16_t tcpc_get_alert_status(void)
{
	uint16_t status = 0;

	if (!gpio_get_level(GPIO_USB_C0_PD_INT_ODL)) {
		if (gpio_get_level(GPIO_USB_C0_PD_RST_L))
			status |= PD_STATUS_TCPC_ALERT_0;
	}

	if (!gpio_get_level(GPIO_USB_C1_PD_INT_ODL)) {
		if (gpio_get_level(GPIO_USB_C1_PD_RST_L))
			status |= PD_STATUS_TCPC_ALERT_1;
	}

	return status;
}

const struct temp_sensor_t temp_sensors[] = {
	{"Battery", TEMP_SENSOR_TYPE_BATTERY, charge_get_battery_temp, 0, 4},

	/* These BD99992GW temp sensors are only readable in S0 */
	{"Ambient", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM0, 4},
	{"Charger", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM1, 4},
	{"DRAM", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM2, 4},
	{"eMMC", TEMP_SENSOR_TYPE_BOARD, bd99992gw_get_val,
	 BD99992GW_ADC_CHANNEL_SYSTHERM3, 4},
	{"Gyro", TEMP_SENSOR_TYPE_BOARD, bmi160_get_sensor_temp, BASE_GYRO, 1},
};
BUILD_ASSERT(ARRAY_SIZE(temp_sensors) == TEMP_SENSOR_COUNT);

const struct button_config buttons[CONFIG_BUTTON_COUNT] = {
	{"Volume Down", KEYBOARD_BUTTON_VOLUME_DOWN, GPIO_VOLUME_DOWN_L,
	 30 * MSEC, 0},
	{"Volume Up", KEYBOARD_BUTTON_VOLUME_UP, GPIO_VOLUME_UP_L,
	 30 * MSEC, 0},
};

static void board_pmic_init(void)
{
	if (system_jumped_to_this_image())
		return;

	/* DISCHGCNT3 - enable 100 ohm discharge on V1.00A */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992, 0x3e, 0x04);

	/* Set CSDECAYEN / VCCIO decays to 0V at assertion of SLP_S0# */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992, 0x30, 0x4a);

	/*
	 * Set V100ACNT / V1.00A Control Register:
	 * Nominal output = 1.0V.
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992, 0x37, 0x1a);

	/*
	 * Set V085ACNT / V0.85A Control Register:
	 * Lower power mode = 0.7V.
	 * Nominal output = 1.0V.
	 */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992, 0x38, 0x7a);

	/* VRMODECTRL - disable low-power mode for all rails */
	i2c_write8(I2C_PORT_PMIC, I2C_ADDR_BD99992, 0x3b, 0x1f);
}
DECLARE_HOOK(HOOK_INIT, board_pmic_init, HOOK_PRIO_DEFAULT);

static void board_set_tablet_mode(void)
{
	tablet_set_mode(!gpio_get_level(GPIO_TABLET_MODE_L));
}

/* Initialize board. */
static void board_init(void)
{
	/* Enabure tablet mode is initialized */
	board_set_tablet_mode();

	/* Enable tablet mode interrupt for input device enable */
	gpio_enable_interrupt(GPIO_TABLET_MODE_L);

	/* Enable charger interrupts */
	gpio_enable_interrupt(GPIO_CHARGER_INT_L);

	/* Provide AC status to the PCH */
	gpio_set_level(GPIO_PCH_ACOK, extpower_is_present());
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

/**
 * Buffer the AC present GPIO to the PCH.
 */
static void board_extpower(void)
{
	gpio_set_level(GPIO_PCH_ACOK, extpower_is_present());
}
DECLARE_HOOK(HOOK_AC_CHANGE, board_extpower, HOOK_PRIO_DEFAULT);

int pd_snk_is_vbus_provided(int port)
{
	enum bd9995x_charge_port bd9995x_port;

	switch (port) {
	case 0:
	case 1:
		bd9995x_port = bd9995x_pd_port_to_chg_port(port);
		break;
	default:
		panic("Invalid charge port\n");
		break;
	}

	return bd9995x_is_vbus_provided(bd9995x_port);
}

/**
 * Set active charge port -- only one port can be active at a time.
 *
 * @param charge_port   Charge port to enable.
 *
 * Returns EC_SUCCESS if charge port is accepted and made active,
 * EC_ERROR_* otherwise.
 */
int board_set_active_charge_port(int charge_port)
{
	enum bd9995x_charge_port bd9995x_port;
	int bd9995x_port_select = 1;
	static int initialized;

	/*
	 * Reject charge port disable if our battery is critical and we
	 * have yet to initialize a charge port - continue to charge using
	 * charger ROM / POR settings.
	 */
	if (!initialized &&
	    charge_port == CHARGE_PORT_NONE &&
	    charge_get_percent() < 2)
		return -1;

	switch (charge_port) {
	case 0:
	case 1:
		/* Don't charge from a source port */
		if (board_vbus_source_enabled(charge_port))
			return -1;

		bd9995x_port = bd9995x_pd_port_to_chg_port(charge_port);
		break;
	case CHARGE_PORT_NONE:
		bd9995x_port_select = 0;
		bd9995x_port = BD9995X_CHARGE_PORT_BOTH;

		/*
		 * To avoid inrush current from the external charger,
		 * enable discharge on AC until the new charger is detected
		 * and charge detect delay has passed.
		 */
		if (charge_get_percent() > 2)
			charger_discharge_on_ac(1);
		break;
	default:
		panic("Invalid charge port\n");
		break;
	}

	CPRINTS("New chg p%d", charge_port);
	initialized = 1;

	return bd9995x_select_input_port(bd9995x_port, bd9995x_port_select);
}

/**
 * Set the charge limit based upon desired maximum.
 *
 * @param port          Port number.
 * @param supplier      Charge supplier type.
 * @param charge_ma     Desired charge limit (mA).
 * @param charge_mv     Negotiated charge voltage (mV).
 */
void board_set_charge_limit(int port, int supplier, int charge_ma,
			    int max_ma, int charge_mv)
{
	/* Enable charging trigger by BC1.2 detection */
	int bc12_enable = (supplier == CHARGE_SUPPLIER_BC12_CDP ||
			   supplier == CHARGE_SUPPLIER_BC12_DCP ||
			   supplier == CHARGE_SUPPLIER_BC12_SDP ||
			   supplier == CHARGE_SUPPLIER_OTHER);

	if (bd9995x_bc12_enable_charging(port, bc12_enable))
		return;

	charge_ma = (charge_ma * 95) / 100;
	charge_set_input_current_limit(MAX(charge_ma,
				   CONFIG_CHARGER_INPUT_CURRENT), charge_mv);
}

/**
 * Return whether ramping is allowed for given supplier
 */
int board_is_ramp_allowed(int supplier)
{
	/* Don't allow ramping in RO when write protected */
	if (system_get_image_copy() != SYSTEM_IMAGE_RW
		&& system_is_locked())
		return 0;
	else
		return (supplier == CHARGE_SUPPLIER_BC12_DCP ||
			supplier == CHARGE_SUPPLIER_BC12_SDP ||
			supplier == CHARGE_SUPPLIER_BC12_CDP ||
			supplier == CHARGE_SUPPLIER_OTHER);
}

/**
 * Return the maximum allowed input current
 */
int board_get_ramp_current_limit(int supplier, int sup_curr)
{
	return bd9995x_get_bc12_ilim(supplier);
}

/**
 * Return if board is consuming full amount of input current
 */
int board_is_consuming_full_charge(void)
{
	int chg_perc = charge_get_percent();

	return chg_perc > 2 && chg_perc < 95;
}

/**
 * Return if VBUS is sagging too low
 */
int board_is_vbus_too_low(int port, enum chg_ramp_vbus_state ramp_state)
{
	return charger_get_vbus_voltage(port) < BD9995X_BC12_MIN_VOLTAGE;
}

/* Clear pending interrupts and enable trackpad for wake */
static void trackpad_wake_enable(int enable)
{
	static int prev_enable = -1;

	if (prev_enable == enable)
		return;
	prev_enable = enable;

	if (enable) {
		gpio_clear_pending_interrupt(GPIO_TRACKPAD_INT_L);
		gpio_enable_interrupt(GPIO_TRACKPAD_INT_L);
	} else {
		gpio_disable_interrupt(GPIO_TRACKPAD_INT_L);
	}
}

/* Enable or disable input devices, based upon chipset state and tablet mode */
static void enable_input_devices(void)
{
	/* We need to turn on tablet mode for motion sense */
	board_set_tablet_mode();

	/*
	 * Then, we disable peripherals only when the lid reaches 360 position.
	 * (It's probably already disabled by motion_sense_task.)
	 * We deliberately do not enable peripherals when the lid is leaving
	 * 360 position. Instead, we let motion_sense_task enable it once it
	 * reaches laptop zone (180 or less).
	 */
	if (tablet_get_mode())
		lid_angle_peripheral_enable(0);
}

/* Enable or disable input devices, based on chipset state and tablet mode */
#ifndef TEST_BUILD
void lid_angle_peripheral_enable(int enable)
{
	/*
	 * If the lid is in 360 position, ignore the lid angle,
	 * which might be faulty. Disable keyboard and trackpad wake.
	 */
	if (tablet_get_mode() || chipset_in_state(CHIPSET_STATE_ANY_OFF))
		enable = 0;
	keyboard_scan_enable(enable, KB_SCAN_DISABLE_LID_ANGLE);

	/* Also disable trackpad wake if not in suspend */
	if (!chipset_in_state(CHIPSET_STATE_SUSPEND))
		enable = 0;
	trackpad_wake_enable(enable);
}
#endif

/* Called on AP S5 -> S3 transition */
static void board_chipset_startup(void)
{
	/* Enable Trackpad */
	gpio_set_level(GPIO_TRACKPAD_SHDN_L, 1);
	hook_call_deferred(&enable_input_devices_data, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_STARTUP, board_chipset_startup, HOOK_PRIO_DEFAULT);

/* Called on AP S3 -> S5 transition */
static void board_chipset_shutdown(void)
{
	/* Disable Trackpad */
	trackpad_wake_enable(0);
	gpio_set_level(GPIO_TRACKPAD_SHDN_L, 0);
	hook_call_deferred(&enable_input_devices_data, 0);
}
DECLARE_HOOK(HOOK_CHIPSET_SHUTDOWN, board_chipset_shutdown, HOOK_PRIO_DEFAULT);

/* Called on AP S0 -> S3 transition */
static void board_chipset_suspend(void)
{
	gpio_set_level(GPIO_ENABLE_BACKLIGHT, 0);
	if (!tablet_get_mode() && lid_is_open())
		trackpad_wake_enable(1);
}
DECLARE_HOOK(HOOK_CHIPSET_SUSPEND, board_chipset_suspend, HOOK_PRIO_DEFAULT);

/* Called on AP S3 -> S0 transition */
static void board_chipset_resume(void)
{
	gpio_set_level(GPIO_ENABLE_BACKLIGHT, 1);
	trackpad_wake_enable(0);
}
DECLARE_HOOK(HOOK_CHIPSET_RESUME, board_chipset_resume, HOOK_PRIO_DEFAULT);

/* Called on lid change */
static void board_lid_change(void)
{
	if (!lid_is_open())
		trackpad_wake_enable(0);
}
DECLARE_HOOK(HOOK_LID_CHANGE, board_lid_change, HOOK_PRIO_DEFAULT);

void board_hibernate_late(void)
{
	int i;
	const uint32_t hibernate_pins[][2] = {
		{GPIO_LID_OPEN, GPIO_INT_RISING},
		/*
		 * BD99956 handles charge input automatically. We'll disable
		 * charge output in hibernate. Charger will assert ACOK_OD
		 * when VBUS or VCC are plugged in.
		 */
		{GPIO_USB_C0_5V_EN, GPIO_INPUT | GPIO_PULL_DOWN},
		{GPIO_USB_C1_5V_EN, GPIO_INPUT | GPIO_PULL_DOWN},
	};

	/* Change GPIOs' state in hibernate for better power consumption */
	for (i = 0; i < ARRAY_SIZE(hibernate_pins); ++i)
		gpio_set_flags(hibernate_pins[i][0], hibernate_pins[i][1]);

	gpio_config_module(MODULE_KEYBOARD_SCAN, 0);

	/*
	 * Calling gpio_config_module sets disabled alternate function pins to
	 * GPIO_INPUT.  But to prevent keypresses causing leakage currents
	 * while hibernating we want to enable GPIO_PULL_UP as well.
	 */
	gpio_set_flags_by_mask(0x2, 0x03, GPIO_INPUT | GPIO_PULL_UP);
	gpio_set_flags_by_mask(0x1, 0x7F, GPIO_INPUT | GPIO_PULL_UP);
	gpio_set_flags_by_mask(0x0, 0xE0, GPIO_INPUT | GPIO_PULL_UP);
	/* KBD_KS02 needs to have a pull-down enabled to match cr50 */
	gpio_set_flags_by_mask(0x1, 0x80, GPIO_INPUT | GPIO_PULL_DOWN);
}

void board_hibernate(void)
{
	/* Enable both the VBUS & VCC ports before entering PG3 */
	bd9995x_select_input_port(BD9995X_CHARGE_PORT_BOTH, 1);

	/* Turn BGATE OFF for power saving */
	bd9995x_set_power_save_mode(BD9995X_PWR_SAVE_MAX);
}

static int gpio_get_ternary(enum gpio_signal gpio)
{
	int pd, pu;
	int flags = gpio_get_default_flags(gpio);

	/* Read GPIO with internal pull-down */
	gpio_set_flags(gpio, GPIO_INPUT | GPIO_PULL_DOWN);
	pd = gpio_get_level(gpio);
	usleep(100);

	/* Read GPIO with internal pull-up */
	gpio_set_flags(gpio, GPIO_INPUT | GPIO_PULL_UP);
	pu = gpio_get_level(gpio);
	usleep(100);

	/* Reset GPIO flags */
	gpio_set_flags(gpio, flags);

	/* Check PU and PD readings to determine tristate */
	return pu && !pd ? 2 : pd;
}

int board_get_version(void)
{
	static int ver;

	if (!ver) {
		/*
		 * Read the board EC ID on the tristate strappings
		 * using ternary encoding: 0 = 0, 1 = 1, Hi-Z = 2
		 */
		uint8_t id0, id1, id2;

		id0 = gpio_get_ternary(GPIO_BOARD_VERSION1);
		id1 = gpio_get_ternary(GPIO_BOARD_VERSION2);
		id2 = gpio_get_ternary(GPIO_BOARD_VERSION3);

		ver = (id2 * 9) + (id1 * 3) + id0;
		CPRINTS("Board ID = %d", ver);
	}

	return ver;
}

/* Base Sensor mutex */
static struct mutex g_base_mutex;

/* Lid Sensor mutex */
static struct mutex g_lid_mutex;

struct kionix_accel_data g_kxcj9_data;
struct bmi160_drv_data_t g_bmi160_data;

/* Matrix to rotate accelrator into standard reference frame */
const matrix_3x3_t mag_standard_ref = {
	{ FLOAT_TO_FP(-1), 0, 0},
	{ 0,  FLOAT_TO_FP(1), 0},
	{ 0, 0, FLOAT_TO_FP(-1)}
};

const matrix_3x3_t lid_standard_ref = {
	{FLOAT_TO_FP(-1),  0,  0},
	{ 0,  FLOAT_TO_FP(-1), 0},
	{ 0,  0, FLOAT_TO_FP(1)}
};

struct motion_sensor_t motion_sensors[] = {

	[LID_ACCEL] = {
	 .name = "Lid Accel",
	 .active_mask = SENSOR_ACTIVE_S0_S3,
	 .chip = MOTIONSENSE_CHIP_KXCJ9,
	 .type = MOTIONSENSE_TYPE_ACCEL,
	 .location = MOTIONSENSE_LOC_LID,
	 .drv = &kionix_accel_drv,
	 .mutex = &g_lid_mutex,
	 .drv_data = &g_kxcj9_data,
	 .port = I2C_PORT_LID_ACCEL,
	 .addr = KXCJ9_ADDR0,
	 .rot_standard_ref = &lid_standard_ref,
	 .default_range = 2, /* g, enough for laptop. */
	 .config = {
		/* AP: by default use EC settings */
		[SENSOR_CONFIG_AP] = {
			.odr = 0,
			.ec_rate = 0,
		},
		/* EC use accel for angle detection */
		[SENSOR_CONFIG_EC_S0] = {
			.odr = 10000 | ROUND_UP_FLAG,
			.ec_rate = 0,
		},
		 /* Sensor on for lid angle detection */
		[SENSOR_CONFIG_EC_S3] = {
			.odr = 10000 | ROUND_UP_FLAG,
			.ec_rate = 0,
		},
		[SENSOR_CONFIG_EC_S5] = {
			.odr = 0,
			.ec_rate = 0,
		},
	 },
		},

	[BASE_ACCEL] = {
	 .name = "Base Accel",
	 .active_mask = SENSOR_ACTIVE_S0_S3,
	 .chip = MOTIONSENSE_CHIP_BMI160,
	 .type = MOTIONSENSE_TYPE_ACCEL,
	 .location = MOTIONSENSE_LOC_BASE,
	 .drv = &bmi160_drv,
	 .mutex = &g_base_mutex,
	 .drv_data = &g_bmi160_data,
	 .port = I2C_PORT_GYRO,
	 .addr = BMI160_ADDR0,
	 .rot_standard_ref = NULL,
	 .default_range = 2,  /* g, enough for laptop. */
	 .config = {
		 /* AP: by default use EC settings */
		 [SENSOR_CONFIG_AP] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* EC use accel for angle detection */
		 [SENSOR_CONFIG_EC_S0] = {
			.odr = 10000 | ROUND_UP_FLAG,
			.ec_rate = 100 * MSEC,
		 },
		 /* Sensor on for lid angle detection */
		 [SENSOR_CONFIG_EC_S3] = {
			.odr = 10000 | ROUND_UP_FLAG,
			.ec_rate = 100 * MSEC,
		 },
		 /* Sensor off in S5 */
		 [SENSOR_CONFIG_EC_S5] = {
			.odr = 0,
			.ec_rate = 0
		 },
	 },
	},

	[BASE_GYRO] = {
	 .name = "Base Gyro",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_BMI160,
	 .type = MOTIONSENSE_TYPE_GYRO,
	 .location = MOTIONSENSE_LOC_BASE,
	 .drv = &bmi160_drv,
	 .mutex = &g_base_mutex,
	 .drv_data = &g_bmi160_data,
	 .port = I2C_PORT_GYRO,
	 .addr = BMI160_ADDR0,
	 .default_range = 1000, /* dps */
	 .rot_standard_ref = NULL,
	 .config = {
		 /* AP: by default shutdown all sensors */
		 [SENSOR_CONFIG_AP] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* EC does not need in S0 */
		 [SENSOR_CONFIG_EC_S0] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* Sensor off in S3/S5 */
		 [SENSOR_CONFIG_EC_S3] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* Sensor off in S3/S5 */
		 [SENSOR_CONFIG_EC_S5] = {
			.odr = 0,
			.ec_rate = 0,
		 },
	 },
	},

	[BASE_MAG] = {
	 .name = "Base Mag",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_BMI160,
	 .type = MOTIONSENSE_TYPE_MAG,
	 .location = MOTIONSENSE_LOC_BASE,
	 .drv = &bmi160_drv,
	 .mutex = &g_base_mutex,
	 .drv_data = &g_bmi160_data,
	 .port = I2C_PORT_GYRO,
	 .addr = BMI160_ADDR0,
	 .default_range = 1 << 11, /* 16LSB / uT, fixed */
	 .rot_standard_ref = &mag_standard_ref,
	 .config = {
		 /* AP: by default shutdown all sensors */
		 [SENSOR_CONFIG_AP] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* EC does not need in S0 */
		 [SENSOR_CONFIG_EC_S0] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* Sensor off in S3/S5 */
		 [SENSOR_CONFIG_EC_S3] = {
			.odr = 0,
			.ec_rate = 0,
		 },
		 /* Sensor off in S3/S5 */
		 [SENSOR_CONFIG_EC_S5] = {
			 .odr = 0,
			 .ec_rate = 0,
		 },
	 },
	},

	[LID_LIGHT] = {
	 .name = "Light",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_SI1141,
	 .type = MOTIONSENSE_TYPE_LIGHT,
	 .location = MOTIONSENSE_LOC_LID,
	 .drv = &si114x_drv,
	 .mutex = &g_lid_mutex,
	 .drv_data = &g_si114x_data,
	 .port = I2C_PORT_ALS,
	 .addr = SI114X_ADDR,
	 .rot_standard_ref = NULL,
	 .default_range = 9000, /* 90%: int = 0 - frac = 9000/10000 */
	 .config = {
		 /* AP: by default shutdown all sensors */
		 [SENSOR_CONFIG_AP] = {
			 .odr = 0,
			 .ec_rate = 0,
		 },
		 /* Run ALS sensor in S0 */
		 [SENSOR_CONFIG_EC_S0] = {
			 .odr = 1000,
			 .ec_rate = 0,
		 },
		 /* Sensor off in S3/S5 */
		 [SENSOR_CONFIG_EC_S3] = {
			 .odr = 0,
			 .ec_rate = 0,
		 },
		 /* Sensor off in S3/S5 */
		 [SENSOR_CONFIG_EC_S5] = {
			 .odr = 0,
			 .ec_rate = 0,
		 },
	 },
	},
};
const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);

/* ALS instances when LPC mapping is needed. Each entry directs to a sensor. */
const struct motion_sensor_t *motion_als_sensors[] = {
	&motion_sensors[LID_LIGHT],
};
BUILD_ASSERT(ARRAY_SIZE(motion_als_sensors) == ALS_COUNT);

