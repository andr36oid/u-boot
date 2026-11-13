/*
 * (C) Copyright 2020 Hardkernel Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <dm.h>
#include <asm/gpio.h>
#include <linux/libfdt.h>
#include <power/fuel_gauge.h>
#include <odroidgoa_status.h>

#define MIN_VOL_LEVEL	3530	/* 3.53V */

#define PWR_LED_GPIO	18	/* GPIO0_C2 */
#define DC_DET_GPIO	11	/* GPIO0_B3 */
#define CHG_LED_GPIO	13	/* GPIO0_B5 */

DECLARE_GLOBAL_DATA_PTR;

/*
 * andr36oid: some clones (the D-28S) wire GPIO0_B5 to the screen's enable pin,
 * not to a charge LED: driving it low switched the screen off. True when the
 * DT has an enable-gpios on it.
 */
/* A guard, never a GPIO writer: 1 is owned, 0 unrelated, <0 unproven.
 * Malformed ownership is fail-closed by the caller (leave the pin alone).
 * Polarity is validated, but both high/low owners exclude a charge LED.
 */
static int panel_gpio0_b5(const void *fdt, const fdt32_t *gpios, int len)
{
	int bank, cells, n, ac;
	const fdt32_t *p;
	const char *name;
	u32 pin, flags;

	if (!gpios)
		return 0;
	if (len != 3 * sizeof(*gpios))
		return -EINVAL;
	bank = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(gpios[0]));
	if (bank < 0)
		return -EINVAL;
	p = fdt_getprop(fdt, bank, "#gpio-cells", &n);
	if (!p || n != sizeof(*p))
		return -EINVAL;
	cells = fdt32_to_cpu(*p);
	pin = fdt32_to_cpu(gpios[1]);
	flags = fdt32_to_cpu(gpios[2]);
	if (cells != 2 || pin >= 32 || (flags & ~1U))
		return -EINVAL;
	name = fdt_get_name(fdt, bank, NULL);
	if (!name || strncmp(name, "gpio0@", 6) || pin != CHG_LED_GPIO)
		return 0;
	/* Do not identify a different controller by its label alone. */
	ac = fdt_address_cells(fdt, fdt_parent_offset(fdt, bank));
	p = fdt_getprop(fdt, bank, "reg", &n);
	if (!p || (ac != 1 && ac != 2) || n < ac * (int)sizeof(*p) ||
	    (ac == 2 && fdt32_to_cpu(p[0])) ||
	    fdt32_to_cpu(p[ac - 1]) != 0xff040000)
		return -EINVAL;
	return 1;
}

/* Follow only the explicit regulator supply chain, bounded against cycles.
 * PMIC regulators without a GPIO are not automatically pin owners.
 */
static int panel_supply_owns_b5(const void *fdt, int panel)
{
	const fdt32_t *p;
	int seen[8], count = 0, node = panel, len, i, ret;
	const char *supply = "power-supply";

	for (;;) {
		p = fdt_getprop(fdt, node, supply, &len);
		if (!p)
			return 0;
		if (len != sizeof(*p))
			return -EINVAL;
		if (count >= ARRAY_SIZE(seen))
			return -ELOOP;
		node = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*p));
		if (node < 0)
			return -EINVAL;
		for (i = 0; i < count; i++)
			if (seen[i] == node)
				return -ELOOP;
		seen[count++] = node;
		if (fdt_node_check_compatible(fdt, node, "regulator-fixed") &&
		    (fdt_getprop(fdt, node, "gpio", NULL) ||
		     fdt_getprop(fdt, node, "gpios", NULL)))
			return -EINVAL; /* unknown GPIO-supply binding: leave it alone */
		if (!fdt_node_check_compatible(fdt, node, "regulator-fixed")) {
			p = fdt_getprop(fdt, node, "gpio", &len);
			ret = panel_gpio0_b5(fdt, p, len);
			if (ret)
				return ret;
			p = fdt_getprop(fdt, node, "gpios", &len);
			ret = panel_gpio0_b5(fdt, p, len);
			if (ret)
				return ret;
			/* This legacy fixed-regulator polarity flag is a boolean.
			 * It never permits us to drive the owned pin as an LED. */
			if (fdt_getprop(fdt, node, "enable-active-high", &len) && len)
				return -EINVAL;
		}
		supply = "vin-supply";
	}
	return -ELOOP;
}

/* Preserve direct enable-gpios ownership and cover a panel switch represented
 * as power-supply -> regulator-fixed gpio, including a switched parent rail.
 */
static bool chg_led_is_panel_enable(void)
{
	const void *fdt = gd->fdt_blob;
	const fdt32_t *p;
	int node, len, ret, count = 0;
	bool panel_seen = false;

	if (!fdt || fdt_check_header(fdt))
		return true;
	for (node = fdt_next_node(fdt, -1, NULL); node >= 0;
	     node = fdt_next_node(fdt, node, NULL)) {
		if (++count > 4096)
			return true;
		p = fdt_getprop(fdt, node, "enable-gpios", &len);
		ret = panel_gpio0_b5(fdt, p, len);
		if (ret)
			return true;
		if (!fdt_node_check_compatible(fdt, node, "simple-panel-dsi") ||
		    !fdt_node_check_compatible(fdt, node, "simple-panel")) {
			panel_seen = true;
			ret = panel_supply_owns_b5(fdt, node);
			if (ret)
				return true;
		}
	}
	return !panel_seen || node != -FDT_ERR_NOTFOUND;
}


void board_chg_led(void)
{
	if (chg_led_is_panel_enable())
		return;
	gpio_request(CHG_LED_GPIO, "chg_led");
	/* default off, controlled by charge animation logic */
	gpio_direction_output(CHG_LED_GPIO, 0);
	gpio_free(CHG_LED_GPIO);
}

void board_pwr_led(bool on)
{
	gpio_request(PWR_LED_GPIO, "pwr_led");
	gpio_direction_output(PWR_LED_GPIO, on);
	gpio_free(PWR_LED_GPIO);
}

int odroid_check_dcjack(void)
{
	/* set pwr led on by default */
	board_pwr_led(true);

	gpio_request(DC_DET_GPIO, "dc_det_gpio");
	if (gpio_get_value(DC_DET_GPIO)) {
		debug("dc jack is connected\n");
		return 1;
	} else {
		debug("dc jack is NOT connected\n");
		return 0;
	}
}

int odroid_check_battery(int *battery)
{
	int ret;
	struct udevice *fg;

	ret = uclass_get_device(UCLASS_FG, 0, &fg);
	if (ret) {
		if (ret == -ENODEV)
			debug("Can't find FG\n");
		else
			debug("Get UCLASS FG failed: %d\n", ret);
		return ret;
	}

	*battery = fuel_gauge_get_voltage(fg);

	debug("BATTERY %d\n", *battery);

	return *battery < MIN_VOL_LEVEL ? 0 : 1;
}

int board_check_power(void)
{
	int battery = 0;
	int dcpower = 0;
	char str[32];

	board_chg_led();

	dcpower = odroid_check_dcjack();

	if (odroid_check_battery(&battery) || dcpower)
		return 0;

	debug("low battery (%d) without dc jack connected\n", battery);
	sprintf(str, "voltage level : %d.%dV", (battery / 1000), (battery % 1000));
	odroid_display_status(LOGO_MODE_LOW_BATT, LOGO_STORAGE_ANYWHERE, str);
	odroid_wait_pwrkey();

	return -1;
}
