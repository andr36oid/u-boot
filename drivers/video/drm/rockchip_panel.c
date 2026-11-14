/*
 * (C) Copyright 2008-2017 Fuzhou Rockchip Electronics Co., Ltd
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <drm/drm_mipi_dsi.h>

#include <config.h>
#include <common.h>
#include <errno.h>
#include <malloc.h>
#include <video.h>
#include <backlight.h>
#include <asm/gpio.h>
#include <dm/device.h>
#include <dm/read.h>
#include <dm/uclass.h>
#include <dm/uclass-id.h>
#include <linux/media-bus-format.h>
#include <power/regulator.h>

#include "rockchip_display.h"
#include "rockchip_crtc.h"
#include "rockchip_connector.h"
#include "rockchip_panel.h"
#include "a36-logo-diag.h"

struct rockchip_cmd_header {
	u8 data_type;
	u8 delay_ms;
	u8 payload_length;
} __packed;

struct rockchip_cmd_desc {
	struct rockchip_cmd_header header;
	const u8 *payload;
};

struct rockchip_panel_cmds {
	struct rockchip_cmd_desc *cmds;
	int cmd_cnt;
};

struct rockchip_panel_plat {
	bool power_invert;
	u32 bus_format;
	unsigned int bpc;

	struct {
		unsigned int prepare;
		unsigned int unprepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int reset;
		unsigned int init;
	} delay;

	struct rockchip_panel_cmds *on_cmds;
	struct rockchip_panel_cmds *off_cmds;
};

struct rockchip_panel_priv {
	bool prepared;
	bool enabled;
	struct udevice *power_supply;
	struct udevice *backlight;
	struct gpio_desc enable_gpio;
	struct gpio_desc reset_gpio;
#if defined(CONFIG_PLATFORM_ODROID_GOADV)
	struct udevice *backlight_supply;
#endif
	int cmd_type;
	struct gpio_desc spi_sdi_gpio;
	struct gpio_desc spi_scl_gpio;
	struct gpio_desc spi_cs_gpio;
};

static inline int get_panel_cmd_type(const char *s)
{
	if (!s)
		return -EINVAL;

	if (strncmp(s, "spi", 3) == 0)
		return CMD_TYPE_SPI;
	else if (strncmp(s, "mcu", 3) == 0)
		return CMD_TYPE_MCU;

	return CMD_TYPE_DEFAULT;
}

static int rockchip_panel_parse_cmds(const u8 *data,int length,
                                     struct rockchip_panel_cmds *pcmds)
{
 const u8 *p=data;int len=length,count=0,i;
 if(!data || length<=0 || length>16384)return -EINVAL;
 while(len) {
  if(len<3 || p[2]>len-3)return -EINVAL;
  len-=3+p[2];p+=3+p[2];count++;
 }
 pcmds->cmds=calloc(count,sizeof(*pcmds->cmds));
 if(!pcmds->cmds)return -ENOMEM;
 pcmds->cmd_cnt=count;p=data;
 for(i=0;i<count;i++) {
  struct rockchip_cmd_desc *d=&pcmds->cmds[i];
  memcpy(&d->header,p,3);d->payload=p+3;p+=3+p[2];
 }
 return 0;
}

static void rockchip_panel_write_spi_cmds(struct rockchip_panel_priv *priv,
					  u8 type, int value)
{
	int i;

	dm_gpio_set_value(&priv->spi_cs_gpio, 0);

	if (type == 0)
		value &= (~(1 << 8));
	else
		value |= (1 << 8);

	for (i = 0; i < 9; i++) {
		if (value & 0x100)
			dm_gpio_set_value(&priv->spi_sdi_gpio, 1);
		else
			dm_gpio_set_value(&priv->spi_sdi_gpio, 0);

		dm_gpio_set_value(&priv->spi_scl_gpio, 0);
		udelay(10);
		dm_gpio_set_value(&priv->spi_scl_gpio, 1);
		value <<= 1;
		udelay(10);
	}

	dm_gpio_set_value(&priv->spi_cs_gpio, 1);
}

static int rockchip_panel_send_mcu_cmds(struct display_state *state,
					struct rockchip_panel_cmds *cmds)
{
	int i;

	if (!cmds)
		return -EINVAL;

	display_send_mcu_cmd(state, MCU_SETBYPASS, 1);
	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct rockchip_cmd_desc *desc = &cmds->cmds[i];
		int value = 0;

		value = desc->payload[0];
		display_send_mcu_cmd(state, desc->header.data_type, value);

		if (desc->header.delay_ms)
			mdelay(desc->header.delay_ms);
	}
	display_send_mcu_cmd(state, MCU_SETBYPASS, 0);

	return 0;
}

static int rockchip_panel_send_spi_cmds(struct display_state *state,
					struct rockchip_panel_cmds *cmds)
{
	struct rockchip_panel *panel = state_get_panel(state);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	int i;

	if (!cmds)
		return -EINVAL;

	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct rockchip_cmd_desc *desc = &cmds->cmds[i];
		int value = 0;

		if (desc->header.payload_length == 2)
			value = (desc->payload[0] << 8) | desc->payload[1];
		else
			value = desc->payload[0];
		rockchip_panel_write_spi_cmds(priv,
					      desc->header.data_type, value);

		if (desc->header.delay_ms)
			mdelay(desc->header.delay_ms);
	}

	return 0;
}

static int rockchip_panel_send_dsi_cmds(struct mipi_dsi_device *dsi,
					struct rockchip_panel_cmds *cmds)
{
	int i, ret;

	if (!cmds)
		return -EINVAL;

	for (i = 0; i < cmds->cmd_cnt; i++) {
		struct rockchip_cmd_desc *desc = &cmds->cmds[i];
		const struct rockchip_cmd_header *header = &desc->header;

		switch (header->data_type) {
		case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
		case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
		case MIPI_DSI_GENERIC_LONG_WRITE:
			ret = mipi_dsi_generic_write(dsi, desc->payload,
						     header->payload_length);
			break;
		case MIPI_DSI_DCS_SHORT_WRITE:
		case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		case MIPI_DSI_DCS_LONG_WRITE:
			ret = mipi_dsi_dcs_write_buffer(dsi, desc->payload,
							header->payload_length);
			break;
		default:
			printf("unsupport command data type: %d\n",
			       header->data_type);
			return -EINVAL;
		}

		/* Transport must report the exact payload, including short packets. */
		if (ret >= 0 && ret != header->payload_length)
			ret = -EIO;
		if (ret < 0) {
			printf("failed to write cmd%d: %d\n", i, ret);
			return ret;
		}

		if (header->delay_ms)
			mdelay(header->delay_ms);
#if 0	// for lcd debug
		{
			int cnt, pos;
			char *cmdp = (char *)desc->payload, cmdstr[256];

			memset(cmdstr, 0x00, sizeof(cmdstr));

			pos = sprintf(&cmdstr[0], "[cmd : %02X] ", cmdp[0]);

			for(cnt = 1; cnt < header->payload_length; cnt++)
				pos += sprintf(&cmdstr[pos], "%02X ", cmdp[cnt]);

			pr_err("%s\n", cmdstr);
			if (header->delay_ms)
				pr_err("[delay(ms) : %d]\n", header->delay_ms);
		}
#endif
	}

	return 0;
}

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
/* A GPIO-switched fixed rail has no programmable voltage operation. Require
 * one exact positive constraint and matching declared readback instead of
 * swallowing ENOSYS from an arbitrary regulator. GPIO enable is checked by
 * the ordinary prepare path and again by the final handoff verifier.
 */
static int panel_simple_set_supply_voltage(struct udevice *panel_dev,
                                            struct udevice *supply)
{
 struct dm_regulator_uclass_platdata *plat = dev_get_uclass_platdata(supply);
 int value;

 if (!plat)
  return -ENODEV;
 if (plat->type != REGULATOR_TYPE_FIXED ||
     !dev_read_bool(panel_dev, "andr36oid,checked-fixed-panel-supply"))
  return regulator_set_value(supply, plat->min_uV);
 if (plat->min_uV <= 0 || plat->min_uV != plat->max_uV)
  return -EINVAL;
 value = regulator_get_value(supply);
 if (value < 0)
  return value;
 return value == plat->min_uV ? 0 : -ERANGE;
}
/* For the opt-in Ultra alignment, verify the completed on-sequence while
 * the host is still in command mode and before display_enable starts VOP.
 * Keep the DT-selected LP transfer flag here; the final in-video verifier
 * separately checks the state again immediately before the kernel jump.
 */
static int panel_simple_check_power_mode(struct mipi_dsi_device *dsi)
{
 u8 status = 0;
 int ret;

 if (!dsi)
  return -ENODEV;
 ret = mipi_dsi_dcs_read(dsi, 0x0a, &status, 1);
 if (ret < 0)
  return ret;
 return ret == 1 && (status & 0x1c) == 0x1c ? 0 : -EIO;
}

#endif

static void panel_simple_prepare(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	struct mipi_dsi_device *dsi = dev_get_parent_platdata(panel->dev);
	int ret;

	if (panel->init_error || priv->prepared)
		return;
	panel->prepared_checked = false;
	a36_logo_invalidate();

	if (priv->power_supply) {
#if defined(CONFIG_PLATFORM_ODROID_GOADV)
		ret = panel_simple_set_supply_voltage(panel->dev, priv->power_supply);
		if (ret)
			goto error;
#endif
		ret = regulator_set_enable(priv->power_supply, !plat->power_invert);
		if (ret)
			goto error;
	}

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
	if (priv->backlight_supply) {
		ret = panel_simple_set_supply_voltage(panel->dev, priv->backlight_supply);
		if (ret)
			goto error;

		ret = regulator_set_enable(priv->backlight_supply, 1);
		if (ret) {
			printf("%s: failed to enable backlight supply",
				__func__);
			goto error;
		}
	}
#endif

	if (dm_gpio_is_valid(&priv->enable_gpio)) {
		ret = dm_gpio_set_value(&priv->enable_gpio, 1);
		if (ret)
			goto error;
	}

	if (plat->delay.prepare)
		mdelay(plat->delay.prepare);

	if (dm_gpio_is_valid(&priv->reset_gpio)) {
		ret = dm_gpio_set_value(&priv->reset_gpio, 1);
		if (ret)
			goto error;
	}

	if (plat->delay.reset)
		mdelay(plat->delay.reset);

	if (dm_gpio_is_valid(&priv->reset_gpio)) {
		ret = dm_gpio_set_value(&priv->reset_gpio, 0);
		if (ret)
			goto error;
	}

	if (plat->delay.init)
		mdelay(plat->delay.init);

	if (plat->on_cmds) {
		if (priv->cmd_type == CMD_TYPE_SPI)
			ret = rockchip_panel_send_spi_cmds(panel->state,
							   plat->on_cmds);
		else if (priv->cmd_type == CMD_TYPE_MCU)
			ret = rockchip_panel_send_mcu_cmds(panel->state,
							   plat->on_cmds);
		else
			ret = rockchip_panel_send_dsi_cmds(dsi, plat->on_cmds);
		if (ret) {
			printf("failed to send on cmds: %d\n", ret);
			goto error;
		}
	}

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
 if (dev_read_bool(panel->dev, "andr36oid,checked-fixed-panel-supply")) {
  ret = panel_simple_check_power_mode(dsi);
  if (ret)
   goto error;
 }
#endif

	priv->prepared = true;
	panel->prepared_checked = true;
	return;
error:
	/* Do not assert reset/power-off without a checked scanout stop. */
	panel->init_error = ret ? ret : -EIO;
	panel->prepared_checked = false;
	printf("panel init transaction failed: %d\n", panel->init_error);
}

static void panel_simple_unprepare(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	struct mipi_dsi_device *dsi = dev_get_parent_platdata(panel->dev);
	int ret;

	a36_logo_invalidate();
	if (!priv->prepared)
		return;

	if (plat->off_cmds) {
		if (priv->cmd_type == CMD_TYPE_SPI)
			ret = rockchip_panel_send_spi_cmds(panel->state,
							   plat->off_cmds);
		else if (priv->cmd_type == CMD_TYPE_MCU)
			ret = rockchip_panel_send_mcu_cmds(panel->state,
							   plat->off_cmds);
		else
			ret = rockchip_panel_send_dsi_cmds(dsi, plat->off_cmds);
		if (ret)
			printf("failed to send off cmds: %d\n", ret);
	}

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
	if (dm_gpio_is_valid(&priv->reset_gpio))
		dm_gpio_set_value(&priv->reset_gpio, 0);

	if (plat->delay.reset)
		mdelay(plat->delay.reset);
#else
	if (dm_gpio_is_valid(&priv->reset_gpio))
		dm_gpio_set_value(&priv->reset_gpio, 1);
#endif

	if (dm_gpio_is_valid(&priv->enable_gpio))
		dm_gpio_set_value(&priv->enable_gpio, 0);

	if (priv->power_supply)
		regulator_set_enable(priv->power_supply, plat->power_invert);

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
	if (priv->backlight_supply) {
		ret = regulator_set_enable(priv->backlight_supply, 0);
		if (ret)
			printf("%s: failed to disable backlight supply",
				__func__);
	}
#endif

	if (plat->delay.unprepare)
		mdelay(plat->delay.unprepare);

	priv->prepared = false;
	panel->prepared_checked = false;
}

static void panel_simple_enable(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);
	int ret;

	if (panel->init_error || priv->enabled)
		return;
	if (!panel->prepared_checked) {
		panel->init_error = -EIO;
		return;
	}

	if (plat->delay.enable)
		mdelay(plat->delay.enable);

	if (priv->backlight) {
		ret = backlight_enable(priv->backlight);
		if (ret) {
			panel->init_error = ret;
			return;
		}
	}

	priv->enabled = true;
	panel->enabled_checked = true;
}

static void panel_simple_disable(struct rockchip_panel *panel)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(panel->dev);
	struct rockchip_panel_priv *priv = dev_get_priv(panel->dev);

	a36_logo_invalidate();
	if (!priv->enabled)
		return;

	if (priv->backlight)
		backlight_disable(priv->backlight);

	if (plat->delay.disable)
		mdelay(plat->delay.disable);

	priv->enabled = false;
	panel->enabled_checked = false;
}

static void panel_simple_init(struct rockchip_panel *panel)
{
	struct display_state *state = panel->state;
	struct connector_state *conn_state = &state->conn_state;

	conn_state->bus_format = panel->bus_format;
}

static const struct rockchip_panel_funcs rockchip_panel_funcs = {
	.init = panel_simple_init,
	.prepare = panel_simple_prepare,
	.unprepare = panel_simple_unprepare,
	.enable = panel_simple_enable,
	.disable = panel_simple_disable,
};

static int rockchip_panel_ofdata_to_platdata(struct udevice *dev)
{
	struct rockchip_panel_plat *plat = dev_get_platdata(dev);
	const void *data;
	int len = 0;
	int ret;

	plat->power_invert = dev_read_bool(dev, "power-invert");

	plat->delay.prepare = dev_read_u32_default(dev, "prepare-delay-ms", 0);
	plat->delay.unprepare = dev_read_u32_default(dev, "unprepare-delay-ms", 0);
	plat->delay.enable = dev_read_u32_default(dev, "enable-delay-ms", 0);
	plat->delay.disable = dev_read_u32_default(dev, "disable-delay-ms", 0);
	plat->delay.init = dev_read_u32_default(dev, "init-delay-ms", 0);
	plat->delay.reset = dev_read_u32_default(dev, "reset-delay-ms", 0);

	plat->bus_format = dev_read_u32_default(dev, "bus-format",
						MEDIA_BUS_FMT_RBG888_1X24);
	plat->bpc = dev_read_u32_default(dev, "bpc", 8);

	data = dev_read_prop(dev, "panel-init-sequence", &len);
	if (data) {
		plat->on_cmds = calloc(1, sizeof(*plat->on_cmds));
		if (!plat->on_cmds)
			return -ENOMEM;

		ret = rockchip_panel_parse_cmds(data, len, plat->on_cmds);
		if (ret) {
			printf("failed to parse panel init sequence\n");
			goto free_on_cmds;
		}
	}

	data = dev_read_prop(dev, "panel-exit-sequence", &len);
	if (data) {
		plat->off_cmds = calloc(1, sizeof(*plat->off_cmds));
		if (!plat->off_cmds) {
			ret = -ENOMEM;
			goto free_on_cmds;
		}

		ret = rockchip_panel_parse_cmds(data, len, plat->off_cmds);
		if (ret) {
			printf("failed to parse panel exit sequence\n");
			goto free_cmds;
		}
	}

	return 0;

free_cmds:
	free(plat->off_cmds);
free_on_cmds:
	free(plat->on_cmds);
	return ret;
}

static int rockchip_panel_probe(struct udevice *dev)
{
	struct rockchip_panel_priv *priv = dev_get_priv(dev);
	struct rockchip_panel_plat *plat = dev_get_platdata(dev);
	struct rockchip_panel *panel;
	int ret;
	const char *cmd_type;

	ret = gpio_request_by_name(dev, "enable-gpios", 0,
				   &priv->enable_gpio, GPIOD_IS_OUT);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get enable GPIO: %d\n", __func__, ret);
		return ret;
	}

	ret = gpio_request_by_name(dev, "reset-gpios", 0,
				   &priv->reset_gpio, GPIOD_IS_OUT);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get reset GPIO: %d\n", __func__, ret);
		return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_PANEL_BACKLIGHT, dev,
					   "backlight", &priv->backlight);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get backlight: %d\n", __func__, ret);
		return ret;
	}

	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
					   "power-supply", &priv->power_supply);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get power supply: %d\n", __func__, ret);
		return ret;
	}

#if defined(CONFIG_PLATFORM_ODROID_GOADV)
	ret = uclass_get_device_by_phandle(UCLASS_REGULATOR, dev,
				   "backlight-supply", &priv->backlight_supply);
	if (ret && ret != -ENOENT) {
		printf("%s: Cannot get backlight supply: %d\n", __func__, ret);
		return ret;
	}
#endif

	ret = dev_read_string_index(dev, "rockchip,cmd-type", 0, &cmd_type);
	if (ret)
		priv->cmd_type = CMD_TYPE_DEFAULT;
	else
		priv->cmd_type = get_panel_cmd_type(cmd_type);

	if (priv->cmd_type == CMD_TYPE_SPI) {
		ret = gpio_request_by_name(dev, "spi-sdi-gpios", 0,
					   &priv->spi_sdi_gpio, GPIOD_IS_OUT);
		if (ret && ret != -ENOENT) {
			printf("%s: Cannot get spi sdi GPIO: %d\n",
			       __func__, ret);
			return ret;
		}
		ret = gpio_request_by_name(dev, "spi-scl-gpios", 0,
					   &priv->spi_scl_gpio, GPIOD_IS_OUT);
		if (ret && ret != -ENOENT) {
			printf("%s: Cannot get spi scl GPIO: %d\n",
			       __func__, ret);
			return ret;
		}
		ret = gpio_request_by_name(dev, "spi-cs-gpios", 0,
					   &priv->spi_cs_gpio, GPIOD_IS_OUT);
		if (ret && ret != -ENOENT) {
			printf("%s: Cannot get spi cs GPIO: %d\n",
			       __func__, ret);
			return ret;
		}
		dm_gpio_set_value(&priv->spi_sdi_gpio, 1);
		dm_gpio_set_value(&priv->spi_scl_gpio, 1);
		dm_gpio_set_value(&priv->spi_cs_gpio, 1);
		dm_gpio_set_value(&priv->reset_gpio, 0);
	}

	panel = calloc(1, sizeof(*panel));
	if (!panel)
		return -ENOMEM;

	dev->driver_data = (ulong)panel;
	panel->dev = dev;
	panel->bus_format = plat->bus_format;
	panel->bpc = plat->bpc;
	panel->funcs = &rockchip_panel_funcs;

	return 0;
}

static const struct udevice_id rockchip_panel_ids[] = {
	{ .compatible = "simple-panel", },
	{ .compatible = "simple-panel-dsi", },
	{}
};

U_BOOT_DRIVER(rockchip_panel) = {
	.name = "rockchip_panel",
	.id = UCLASS_PANEL,
	.of_match = rockchip_panel_ids,
	.ofdata_to_platdata = rockchip_panel_ofdata_to_platdata,
	.probe = rockchip_panel_probe,
	.priv_auto_alloc_size = sizeof(struct rockchip_panel_priv),
	.platdata_auto_alloc_size = sizeof(struct rockchip_panel_plat),
};

int rockchip_panel_loader_status(struct rockchip_panel *panel,u8 *status)
{
 struct rockchip_panel_plat *plat;struct rockchip_panel_priv *priv;
 struct mipi_dsi_device *dsi;unsigned long flags;int ret;
#define PANEL_RETURN(reason, error) do { \
 a36_logo_diag_panel(A36_D_PANEL_REASON,reason); \
 a36_logo_diag_panel(A36_D_PANEL_RESULT,error);return error; \
} while (0)
 if(!panel || panel->funcs!=&rockchip_panel_funcs || panel->init_error ||
    !panel->prepared_checked || !panel->enabled_checked)PANEL_RETURN(1,-EINVAL);
 plat=dev_get_platdata(panel->dev);priv=dev_get_priv(panel->dev);
 a36_logo_diag_panel(A36_D_POWER_INVERT,plat->power_invert);
 a36_logo_diag_panel(A36_D_PRIVATE_PREPARED,priv->prepared);
 a36_logo_diag_panel(A36_D_PRIVATE_ENABLED,priv->enabled);
 a36_logo_diag_panel(A36_D_COMMAND_TYPE,priv->cmd_type);
 if(plat->power_invert || !priv->prepared || !priv->enabled ||
    priv->cmd_type==CMD_TYPE_SPI || priv->cmd_type==CMD_TYPE_MCU)PANEL_RETURN(2,-EINVAL);
 if(priv->power_supply){ret=regulator_get_enable(priv->power_supply);
  a36_logo_diag_panel(A36_D_SUPPLY,ret);if(ret!=1)PANEL_RETURN(3,-EIO);}
 if(priv->backlight_supply){ret=regulator_get_enable(priv->backlight_supply);
  a36_logo_diag_panel(A36_D_BACKLIGHT_SUPPLY,ret);if(ret!=1)PANEL_RETURN(4,-EIO);}
 if(dm_gpio_is_valid(&priv->enable_gpio)){ret=dm_gpio_get_value(&priv->enable_gpio);
  a36_logo_diag_panel(A36_D_ENABLE_GPIO,ret);if(ret!=1)PANEL_RETURN(5,-EIO);}
 if(dm_gpio_is_valid(&priv->reset_gpio)){ret=dm_gpio_get_value(&priv->reset_gpio);
  a36_logo_diag_panel(A36_D_RESET_GPIO,ret);if(ret!=0)PANEL_RETURN(6,-EIO);}
 dsi=dev_get_parent_platdata(panel->dev);if(!dsi)PANEL_RETURN(7,-EINVAL);
 flags=dsi->mode_flags;dsi->mode_flags&=~MIPI_DSI_MODE_LPM;
 a36_logo_diag_panel(A36_D_DSI_FLAGS,flags);
 ret=mipi_dsi_dcs_read(dsi,0x0a,status,1);dsi->mode_flags=flags;
 a36_logo_diag_panel(A36_D_DCS_RET,ret);
 if(ret<0)PANEL_RETURN(8,ret);
 if(ret!=1)PANEL_RETURN(9,-EIO);
 if((*status&0x1c)!=0x1c)PANEL_RETURN(10,-EIO);
 PANEL_RETURN(0,0);
#undef PANEL_RETURN
}
