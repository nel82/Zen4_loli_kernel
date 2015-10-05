/*
 * rt5640.c  --  RT5640 ALSA SoC audio codec driver
 *
 * Copyright 2011 Realtek Semiconductor Corp.
 * Author: Johnny Hsu <johnnyhsu@realtek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
//#define DEBUG 1 //bard
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/initval.h>
#include <sound/tlv.h>
#include <asm/intel-mid.h>
#include <linux/HWVersion.h>
#ifdef CONFIG_EEPROM_PADSTATION
#include <linux/microp_api.h>
#include <linux/microp_pin_def.h>
#endif 

#define RTK_IOCTL
#ifdef RTK_IOCTL
#if defined(CONFIG_SND_HWDEP) || defined(CONFIG_SND_HWDEP_MODULE)
#include "rt56xx_ioctl.h"
#include "rt5640_ioctl.h"
#endif
#endif

#include "rt5640.h"
#if (CONFIG_SND_SOC_RT5642_MODULE | CONFIG_SND_SOC_RT5642)
#include "rt5640-dsp.h"
#endif

#define RT5640_REG_RW 1 /* for debug */
#define RT5640_DET_EXT_MIC 0
#define USE_ONEBIT_DEPOP 0  /* for one bit depop */
#define USE_EQ
#define USE_ASRC
#define VERSION "0.8.4 alsa 1.0.25"

#if defined(CONFIG_ME302C_SPC_PWR_SET) 
#define PIN_3VSUS_EN 45
#define PIN_5VSUS_EN 160
#endif
#define JACK_DETECT_PIN 34
#define MIC2_BIAS_EN 51//gpio-aon-51
#define ME175CG_PIN_3VSUS_EN 159
#define PF400CG_P_SPK_EN_5 42
#define CLV_I2S1_SEL 90
extern int Read_PROJ_ID(void);
static int PROJ_ID;
extern int Read_HW_ID(void);
static int HW_ID;

extern int is_csv_call_active(void);
extern int output_device_from_hal(void);
static int is_asrc5_changed;

#ifdef CONFIG_A400CG
static bool RingTone_coming = false;
#endif
static int headphone_retry_time = 0;
static int dac_is_unmute = 0;
static int rt5640_index_write(struct snd_soc_codec *codec,
                unsigned int reg, unsigned int value);

// Jericho eq
static  int Hal_user_scenario_mode;
static int hp_event_eq_val;

static int bt_switch_ctrl = Codec;
static bool is_csv_call = false;

// joe_cheng
static int headset_status = 0; // NO->0, Headset->1, Headphone->2
static struct snd_soc_codec *rt5640_codec;

// joe_cheng : for ATD audio_codec_status
static int ret_codec_status = 0;
static ssize_t codec_show(struct device *dev, struct device_attribute *attr,
		char *buf);
static ssize_t codec_store(struct device *dev,struct device_attribute *attr, 
	    	const char *buf, size_t count);

static DEVICE_ATTR(audio_codec_status, S_IWUSR | S_IRUGO, codec_show, codec_store);

static ssize_t codec_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	if(ret_codec_status == 1)
		return sprintf(buf, "1\n");
	else
		return sprintf(buf, "0\n");
}

static ssize_t codec_store(struct device *dev,struct device_attribute *attr, 
	    	const char *buf, size_t count)
{
	return 0;
}

#ifdef CONFIG_EEPROM_PADSTATION
static int p72g_atd_device = 0;
static ssize_t p72g_device_show(struct device *dev, struct device_attribute *attr,
		char *buf);
static ssize_t p72g_device_store(struct device *dev, struct device_attribute *attr,
                        const char *buf, size_t count);

static DEVICE_ATTR(p72g_device_status, S_IWUSR | S_IRUGO, p72g_device_show, p72g_device_store);

static ssize_t p72g_device_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return 0;
}

static ssize_t p72g_device_store(struct device *dev, struct device_attribute *attr,
                const char *buf, size_t count)
{
        if (buf[0] == 'r') { //RCV
		if (AX_MicroP_getHWID() == 3) {
			AX_MicroP_setGPIOOutputPin(OUT_up_RCV_SEL, 1);
			printk("%s : enable RCV_SEL\n", __func__);
		} else {
			snd_soc_update_bits(rt5640_codec, RT5640_OUTPUT, 0x0080, 0x0);
		}
		p72g_atd_device = 1;
	}
	else if (buf[0] == 's') { //SPK
		if (AX_MicroP_getHWID() == 3) {
			AX_MicroP_setGPIOOutputPin(OUT_up_SPK_SEL, 1);
			printk("%s : enable SPK_SEL\n", __func__);
		} else {
			snd_soc_update_bits(rt5640_codec, RT5640_OUTPUT, 0x8000, 0x0);
		}
		p72g_atd_device = 2;
	}
	else if(buf[0] == 't') { //Stop
		printk("%s : disable SPK_SEL and RCV_SEL\n", __func__);
		AX_MicroP_setGPIOOutputPin(OUT_up_RCV_SEL, 0);
		AX_MicroP_setGPIOOutputPin(OUT_up_SPK_SEL, 0);
		snd_soc_update_bits(rt5640_codec, RT5640_OUTPUT, 0x8080, 0x8080);
		p72g_atd_device = 0;
	}

	printk("%s : p72g_atd_device %d\n", __func__, p72g_atd_device);
	return 1;
}
#endif
// joe_cheng : end

static int gpio;

struct rt5640_init_reg {
	u8 reg;
	u16 val;
};

static struct timer_list mclk_check_timer;
struct delayed_work hp_amp_work, spk_amp_l_work, spk_amp_r_work, DAC_unmute_work,ringtone_drc_work;
struct work_struct  mclk_check_work;

static struct rt5640_init_reg init_list[] = {
#ifdef USE_ASRC
	{RT5640_GEN_CTRL1	, 0x3f71},//fa[12:13] = 1'b; fa[8~11]=1; fa[0]=1
	{RT5640_ASRC_4		, 0x23d7},
	//{RT5640_ASRC_5		, 0xb69e}, // Change from 23d7 to b69e to solve the weird sound in csv call for 16KHz
	{RT5640_ASRC_5		, 0x23d7}, // Change from 23d7 to b69e to solve the weird sound in csv call for 16KHz
	{RT5640_JD_CTRL		, 0x0003},
#else
	{RT5640_GEN_CTRL1	, 0x3701},//fa[12:13] = 1'b; fa[8~11]=1; fa[0]=1
#endif
	{RT5640_ADDA_CLK1	, 0x0014},//73[2] = 1'b
	{RT5640_MICBIAS		, 0x3030},//93[5:4] = 11'b
	{RT5640_CLS_D_OUT	, 0xa000},//8d[11] = 0'b
	{RT5640_CLS_D_OVCD	, 0x0334},//8c[8] = 1'b
	{RT5640_PRIV_INDEX	, 0x001d},//PR1d[8] = 1'b;
	{RT5640_PRIV_DATA	, 0x0347},
	{RT5640_PRIV_INDEX	, 0x003d},//PR3d[12] = 0'b; PR3d[9] = 1'b
	{RT5640_PRIV_DATA	, 0x2600},
	{RT5640_PRIV_INDEX	, 0x0012},//PR12 = 0aa8'h
	{RT5640_PRIV_DATA	, 0x0aa8},
	{RT5640_PRIV_INDEX	, 0x0014},//PR14 = 8aaa'h
	{RT5640_PRIV_DATA	, 0x8aaa},
	{RT5640_PRIV_INDEX	, 0x0020},//PR20 = 6115'h
	{RT5640_PRIV_DATA	, 0x6115},
	{RT5640_PRIV_INDEX	, 0x0023},//PR23 = 0804'h
	{RT5640_PRIV_DATA	, 0x0804},
	{RT5640_PRIV_INDEX	, 0x006e}, 
	{RT5640_PRIV_DATA	, 0x3019},
	/*playback*/
	{RT5640_STO_DAC_MIXER	, 0x1414},//Dig inf 1 -> Sto DAC mixer -> DACL
	{RT5640_OUT_L3_MIXER	, 0x01fe},//DACL1 -> OUTMIXL
	{RT5640_OUT_R3_MIXER	, 0x01fe},//DACR1 -> OUTMIXR
	{RT5640_HP_VOL		, 0x8888},//OUTMIX -> HPVOL
	{RT5640_HPO_MIXER	, 0xc000},//HPVOL -> HPOLMIX
//	{RT5640_HPO_MIXER	, 0xa000},//DAC1 -> HPOLMIX
//	{RT5640_CHARGE_PUMP	, 0x0f00},
	{RT5640_PRIV_INDEX	, 0x0090},
	{RT5640_PRIV_DATA	, 0x2000},
	{RT5640_PRIV_INDEX	, 0x0091},
	{RT5640_PRIV_DATA	, 0x1000},
//	{RT5640_HP_CALIB_AMP_DET, 0x0420},
	{RT5640_MONO_DAC_MIXER	, 0x4444},
	{RT5640_SPK_L_MIXER	, 0x0036},//DACL1 -> SPKMIXL
	{RT5640_SPK_R_MIXER	, 0x0036},//DACR1 -> SPKMIXR
	{RT5640_SPK_VOL		, 0x8b8b},//SPKMIX -> SPKVOL
	{RT5640_SPO_CLSD_RATIO	, 0x0004},// 0dB
	{RT5640_SPO_L_MIXER	, 0xe800},//SPKVOLL -> SPOLMIX
	{RT5640_SPO_R_MIXER	, 0x2800},//SPKVOLR -> SPORMIX
//	{RT5640_SPO_L_MIXER	, 0xb800},//DAC -> SPOLMIX
//	{RT5640_SPO_R_MIXER	, 0x1800},//DAC -> SPORMIX
//	{RT5640_I2S1_SDP	, 0xD000},//change IIS1 and IIS2
	/*record*/
#ifdef CONFIG_EEPROM_PADSTATION
	{RT5640_IN1_IN2		, 0x0100},//IN1 boost 0db and single ended mode, IN3 boost 20dB and single ended mode
#else	
	{RT5640_IN1_IN2		, 0x0000},//IN1 boost 0db and single ended mode
#endif
	{RT5640_IN3_IN4		, 0x0140},//IN2 boost 20db and differential mode
	{RT5640_REC_R2_MIXER	, 0x007f},//Mic2 -> RECMIXR
//	{RT5640_REC_L2_MIXER	, 0x006f},//Mic2 -> RECMIXL
//	{RT5640_REC_R2_MIXER	, 0x006f},//Mic2 -> RECMIXR
#ifdef CONFIG_EEPROM_PADSTATION
	{RT5640_REC_L2_MIXER	, 0x007d},//Mic2 -> RECMIXR
	{RT5640_STO_ADC_MIXER	, 0x7440},//DMIC2 -> Stereo ADC Mixer 
	{RT5640_MONO_ADC_MIXER	, 0x7454},//DMIC2 -> Mono ADC Mixer
#else
	{RT5640_REC_L2_MIXER	, 0x006f},//Mic2 -> RECMIXL
	{RT5640_STO_ADC_MIXER	, 0x5040},//DMIC1 -> Stereo ADC Mixer 
	{RT5640_MONO_ADC_MIXER	, 0x5050},//DMIC1 -> Mono ADC Mixer
#endif		
	{RT5640_ADC_DIG_VOL	, 0xafaf},
	{RT5640_MONO_MIXER	, 0xe800},//OUTMIX -> MONOMIX
#if (CONFIG_SND_SOC_RT5642_MODULE | CONFIG_SND_SOC_RT5642)
	{RT5640_DSP_PATH2	, 0xa000},
#else
	{RT5640_DSP_PATH2	, 0x0c00},
#endif
#ifdef CONFIG_EEPROM_PADSTATION
	{RT5640_OUTPUT		, 0x8888},//OUTMIXL -> OUTVOLL
	{RT5640_LOUT_MIXER	, 0xf000},//change from 0xc000 to 0xf000 for LOUT power

#else	
	{RT5640_OUTPUT		, 0x88c8},//OUTMIXL -> OUTVOLL
#endif	
#if RT5640_DET_EXT_MIC
	{RT5640_MICBIAS		, 0x3800},/* enable MICBIAS short current */
	{RT5640_GPIO_CTRL1	, 0x8400},/* set GPIO1 to IRQ */
	{RT5640_GPIO_CTRL3	, 0x0004},/* set GPIO1 output */
	{RT5640_IRQ_CTRL2	, 0x8000},/*set MICBIAS short current to IRQ */
					/*( if sticky set regBE : 8800 ) */
#endif
};
#define RT5640_INIT_REG_LEN ARRAY_SIZE(init_list)


static int rt5640_reg_init(struct snd_soc_codec *codec)
{
	int i;

	for (i = 0; i < RT5640_INIT_REG_LEN; i++)
		snd_soc_write(codec, init_list[i].reg, init_list[i].val);
	
	// joe_cheng : change class d amp ratio to 2.55x for PR and MP
	if ( (PROJ_ID == PROJ_ID_PF400CG && (HW_ID == 6 || HW_ID == 1)) || PROJ_ID == PROJ_ID_A400CG) {	
		snd_soc_write(codec, RT5640_CLS_D_OUT, 0x6000);
	}
 
        if ( PROJ_ID == PROJ_ID_ME175CG ) {
                rt5640_index_write(codec, RT5640_WND_3, 0x3259);
                snd_soc_write(codec, RT5640_IN1_IN2, 0x1100);
        }

	return 0;
}

static int rt5640_index_sync(struct snd_soc_codec *codec)
{
	int i;

	for (i = 0; i < RT5640_INIT_REG_LEN; i++)
		if (RT5640_PRIV_INDEX == init_list[i].reg ||
			RT5640_PRIV_DATA == init_list[i].reg)
			snd_soc_write(codec, init_list[i].reg,
					init_list[i].val);
	return 0;
}

static const u16 rt5640_reg[RT5640_VENDOR_ID2 + 1] = {
	[RT5640_RESET] = 0x000c,
	[RT5640_SPK_VOL] = 0xc8c8,
	[RT5640_HP_VOL] = 0xc8c8,
	[RT5640_OUTPUT] = 0xc8c8,
	[RT5640_MONO_OUT] = 0x8000,
	[RT5640_INL_INR_VOL] = 0x0808,
	[RT5640_DAC1_DIG_VOL] = 0xafaf,
	[RT5640_DAC2_DIG_VOL] = 0xafaf,
	[RT5640_ADC_DIG_VOL] = 0x2f2f,
	[RT5640_ADC_DATA] = 0x2f2f,
	[RT5640_STO_ADC_MIXER] = 0x7060,
	[RT5640_MONO_ADC_MIXER] = 0x7070,
	[RT5640_AD_DA_MIXER] = 0x8080,
	[RT5640_STO_DAC_MIXER] = 0x5454,
	[RT5640_MONO_DAC_MIXER] = 0x5454,
	[RT5640_DIG_MIXER] = 0xaa00,
	[RT5640_DSP_PATH2] = 0xa000,
	[RT5640_REC_L2_MIXER] = 0x007f,
	[RT5640_REC_R2_MIXER] = 0x007f,
	[RT5640_HPO_MIXER] = 0xe000,
	[RT5640_SPK_L_MIXER] = 0x003e,
	[RT5640_SPK_R_MIXER] = 0x003e,
	[RT5640_SPO_L_MIXER] = 0xf800,
	[RT5640_SPO_R_MIXER] = 0x3800,
	[RT5640_SPO_CLSD_RATIO] = 0x0004,
	[RT5640_MONO_MIXER] = 0xfc00,
	[RT5640_OUT_L3_MIXER] = 0x01ff,
	[RT5640_OUT_R3_MIXER] = 0x01ff,
	[RT5640_LOUT_MIXER] = 0xf000,
	[RT5640_PWR_ANLG1] = 0x00c0,
	[RT5640_I2S1_SDP] = 0x8000,
	[RT5640_I2S2_SDP] = 0x8000,
	[RT5640_I2S3_SDP] = 0x8000,
	[RT5640_ADDA_CLK1] = 0x1110,
	[RT5640_ADDA_CLK2] = 0x0c00,
	[RT5640_DMIC] = 0x1d00,
	[RT5640_ASRC_3] = 0x0008,
	[RT5640_HP_OVCD] = 0x0600,
	[RT5640_CLS_D_OVCD] = 0x0228,
	[RT5640_CLS_D_OUT] = 0xa800,
	[RT5640_DEPOP_M1] = 0x0004,
	[RT5640_DEPOP_M2] = 0x1100,
	[RT5640_DEPOP_M3] = 0x0646,
	[RT5640_CHARGE_PUMP] = 0x0c00,
	[RT5640_MICBIAS] = 0x3000,
	[RT5640_EQ_CTRL1] = 0x2080,
	[RT5640_DRC_AGC_1] = 0x2206,
	[RT5640_DRC_AGC_2] = 0x1f00,
	[RT5640_ANC_CTRL1] = 0x034b,
	[RT5640_ANC_CTRL2] = 0x0066,
	[RT5640_ANC_CTRL3] = 0x000b,
	[RT5640_GPIO_CTRL1] = 0x0400,
	[RT5640_DSP_CTRL3] = 0x2000,
	[RT5640_BASE_BACK] = 0x0013,
	[RT5640_MP3_PLUS1] = 0x0680,
	[RT5640_MP3_PLUS2] = 0x1c17,
	[RT5640_3D_HP] = 0x8c00,
	[RT5640_ADJ_HPF] = 0xaa20,
	[RT5640_HP_CALIB_AMP_DET] = 0x0400,
	[RT5640_SV_ZCD1] = 0x0809,
	[RT5640_VENDOR_ID1] = 0x10ec,
	[RT5640_VENDOR_ID2] = 0x6231,
};

static int rt5640_reset(struct snd_soc_codec *codec)
{
	return snd_soc_write(codec, RT5640_RESET, 0);
}

/**
 * rt5640_index_write - Write private register.
 * @codec: SoC audio codec device.
 * @reg: Private register index.
 * @value: Private register Data.
 *
 * Modify private register for advanced setting. It can be written through
 * private index (0x6a) and data (0x6c) register.
 *
 * Returns 0 for success or negative error code.
 */
static int rt5640_index_write(struct snd_soc_codec *codec,
		unsigned int reg, unsigned int value)
{
	int ret;

	ret = snd_soc_write(codec, RT5640_PRIV_INDEX, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set private addr: %d\n", ret);
		goto err;
	}
	ret = snd_soc_write(codec, RT5640_PRIV_DATA, value);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set private value: %d\n", ret);
		goto err;
	}
	return 0;

err:
	return ret;
}

/**
 * rt5640_index_read - Read private register.
 * @codec: SoC audio codec device.
 * @reg: Private register index.
 *
 * Read advanced setting from private register. It can be read through
 * private index (0x6a) and data (0x6c) register.
 *
 * Returns private register value or negative error code.
 */
static unsigned int rt5640_index_read(
	struct snd_soc_codec *codec, unsigned int reg)
{
	int ret;

	ret = snd_soc_write(codec, RT5640_PRIV_INDEX, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to set private addr: %d\n", ret);
		return ret;
	}
	return snd_soc_read(codec, RT5640_PRIV_DATA);
}

/**
 * rt5640_index_update_bits - update private register bits
 * @codec: audio codec
 * @reg: Private register index.
 * @mask: register mask
 * @value: new value
 *
 * Writes new register value.
 *
 * Returns 1 for change, 0 for no change, or negative error code.
 */
static int rt5640_index_update_bits(struct snd_soc_codec *codec,
	unsigned int reg, unsigned int mask, unsigned int value)
{
	unsigned int old, new;
	int change, ret;

	ret = rt5640_index_read(codec, reg);
	if (ret < 0) {
		dev_err(codec->dev, "Failed to read private reg: %d\n", ret);
		goto err;
	}

	old = ret;
	new = (old & ~mask) | (value & mask);
	change = old != new;
	if (change) {
		ret = rt5640_index_write(codec, reg, new);
		if (ret < 0) {
			dev_err(codec->dev,
				"Failed to write private reg: %d\n", ret);
			goto err;
		}
	}
	return change;

err:
	return ret;
}

bool checkMclkError(struct snd_soc_codec *codec)
{
        int CodecValue1,CodecValue2;
        int valB8, val61;

	/* backup MX61 value */
	val61 = snd_soc_read(codec,RT5640_PWR_DIG1);
	/* enable ANC related power */
	snd_soc_update_bits(codec,RT5640_PWR_DIG1,0x9806, 0x9806);
	/* backup MXb8 value */
	valB8 = snd_soc_read(codec,RT5640_ANC_CTRL1);
	snd_soc_write(codec,RT5640_ANC_CTRL1,0x434b);

	CodecValue1 = rt5640_index_read(codec,0x2c);
	mdelay(1);
	CodecValue2 = rt5640_index_read(codec,0x2c);

	/* restore mxb8 and mx61 value*/
	snd_soc_write(codec,RT5640_ANC_CTRL1,valB8);
	snd_soc_write(codec,RT5640_PWR_DIG1,val61);

	if(CodecValue1==CodecValue2)
		return true;
	else
		return false;

}

static void mclk_check_work_handler(struct work_struct *work)
{
	struct snd_soc_codec *codec = rt5640_codec;

//	pr_debug("joe %s\n", __func__);

	if (checkMclkError(codec)) {
		pr_debug("\nNO MCLK\n");
		snd_soc_update_bits(codec, RT5640_SPK_VOL,
			RT5640_L_MUTE | RT5640_R_MUTE,
			RT5640_L_MUTE | RT5640_R_MUTE);
		snd_soc_update_bits(codec, RT5640_PWR_DIG1,
			RT5640_PWR_CLS_D, 0);
	} else {
		snd_soc_update_bits(codec, RT5640_PWR_DIG1,
			RT5640_PWR_CLS_D, RT5640_PWR_CLS_D);
		snd_soc_update_bits(codec, RT5640_SPK_VOL,
			RT5640_L_MUTE | RT5640_R_MUTE, 0);
	}
}

void mclk_check_timer_callback(unsigned long data )
{
	int ret = 0;

	schedule_work(&mclk_check_work);

	ret = mod_timer(&mclk_check_timer, jiffies + msecs_to_jiffies(2000));
	if (ret)
		pr_debug("Error in mod_timer\n");
}


static int rt5640_volatile_register(
	struct snd_soc_codec *codec, unsigned int reg)
{
	switch (reg) {
	case RT5640_RESET:
	case RT5640_PRIV_DATA:
	case RT5640_EQ_CTRL1:
	case RT5640_DRC_AGC_1:
	case RT5640_ANC_CTRL1:
	case RT5640_IRQ_CTRL2:
	case RT5640_INT_IRQ_ST:
	case RT5640_DSP_CTRL2:
	case RT5640_DSP_CTRL3:
	case RT5640_PGM_REG_ARR1:
	case RT5640_PGM_REG_ARR3:
	case RT5640_VENDOR_ID:
	case RT5640_VENDOR_ID1:
	case RT5640_VENDOR_ID2:
		return 1;
	default:
		return 0;
	}
}

static int rt5640_readable_register(
	struct snd_soc_codec *codec, unsigned int reg)
{
	switch (reg) {
	case RT5640_RESET:
	case RT5640_SPK_VOL:
	case RT5640_HP_VOL:
	case RT5640_OUTPUT:
	case RT5640_MONO_OUT:
	case RT5640_IN1_IN2:
	case RT5640_IN3_IN4:
	case RT5640_INL_INR_VOL:
	case RT5640_DAC1_DIG_VOL:
	case RT5640_DAC2_DIG_VOL:
	case RT5640_DAC2_CTRL:
	case RT5640_ADC_DIG_VOL:
	case RT5640_ADC_DATA:
	case RT5640_ADC_BST_VOL:
	case RT5640_STO_ADC_MIXER:
	case RT5640_MONO_ADC_MIXER:
	case RT5640_AD_DA_MIXER:
	case RT5640_STO_DAC_MIXER:
	case RT5640_MONO_DAC_MIXER:
	case RT5640_DIG_MIXER:
	case RT5640_DSP_PATH1:
	case RT5640_DSP_PATH2:
	case RT5640_DIG_INF_DATA:
	case RT5640_REC_L1_MIXER:
	case RT5640_REC_L2_MIXER:
	case RT5640_REC_R1_MIXER:
	case RT5640_REC_R2_MIXER:
	case RT5640_HPO_MIXER:
	case RT5640_SPK_L_MIXER:
	case RT5640_SPK_R_MIXER:
	case RT5640_SPO_L_MIXER:
	case RT5640_SPO_R_MIXER:
	case RT5640_SPO_CLSD_RATIO:
	case RT5640_MONO_MIXER:
	case RT5640_OUT_L1_MIXER:
	case RT5640_OUT_L2_MIXER:
	case RT5640_OUT_L3_MIXER:
	case RT5640_OUT_R1_MIXER:
	case RT5640_OUT_R2_MIXER:
	case RT5640_OUT_R3_MIXER:
	case RT5640_LOUT_MIXER:
	case RT5640_PWR_DIG1:
	case RT5640_PWR_DIG2:
	case RT5640_PWR_ANLG1:
	case RT5640_PWR_ANLG2:
	case RT5640_PWR_MIXER:
	case RT5640_PWR_VOL:
	case RT5640_PRIV_INDEX:
	case RT5640_PRIV_DATA:
	case RT5640_I2S1_SDP:
	case RT5640_I2S2_SDP:
	case RT5640_I2S3_SDP:
	case RT5640_ADDA_CLK1:
	case RT5640_ADDA_CLK2:
	case RT5640_DMIC:
	case RT5640_GLB_CLK:
	case RT5640_PLL_CTRL1:
	case RT5640_PLL_CTRL2:
	case RT5640_ASRC_1:
	case RT5640_ASRC_2:
	case RT5640_ASRC_3:
	case RT5640_ASRC_4:
	case RT5640_ASRC_5:
	case RT5640_HP_OVCD:
	case RT5640_CLS_D_OVCD:
	case RT5640_CLS_D_OUT:
	case RT5640_DEPOP_M1:
	case RT5640_DEPOP_M2:
	case RT5640_DEPOP_M3:
	case RT5640_CHARGE_PUMP:
	case RT5640_PV_DET_SPK_G:
	case RT5640_MICBIAS:
	case RT5640_EQ_CTRL1:
	case RT5640_EQ_CTRL2:
	case RT5640_WIND_FILTER:
	case RT5640_DRC_AGC_1:
	case RT5640_DRC_AGC_2:
	case RT5640_DRC_AGC_3:
	case RT5640_SVOL_ZC:
	case RT5640_ANC_CTRL1:
	case RT5640_ANC_CTRL2:
	case RT5640_ANC_CTRL3:
	case RT5640_JD_CTRL:
	case RT5640_ANC_JD:
	case RT5640_IRQ_CTRL1:
	case RT5640_IRQ_CTRL2:
	case RT5640_INT_IRQ_ST:
	case RT5640_GPIO_CTRL1:
	case RT5640_GPIO_CTRL2:
	case RT5640_GPIO_CTRL3:
	case RT5640_DSP_CTRL1:
	case RT5640_DSP_CTRL2:
	case RT5640_DSP_CTRL3:
	case RT5640_DSP_CTRL4:
	case RT5640_PGM_REG_ARR1:
	case RT5640_PGM_REG_ARR2:
	case RT5640_PGM_REG_ARR3:
	case RT5640_PGM_REG_ARR4:
	case RT5640_PGM_REG_ARR5:
	case RT5640_SCB_FUNC:
	case RT5640_SCB_CTRL:
	case RT5640_BASE_BACK:
	case RT5640_MP3_PLUS1:
	case RT5640_MP3_PLUS2:
	case RT5640_3D_HP:
	case RT5640_ADJ_HPF:
	case RT5640_HP_CALIB_AMP_DET:
	case RT5640_HP_CALIB2:
	case RT5640_SV_ZCD1:
	case RT5640_SV_ZCD2:
	case RT5640_GEN_CTRL1:
	case RT5640_GEN_CTRL2:
	case RT5640_GEN_CTRL3:
	case RT5640_VENDOR_ID:
	case RT5640_VENDOR_ID1:
	case RT5640_VENDOR_ID2:
	case RT5640_DUMMY_PR3F:
		return 1;
	default:
		return 0;
	}
}

void DC_Calibrate(struct snd_soc_codec *codec)
{
	unsigned int sclk_src;

	sclk_src = snd_soc_read(codec, RT5640_GLB_CLK) &
		RT5640_SCLK_SRC_MASK;

	snd_soc_update_bits(codec, RT5640_PWR_ANLG2,
		RT5640_PWR_MB1, RT5640_PWR_MB1);
	snd_soc_update_bits(codec, RT5640_DEPOP_M2,
                RT5640_DEPOP_MASK, RT5640_DEPOP_MAN);
        snd_soc_update_bits(codec, RT5640_DEPOP_M1,
                RT5640_HP_CP_MASK | RT5640_HP_SG_MASK | RT5640_HP_CB_MASK,
                RT5640_HP_CP_PU | RT5640_HP_SG_DIS | RT5640_HP_CB_PU);

	snd_soc_update_bits(codec, RT5640_GLB_CLK,
		RT5640_SCLK_SRC_MASK, 0x2 << RT5640_SCLK_SRC_SFT);

        rt5640_index_write(codec, RT5640_HP_DCC_INT1, 0x9f00);
	snd_soc_update_bits(codec, RT5640_PWR_ANLG2,
		RT5640_PWR_MB1, 0);
	snd_soc_update_bits(codec, RT5640_GLB_CLK,
		RT5640_SCLK_SRC_MASK, sclk_src);
        /* Headset touch sounds playback DEPOP */
        snd_soc_write(codec, RT5640_DEPOP_M1, 0x0009);
        snd_soc_write(codec, RT5640_DEPOP_M2, 0x3100);
        snd_soc_write(codec, RT5640_CHARGE_PUMP, 0x0f00);	
}

/**
 * rt5640_headset_detect - Detect headset.
 * @codec: SoC audio codec device.
 * @jack_insert: Jack insert or not.
 *
 * Detect whether is headset or not when jack inserted.
 *
 * Returns detect status.
 */
int rt5640_headset_detect(struct snd_soc_codec *codec, int jack_insert)
{
	int jack_type;
	int sclk_src;

	if(jack_insert) {
		if (SND_SOC_BIAS_OFF == codec->dapm.bias_level) {
			snd_soc_write(codec, RT5640_PWR_ANLG1, 0x2004);
			snd_soc_write(codec, RT5640_MICBIAS, 0x3830);
			snd_soc_update_bits(codec, RT5640_GEN_CTRL1 , 0x1, 0x1);
			// 0410 Oder : move RT5640_GB_CLK here
			sclk_src = snd_soc_read(codec, RT5640_GLB_CLK) &
				RT5640_SCLK_SRC_MASK;
			snd_soc_update_bits(codec, RT5640_GLB_CLK,
				RT5640_SCLK_SRC_MASK, 0x3 << RT5640_SCLK_SRC_SFT);
		}
		// joe_cheng : fix TT#269616 : noise occurs when jack is inserting during playback
		//             In RT5640's design, this clock can be used on jack detection. And MCLK 
		//             will have to change to internal clock. But in ME302C, we don't use the jack
		//             detection Realtek provided.  This clock change will lead to noise occured. 
		//sclk_src = snd_soc_read(codec, RT5640_GLB_CLK) &
		//	RT5640_SCLK_SRC_MASK;
		//snd_soc_update_bits(codec, RT5640_GLB_CLK,
		//	RT5640_SCLK_SRC_MASK, 0x3 << RT5640_SCLK_SRC_SFT);
		snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
			RT5640_PWR_LDO2, RT5640_PWR_LDO2);
		snd_soc_update_bits(codec, RT5640_PWR_ANLG2,
			RT5640_PWR_MB1, RT5640_PWR_MB1);
		snd_soc_update_bits(codec, RT5640_MICBIAS,
			RT5640_MIC1_OVCD_MASK | RT5640_MIC1_OVTH_MASK |
			RT5640_PWR_CLK25M_MASK | RT5640_PWR_MB_MASK,
			RT5640_MIC1_OVCD_EN | RT5640_MIC1_OVTH_600UA |
			RT5640_PWR_MB_PU | RT5640_PWR_CLK25M_PU);
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1,
			0x1, 0x1);
		msleep(100);
		if (snd_soc_read(codec, RT5640_IRQ_CTRL2) & 0x8) {
			headphone_retry_time++;
			jack_type = RT5640_HEADPHO_DET;
		}
		else
			jack_type = RT5640_HEADSET_DET;
		
		snd_soc_update_bits(codec, RT5640_IRQ_CTRL2,
			RT5640_MB1_OC_CLR, 0);
		// 0410 Oder
		if (SND_SOC_BIAS_OFF == codec->dapm.bias_level) {
			snd_soc_update_bits(codec, RT5640_GLB_CLK,
				RT5640_SCLK_SRC_MASK, sclk_src);
		}

		// joe_cheng : Workaround for TT#331186, TT#333088
                if (PROJ_ID != PROJ_ID_ME372CG) {
			// joe_cheng : wrokaround for TT#548615
#ifdef USE_EQ   
			hp_event_eq_val = snd_soc_read(codec, RT5640_EQ_CTRL2);
			snd_soc_update_bits(rt5640_codec, RT5640_EQ_CTRL2, RT5640_EQ_CTRL_MASK, 0);
			snd_soc_update_bits(rt5640_codec, RT5640_EQ_CTRL1,
					RT5640_EQ_UPD, RT5640_EQ_UPD);
			snd_soc_update_bits(rt5640_codec, RT5640_EQ_CTRL1, RT5640_EQ_UPD, 0);
#endif
		    if (jack_type == RT5640_HEADPHO_DET) {
		 	if (headphone_retry_time < 3 && 
			   ((snd_soc_read(codec, RT5640_PWR_DIG1) & 0x0800) != 0 || (snd_soc_read(codec, RT5640_PWR_DIG1) & 0x1000) != 0))
				snd_soc_update_bits(codec, RT5640_PWR_DIG1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1, 0x0);
                                schedule_delayed_work(&DAC_unmute_work,msecs_to_jiffies(800));
		    } 
		    if (jack_type == RT5640_HEADSET_DET) {
			if ((snd_soc_read(codec, RT5640_PWR_DIG1) & 0x0800) != 0 || (snd_soc_read(codec, RT5640_PWR_DIG1) & 0x1000) != 0)
				snd_soc_update_bits(codec, RT5640_PWR_DIG1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1, 0x0);
                                schedule_delayed_work(&DAC_unmute_work,msecs_to_jiffies(800));
		    }
                }
	} else {
		snd_soc_update_bits(codec, RT5640_MICBIAS,
			RT5640_MIC1_OVCD_MASK,
			RT5640_MIC1_OVCD_DIS);
                if (PROJ_ID != PROJ_ID_ME175CG)
		snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
			RT5640_PWR_LDO2, 0);
		
                cancel_delayed_work_sync(&DAC_unmute_work);
		jack_type = RT5640_NO_JACK;
		headphone_retry_time = 0;
		dac_is_unmute = 0;
	}

	return jack_type;
}
EXPORT_SYMBOL(rt5640_headset_detect);

// joe_cheng  : enable micbias when rt5640 resume

int rt5640_enable_micbias(int headset_devices)
{
	headset_status = headset_devices;
	pr_debug("%s headset status %d\n", __func__, headset_status);
	return 0;
}
EXPORT_SYMBOL(rt5640_enable_micbias);

static const char *rt5640_dacr2_src[] = { "TxDC_R", "TxDP_R" };

static const SOC_ENUM_SINGLE_DECL(rt5640_dacr2_enum,RT5640_DUMMY_PR3F,
	14, rt5640_dacr2_src);
static const struct snd_kcontrol_new rt5640_dacr2_mux =
	SOC_DAPM_ENUM("Mono dacr source", rt5640_dacr2_enum);

static const DECLARE_TLV_DB_SCALE(out_vol_tlv, -4650, 150, 0);
static const DECLARE_TLV_DB_SCALE(dac_vol_tlv, -65625, 375, 0);
static const DECLARE_TLV_DB_SCALE(in_vol_tlv, -3450, 150, 0);
static const DECLARE_TLV_DB_SCALE(adc_vol_tlv, -17625, 375, 0);
static const DECLARE_TLV_DB_SCALE(adc_bst_tlv, 0, 1200, 0);



static const DECLARE_TLV_DB_SCALE(ctt_tlv, 0, 37, 0);
static const DECLARE_TLV_DB_SCALE(btt_tlv, 0, 150, 0);
static const DECLARE_TLV_DB_SCALE(dtt_tlv, 0, 300, 0);
static const DECLARE_TLV_DB_SCALE(ett_tlv, -8200, 150, 0);     



/* {0, +20, +24, +30, +35, +40, +44, +50, +52} dB */
static unsigned int bst_tlv[] = {
	TLV_DB_RANGE_HEAD(7),
	0, 0, TLV_DB_SCALE_ITEM(0, 0, 0),
	1, 1, TLV_DB_SCALE_ITEM(2000, 0, 0),
	2, 2, TLV_DB_SCALE_ITEM(2400, 0, 0),
	3, 5, TLV_DB_SCALE_ITEM(3000, 500, 0),
	6, 6, TLV_DB_SCALE_ITEM(4400, 0, 0),
	7, 7, TLV_DB_SCALE_ITEM(5000, 0, 0),
	8, 8, TLV_DB_SCALE_ITEM(5200, 0, 0),
};

static int rt5640_dmic_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = rt5640->dmic_en;

	return 0;
}

static int rt5640_scenario_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

	ucontrol->value.integer.value[0] = Hal_user_scenario_mode;
	printk("rt5640_scenario_get: value %d\n",Hal_user_scenario_mode);

	return 0;
}

static int rt5640_scenario_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
#ifdef CONFIG_A400CG
        unsigned int val;
#endif

	pr_info("rt5640_scenario_put: value %d\n",ucontrol->value.integer.value[0]);

	if (Hal_user_scenario_mode  == ucontrol->value.integer.value[0]) {
		pr_info("rt5640_scenario_put: return since mode not change %d\n",ucontrol->value.integer.value[0]);
		return 0;
	}

	Hal_user_scenario_mode = ucontrol->value.integer.value[0];

        if(PROJ_ID == PROJ_ID_A400CG || PROJ_ID == PROJ_ID_ME175CG)
            rt5640_update_eqmode(codec, NONE, NONE);

#ifdef CONFIG_A400CG
        val = snd_soc_read(codec, RT5640_STO_DAC_MIXER) & 0x4040;
        snd_soc_update_bits(codec, RT5640_STO_DAC_MIXER, 0x4040, 0x4040);
#ifdef USE_EQ
        if(Hal_user_scenario_mode == RINGTONE) {
            RingTone_coming = true;
            //rt5640_update_eqmode(codec, Hal_user_scenario_mode, A12_SPEAKER);
	    schedule_delayed_work(&ringtone_drc_work, msecs_to_jiffies(1200));
        } else if(RingTone_coming) {
            RingTone_coming = false;
            if(Hal_user_scenario_mode == RECORD) {
                if(gpio_get_value(JACK_DETECT_PIN) != 0) { //Board-mic DMIC
                      snd_soc_write(codec, RT5640_DRC_AGC_1, 0xe226);
                      snd_soc_write(codec, RT5640_DRC_AGC_2, 0x33b2);
                      snd_soc_write(codec, RT5640_DRC_AGC_3, 0xa557);
                } else { //Headset-mic AMIC
                      snd_soc_write(codec, RT5640_DRC_AGC_1, 0x2206);
                      snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                      snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);
                }
                printk("%s : update agc for recording after ringtone\n", __func__);
            } else if(Hal_user_scenario_mode == PLAYBACK) {
                //rt5640_update_eqmode(codec, Hal_user_scenario_mode, A12_SPEAKER);
                snd_soc_write(codec, RT5640_DRC_AGC_1, 0x620d);
                snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1faa);
                snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);
            } else {
                snd_soc_write(codec, RT5640_DRC_AGC_1, 0x2206);
                snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);
            }

        }
#endif
        snd_soc_update_bits(codec, RT5640_STO_DAC_MIXER, 0x4040, val);
#endif
	return 0;
}

static int rt5640_bt_switch_get(struct snd_kcontrol *kcontrol,
                struct snd_ctl_elem_value *ucontrol)
{
        struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
        struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

        ucontrol->value.integer.value[0] = bt_switch_ctrl;
        printk("rt5640_bt_switch_get: value %d\n", bt_switch_ctrl);

        return 0;
}

static int rt5640_bt_switch_put(struct snd_kcontrol *kcontrol,
                struct snd_ctl_elem_value *ucontrol)
{
        struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
        struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

        printk("rt5640_bt_switch_put: value %d\n",ucontrol->value.integer.value[0]);

        if (bt_switch_ctrl == ucontrol->value.integer.value[0])
                return 0;

        bt_switch_ctrl = ucontrol->value.integer.value[0];

        /* Configure I2S HW switch for audio route */
        int gpio_value = 0;

        /* Get GPIO status */
        gpio_value = gpio_get_value(CLV_I2S1_SEL);
        pr_debug("I2S HW switch gpio_value=0x%x\n", gpio_value);
        if (bt_switch_ctrl) {
                if (!gpio_value) {
                        /*Set GPIO O(H) to BT */
                        gpio_set_value(CLV_I2S1_SEL, 1);
                        pr_info("I2S HW switch O(H) to BT 0x%x\n",
                                gpio_get_value(CLV_I2S1_SEL));
                }
        } else {
                if (gpio_value) {
                        /*Set GPIO O(L) to Codec */
                        gpio_set_value(CLV_I2S1_SEL, 0);
                        pr_info("I2S HW switch O(L) to Codec 0x%x\n",
                                gpio_get_value(CLV_I2S1_SEL));
                }
        }

        return 0;
}

static int rt5640_csv_call_get(struct snd_kcontrol *kcontrol,
                struct snd_ctl_elem_value *ucontrol)
{
        struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
        struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

        ucontrol->value.integer.value[0] = is_csv_call;
        printk("rt5640_csv_call_get: value %d\n", is_csv_call);

        return 0;
}

static int rt5640_csv_call_put(struct snd_kcontrol *kcontrol,
                struct snd_ctl_elem_value *ucontrol)
{
        struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
        struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

        printk("rt5640_csv_call_put: value %d\n",ucontrol->value.integer.value[0]);

        if (is_csv_call  == ucontrol->value.integer.value[0])
                return 0;

        is_csv_call = ucontrol->value.integer.value[0];

        return 0;
}

static int rt5640_dmic_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

	if (rt5640->dmic_en == ucontrol->value.integer.value[0])
		return 0;

	rt5640->dmic_en = ucontrol->value.integer.value[0];
	switch (rt5640->dmic_en) {
	case RT5640_DMIC_DIS:
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK | RT5640_GP3_PIN_MASK |
			RT5640_GP4_PIN_MASK,
			RT5640_GP2_PIN_GPIO2 | RT5640_GP3_PIN_GPIO3 |
			RT5640_GP4_PIN_GPIO4);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_1_DP_MASK | RT5640_DMIC_2_DP_MASK,
			RT5640_DMIC_1_DP_GPIO3 | RT5640_DMIC_2_DP_GPIO4);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_1_EN_MASK | RT5640_DMIC_2_EN_MASK,
			RT5640_DMIC_1_DIS | RT5640_DMIC_2_DIS);
		break;

	case RT5640_DMIC1:
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK | RT5640_GP3_PIN_MASK,
			RT5640_GP2_PIN_DMIC1_SCL | RT5640_GP3_PIN_DMIC1_SDA);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_1L_LH_MASK | RT5640_DMIC_1R_LH_MASK |
			RT5640_DMIC_1_DP_MASK,
			RT5640_DMIC_1L_LH_RISING | RT5640_DMIC_1R_LH_FALLING |
			RT5640_DMIC_1_DP_IN1P);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_1_EN_MASK, RT5640_DMIC_1_EN);
		break;

	case RT5640_DMIC2:
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK | RT5640_GP4_PIN_MASK,
			RT5640_GP2_PIN_DMIC1_SCL | RT5640_GP4_PIN_DMIC2_SDA);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_2L_LH_MASK | RT5640_DMIC_2R_LH_MASK |
			RT5640_DMIC_2_DP_MASK,
			RT5640_DMIC_2L_LH_FALLING | RT5640_DMIC_2R_LH_RISING |
			RT5640_DMIC_2_DP_IN1N);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_2_EN_MASK, RT5640_DMIC_2_EN);
		break;

	default:
		return -EINVAL;
	}

	return 0;
}


/* IN1/IN2 Input Type */
static const char *rt5640_input_mode[] = {
	"Single ended", "Differential"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_in1_mode_enum, RT5640_IN1_IN2,
	RT5640_IN_SFT1, rt5640_input_mode);

static const SOC_ENUM_SINGLE_DECL(
	rt5640_in2_mode_enum, RT5640_IN3_IN4,
	RT5640_IN_SFT2, rt5640_input_mode);

/* Interface data select */
static const char *rt5640_data_select[] = {
	"Normal", "Swap", "left copy to right", "right copy to left"};

static const SOC_ENUM_SINGLE_DECL(rt5640_if1_dac_enum, RT5640_DIG_INF_DATA,
				RT5640_IF1_DAC_SEL_SFT, rt5640_data_select);

static const SOC_ENUM_SINGLE_DECL(rt5640_if1_adc_enum, RT5640_DIG_INF_DATA,
				RT5640_IF1_ADC_SEL_SFT, rt5640_data_select);

static const SOC_ENUM_SINGLE_DECL(rt5640_if2_dac_enum, RT5640_DIG_INF_DATA,
				RT5640_IF2_DAC_SEL_SFT, rt5640_data_select);

static const SOC_ENUM_SINGLE_DECL(rt5640_if2_adc_enum, RT5640_DIG_INF_DATA,
				RT5640_IF2_ADC_SEL_SFT, rt5640_data_select);

static const SOC_ENUM_SINGLE_DECL(rt5640_if3_dac_enum, RT5640_DIG_INF_DATA,
				RT5640_IF3_DAC_SEL_SFT, rt5640_data_select);

static const SOC_ENUM_SINGLE_DECL(rt5640_if3_adc_enum, RT5640_DIG_INF_DATA,
				RT5640_IF3_ADC_SEL_SFT, rt5640_data_select);

/* Class D speaker gain ratio */
static const char *rt5640_clsd_spk_ratio[] = {"1.66x", "1.83x", "1.94x", "2x",
	"2.11x", "2.22x", "2.33x", "2.44x", "2.55x", "2.66x", "2.77x"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_clsd_spk_ratio_enum, RT5640_CLS_D_OUT,
	RT5640_CLSD_RATIO_SFT, rt5640_clsd_spk_ratio);

/* SPO  speaker gain ctrl */
static const char *rt5640_spo_gain_ratio[] = {"-6dB", "-4.5dB", "-3dB", "-1.5dB",
	"0dB", "0.83dB", "1.58dB", "2.22dB"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_spo_gain_ratio_enum, RT5640_SPO_CLSD_RATIO,
	RT5640_SPO_CLSD_RATIO_SFT, rt5640_spo_gain_ratio);

/* SPKMIXL DAC L1 Gain Control */
static const char *rt5640_spkmix_dac1_gain[] = {"0dB", "-3dB", "-6dB", "-9dB"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_spkmixl_dacl1_gain_enum, RT5640_SPK_L_MIXER,
	RT5640_G_DAC_L1_SM_L_SFT, rt5640_spkmix_dac1_gain);

/* SPKMIXL DAC R1 Gain Control */
static const SOC_ENUM_SINGLE_DECL(
	rt5640_spkmixr_dacr1_gain_enum, RT5640_SPK_R_MIXER,
	RT5640_G_DAC_R1_SM_R_SFT, rt5640_spkmix_dac1_gain);

/* DMIC */
static const char *rt5640_dmic_mode[] = {"Disable", "DMIC1", "DMIC2"};

static const SOC_ENUM_SINGLE_DECL(rt5640_dmic_enum, 0, 0, rt5640_dmic_mode);
 // Jericho eq
static const char *rt5640_scenario_mode[] = {"None", "Playback", "Record", "Voip", "RingTone", "VR", "Voice"};

static const SOC_ENUM_SINGLE_DECL(rt5640_user_scenario_enum, 0, 0, rt5640_scenario_mode);

//BT hardware switch
static const char *rt5640_bt_switch_mode[] = {"Codec", "BT"};

static const SOC_ENUM_SINGLE_DECL(rt5640_user_bt_switch_enum, 0, 0, rt5640_bt_switch_mode);

//Is CSV Call
static const char *rt5640_csv_call_mode[] = {"Off", "On"};

static const SOC_ENUM_SINGLE_DECL(rt5640_user_csv_call_enum, 0, 0, rt5640_csv_call_mode);

#ifdef RT5640_REG_RW
#define REGVAL_MAX 0xffff
static unsigned int regctl_addr;
static int rt5640_regctl_info(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_info *uinfo)
{
	uinfo->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	uinfo->count = 2;
	uinfo->value.integer.min = 0;
	uinfo->value.integer.max = REGVAL_MAX;
	return 0;
}

static int rt5640_regctl_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	ucontrol->value.integer.value[0] = regctl_addr;
	ucontrol->value.integer.value[1] = snd_soc_read(codec, regctl_addr);
	return 0;
}

static int rt5640_regctl_put(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	regctl_addr = ucontrol->value.integer.value[0];
	if(ucontrol->value.integer.value[1] <= REGVAL_MAX)
		snd_soc_write(codec, regctl_addr, ucontrol->value.integer.value[1]);
	return 0;
}
#endif


static int rt5640_vol_rescale_get(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned int val = snd_soc_read(codec, mc->reg);

        if(PROJ_ID == PROJ_ID_ME175CG) {
          ucontrol->value.integer.value[0] = ME175CG_RT5640_VOL_RSCL_MAX -
                ((val & RT5640_L_VOL_MASK) >> mc->shift);
          ucontrol->value.integer.value[1] = ME175CG_RT5640_VOL_RSCL_MAX -
                (val & RT5640_R_VOL_MASK);
        }
        else if(PROJ_ID == PROJ_ID_ME372CL) {
          ucontrol->value.integer.value[0] = ME372CL_RT5640_VOL_RSCL_MAX -
                ((val & RT5640_L_VOL_MASK) >> mc->shift);
          ucontrol->value.integer.value[1] = ME372CL_RT5640_VOL_RSCL_MAX -
                (val & RT5640_R_VOL_MASK);
	}
	else { 
          ucontrol->value.integer.value[0] = RT5640_VOL_RSCL_MAX -
		((val & RT5640_L_VOL_MASK) >> mc->shift);
	  ucontrol->value.integer.value[1] = RT5640_VOL_RSCL_MAX -
		(val & RT5640_R_VOL_MASK);
        }
	return 0;
}

static int rt5640_vol_rescale_put(struct snd_kcontrol *kcontrol,
		struct snd_ctl_elem_value *ucontrol)
{
	struct soc_mixer_control *mc =
		(struct soc_mixer_control *)kcontrol->private_value;
	struct snd_soc_codec *codec = snd_kcontrol_chip(kcontrol);
	unsigned int val, val2;

        if(PROJ_ID == PROJ_ID_ME175CG) {
          val = ME175CG_RT5640_VOL_RSCL_MAX - ucontrol->value.integer.value[0];
          val2 = ME175CG_RT5640_VOL_RSCL_MAX - ucontrol->value.integer.value[1];
        }
        else if(PROJ_ID == PROJ_ID_ME372CL) {
          val = ME372CL_RT5640_VOL_RSCL_MAX - ucontrol->value.integer.value[0];
          val2 = ME372CL_RT5640_VOL_RSCL_MAX - ucontrol->value.integer.value[1];
        }
        else {
	  val = RT5640_VOL_RSCL_MAX - ucontrol->value.integer.value[0];
	  val2 = RT5640_VOL_RSCL_MAX - ucontrol->value.integer.value[1];
        }
	return snd_soc_update_bits_locked(codec, mc->reg, RT5640_L_VOL_MASK |
			RT5640_R_VOL_MASK, val << mc->shift | val2);
}


static const struct snd_kcontrol_new rt5640_snd_controls[] = {
	/* Speaker Output Volume */
	SOC_DOUBLE("Speaker Playback Switch", RT5640_SPK_VOL,
		RT5640_L_MUTE_SFT, RT5640_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE_EXT_TLV("Speaker Playback Volume", RT5640_SPK_VOL,
		RT5640_L_VOL_SFT, RT5640_R_VOL_SFT, RT5640_VOL_RSCL_RANGE, 0,
		rt5640_vol_rescale_get, rt5640_vol_rescale_put, out_vol_tlv),
	/* Headphone Output Volume */
	SOC_DOUBLE("HP Playback Switch", RT5640_HP_VOL,
		RT5640_L_MUTE_SFT, RT5640_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE_EXT_TLV("HP Playback Volume", RT5640_HP_VOL,
		RT5640_L_VOL_SFT, RT5640_R_VOL_SFT, RT5640_VOL_RSCL_RANGE, 0,
		rt5640_vol_rescale_get, rt5640_vol_rescale_put, out_vol_tlv),
	/* OUTPUT Control */
	SOC_DOUBLE("OUT Playback Switch", RT5640_OUTPUT,
		RT5640_L_MUTE_SFT, RT5640_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE("OUT Channel Switch", RT5640_OUTPUT,
		RT5640_VOL_L_SFT, RT5640_VOL_R_SFT, 1, 1),
	SOC_DOUBLE_TLV("OUT Playback Volume", RT5640_OUTPUT,
		RT5640_L_VOL_SFT, RT5640_R_VOL_SFT, 39, 1, out_vol_tlv),
	/* MONO Output Control */
	SOC_SINGLE("Mono Playback Switch", RT5640_MONO_OUT,
				RT5640_L_MUTE_SFT, 1, 1),
	/* DAC Digital Volume */
	SOC_DOUBLE("DAC2 Playback Switch", RT5640_DAC2_CTRL,
		RT5640_M_DAC_L2_VOL_SFT, RT5640_M_DAC_R2_VOL_SFT, 1, 1),
	SOC_DOUBLE_TLV("DAC1 Playback Volume", RT5640_DAC1_DIG_VOL,
			RT5640_L_VOL_SFT, RT5640_R_VOL_SFT,
			175, 0, dac_vol_tlv),
	SOC_DOUBLE_TLV("Mono DAC Playback Volume", RT5640_DAC2_DIG_VOL,
			RT5640_L_VOL_SFT, RT5640_R_VOL_SFT,
			175, 0, dac_vol_tlv),
	/* IN1/IN2 Control */
	SOC_ENUM("IN1 Mode Control",  rt5640_in1_mode_enum),
	SOC_SINGLE_TLV("IN1 Boost", RT5640_IN1_IN2,
		RT5640_BST_SFT1, 8, 0, bst_tlv),
	SOC_ENUM("IN3 Mode Control",  rt5640_in1_mode_enum),
	SOC_SINGLE_TLV("IN3 Boost", RT5640_IN1_IN2,
		RT5640_BST_SFT2, 8, 0, bst_tlv),
	SOC_ENUM("IN2 Mode Control", rt5640_in2_mode_enum),
	SOC_SINGLE_TLV("IN2 Boost", RT5640_IN3_IN4,
		RT5640_BST_SFT2, 8, 0, bst_tlv),
	/* INL/INR Volume Control */
	SOC_DOUBLE_TLV("IN Capture Volume", RT5640_INL_INR_VOL,
			RT5640_INL_VOL_SFT, RT5640_INR_VOL_SFT,
			31, 1, in_vol_tlv),
	/* ADC Digital Volume Control */
	SOC_DOUBLE("ADC Capture Switch", RT5640_ADC_DIG_VOL,
		RT5640_L_MUTE_SFT, RT5640_R_MUTE_SFT, 1, 1),
	SOC_DOUBLE_TLV("ADC Capture Volume", RT5640_ADC_DIG_VOL,
			RT5640_L_VOL_SFT, RT5640_R_VOL_SFT,
			127, 0, adc_vol_tlv),
	SOC_DOUBLE_TLV("Mono ADC Capture Volume", RT5640_ADC_DATA,
			RT5640_L_VOL_SFT, RT5640_R_VOL_SFT,
			127, 0, adc_vol_tlv),
	/* ADC Boost Volume Control */
	SOC_DOUBLE_TLV("ADC Boost Gain", RT5640_ADC_BST_VOL,
			RT5640_ADC_L_BST_SFT, RT5640_ADC_R_BST_SFT,
			3, 0, adc_bst_tlv),
	/* Class D speaker gain ratio */
	SOC_ENUM("Class D SPK Ratio Control", rt5640_clsd_spk_ratio_enum),

	// Jericho eq
	SOC_ENUM_EXT("Scenario", rt5640_user_scenario_enum,
			rt5640_scenario_get, rt5640_scenario_put),

        //BT SCO hardware switch control 
        SOC_ENUM_EXT("BT Switch", rt5640_user_bt_switch_enum,
                        rt5640_bt_switch_get, rt5640_bt_switch_put),

        //CSV Call
        SOC_ENUM_EXT("CsvCall", rt5640_user_csv_call_enum,
                        rt5640_csv_call_get, rt5640_csv_call_put),

	SOC_ENUM("SPOMIX GAIN CTRL", rt5640_spo_gain_ratio_enum),
	SOC_ENUM("ADC IF1 Data Switch", rt5640_if1_adc_enum),
	SOC_ENUM("DAC IF1 Data Switch", rt5640_if1_dac_enum),
	SOC_ENUM("ADC IF2 Data Switch", rt5640_if2_adc_enum),
	SOC_ENUM("DAC IF2 Data Switch", rt5640_if2_dac_enum),
	SOC_ENUM("SPKMIXL DACL1 GAIN CTRL", rt5640_spkmixl_dacl1_gain_enum),
	SOC_ENUM("SPKMIXR DACR1 GAIN CTRL", rt5640_spkmixr_dacr1_gain_enum),
#ifdef RT5640_REG_RW
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Register Control",
		.info = rt5640_regctl_info,
		.get = rt5640_regctl_get,
		.put = rt5640_regctl_put,
	},
#endif
};

/**
 * set_dmic_clk - Set parameter of dmic.
 *
 * @w: DAPM widget.
 * @kcontrol: The kcontrol of this widget.
 * @event: Event id.
 *
 * Choose dmic clock between 1MHz and 3MHz.
 * It is better for clock to approximate 3MHz.
 */
static int set_dmic_clk(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	int div[] = {2, 3, 4, 6, 8, 12}, idx = -EINVAL, i, rate, red, bound, temp;

	rate = rt5640->lrck[rt5640->aif_pu] << 8;
	red = 3000000 * 12;
	for (i = 0; i < ARRAY_SIZE(div); i++) {
		bound = div[i] * 3000000;
		if (rate > bound)
			continue;
		temp = bound - rate;
		if (temp < red) {
			red = temp;
			idx = i;
		}
	}
	if (idx < 0)
		dev_err(codec->dev, "Failed to set DMIC clock\n");
	else {
#ifdef USE_ASRC
		idx = 4;
#endif
		snd_soc_update_bits(codec, RT5640_DMIC, RT5640_DMIC_CLK_MASK,
					idx << RT5640_DMIC_CLK_SFT);
	}

	return idx;
}


static int check_sysclk1_source(struct snd_soc_dapm_widget *source,
			 struct snd_soc_dapm_widget *sink)
{
	unsigned int val;

	val = snd_soc_read(source->codec, RT5640_GLB_CLK);
	val &= RT5640_SCLK_SRC_MASK;
	if (val == RT5640_SCLK_SRC_PLL1)
		return 1;
	else
		return 0;
}

/* Digital Mixer */
static const struct snd_kcontrol_new rt5640_sto_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5640_STO_ADC_MIXER,
			RT5640_M_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5640_STO_ADC_MIXER,
			RT5640_M_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_sto_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5640_STO_ADC_MIXER,
			RT5640_M_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5640_STO_ADC_MIXER,
			RT5640_M_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_mono_adc_l_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5640_MONO_ADC_MIXER,
			RT5640_M_MONO_ADC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5640_MONO_ADC_MIXER,
			RT5640_M_MONO_ADC_L2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_mono_adc_r_mix[] = {
	SOC_DAPM_SINGLE("ADC1 Switch", RT5640_MONO_ADC_MIXER,
			RT5640_M_MONO_ADC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("ADC2 Switch", RT5640_MONO_ADC_MIXER,
			RT5640_M_MONO_ADC_R2_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_dac_l_mix[] = {
	SOC_DAPM_SINGLE("Stereo ADC Switch", RT5640_AD_DA_MIXER,
			RT5640_M_ADCMIX_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("INF1 Switch", RT5640_AD_DA_MIXER,
			RT5640_M_IF1_DAC_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_dac_r_mix[] = {
	SOC_DAPM_SINGLE("Stereo ADC Switch", RT5640_AD_DA_MIXER,
			RT5640_M_ADCMIX_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("INF1 Switch", RT5640_AD_DA_MIXER,
			RT5640_M_IF1_DAC_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_sto_dac_l_mix[] = {
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_STO_DAC_MIXER,
			RT5640_M_DAC_L1_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_STO_DAC_MIXER,
			RT5640_M_DAC_L2_SFT, 1, 1),
	SOC_DAPM_SINGLE("ANC Switch", RT5640_STO_DAC_MIXER,
			RT5640_M_ANC_DAC_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_sto_dac_r_mix[] = {
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_STO_DAC_MIXER,
			RT5640_M_DAC_R1_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_STO_DAC_MIXER,
			RT5640_M_DAC_R2_SFT, 1, 1),
	SOC_DAPM_SINGLE("ANC Switch", RT5640_STO_DAC_MIXER,
			RT5640_M_ANC_DAC_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_mono_dac_l_mix[] = {
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_MONO_DAC_MIXER,
			RT5640_M_DAC_L1_MONO_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_MONO_DAC_MIXER,
			RT5640_M_DAC_L2_MONO_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_MONO_DAC_MIXER,
			RT5640_M_DAC_R2_MONO_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_mono_dac_r_mix[] = {
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_MONO_DAC_MIXER,
			RT5640_M_DAC_R1_MONO_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_MONO_DAC_MIXER,
			RT5640_M_DAC_R2_MONO_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_MONO_DAC_MIXER,
			RT5640_M_DAC_L2_MONO_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_dig_l_mix[] = {
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_DIG_MIXER,
			RT5640_M_STO_L_DAC_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_DIG_MIXER,
			RT5640_M_DAC_L2_DAC_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_dig_r_mix[] = {
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_DIG_MIXER,
			RT5640_M_STO_R_DAC_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_DIG_MIXER,
			RT5640_M_DAC_R2_DAC_R_SFT, 1, 1),
};

/* Analog Input Mixer */
static const struct snd_kcontrol_new rt5640_rec_l_mix[] = {
	SOC_DAPM_SINGLE("HPOL Switch", RT5640_REC_L2_MIXER,
			RT5640_M_HP_L_RM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("INL Switch", RT5640_REC_L2_MIXER,
			RT5640_M_IN_L_RM_L_SFT, 1, 1),
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	SOC_DAPM_SINGLE("BST4 Switch", RT5640_REC_L2_MIXER,
			RT5640_M_BST4_RM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST3 Switch", RT5640_REC_L2_MIXER,
			RT5640_M_BST3_RM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST2 Switch", RT5640_REC_L2_MIXER,
			RT5640_M_BST2_RM_L_SFT, 1, 1),
#else
	SOC_DAPM_SINGLE("BST3 Switch", RT5640_REC_L2_MIXER,
			RT5640_M_BST2_RM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST2 Switch", RT5640_REC_L2_MIXER,
			RT5640_M_BST4_RM_L_SFT, 1, 1),
#endif
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_REC_L2_MIXER,
			RT5640_M_BST1_RM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUT MIXL Switch", RT5640_REC_L2_MIXER,
			RT5640_M_OM_L_RM_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_rec_r_mix[] = {
	SOC_DAPM_SINGLE("HPOR Switch", RT5640_REC_R2_MIXER,
			RT5640_M_HP_R_RM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("INR Switch", RT5640_REC_R2_MIXER,
			RT5640_M_IN_R_RM_R_SFT, 1, 1),
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	SOC_DAPM_SINGLE("BST4 Switch", RT5640_REC_R2_MIXER,
			RT5640_M_BST4_RM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST3 Switch", RT5640_REC_R2_MIXER,
			RT5640_M_BST3_RM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST2 Switch", RT5640_REC_R2_MIXER,
			RT5640_M_BST2_RM_R_SFT, 1, 1),
#else
	SOC_DAPM_SINGLE("BST3 Switch", RT5640_REC_R2_MIXER,
			RT5640_M_BST2_RM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST2 Switch", RT5640_REC_R2_MIXER,
			RT5640_M_BST4_RM_R_SFT, 1, 1),
#endif
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_REC_R2_MIXER,
			RT5640_M_BST1_RM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUT MIXR Switch", RT5640_REC_R2_MIXER,
			RT5640_M_OM_R_RM_R_SFT, 1, 1),
};

/* Analog Output Mixer */
static const struct snd_kcontrol_new rt5640_spk_l_mix[] = {
	SOC_DAPM_SINGLE("REC MIXL Switch", RT5640_SPK_L_MIXER,
			RT5640_M_RM_L_SM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("INL Switch", RT5640_SPK_L_MIXER,
			RT5640_M_IN_L_SM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_SPK_L_MIXER,
			RT5640_M_DAC_L1_SM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_SPK_L_MIXER,
			RT5640_M_DAC_L2_SM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUT MIXL Switch", RT5640_SPK_L_MIXER,
			RT5640_M_OM_L_SM_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_spk_r_mix[] = {
	SOC_DAPM_SINGLE("REC MIXR Switch", RT5640_SPK_R_MIXER,
			RT5640_M_RM_R_SM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("INR Switch", RT5640_SPK_R_MIXER,
			RT5640_M_IN_R_SM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_SPK_R_MIXER,
			RT5640_M_DAC_R1_SM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_SPK_R_MIXER,
			RT5640_M_DAC_R2_SM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUT MIXR Switch", RT5640_SPK_R_MIXER,
			RT5640_M_OM_R_SM_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_out_l_mix[] = {
	SOC_DAPM_SINGLE("SPK MIXL Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_SM_L_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST3 Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_BST2_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_BST1_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("INL Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_IN_L_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("REC MIXL Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_RM_L_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_DAC_R2_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_DAC_L2_OM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_OUT_L3_MIXER,
			RT5640_M_DAC_L1_OM_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_out_r_mix[] = {
	SOC_DAPM_SINGLE("SPK MIXR Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_SM_L_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST3 Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_BST2_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST2 Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_BST4_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_BST1_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("INR Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_IN_R_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("REC MIXR Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_RM_R_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_DAC_L2_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_DAC_R2_OM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_OUT_R3_MIXER,
			RT5640_M_DAC_R1_OM_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_spo_l_mix[] = {
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_SPO_L_MIXER,
			RT5640_M_DAC_R1_SPM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_SPO_L_MIXER,
			RT5640_M_DAC_L1_SPM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("SPKVOL R Switch", RT5640_SPO_L_MIXER,
			RT5640_M_SV_R_SPM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("SPKVOL L Switch", RT5640_SPO_L_MIXER,
			RT5640_M_SV_L_SPM_L_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_SPO_L_MIXER,
			RT5640_M_BST1_SPM_L_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_spo_r_mix[] = {
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_SPO_R_MIXER,
			RT5640_M_DAC_R1_SPM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("SPKVOL R Switch", RT5640_SPO_R_MIXER,
			RT5640_M_SV_R_SPM_R_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_SPO_R_MIXER,
			RT5640_M_BST1_SPM_R_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_hpo_mix[] = {
	SOC_DAPM_SINGLE("DAC2 Switch", RT5640_HPO_MIXER,
			RT5640_M_DAC2_HM_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC1 Switch", RT5640_HPO_MIXER,
			RT5640_M_DAC1_HM_SFT, 1, 1),
	SOC_DAPM_SINGLE("HPVOL Switch", RT5640_HPO_MIXER,
			RT5640_M_HPVOL_HM_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_lout_mix[] = {
	SOC_DAPM_SINGLE("DAC L1 Switch", RT5640_LOUT_MIXER,
			RT5640_M_DAC_L1_LM_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC R1 Switch", RT5640_LOUT_MIXER,
			RT5640_M_DAC_R1_LM_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUTVOL L Switch", RT5640_LOUT_MIXER,
			RT5640_M_OV_L_LM_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUTVOL R Switch", RT5640_LOUT_MIXER,
			RT5640_M_OV_R_LM_SFT, 1, 1),
};

static const struct snd_kcontrol_new rt5640_mono_mix[] = {
	SOC_DAPM_SINGLE("DAC R2 Switch", RT5640_MONO_MIXER,
			RT5640_M_DAC_R2_MM_SFT, 1, 1),
	SOC_DAPM_SINGLE("DAC L2 Switch", RT5640_MONO_MIXER,
			RT5640_M_DAC_L2_MM_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUTVOL R Switch", RT5640_MONO_MIXER,
			RT5640_M_OV_R_MM_SFT, 1, 1),
	SOC_DAPM_SINGLE("OUTVOL L Switch", RT5640_MONO_MIXER,
			RT5640_M_OV_L_MM_SFT, 1, 1),
	SOC_DAPM_SINGLE("BST1 Switch", RT5640_MONO_MIXER,
			RT5640_M_BST1_MM_SFT, 1, 1),
};

/* INL/R source */
static const char *rt5640_inl_src[] = {"IN2P", "MonoP"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_inl_enum, RT5640_INL_INR_VOL,
	RT5640_INL_SEL_SFT, rt5640_inl_src);

static const struct snd_kcontrol_new rt5640_inl_mux =
	SOC_DAPM_ENUM("INL source", rt5640_inl_enum);

static const char *rt5640_inr_src[] = {"IN2N", "MonoN"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_inr_enum, RT5640_INL_INR_VOL,
	RT5640_INR_SEL_SFT, rt5640_inr_src);

static const struct snd_kcontrol_new rt5640_inr_mux =
	SOC_DAPM_ENUM("INR source", rt5640_inr_enum);

/* Stereo ADC source */
static const char *rt5640_stereo_adc1_src[] = {"DIG MIX", "ADC"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_stereo_adc1_enum, RT5640_STO_ADC_MIXER,
	RT5640_ADC_1_SRC_SFT, rt5640_stereo_adc1_src);

static const struct snd_kcontrol_new rt5640_sto_adc_1_mux =
	SOC_DAPM_ENUM("Stereo ADC1 source", rt5640_stereo_adc1_enum);

static const char *rt5640_stereo_adc2_src[] = {"DMIC1", "DMIC2", "DIG MIX"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_stereo_adc2_enum, RT5640_STO_ADC_MIXER,
	RT5640_ADC_2_SRC_SFT, rt5640_stereo_adc2_src);

static const struct snd_kcontrol_new rt5640_sto_adc_2_mux =
	SOC_DAPM_ENUM("Stereo ADC2 source", rt5640_stereo_adc2_enum);

/* Mono ADC source */
static const char *rt5640_mono_adc_l1_src[] = {"Mono DAC MIXL", "ADCL"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_mono_adc_l1_enum, RT5640_MONO_ADC_MIXER,
	RT5640_MONO_ADC_L1_SRC_SFT, rt5640_mono_adc_l1_src);

static const struct snd_kcontrol_new rt5640_mono_adc_l1_mux =
	SOC_DAPM_ENUM("Mono ADC1 left source", rt5640_mono_adc_l1_enum);

static const char *rt5640_mono_adc_l2_src[] =
	{"DMIC L1", "DMIC L2", "Mono DAC MIXL"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_mono_adc_l2_enum, RT5640_MONO_ADC_MIXER,
	RT5640_MONO_ADC_L2_SRC_SFT, rt5640_mono_adc_l2_src);

static const struct snd_kcontrol_new rt5640_mono_adc_l2_mux =
	SOC_DAPM_ENUM("Mono ADC2 left source", rt5640_mono_adc_l2_enum);

static const char *rt5640_mono_adc_r1_src[] = {"Mono DAC MIXR", "ADCR"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_mono_adc_r1_enum, RT5640_MONO_ADC_MIXER,
	RT5640_MONO_ADC_R1_SRC_SFT, rt5640_mono_adc_r1_src);

static const struct snd_kcontrol_new rt5640_mono_adc_r1_mux =
	SOC_DAPM_ENUM("Mono ADC1 right source", rt5640_mono_adc_r1_enum);

static const char *rt5640_mono_adc_r2_src[] =
	{"DMIC R1", "DMIC R2", "Mono DAC MIXR"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_mono_adc_r2_enum, RT5640_MONO_ADC_MIXER,
	RT5640_MONO_ADC_R2_SRC_SFT, rt5640_mono_adc_r2_src);

static const struct snd_kcontrol_new rt5640_mono_adc_r2_mux =
	SOC_DAPM_ENUM("Mono ADC2 right source", rt5640_mono_adc_r2_enum);

/* DAC2 channel source */
static const char *rt5640_dac_l2_src[] = {"IF2", "IF3", "TxDC", "Base L/R"};

static const SOC_ENUM_SINGLE_DECL(rt5640_dac_l2_enum, RT5640_DSP_PATH2,
				RT5640_DAC_L2_SEL_SFT, rt5640_dac_l2_src);

static const struct snd_kcontrol_new rt5640_dac_l2_mux =
	SOC_DAPM_ENUM("DAC2 left channel source", rt5640_dac_l2_enum);

static const char *rt5640_dac_r2_src[] = {"IF2", "IF3", "TxDC"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_dac_r2_enum, RT5640_DSP_PATH2,
	RT5640_DAC_R2_SEL_SFT, rt5640_dac_r2_src);

static const struct snd_kcontrol_new rt5640_dac_r2_mux =
	SOC_DAPM_ENUM("DAC2 right channel source", rt5640_dac_r2_enum);

/* Interface 2  ADC channel source */
static const char *rt5640_if2_adc_l_src[] = {"TxDP", "Mono ADC MIXL"};

static const SOC_ENUM_SINGLE_DECL(rt5640_if2_adc_l_enum, RT5640_DSP_PATH2,
			RT5640_IF2_ADC_L_SEL_SFT, rt5640_if2_adc_l_src);

static const struct snd_kcontrol_new rt5640_if2_adc_l_mux =
	SOC_DAPM_ENUM("IF2 ADC left channel source", rt5640_if2_adc_l_enum);

static const char *rt5640_if2_adc_r_src[] = {"TxDP", "Mono ADC MIXR"};

static const SOC_ENUM_SINGLE_DECL(rt5640_if2_adc_r_enum, RT5640_DSP_PATH2,
			RT5640_IF2_ADC_R_SEL_SFT, rt5640_if2_adc_r_src);

static const struct snd_kcontrol_new rt5640_if2_adc_r_mux =
	SOC_DAPM_ENUM("IF2 ADC right channel source", rt5640_if2_adc_r_enum);

/* digital interface and iis interface map */
static const char *rt5640_dai_iis_map[] = {"1:1|2:2|3:3", "1:1|2:3|3:2",
	"1:3|2:1|3:2", "1:3|2:2|3:1", "1:2|2:3|3:1",
	"1:2|2:1|3:3", "1:1|2:1|3:3", "1:2|2:2|3:3"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_dai_iis_map_enum, RT5640_I2S1_SDP,
	RT5640_I2S_IF_SFT, rt5640_dai_iis_map);

static const struct snd_kcontrol_new rt5640_dai_mux =
	SOC_DAPM_ENUM("DAI select", rt5640_dai_iis_map_enum);

/* SDI select */
static const char *rt5640_sdi_sel[] = {"IF1", "IF2"};

static const SOC_ENUM_SINGLE_DECL(
	rt5640_sdi_sel_enum, RT5640_I2S2_SDP,
	RT5640_I2S2_SDI_SFT, rt5640_sdi_sel);

static const struct snd_kcontrol_new rt5640_sdi_mux =
	SOC_DAPM_ENUM("SDI select", rt5640_sdi_sel_enum);

static int rt5640_adc_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	unsigned int val, mask;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
                if (PROJ_ID == PROJ_ID_GEMINI) {
#if defined(CONFIG_ME302C_SPC_PWR_SET)			
                        gpio_set_value(PIN_3VSUS_EN, 1);
                        pr_debug("%s : 3v pull high %d\n", __func__, gpio_get_value(PIN_3VSUS_EN));
#endif

                        snd_soc_write(codec, RT5640_DRC_AGC_1, 0xe206);
                        snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                        snd_soc_write(codec, RT5640_DRC_AGC_3, 0x6046);
                }

                // Enable the micbias and LDO for ME175CG Board-mic
                if (PROJ_ID == PROJ_ID_ME175CG) {
                        if(gpio_get_value(JACK_DETECT_PIN) != 0) {
                          snd_soc_write(codec, RT5640_DRC_AGC_1, 0xe226);
                          snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                          snd_soc_write(codec, RT5640_DRC_AGC_3, 0xa657);
                        }
                        snd_soc_update_bits(codec, RT5640_PWR_ANLG1,RT5640_PWR_LDO2, RT5640_PWR_LDO2);
                        snd_soc_update_bits(codec, RT5640_PWR_ANLG2, RT5640_PWR_MB1, RT5640_PWR_MB1);
                        gpio_set_value(MIC2_BIAS_EN, 1);
                }

                if (PROJ_ID == PROJ_ID_PF400CG || PROJ_ID == PROJ_ID_A400CG) {
#ifdef USE_EQ		
		  if (output_device_from_hal() == 0x40000) {
//			rt5640_update_drc_agc_mode(codec, Hal_user_scenario_mode, P72G_MIC, true);
			snd_soc_write(codec, RT5640_DRC_AGC_1, 0xe226);
			snd_soc_write(codec, RT5640_DRC_AGC_2, 0x33b1);
			snd_soc_write(codec, RT5640_DRC_AGC_3, 0xa557);			
		  } /*else if (output_device_from_hal() == 0x100000) {
			//rt5640_update_drc_agc_mode(codec, Hal_user_scenario_mode, HEADSET_MIC, true);
			snd_soc_write(codec, RT5640_DRC_AGC_1, 0xe226);
			snd_soc_write(codec, RT5640_DRC_AGC_2, 0x33b2);
			snd_soc_write(codec, RT5640_DRC_AGC_3, 0xa557);			
		  }*/
#endif
                }
		rt5640_index_update_bits(codec,
			RT5640_CHOP_DAC_ADC, 0x1000, 0x1000);
#ifdef CONFIG_EEPROM_PADSTATION
		AX_MicroP_setGPIOOutputPin(OUT_uP_AUD_PWR_EN, 1);
		pr_info("%s : PMU set AUD_PWR_EN on", __func__);
#endif
		break;

	case SND_SOC_DAPM_POST_PMD:
                if (PROJ_ID == PROJ_ID_GEMINI) {
#if defined(CONFIG_ME302C_SPC_PWR_SET)			
                        gpio_set_value(PIN_3VSUS_EN, 0);
                        pr_debug("%s : 3v pull low %d\n", __func__, gpio_get_value(PIN_3VSUS_EN));
#endif

                        snd_soc_write(codec, RT5640_DRC_AGC_1, 0x2206);
                        snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                        snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);
                }

                // Disable the micbias and LDO for ME175CG Board-mic
                if (PROJ_ID == PROJ_ID_ME175CG) {
                        if(gpio_get_value(JACK_DETECT_PIN) != 0) {
                          snd_soc_update_bits(codec, RT5640_PWR_ANLG1,RT5640_PWR_LDO2, 0);
                          snd_soc_update_bits(codec, RT5640_PWR_ANLG2, RT5640_PWR_MB1, 0);
                          snd_soc_write(codec, RT5640_DRC_AGC_1, 0x2206);
                          snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                          snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);
                        }
                        gpio_set_value(MIC2_BIAS_EN, 0);
                }
     
                if (PROJ_ID == PROJ_ID_PF400CG || PROJ_ID == PROJ_ID_A400CG) {
#ifdef USE_EQ		
		  snd_soc_write(codec, RT5640_DRC_AGC_1, 0x2206);
		  snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
		  snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);
#endif
	/*	
		  if (output_device_from_hal() == 0x40000) {
			rt5640_update_drc_agc_mode(codec, Hal_user_scenario_mode, P72G_MIC, false);
			//rt5640_update_drc_agc_mode(codec, NONE, P72G_MIC, false);
		  } else if (output_device_from_hal() == 0x100000) {
			rt5640_update_drc_agc_mode(codec, Hal_user_scenario_mode, HEADSET_MIC, false);
			//rt5640_update_drc_agc_mode(codec, NONE, HEADSET_MIC, false);
		  }
	*/
                }
		rt5640_index_update_bits(codec,
			RT5640_CHOP_DAC_ADC, 0x1000, 0x0000);
#ifdef CONFIG_EEPROM_PADSTATION
		AX_MicroP_setGPIOOutputPin(OUT_uP_AUD_PWR_EN, 0);
		pr_info("%s : PMD set AUD_PWR_EN off", __func__);
#endif
		break;

	default:
		return 0;
	}

	return 0;
}

int rt5640_sto_adcl_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
		case SND_SOC_DAPM_POST_PMU:
			snd_soc_update_bits(codec, RT5640_ADC_DIG_VOL,
				RT5640_L_MUTE, 0);
			break;
		case SND_SOC_DAPM_PRE_PMD:
			snd_soc_update_bits(codec, RT5640_ADC_DIG_VOL,
				RT5640_L_MUTE,
				RT5640_L_MUTE);
			break;
	
		default:
			return 0;
	}
	
	return 0;
}
	
static int rt5640_sto_adcr_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
		
	switch (event) {
		case SND_SOC_DAPM_POST_PMU:
			snd_soc_update_bits(codec, RT5640_ADC_DIG_VOL,
					RT5640_R_MUTE, 0);
			break;
		case SND_SOC_DAPM_PRE_PMD:
			snd_soc_update_bits(codec, RT5640_ADC_DIG_VOL,
					RT5640_R_MUTE,
					RT5640_R_MUTE);
			break;
		default:
			return 0;
	}
	return 0;
}

static int rt5640_mono_adcl_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	unsigned int val, mask;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1,
			RT5640_M_MAMIX_L, 0);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1,
			RT5640_M_MAMIX_L,
			RT5640_M_MAMIX_L);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5640_mono_adcr_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;
	unsigned int val, mask;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1,
			RT5640_M_MAMIX_R, 0);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1,
			RT5640_M_MAMIX_R,
			RT5640_M_MAMIX_R);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5640_spk_l_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	pr_debug("%s event %d\n", __func__, event);
	switch (event) {
		case SND_SOC_DAPM_POST_PMU:
#ifdef USE_EQ
                        if (PROJ_ID == PROJ_ID_ME175CG) {
                                if (Hal_user_scenario_mode != VOICE) {
                                        rt5640_update_eqmode(codec, PLAYBACK, A12_SPEAKER);
                                }
                        }

			if (PROJ_ID == PROJ_ID_PF400CG || PROJ_ID == PROJ_ID_A400CG) {
				if (Hal_user_scenario_mode == RINGTONE) {
					pr_debug("%s : rt5640_update_eqmode RINGTONE\n", __func__, Hal_user_scenario_mode);
				        rt5640_update_eqmode(codec, RINGTONE, A12_SPEAKER);
					schedule_delayed_work(&ringtone_drc_work, msecs_to_jiffies(50));
				} else if (Hal_user_scenario_mode == PLAYBACK) {
					pr_debug("%s : rt5640_update_eqmode PLAYBACK\n", __func__, Hal_user_scenario_mode);
				        rt5640_update_eqmode(codec, PLAYBACK, A12_SPEAKER);
					rt5640_update_drc_agc_mode(codec, PLAYBACK, A12_SPEAKER);
				}
                        }

                        if (PROJ_ID == PROJ_ID_ME372CG) {
                          if (Hal_user_scenario_mode == RINGTONE) {
                                snd_soc_write(codec, RT5640_DRC_AGC_1, 0x6408);
                                snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1fe8);
                          }
                        }
#endif
			schedule_delayed_work(&spk_amp_l_work,
					msecs_to_jiffies(50));
			break;

		case SND_SOC_DAPM_PRE_PMD:
#ifdef USE_EQ
                        if (PROJ_ID == PROJ_ID_ME175CG) {
                                 rt5640_update_eqmode(codec, NONE, NONE);
                        }

			if (PROJ_ID == PROJ_ID_PF400CG || PROJ_ID == PROJ_ID_A400CG) {
				rt5640_update_eqmode(codec, NONE, NONE);
				if(Hal_user_scenario_mode == RECORD) {
					pr_info("%s : don't update drc for ringtone\n", __func__);
				} else {
					rt5640_update_drc_agc_mode(codec, NONE, A12_SPEAKER);
				}
			}

                        if (PROJ_ID == PROJ_ID_ME372CG) {
                         if (Hal_user_scenario_mode == RINGTONE) {
                                snd_soc_write(codec, RT5640_DRC_AGC_1, 0x0206);
                                snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
                          }
                        }
#endif
			cancel_delayed_work_sync(&spk_amp_l_work);
			snd_soc_update_bits(codec, RT5640_PWR_DIG1,
					RT5640_PWR_CLS_D, 0);
			snd_soc_update_bits(codec, RT5640_SPK_VOL,
					RT5640_L_MUTE, RT5640_L_MUTE);
			break;

		default:
			return 0;
	}

	return 0;
}

static int rt5640_spk_r_event(struct snd_soc_dapm_widget *w,
		struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
		case SND_SOC_DAPM_POST_PMU:
                        if (PROJ_ID == PROJ_ID_ME372CL){
                           schedule_delayed_work(&spk_amp_r_work,
                                           msecs_to_jiffies(50));
                        }
                        else{  
			   schedule_delayed_work(&spk_amp_r_work,
					   msecs_to_jiffies(1000));
                        }
			break;

		case SND_SOC_DAPM_PRE_PMD:
			cancel_delayed_work_sync(&spk_amp_r_work);
			snd_soc_update_bits(codec, RT5640_PWR_DIG1,
					RT5640_PWR_CLS_D, 0);
			snd_soc_update_bits(codec, RT5640_SPK_VOL,
					RT5640_R_MUTE, RT5640_R_MUTE);
			break;

		default:
			return 0;
	}

	return 0;
}

static int rt5640_set_dmic1_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK | RT5640_GP3_PIN_MASK,
			RT5640_GP2_PIN_DMIC1_SCL | RT5640_GP3_PIN_DMIC1_SDA);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_1L_LH_MASK | RT5640_DMIC_1R_LH_MASK |
			RT5640_DMIC_1_DP_MASK,
			RT5640_DMIC_1L_LH_RISING | RT5640_DMIC_1R_LH_FALLING |
			RT5640_DMIC_1_DP_IN1P);
	default:
		return 0;
	}

	return 0;
}

static int rt5640_set_dmic2_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		pr_info("%s : Hal_user_scenario_mode %d\n", __func__, Hal_user_scenario_mode);
		//rt5640_update_drc_agc_mode(codec, Hal_user_scenario_mode, A12_MIC, true);
#ifdef USE_EQ		
		snd_soc_write(codec, RT5640_DRC_AGC_1, 0xe226);
		snd_soc_write(codec, RT5640_DRC_AGC_2, 0x33b2);
		snd_soc_write(codec, RT5640_DRC_AGC_3, 0xa557);			
#endif
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK | RT5640_GP4_PIN_MASK,
			RT5640_GP2_PIN_DMIC1_SCL | RT5640_GP4_PIN_DMIC2_SDA);
		// joe_cheng : Fix TT#342847, 345953, voice call update link noise
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_2L_LH_MASK | RT5640_DMIC_2R_LH_MASK |
			RT5640_DMIC_2_DP_MASK,
			RT5640_DMIC_2L_LH_FALLING | RT5640_DMIC_2R_LH_FALLING |
			RT5640_DMIC_2_DP_IN1N);
		break;
	case SND_SOC_DAPM_POST_PMD:
		//rt5640_update_drc_agc_mode(codec, NONE, A12_MIC, false);
#ifdef USE_EQ		
		snd_soc_write(codec, RT5640_DRC_AGC_1, 0x0206);
		snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
		snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);			
#endif
		break;
	default:
		return 0;
	}

	return 0;
}

#if USE_ONEBIT_DEPOP
void hp_amp_power(struct snd_soc_codec *codec, int on)
{
	static int hp_amp_power_count;
//	pr_debug("one bit hp_amp_power on=%d hp_amp_power_count=%d\n",on,hp_amp_power_count);

	if(on) {
		if(hp_amp_power_count <= 0) {
			/* depop parameters */
			rt5640_index_update_bits(codec, RT5640_CHPUMP_INT_REG1,0x0700, 0x0200);
			snd_soc_update_bits(codec, RT5640_DEPOP_M2,
				RT5640_DEPOP_MASK, RT5640_DEPOP_MAN);
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_HP_CP_MASK | RT5640_HP_SG_MASK | RT5640_HP_CB_MASK,
				RT5640_HP_CP_PU | RT5640_HP_SG_DIS | RT5640_HP_CB_PU);
			/* headphone amp power on */
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_FV1 | RT5640_PWR_FV2, 0);
			msleep(5);
			
			snd_soc_update_bits(codec, RT5640_PWR_VOL,
				RT5640_PWR_HV_L | RT5640_PWR_HV_R,
				RT5640_PWR_HV_L | RT5640_PWR_HV_R);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_HP_L | RT5640_PWR_HP_R | RT5640_PWR_HA,
				RT5640_PWR_HP_L | RT5640_PWR_HP_R | RT5640_PWR_HA);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_FV1 | RT5640_PWR_FV2 ,
				RT5640_PWR_FV1 | RT5640_PWR_FV2 );
			snd_soc_update_bits(codec, RT5640_DEPOP_M2,
				RT5640_DEPOP_MASK | RT5640_DIG_DP_MASK,
				RT5640_DEPOP_AUTO | RT5640_DIG_DP_EN);
			snd_soc_update_bits(codec, RT5640_CHARGE_PUMP,
				RT5640_PM_HP_MASK, RT5640_PM_HP_HV);
			snd_soc_update_bits(codec, RT5640_DEPOP_M3,
				RT5640_CP_FQ1_MASK | RT5640_CP_FQ2_MASK | RT5640_CP_FQ3_MASK,
				(RT5640_CP_FQ_192_KHZ << RT5640_CP_FQ1_SFT) |
				(RT5640_CP_FQ_24_KHZ << RT5640_CP_FQ2_SFT) |
				(RT5640_CP_FQ_192_KHZ << RT5640_CP_FQ3_SFT));
			rt5640_index_write(codec, RT5640_MAMP_INT_REG2, 0x1c00);
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_HP_CP_MASK | RT5640_HP_SG_MASK,
				RT5640_HP_CP_PD | RT5640_HP_SG_EN);
			rt5640_index_update_bits(codec, RT5640_CHPUMP_INT_REG1,0x0700, 0x0400);
		}
		hp_amp_power_count++;
	} else {
		hp_amp_power_count--;
		if(hp_amp_power_count <= 0) {
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_HP_CB_MASK, RT5640_HP_CB_PD);
                        //Alex: Mark for fix #TT294679: Get pop sound when closing alarm
			//msleep(30);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_HP_L | RT5640_PWR_HP_R | RT5640_PWR_HA,
				0);
			snd_soc_write(codec, RT5640_DEPOP_M2, 0x3100);
		}
	}
}

static void rt5640_pmu_depop(struct snd_soc_codec *codec)
{
	hp_amp_power(codec, 1);
	/* headphone unmute sequence */
	msleep(5);
        // TT#323685: solve the headset pop noise issue
//        schedule_delayed_work(&hp_amp_work,msecs_to_jiffies(90));

	// joe_cheng : Remove hp_amp_work for TT#353209, and the pop noise could be avoid by mute DAC
        if ((snd_soc_read(rt5640_codec, RT5640_PWR_DIG1) & 0x18c0) == 0) {
		snd_soc_update_bits(rt5640_codec, RT5640_PWR_DIG1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1, 
		                                                   RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1);
	}
	msleep(5);
        snd_soc_update_bits(rt5640_codec, RT5640_HP_VOL,
	                RT5640_L_MUTE | RT5640_R_MUTE, 0);	
}

static void rt5640_pmd_depop(struct snd_soc_codec *codec)
{
//        cancel_delayed_work_sync(&hp_amp_work);
	snd_soc_update_bits(codec, RT5640_DEPOP_M3,
		RT5640_CP_FQ1_MASK | RT5640_CP_FQ2_MASK | RT5640_CP_FQ3_MASK,
		(RT5640_CP_FQ_96_KHZ << RT5640_CP_FQ1_SFT) |
		(RT5640_CP_FQ_12_KHZ << RT5640_CP_FQ2_SFT) |
		(RT5640_CP_FQ_96_KHZ << RT5640_CP_FQ3_SFT));
	rt5640_index_write(codec, RT5640_MAMP_INT_REG2, 0x7c00);
	
	snd_soc_update_bits(codec, RT5640_HP_VOL,
		RT5640_L_MUTE | RT5640_R_MUTE,
		RT5640_L_MUTE | RT5640_R_MUTE);
        //Alex: Mark for fix #TT294679: Get pop sound when closing alarm
	//msleep(50);
	hp_amp_power(codec, 0);	
}

#else //seq
void hp_amp_power(struct snd_soc_codec *codec, int on)
{
	static int hp_amp_power_count;
//	pr_debug("hp_amp_power on=%d hp_amp_power_count=%d\n",on,hp_amp_power_count);

	if(on) {
		if(hp_amp_power_count <= 0) {
			/* depop parameters */
			rt5640_index_update_bits(codec, RT5640_CHPUMP_INT_REG1,0x0700, 0x0200);
			/*
			snd_soc_update_bits(codec, RT5640_DEPOP_M2,
				RT5640_DEPOP_MASK, RT5640_DEPOP_MAN);
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_HP_CP_MASK | RT5640_HP_SG_MASK | RT5640_HP_CB_MASK,
				RT5640_HP_CP_PU | RT5640_HP_SG_DIS | RT5640_HP_CB_PU);
			*/
			snd_soc_write(codec, RT5640_DEPOP_M2, 0x3100);
			snd_soc_write(codec, RT5640_DEPOP_M1, 0x0009);
			/* headphone amp power on */
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_FV1 | RT5640_PWR_FV2 , 0);
			snd_soc_update_bits(codec, RT5640_PWR_VOL,
				RT5640_PWR_HV_L | RT5640_PWR_HV_R,
				RT5640_PWR_HV_L | RT5640_PWR_HV_R);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_HP_L | RT5640_PWR_HP_R | RT5640_PWR_HA,
				RT5640_PWR_HP_L | RT5640_PWR_HP_R | RT5640_PWR_HA);
			msleep(5);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_FV1 | RT5640_PWR_FV2,
				RT5640_PWR_FV1 | RT5640_PWR_FV2);
				
			snd_soc_update_bits(codec, RT5640_CHARGE_PUMP,
				RT5640_PM_HP_MASK, RT5640_PM_HP_HV);
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_HP_CO_MASK | RT5640_HP_SG_MASK,
				RT5640_HP_CO_EN | RT5640_HP_SG_EN);
			rt5640_index_update_bits(codec, RT5640_CHPUMP_INT_REG1,0x0700, 0x0400);
		}
		hp_amp_power_count++;
	} else {
		hp_amp_power_count--;
		if(hp_amp_power_count <= 0) {
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_HP_SG_MASK | RT5640_HP_L_SMT_MASK |
				RT5640_HP_R_SMT_MASK, RT5640_HP_SG_DIS |
				RT5640_HP_L_SMT_DIS | RT5640_HP_R_SMT_DIS);
			/* headphone amp power down */
			snd_soc_update_bits(codec, RT5640_DEPOP_M1,
				RT5640_SMT_TRIG_MASK | RT5640_HP_CD_PD_MASK |
				RT5640_HP_CO_MASK | RT5640_HP_CP_MASK |
				RT5640_HP_SG_MASK | RT5640_HP_CB_MASK,
				RT5640_SMT_TRIG_DIS | RT5640_HP_CD_PD_EN |
				RT5640_HP_CO_DIS | RT5640_HP_CP_PD |
				RT5640_HP_SG_EN | RT5640_HP_CB_PD);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_HP_L | RT5640_PWR_HP_R | RT5640_PWR_HA,
				0);
		}
	}
}

static void rt5640_pmu_depop(struct snd_soc_codec *codec)
{
	hp_amp_power(codec, 1);
	schedule_delayed_work(&hp_amp_work,msecs_to_jiffies(90));
	/* headphone unmute sequence */
	/*
	snd_soc_update_bits(codec, RT5640_DEPOP_M3,
		RT5640_CP_FQ1_MASK | RT5640_CP_FQ2_MASK | RT5640_CP_FQ3_MASK,
		(RT5640_CP_FQ_192_KHZ << RT5640_CP_FQ1_SFT) |
		(RT5640_CP_FQ_12_KHZ << RT5640_CP_FQ2_SFT) |
		(RT5640_CP_FQ_192_KHZ << RT5640_CP_FQ3_SFT));
	rt5640_index_write(codec, RT5640_MAMP_INT_REG2, 0xfc00);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_SMT_TRIG_MASK, RT5640_SMT_TRIG_EN);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_RSTN_MASK, RT5640_RSTN_EN);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_RSTN_MASK | RT5640_HP_L_SMT_MASK | RT5640_HP_R_SMT_MASK,
		RT5640_RSTN_DIS | RT5640_HP_L_SMT_EN | RT5640_HP_R_SMT_EN);
	snd_soc_update_bits(codec, RT5640_HP_VOL,
		RT5640_L_MUTE | RT5640_R_MUTE, 0);
	msleep(40);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_HP_SG_MASK | RT5640_HP_L_SMT_MASK |
		RT5640_HP_R_SMT_MASK, RT5640_HP_SG_DIS |
		RT5640_HP_L_SMT_DIS | RT5640_HP_R_SMT_DIS);
	*/

}

static void rt5640_pmd_depop(struct snd_soc_codec *codec)
{
	cancel_delayed_work_sync(&hp_amp_work);
	/* headphone mute sequence */
	snd_soc_update_bits(codec, RT5640_DEPOP_M3,
		RT5640_CP_FQ1_MASK | RT5640_CP_FQ2_MASK | RT5640_CP_FQ3_MASK,
		(RT5640_CP_FQ_96_KHZ << RT5640_CP_FQ1_SFT) |
		(RT5640_CP_FQ_12_KHZ << RT5640_CP_FQ2_SFT) |
		(RT5640_CP_FQ_96_KHZ << RT5640_CP_FQ3_SFT));
	rt5640_index_write(codec, RT5640_MAMP_INT_REG2, 0xfc00);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_HP_SG_MASK, RT5640_HP_SG_EN);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_RSTP_MASK, RT5640_RSTP_EN);
	snd_soc_update_bits(codec, RT5640_DEPOP_M1,
		RT5640_RSTP_MASK | RT5640_HP_L_SMT_MASK |
		RT5640_HP_R_SMT_MASK, RT5640_RSTP_DIS |
		RT5640_HP_L_SMT_EN | RT5640_HP_R_SMT_EN);

	snd_soc_update_bits(codec, RT5640_HP_VOL,
		RT5640_L_MUTE | RT5640_R_MUTE, RT5640_L_MUTE | RT5640_R_MUTE);
	msleep(30);

	hp_amp_power(codec, 0);
}
#endif

static int rt5640_hp_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
//Jericho eq	
#ifdef USE_EQ   
//		rt5640_update_eqmode(codec, Hal_user_scenario_mode, HEADPHONE);
#endif
		rt5640_pmu_depop(codec);
		break;

	case SND_SOC_DAPM_PRE_PMD:
#ifdef USE_EQ   
//		rt5640_update_eqmode(codec, NONE, NONE);
#endif
		rt5640_pmd_depop(codec);
		break;

	default:
		return 0;
	}
	
	return 0;
}

static int rt5640_mono_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(codec, RT5640_MONO_OUT,
				RT5640_L_MUTE, 0);
		break;

	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(codec, RT5640_MONO_OUT,
			RT5640_L_MUTE, RT5640_L_MUTE);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5640_lout_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		hp_amp_power(codec,1);
		snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
			RT5640_PWR_LM, RT5640_PWR_LM);
#ifdef USE_EQ
		rt5640_update_eqmode(codec, Hal_user_scenario_mode, A12_SPEAKER);
		if (Hal_user_scenario_mode == RINGTONE) {
			snd_soc_write(codec, RT5640_DRC_AGC_1, 0x6006);
			snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1fe8);
			snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);			
		}
#endif
#ifdef CONFIG_EEPROM_PADSTATION		
		pr_debug("%s: Hal_user_scenario_mode %d\n", __func__, Hal_user_scenario_mode);
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1, 0x4000, 0x4000);
		if (AX_MicroP_getHWID() == 3) { // MP
			if (snd_soc_read(codec, RT5640_OUTPUT) == 0x0808 && output_device_from_hal() == 0x1) {
				if (Hal_user_scenario_mode == 6 ) {
					pr_debug("%s : Update MX-03 to 0000\n", __func__);
					snd_soc_write(codec, RT5640_OUTPUT, 0x0000);
				} else if (Hal_user_scenario_mode == 3) {
					pr_debug("%s : Update MX-03 to 0404\n", __func__);
					snd_soc_write(codec, RT5640_OUTPUT, 0x0404);
				}
			} 
		} 
#endif
		break;

	case SND_SOC_DAPM_PRE_PMD:
#ifdef USE_EQ
		rt5640_update_eqmode(codec, NONE, NONE);
		if (Hal_user_scenario_mode == RINGTONE) {
			snd_soc_write(codec, RT5640_DRC_AGC_1, 0x0206);
			snd_soc_write(codec, RT5640_DRC_AGC_2, 0x1f00);
			snd_soc_write(codec, RT5640_DRC_AGC_3, 0x0000);			
		} else if(Hal_user_scenario_mode == RECORD) {
			printk("%s : don't update drc for ringtone\n", __func__);
		}
#endif
#ifdef CONFIG_EEPROM_PADSTATION		
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1, 0x4000, 0x0000);
#endif
		snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
			RT5640_PWR_LM, 0);
		hp_amp_power(codec,0);
		break;

	default:
		return 0;
	}

	return 0;
}

static int rt5640_index_sync_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
		rt5640_index_write(codec, RT5640_MIXER_INT_REG, snd_soc_read(codec,RT5640_DUMMY_PR3F));
		
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5640_dac1_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMD:
		break;
	case SND_SOC_DAPM_POST_PMU:
		break;
	default:
		return 0;
	}

	return 0;
}


static int rt5640_asrc_event(struct snd_soc_dapm_widget *w,
			     struct snd_kcontrol *kcontrol, int event)
{

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (PROJ_ID == PROJ_ID_PF400CG || PROJ_ID == PROJ_ID_ME175CG || PROJ_ID == PROJ_ID_A400CG) {
			snd_soc_write(w->codec, RT5640_ASRC_1, 0x9b00); // DMIC2 ASRC mode
		} else {
			snd_soc_write(w->codec, RT5640_ASRC_1, 0x9a00);
		}
		snd_soc_write(w->codec, RT5640_ASRC_2, 0xf820);
		if (PROJ_ID != PROJ_ID_ME302C && PROJ_ID != PROJ_ID_GEMINI) {
			if (is_csv_call) {
				is_asrc5_changed = 1;
				snd_soc_write(w->codec, RT5640_ASRC_5, 0xb69e);
				pr_debug("%s : set ASRC5(0x87) to 0xb69e\n", __func__);
			}
		}
		break;
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_write(w->codec, RT5640_ASRC_1, 0);
		snd_soc_write(w->codec, RT5640_ASRC_2, 0);
		if (PROJ_ID != PROJ_ID_ME302C && PROJ_ID != PROJ_ID_GEMINI) {
			if(is_asrc5_changed == 1) {
				is_asrc5_changed = 0;
				snd_soc_write(w->codec, RT5640_ASRC_5, 0x23d7);
				pr_debug("%s : set ASRC5(0x87) to 0x23d7\n", __func__);
			}
		}
	default:
		return 0;
	}

	return 0;
}

static int rt5640_inl_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
	case SND_SOC_DAPM_PRE_PMD:
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x10);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x20);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x30);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x40);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x50);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x60);
		snd_soc_update_bits(w->codec, RT5640_OUT_L3_MIXER,
			RT5640_M_IN_L_OM_L, RT5640_M_IN_L_OM_L);
		break;
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(w->codec, RT5640_OUT_L3_MIXER,
			RT5640_M_IN_L_OM_L, 0);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x50);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x40);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x30);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x20);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x10);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x4d, 0x70, 0x00);
		break;
	default:
		return 0;
	}

	return 0;
}

static int rt5640_inr_event(struct snd_soc_dapm_widget *w, 
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_codec *codec = w->codec;

	switch (event) {
	case SND_SOC_DAPM_PRE_PMU:
	case SND_SOC_DAPM_PRE_PMD:
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x10);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x20);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x30);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x40);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x50);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x60);
		snd_soc_update_bits(w->codec, RT5640_OUT_R3_MIXER,
			RT5640_M_IN_R_OM_R, RT5640_M_IN_R_OM_R);
		break;
	case SND_SOC_DAPM_POST_PMU:
		snd_soc_update_bits(w->codec, RT5640_OUT_R3_MIXER,
			RT5640_M_IN_R_OM_R, 0);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x50);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x40);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x30);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x20);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x10);
		mdelay(5);
		snd_soc_update_bits(w->codec, 0x50, 0x70, 0x00);
		break;
	default:
		return 0;
	}

	return 0;
}

static const struct snd_soc_dapm_widget rt5640_dapm_widgets[] = {
	SND_SOC_DAPM_SUPPLY_S("ASRC enable", 2, SND_SOC_NOPM, 0, 0,
		rt5640_asrc_event, SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD), 
	SND_SOC_DAPM_SUPPLY("PLL1", RT5640_PWR_ANLG2,
			RT5640_PWR_PLL_BIT, 0, NULL, 0),
	/* Input Side */
	/* micbias */
	SND_SOC_DAPM_SUPPLY("LDO2", RT5640_PWR_ANLG1,
			RT5640_PWR_LDO2_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MICBIAS("micbias1", RT5640_PWR_ANLG2,
			RT5640_PWR_MB1_BIT, 0),
	SND_SOC_DAPM_MICBIAS("micbias2", RT5640_PWR_ANLG2,
			RT5640_PWR_MB2_BIT, 0),
	/* Input Lines */
	SND_SOC_DAPM_INPUT("DMIC1"),
	SND_SOC_DAPM_INPUT("DMIC2"),

	SND_SOC_DAPM_INPUT("IN1P"),
	SND_SOC_DAPM_INPUT("IN1N"),
	SND_SOC_DAPM_INPUT("IN2P"),
	SND_SOC_DAPM_INPUT("IN2N"),
	SND_SOC_DAPM_INPUT("IN3P"),
	SND_SOC_DAPM_INPUT("IN3N"),
	SND_SOC_DAPM_PGA_E("DMIC L1", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5640_set_dmic1_event, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_PGA_E("DMIC R1", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5640_set_dmic1_event, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_PGA_E("DMIC L2", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5640_set_dmic2_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_PGA_E("DMIC R2", SND_SOC_NOPM, 0, 0, NULL, 0,
		rt5640_set_dmic2_event, SND_SOC_DAPM_PRE_PMU | SND_SOC_DAPM_POST_PMD),
	SND_SOC_DAPM_SUPPLY("DMIC CLK", SND_SOC_NOPM, 0, 0,
		set_dmic_clk, SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_SUPPLY("DMIC1 Power", RT5640_DMIC,
		RT5640_DMIC_1_EN_SFT, 0, rt5640_set_dmic1_event,
		SND_SOC_DAPM_PRE_PMU),
	SND_SOC_DAPM_SUPPLY("DMIC2 Power", RT5640_DMIC,
		RT5640_DMIC_2_EN_SFT, 0, rt5640_set_dmic2_event,
		SND_SOC_DAPM_PRE_PMU),		
	/* Boost */
	SND_SOC_DAPM_PGA("BST1", RT5640_PWR_ANLG2,
		RT5640_PWR_BST1_BIT, 0, NULL, 0),
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	SND_SOC_DAPM_PGA("BST2", RT5640_PWR_ANLG2,
		RT5640_PWR_BST2_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("BST3", RT5640_PWR_ANLG2,
		RT5640_PWR_BST3_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("BST4", RT5640_PWR_ANLG2,
		RT5640_PWR_BST4_BIT, 0, NULL, 0),
#else
	SND_SOC_DAPM_PGA("BST2", RT5640_PWR_ANLG2,
		RT5640_PWR_BST4_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("BST3", RT5640_PWR_ANLG2,
		RT5640_PWR_BST2_BIT, 0, NULL, 0),
#endif
	/* Input Volume */
	SND_SOC_DAPM_PGA_E("INL VOL", RT5640_PWR_VOL,
		RT5640_PWR_IN_L_BIT, 0, NULL, 0,
		rt5640_inl_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_PGA_E("INR VOL", RT5640_PWR_VOL,
		RT5640_PWR_IN_R_BIT, 0, NULL, 0,
		rt5640_inr_event, SND_SOC_DAPM_PRE_PMU |
		SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	/* IN Mux */
	SND_SOC_DAPM_MUX("INL Mux", SND_SOC_NOPM, 0, 0, &rt5640_inl_mux),
	SND_SOC_DAPM_MUX("INR Mux", SND_SOC_NOPM, 0, 0, &rt5640_inr_mux),
	/* REC Mixer */
	SND_SOC_DAPM_MIXER("RECMIXL", RT5640_PWR_MIXER, RT5640_PWR_RM_L_BIT, 0,
			rt5640_rec_l_mix, ARRAY_SIZE(rt5640_rec_l_mix)),
	SND_SOC_DAPM_MIXER("RECMIXR", RT5640_PWR_MIXER, RT5640_PWR_RM_R_BIT, 0,
			rt5640_rec_r_mix, ARRAY_SIZE(rt5640_rec_r_mix)),
	/* ADCs */
	SND_SOC_DAPM_ADC("ADC L", NULL, SND_SOC_NOPM,
		0, 0),
	SND_SOC_DAPM_ADC("ADC R", NULL, SND_SOC_NOPM,
		0, 0),

	SND_SOC_DAPM_SUPPLY("ADC L power",RT5640_PWR_DIG1,
			RT5640_PWR_ADC_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC R power",RT5640_PWR_DIG1,
			RT5640_PWR_ADC_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("ADC clock",SND_SOC_NOPM, 0, 0,
		rt5640_adc_event, SND_SOC_DAPM_POST_PMD | SND_SOC_DAPM_POST_PMU),
	/* ADC Mux */
	SND_SOC_DAPM_MUX("Stereo ADC2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_sto_adc_2_mux),
	SND_SOC_DAPM_MUX("Stereo ADC1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_sto_adc_1_mux),
	SND_SOC_DAPM_MUX("Mono ADC L2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_mono_adc_l2_mux),
	SND_SOC_DAPM_MUX("Mono ADC L1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_mono_adc_l1_mux),
	SND_SOC_DAPM_MUX("Mono ADC R1 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_mono_adc_r1_mux),
	SND_SOC_DAPM_MUX("Mono ADC R2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_mono_adc_r2_mux),
	/* ADC Mixer */
	SND_SOC_DAPM_SUPPLY("stereo filter", RT5640_PWR_DIG2,
		RT5640_PWR_ADC_SF_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MIXER_E("Stereo ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5640_sto_adc_l_mix, ARRAY_SIZE(rt5640_sto_adc_l_mix),
		rt5640_sto_adcl_event, SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_MIXER_E("Stereo ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5640_sto_adc_r_mix, ARRAY_SIZE(rt5640_sto_adc_r_mix),
		rt5640_sto_adcr_event, SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_SUPPLY("mono left filter", RT5640_PWR_DIG2,
		RT5640_PWR_ADC_MF_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MIXER_E("Mono ADC MIXL", SND_SOC_NOPM, 0, 0,
		rt5640_mono_adc_l_mix, ARRAY_SIZE(rt5640_mono_adc_l_mix),
		rt5640_mono_adcl_event, SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_SUPPLY("mono right filter", RT5640_PWR_DIG2,
		RT5640_PWR_ADC_MF_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_MIXER_E("Mono ADC MIXR", SND_SOC_NOPM, 0, 0,
		rt5640_mono_adc_r_mix, ARRAY_SIZE(rt5640_mono_adc_r_mix),
		rt5640_mono_adcr_event, SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),

	/* IF2 Mux */
	SND_SOC_DAPM_MUX("IF2 ADC L Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_if2_adc_l_mux),
	SND_SOC_DAPM_MUX("IF2 ADC R Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_if2_adc_r_mux),

	/* Digital Interface */
	SND_SOC_DAPM_SUPPLY("I2S1", RT5640_PWR_DIG1,
		RT5640_PWR_I2S1_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 DAC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF1 ADC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("I2S2", RT5640_PWR_DIG1,
		RT5640_PWR_I2S2_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 DAC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF2 ADC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("I2S3", RT5640_PWR_DIG1,
		RT5640_PWR_I2S3_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 DAC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 DAC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 DAC R", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 ADC", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 ADC L", SND_SOC_NOPM, 0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("IF3 ADC R", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Digital Interface Select */
	SND_SOC_DAPM_MUX("DAI1 RX Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI1 TX Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI1 IF1 Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI1 IF2 Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("SDI1 TX Mux", SND_SOC_NOPM, 0, 0, &rt5640_sdi_mux),

	SND_SOC_DAPM_MUX("DAI2 RX Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI2 TX Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI2 IF1 Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI2 IF2 Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("SDI2 TX Mux", SND_SOC_NOPM, 0, 0, &rt5640_sdi_mux),

	SND_SOC_DAPM_MUX("DAI3 RX Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),
	SND_SOC_DAPM_MUX("DAI3 TX Mux", SND_SOC_NOPM, 0, 0, &rt5640_dai_mux),

	/* Audio Interface */
	SND_SOC_DAPM_AIF_IN("AIF1RX", "AIF1 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF1TX", "AIF1 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF2RX", "AIF2 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF2TX", "AIF2 Capture", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_IN("AIF3RX", "AIF3 Playback", 0, SND_SOC_NOPM, 0, 0),
	SND_SOC_DAPM_AIF_OUT("AIF3TX", "AIF3 Capture", 0, SND_SOC_NOPM, 0, 0),

	/* Audio DSP */
	SND_SOC_DAPM_PGA("Audio DSP", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* ANC */
	SND_SOC_DAPM_PGA("ANC", SND_SOC_NOPM, 0, 0, NULL, 0),

	/* Output Side */
	/* DAC mixer before sound effect  */
	SND_SOC_DAPM_MIXER("DAC MIXL", SND_SOC_NOPM, 0, 0,
		rt5640_dac_l_mix, ARRAY_SIZE(rt5640_dac_l_mix)),
	SND_SOC_DAPM_MIXER("DAC MIXR", SND_SOC_NOPM, 0, 0,
		rt5640_dac_r_mix, ARRAY_SIZE(rt5640_dac_r_mix)),

	/* DAC2 channel Mux */
	SND_SOC_DAPM_MUX("DAC L2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_dac_l2_mux),
	SND_SOC_DAPM_MUX("DAC R2 Mux", SND_SOC_NOPM, 0, 0,
				&rt5640_dac_r2_mux),
	SND_SOC_DAPM_PGA("DAC L2 Volume", SND_SOC_NOPM,
			0, 0, NULL, 0), 
	SND_SOC_DAPM_PGA("DAC R2 Volume", SND_SOC_NOPM,
			0, 0, NULL, 0), 

	/* DAC Mixer */
	SND_SOC_DAPM_MIXER("Stereo DAC MIXL", SND_SOC_NOPM, 0, 0,
		rt5640_sto_dac_l_mix, ARRAY_SIZE(rt5640_sto_dac_l_mix)),
	SND_SOC_DAPM_MIXER("Stereo DAC MIXR", SND_SOC_NOPM, 0, 0,
		rt5640_sto_dac_r_mix, ARRAY_SIZE(rt5640_sto_dac_r_mix)),
	SND_SOC_DAPM_MIXER("Mono DAC MIXL", SND_SOC_NOPM, 0, 0,
		rt5640_mono_dac_l_mix, ARRAY_SIZE(rt5640_mono_dac_l_mix)),
	SND_SOC_DAPM_MIXER("Mono DAC MIXR", SND_SOC_NOPM, 0, 0,
		rt5640_mono_dac_r_mix, ARRAY_SIZE(rt5640_mono_dac_r_mix)),
	SND_SOC_DAPM_MIXER("DIG MIXL", SND_SOC_NOPM, 0, 0,
		rt5640_dig_l_mix, ARRAY_SIZE(rt5640_dig_l_mix)),
	SND_SOC_DAPM_MIXER("DIG MIXR", SND_SOC_NOPM, 0, 0,
		rt5640_dig_r_mix, ARRAY_SIZE(rt5640_dig_r_mix)),
	SND_SOC_DAPM_MUX_E("Mono dacr Mux", SND_SOC_NOPM, 0, 0,
		&rt5640_dacr2_mux, rt5640_index_sync_event, SND_SOC_DAPM_PRE_PMU),

	/* DACs */
	SND_SOC_DAPM_SUPPLY("DAC L1 power",RT5640_PWR_DIG1,
			RT5640_PWR_DAC_L1_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("DAC R1 power",RT5640_PWR_DIG1,
			RT5640_PWR_DAC_R1_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("DAC L2 power",RT5640_PWR_DIG1,
			RT5640_PWR_DAC_L2_BIT, 0, NULL, 0),
	SND_SOC_DAPM_SUPPLY("DAC R2 power",RT5640_PWR_DIG1,
			RT5640_PWR_DAC_R2_BIT, 0, NULL, 0),

	SND_SOC_DAPM_DAC_E("DAC L1", NULL, SND_SOC_NOPM,
			0, 0, rt5640_dac1_event,
			SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_DAC("DAC L2", NULL, SND_SOC_NOPM,
			0, 0),
	SND_SOC_DAPM_DAC_E("DAC R1", NULL, SND_SOC_NOPM,
			0, 0, rt5640_dac1_event,
			SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_DAC("DAC R2", NULL, SND_SOC_NOPM,
			0, 0),

	/* SPK/OUT Mixer */
	SND_SOC_DAPM_MIXER("SPK MIXL", RT5640_PWR_MIXER, RT5640_PWR_SM_L_BIT,
		0, rt5640_spk_l_mix, ARRAY_SIZE(rt5640_spk_l_mix)),
	SND_SOC_DAPM_MIXER("SPK MIXR", RT5640_PWR_MIXER, RT5640_PWR_SM_R_BIT,
		0, rt5640_spk_r_mix, ARRAY_SIZE(rt5640_spk_r_mix)),
	SND_SOC_DAPM_MIXER("OUT MIXL", RT5640_PWR_MIXER, RT5640_PWR_OM_L_BIT,
		0, rt5640_out_l_mix, ARRAY_SIZE(rt5640_out_l_mix)),
	SND_SOC_DAPM_MIXER("OUT MIXR", RT5640_PWR_MIXER, RT5640_PWR_OM_R_BIT,
		0, rt5640_out_r_mix, ARRAY_SIZE(rt5640_out_r_mix)),
	/* Ouput Volume */
	SND_SOC_DAPM_PGA("SPKVOL L", RT5640_PWR_VOL,
		RT5640_PWR_SV_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("SPKVOL R", RT5640_PWR_VOL,
		RT5640_PWR_SV_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OUTVOL L", RT5640_PWR_VOL,
		RT5640_PWR_OV_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("OUTVOL R", RT5640_PWR_VOL,
		RT5640_PWR_OV_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HPOVOL L", RT5640_PWR_VOL,
		RT5640_PWR_HV_L_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HPOVOL R", RT5640_PWR_VOL,
		RT5640_PWR_HV_R_BIT, 0, NULL, 0),
	SND_SOC_DAPM_PGA("DAC 1", SND_SOC_NOPM,
		0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("DAC 2", SND_SOC_NOPM,
		0, 0, NULL, 0),
	SND_SOC_DAPM_PGA("HPOVOL", SND_SOC_NOPM,
		0, 0, NULL, 0),
	/* SPO/HPO/LOUT/Mono Mixer */
	SND_SOC_DAPM_MIXER("SPOL MIX", SND_SOC_NOPM, 0,
		0, rt5640_spo_l_mix, ARRAY_SIZE(rt5640_spo_l_mix)),
	SND_SOC_DAPM_MIXER("SPOR MIX", SND_SOC_NOPM, 0,
		0, rt5640_spo_r_mix, ARRAY_SIZE(rt5640_spo_r_mix)),
	SND_SOC_DAPM_MIXER("HPO MIX", SND_SOC_NOPM, 0, 0,
		rt5640_hpo_mix, ARRAY_SIZE(rt5640_hpo_mix)),
	SND_SOC_DAPM_MIXER("LOUT MIX", SND_SOC_NOPM, 0, 0,
		rt5640_lout_mix, ARRAY_SIZE(rt5640_lout_mix)),
	SND_SOC_DAPM_MIXER("Mono MIX", RT5640_PWR_ANLG1, RT5640_PWR_MM_BIT, 0,
		rt5640_mono_mix, ARRAY_SIZE(rt5640_mono_mix)),

	SND_SOC_DAPM_PGA_S("HP amp", 1, SND_SOC_NOPM, 0, 0,
		rt5640_hp_event, SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("SPK L amp", 1, SND_SOC_NOPM, 0, 0,
			rt5640_spk_l_event,
			SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("SPK R amp", 1, SND_SOC_NOPM, 0, 0,
			rt5640_spk_r_event,
			SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("LOUT amp", 1, SND_SOC_NOPM, 0, 0,
		rt5640_lout_event, SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),
	SND_SOC_DAPM_PGA_S("Mono amp", 1, RT5640_PWR_ANLG1,
		RT5640_PWR_MA_BIT, 0, rt5640_mono_event,
		SND_SOC_DAPM_PRE_PMD | SND_SOC_DAPM_POST_PMU),

	/* Output Lines */
	SND_SOC_DAPM_OUTPUT("SPOLP"),
	SND_SOC_DAPM_OUTPUT("SPOLN"),
	SND_SOC_DAPM_OUTPUT("SPORP"),
	SND_SOC_DAPM_OUTPUT("SPORN"),
	SND_SOC_DAPM_OUTPUT("HPOL"),
	SND_SOC_DAPM_OUTPUT("HPOR"),
	SND_SOC_DAPM_OUTPUT("LOUTL"),
	SND_SOC_DAPM_OUTPUT("LOUTR"),
	SND_SOC_DAPM_OUTPUT("MonoP"),
	SND_SOC_DAPM_OUTPUT("MonoN"),

};

static const struct snd_soc_dapm_route rt5640_dapm_routes[] = {
	{"INL VOL", NULL, "I2S1"},
	{"INR VOL", NULL, "I2S1"},
	{"INL VOL", NULL, "DAC L1 power"},
	{"INR VOL", NULL, "DAC R1 power"},
	{"INL VOL", NULL, "ASRC enable"},
	{"INR VOL", NULL, "ASRC enable"},
	
	// joe_cheng : micbias1 will be disabled even if headset is still connected.
	// When starting AMIC recording, LDO2 will be closed and then enabled. 
	// This causes button interrupt. And music player will be activated.
	// After stop AMIC recording, micbias will be closed unexpectedly. 
//	{"IN2P", NULL, "LDO2"},
//	{"IN3P", NULL, "LDO2"},	
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	{"IN4P", NULL, "LDO2"},
#endif
	// joe_cheng : TT#301720
	{"I2S1", NULL, "ASRC enable"},
	{"I2S2", NULL, "ASRC enable"},

	{"DMIC L1", NULL, "DMIC1"},
	{"DMIC R1", NULL, "DMIC1"},
	{"DMIC L2", NULL, "DMIC2"},
	{"DMIC R2", NULL, "DMIC2"},

	{"BST1", NULL, "IN1P"},
	{"BST1", NULL, "IN1N"},
	{"BST2", NULL, "IN2P"},
	{"BST2", NULL, "IN2N"},
	{"BST3", NULL, "IN3P"},
	{"BST3", NULL, "IN3N"},
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	{"BST4", NULL, "IN4P"},
	{"BST4", NULL, "IN4N"},
#endif

	{"INL VOL", NULL, "IN2P"},
	{"INR VOL", NULL, "IN2N"},

	{"RECMIXL", "HPOL Switch", "HPOL"},
	{"RECMIXL", "INL Switch", "INL VOL"},
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	{"RECMIXL", "BST4 Switch", "BST4"},
#endif
	{"RECMIXL", "BST3 Switch", "BST3"},
	{"RECMIXL", "BST2 Switch", "BST2"},
	{"RECMIXL", "BST1 Switch", "BST1"},
	{"RECMIXL", "OUT MIXL Switch", "OUT MIXL"},

	{"RECMIXR", "HPOR Switch", "HPOR"},
	{"RECMIXR", "INR Switch", "INR VOL"},
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	{"RECMIXR", "BST4 Switch", "BST4"},
#endif
	{"RECMIXR", "BST3 Switch", "BST3"},
	{"RECMIXR", "BST2 Switch", "BST2"},
	{"RECMIXR", "BST1 Switch", "BST1"},
	{"RECMIXR", "OUT MIXR Switch", "OUT MIXR"},

	{"ADC L", NULL, "RECMIXL"},
	{"ADC L", NULL, "ADC L power"},
	{"ADC L", NULL, "ASRC enable"},
	{"ADC L", NULL, "ADC clock"},
	{"ADC R", NULL, "RECMIXR"},
	{"ADC R", NULL, "ADC R power"},
	{"ADC R", NULL, "ASRC enable"}, 
	{"ADC R", NULL, "ADC clock"},

	{"DMIC L1", NULL, "DMIC CLK"},
	{"DMIC L1", NULL, "DMIC1 Power"},
	{"DMIC R1", NULL, "DMIC CLK"},
	{"DMIC R1", NULL, "DMIC1 Power"},
	{"DMIC L2", NULL, "DMIC CLK"},
	{"DMIC L2", NULL, "DMIC2 Power"},
	{"DMIC R2", NULL, "DMIC CLK"},
	{"DMIC R2", NULL, "DMIC2 Power"},

	{"Stereo ADC2 Mux", "DMIC1", "DMIC L1"},
	{"Stereo ADC2 Mux", "DMIC2", "DMIC L2"},
	{"Stereo ADC2 Mux", "DIG MIX", "DIG MIXL"},
	{"Stereo ADC1 Mux", "ADC", "ADC L"},
	{"Stereo ADC1 Mux", "DIG MIX", "DIG MIXL"},

	{"Stereo ADC1 Mux", "ADC", "ADC R"},
	{"Stereo ADC1 Mux", "DIG MIX", "DIG MIXR"},
	{"Stereo ADC2 Mux", "DMIC1", "DMIC R1"},
	{"Stereo ADC2 Mux", "DMIC2", "DMIC R2"},
	{"Stereo ADC2 Mux", "DIG MIX", "DIG MIXR"},

	{"Mono ADC L2 Mux", "DMIC L1", "DMIC L1"},
	{"Mono ADC L2 Mux", "DMIC L2", "DMIC L2"},
	{"Mono ADC L2 Mux", "Mono DAC MIXL", "Mono DAC MIXL"},
	{"Mono ADC L1 Mux", "Mono DAC MIXL", "Mono DAC MIXL"},
	{"Mono ADC L1 Mux", "ADCL", "ADC L"},

	{"Mono ADC R1 Mux", "Mono DAC MIXR", "Mono DAC MIXR"},
	{"Mono ADC R1 Mux", "ADCR", "ADC R"},
	{"Mono ADC R2 Mux", "DMIC R1", "DMIC R1"},
	{"Mono ADC R2 Mux", "DMIC R2", "DMIC R2"},
	{"Mono ADC R2 Mux", "Mono DAC MIXR", "Mono DAC MIXR"},

	{"Stereo ADC MIXL", "ADC1 Switch", "Stereo ADC1 Mux"},
	{"Stereo ADC MIXL", "ADC2 Switch", "Stereo ADC2 Mux"},
	{"Stereo ADC MIXL", NULL, "stereo filter"},
	{"stereo filter", NULL, "PLL1", check_sysclk1_source},

	{"Stereo ADC MIXR", "ADC1 Switch", "Stereo ADC1 Mux"},
	{"Stereo ADC MIXR", "ADC2 Switch", "Stereo ADC2 Mux"},
	{"Stereo ADC MIXR", NULL, "stereo filter"},
	{"stereo filter", NULL, "PLL1", check_sysclk1_source},

	{"Mono ADC MIXL", "ADC1 Switch", "Mono ADC L1 Mux"},
	{"Mono ADC MIXL", "ADC2 Switch", "Mono ADC L2 Mux"},
	{"Mono ADC MIXL", NULL, "mono left filter"},
	{"mono left filter", NULL, "PLL1", check_sysclk1_source},

	{"Mono ADC MIXR", "ADC1 Switch", "Mono ADC R1 Mux"},
	{"Mono ADC MIXR", "ADC2 Switch", "Mono ADC R2 Mux"},
	{"Mono ADC MIXR", NULL, "mono right filter"},
	{"mono right filter", NULL, "PLL1", check_sysclk1_source},

	{"IF2 ADC L Mux", "Mono ADC MIXL", "Mono ADC MIXL"},
	{"IF2 ADC R Mux", "Mono ADC MIXR", "Mono ADC MIXR"},

	{"IF2 ADC L", NULL, "IF2 ADC L Mux"},
	{"IF2 ADC R", NULL, "IF2 ADC R Mux"},
	{"IF3 ADC L", NULL, "Mono ADC MIXL"},
	{"IF3 ADC R", NULL, "Mono ADC MIXR"},
	{"IF1 ADC L", NULL, "Stereo ADC MIXL"},
	{"IF1 ADC R", NULL, "Stereo ADC MIXR"},

	{"IF1 ADC", NULL, "I2S1"},
	{"IF1 ADC", NULL, "IF1 ADC L"},
	{"IF1 ADC", NULL, "IF1 ADC R"},
	{"IF2 ADC", NULL, "I2S2"},
	{"IF2 ADC", NULL, "IF2 ADC L"},
	{"IF2 ADC", NULL, "IF2 ADC R"},
	{"IF3 ADC", NULL, "I2S3"},
	{"IF3 ADC", NULL, "IF3 ADC L"},
	{"IF3 ADC", NULL, "IF3 ADC R"},

	{"DAI1 TX Mux", "1:1|2:2|3:3", "IF1 ADC"},
	{"DAI1 TX Mux", "1:1|2:3|3:2", "IF1 ADC"},
	{"DAI1 TX Mux", "1:3|2:1|3:2", "IF2 ADC"},
	{"DAI1 TX Mux", "1:2|2:1|3:3", "IF2 ADC"},
	{"DAI1 TX Mux", "1:3|2:2|3:1", "IF3 ADC"},
	{"DAI1 TX Mux", "1:2|2:3|3:1", "IF3 ADC"},
	{"DAI1 IF1 Mux", "1:1|2:1|3:3", "IF1 ADC"},
	{"DAI1 IF2 Mux", "1:1|2:1|3:3", "IF2 ADC"},
	{"SDI1 TX Mux", "IF1", "DAI1 IF1 Mux"},
	{"SDI1 TX Mux", "IF2", "DAI1 IF2 Mux"},

	{"DAI2 TX Mux", "1:2|2:3|3:1", "IF1 ADC"},
	{"DAI2 TX Mux", "1:2|2:1|3:3", "IF1 ADC"},
	{"DAI2 TX Mux", "1:1|2:2|3:3", "IF2 ADC"},
	{"DAI2 TX Mux", "1:3|2:2|3:1", "IF2 ADC"},
	{"DAI2 TX Mux", "1:1|2:3|3:2", "IF3 ADC"},
	{"DAI2 TX Mux", "1:3|2:1|3:2", "IF3 ADC"},
	{"DAI2 IF1 Mux", "1:2|2:2|3:3", "IF1 ADC"},
	{"DAI2 IF2 Mux", "1:2|2:2|3:3", "IF2 ADC"},
	{"SDI2 TX Mux", "IF1", "DAI2 IF1 Mux"},
	{"SDI2 TX Mux", "IF2", "DAI2 IF2 Mux"},

	{"DAI3 TX Mux", "1:3|2:1|3:2", "IF1 ADC"},
	{"DAI3 TX Mux", "1:3|2:2|3:1", "IF1 ADC"},
	{"DAI3 TX Mux", "1:1|2:3|3:2", "IF2 ADC"},
	{"DAI3 TX Mux", "1:2|2:3|3:1", "IF2 ADC"},
	{"DAI3 TX Mux", "1:1|2:2|3:3", "IF3 ADC"},
	{"DAI3 TX Mux", "1:2|2:1|3:3", "IF3 ADC"},
	{"DAI3 TX Mux", "1:1|2:1|3:3", "IF3 ADC"},
	{"DAI3 TX Mux", "1:2|2:2|3:3", "IF3 ADC"},

	{"AIF1TX", NULL, "DAI1 TX Mux"},
	{"AIF1TX", NULL, "SDI1 TX Mux"},
	{"AIF2TX", NULL, "DAI2 TX Mux"},
	{"AIF2TX", NULL, "SDI2 TX Mux"},
	{"AIF3TX", NULL, "DAI3 TX Mux"},

	{"DAI1 RX Mux", "1:1|2:2|3:3", "AIF1RX"},
	{"DAI1 RX Mux", "1:1|2:3|3:2", "AIF1RX"},
	{"DAI1 RX Mux", "1:1|2:1|3:3", "AIF1RX"},
	{"DAI1 RX Mux", "1:2|2:3|3:1", "AIF2RX"},
	{"DAI1 RX Mux", "1:2|2:1|3:3", "AIF2RX"},
	{"DAI1 RX Mux", "1:2|2:2|3:3", "AIF2RX"},
	{"DAI1 RX Mux", "1:3|2:1|3:2", "AIF3RX"},
	{"DAI1 RX Mux", "1:3|2:2|3:1", "AIF3RX"},

	{"DAI2 RX Mux", "1:3|2:1|3:2", "AIF1RX"},
	{"DAI2 RX Mux", "1:2|2:1|3:3", "AIF1RX"},
	{"DAI2 RX Mux", "1:1|2:1|3:3", "AIF1RX"},
	{"DAI2 RX Mux", "1:1|2:2|3:3", "AIF2RX"},
	{"DAI2 RX Mux", "1:3|2:2|3:1", "AIF2RX"},
	{"DAI2 RX Mux", "1:2|2:2|3:3", "AIF2RX"},
	{"DAI2 RX Mux", "1:1|2:3|3:2", "AIF3RX"},
	{"DAI2 RX Mux", "1:2|2:3|3:1", "AIF3RX"},

	{"DAI3 RX Mux", "1:3|2:2|3:1", "AIF1RX"},
	{"DAI3 RX Mux", "1:2|2:3|3:1", "AIF1RX"},
	{"DAI3 RX Mux", "1:1|2:3|3:2", "AIF2RX"},
	{"DAI3 RX Mux", "1:3|2:1|3:2", "AIF2RX"},
	{"DAI3 RX Mux", "1:1|2:2|3:3", "AIF3RX"},
	{"DAI3 RX Mux", "1:2|2:1|3:3", "AIF3RX"},
	{"DAI3 RX Mux", "1:1|2:1|3:3", "AIF3RX"},
	{"DAI3 RX Mux", "1:2|2:2|3:3", "AIF3RX"},

	{"IF1 DAC", NULL, "I2S1"},
	{"IF1 DAC", NULL, "DAI1 RX Mux"},
	{"IF2 DAC", NULL, "I2S2"},
	{"IF2 DAC", NULL, "DAI2 RX Mux"},
	{"IF3 DAC", NULL, "I2S3"},
	{"IF3 DAC", NULL, "DAI3 RX Mux"},

	{"IF1 DAC L", NULL, "IF1 DAC"},
	{"IF1 DAC R", NULL, "IF1 DAC"},
	{"IF2 DAC L", NULL, "IF2 DAC"},
	{"IF2 DAC R", NULL, "IF2 DAC"},
	{"IF3 DAC L", NULL, "IF3 DAC"},
	{"IF3 DAC R", NULL, "IF3 DAC"},

	{"DAC MIXL", "Stereo ADC Switch", "Stereo ADC MIXL"},
	{"DAC MIXL", "INF1 Switch", "IF1 DAC L"},
	{"DAC MIXL", NULL, "DAC L1 power"},
	{"DAC MIXR", "Stereo ADC Switch", "Stereo ADC MIXR"},
	{"DAC MIXR", "INF1 Switch", "IF1 DAC R"},
	{"DAC MIXR", NULL, "DAC R1 power"}, 

	{"ANC", NULL, "Stereo ADC MIXL"},
	{"ANC", NULL, "Stereo ADC MIXR"},

	{"Audio DSP", NULL, "DAC MIXL"},
	{"Audio DSP", NULL, "DAC MIXR"},

	{"DAC L2 Mux", "IF2", "IF2 DAC L"},
	{"DAC L2 Mux", "IF3", "IF3 DAC L"},
	{"DAC L2 Mux", "Base L/R", "Audio DSP"},
	{"DAC L2 Volume", NULL, "DAC L2 Mux"},
	{"DAC L2 Volume", NULL, "DAC L2 power"}, 

	{"DAC R2 Mux", "IF2", "IF2 DAC R"},
	{"DAC R2 Mux", "IF3", "IF3 DAC R"},
	{"DAC R2 Volume", NULL, "Mono dacr Mux"},
	{"DAC R2 Volume", NULL, "DAC R2 power"}, 
	{"Mono dacr Mux", "TxDC_R", "DAC R2 Mux"},
	{"Mono dacr Mux", "TxDP_R", "IF2 ADC R Mux"},

	{"Stereo DAC MIXL", "DAC L1 Switch", "DAC MIXL"},
	{"Stereo DAC MIXL", "DAC L2 Switch", "DAC L2 Volume"},
	{"Stereo DAC MIXL", "ANC Switch", "ANC"},
	{"Stereo DAC MIXR", "DAC R1 Switch", "DAC MIXR"},
	{"Stereo DAC MIXR", "DAC R2 Switch", "DAC R2 Volume"},
	{"Stereo DAC MIXR", "ANC Switch", "ANC"},

	{"Mono DAC MIXL", "DAC L1 Switch", "DAC MIXL"},
	{"Mono DAC MIXL", "DAC L2 Switch", "DAC L2 Volume"},
	{"Mono DAC MIXL", "DAC R2 Switch", "DAC R2 Volume"},
	{"Mono DAC MIXL", NULL, "DAC L2 power"},
	{"Mono DAC MIXR", "DAC R1 Switch", "DAC MIXR"},
	{"Mono DAC MIXR", "DAC R2 Switch", "DAC R2 Volume"},
	{"Mono DAC MIXR", "DAC L2 Switch", "DAC L2 Volume"},
	{"Mono DAC MIXR", NULL, "DAC R2 power"},

	{"DIG MIXL", "DAC L1 Switch", "DAC MIXL"},
	{"DIG MIXL", "DAC L2 Switch", "DAC L2 Volume"},
	{"DIG MIXR", "DAC R1 Switch", "DAC MIXR"},
	{"DIG MIXR", "DAC R2 Switch", "DAC R2 Volume"},

	{"DAC L1", NULL, "Stereo DAC MIXL"},
	{"DAC L1", NULL, "DAC L1 power"}, 
	{"DAC L1", NULL, "ASRC enable"}, 
	{"DAC L1", NULL, "PLL1", check_sysclk1_source},
	{"DAC R1", NULL, "Stereo DAC MIXR"},
	{"DAC R1", NULL, "DAC R1 power"}, 
	{"DAC R1", NULL, "ASRC enable"}, 
	{"DAC R1", NULL, "PLL1", check_sysclk1_source},
	{"DAC L2", NULL, "Mono DAC MIXL"},
	{"DAC L2", NULL, "DAC L2 power"}, 
	{"DAC L2", NULL, "ASRC enable"}, 
	{"DAC L2", NULL, "PLL1", check_sysclk1_source},
	{"DAC R2", NULL, "Mono DAC MIXR"},
	{"DAC R2", NULL, "DAC R2 power"}, 
	{"DAC R2", NULL, "ASRC enable"}, 
	{"DAC R2", NULL, "PLL1", check_sysclk1_source},

	{"SPK MIXL", "REC MIXL Switch", "RECMIXL"},
	{"SPK MIXL", "INL Switch", "INL VOL"},
	{"SPK MIXL", "DAC L1 Switch", "DAC L1"},
	{"SPK MIXL", "DAC L2 Switch", "DAC L2"},
	{"SPK MIXL", "OUT MIXL Switch", "OUT MIXL"},
	{"SPK MIXR", "REC MIXR Switch", "RECMIXR"},
	{"SPK MIXR", "INR Switch", "INR VOL"},
	{"SPK MIXR", "DAC R1 Switch", "DAC R1"},
	{"SPK MIXR", "DAC R2 Switch", "DAC R2"},
	{"SPK MIXR", "OUT MIXR Switch", "OUT MIXR"},

	{"OUT MIXL", "BST3 Switch", "BST3"},
	{"OUT MIXL", "BST1 Switch", "BST1"},
	{"OUT MIXL", "INL Switch", "INL VOL"},
	{"OUT MIXL", "REC MIXL Switch", "RECMIXL"},
	{"OUT MIXL", "DAC R2 Switch", "DAC R2"},
	{"OUT MIXL", "DAC L2 Switch", "DAC L2"},
	{"OUT MIXL", "DAC L1 Switch", "DAC L1"},

	{"OUT MIXR", "BST3 Switch", "BST3"},
	{"OUT MIXR", "BST2 Switch", "BST2"},
	{"OUT MIXR", "BST1 Switch", "BST1"},
	{"OUT MIXR", "INR Switch", "INR VOL"},
	{"OUT MIXR", "REC MIXR Switch", "RECMIXR"},
	{"OUT MIXR", "DAC L2 Switch", "DAC L2"},
	{"OUT MIXR", "DAC R2 Switch", "DAC R2"},
	{"OUT MIXR", "DAC R1 Switch", "DAC R1"},

	{"SPKVOL L", NULL, "SPK MIXL"},
	{"SPKVOL R", NULL, "SPK MIXR"},
	{"HPOVOL L", NULL, "OUT MIXL"},
	{"HPOVOL R", NULL, "OUT MIXR"},
	{"OUTVOL L", NULL, "OUT MIXL"},
	{"OUTVOL R", NULL, "OUT MIXR"},

	{"SPOL MIX", "DAC R1 Switch", "DAC R1"},
	{"SPOL MIX", "DAC L1 Switch", "DAC L1"},
	{"SPOL MIX", "SPKVOL R Switch", "SPKVOL R"},
	{"SPOL MIX", "SPKVOL L Switch", "SPKVOL L"},
	{"SPOL MIX", "BST1 Switch", "BST1"},
	{"SPOR MIX", "DAC R1 Switch", "DAC R1"},
	{"SPOR MIX", "SPKVOL R Switch", "SPKVOL R"},
	{"SPOR MIX", "BST1 Switch", "BST1"},

	{"DAC 2", NULL, "DAC L2"},
	{"DAC 2", NULL, "DAC R2"},
	{"DAC 1", NULL, "DAC L1"},
	{"DAC 1", NULL, "DAC R1"},
	{"HPOVOL", NULL, "HPOVOL L"},
	{"HPOVOL", NULL, "HPOVOL R"},
	{"HPO MIX", "DAC2 Switch", "DAC 2"},
	{"HPO MIX", "DAC1 Switch", "DAC 1"},
	{"HPO MIX", "HPVOL Switch", "HPOVOL"},

	{"LOUT MIX", "DAC L1 Switch", "DAC L1"},
	{"LOUT MIX", "DAC R1 Switch", "DAC R1"},
	{"LOUT MIX", "OUTVOL L Switch", "OUTVOL L"},
	{"LOUT MIX", "OUTVOL R Switch", "OUTVOL R"},

	{"Mono MIX", "DAC R2 Switch", "DAC R2"},
	{"Mono MIX", "DAC L2 Switch", "DAC L2"},
	{"Mono MIX", "OUTVOL R Switch", "OUTVOL R"},
	{"Mono MIX", "OUTVOL L Switch", "OUTVOL L"},
	{"Mono MIX", "BST1 Switch", "BST1"},
	
	{"SPK L amp", NULL, "SPOL MIX"},
	{"SPK R amp", NULL, "SPOR MIX"},
	{"SPOLP", NULL, "SPK L amp"},
	{"SPOLN", NULL, "SPK L amp"},
	{"SPORP", NULL, "SPK R amp"},
	{"SPORN", NULL, "SPK R amp"},

	{"HP amp", NULL, "HPO MIX"},
	{"HPOL", NULL, "HP amp"},
	{"HPOR", NULL, "HP amp"},

	{"LOUT amp", NULL, "LOUT MIX"},
	{"LOUTL", NULL, "LOUT amp"},
	{"LOUTR", NULL, "LOUT amp"},

	{"Mono amp", NULL, "Mono MIX"},
	{"MonoP", NULL, "Mono amp"},
	{"MonoN", NULL, "Mono amp"},
};

static int get_sdp_info(struct snd_soc_codec *codec, int dai_id)
{
	int ret = 0, val;

	if(codec == NULL)
		return -EINVAL;

	val = snd_soc_read(codec, RT5640_I2S1_SDP);
	val = (val & RT5640_I2S_IF_MASK) >> RT5640_I2S_IF_SFT;
	switch (dai_id) {
	case RT5640_AIF1:
		if (val == RT5640_IF_123 || val == RT5640_IF_132 ||
			val == RT5640_IF_113)
			ret |= RT5640_U_IF1;
		if (val == RT5640_IF_312 || val == RT5640_IF_213 ||
			val == RT5640_IF_113)
			ret |= RT5640_U_IF2;
		if (val == RT5640_IF_321 || val == RT5640_IF_231)
			ret |= RT5640_U_IF3;
		break;

	case RT5640_AIF2:
		if (val == RT5640_IF_231 || val == RT5640_IF_213 ||
			val == RT5640_IF_223)
			ret |= RT5640_U_IF1;
		if (val == RT5640_IF_123 || val == RT5640_IF_321 ||
			val == RT5640_IF_223)
			ret |= RT5640_U_IF2;
		if (val == RT5640_IF_132 || val == RT5640_IF_312)
			ret |= RT5640_U_IF3;
		break;

#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	case RT5640_AIF3:
		if (val == RT5640_IF_312 || val == RT5640_IF_321)
			ret |= RT5640_U_IF1;
		if (val == RT5640_IF_132 || val == RT5640_IF_231)
			ret |= RT5640_U_IF2;
		if (val == RT5640_IF_123 || val == RT5640_IF_213 ||
			val == RT5640_IF_113 || val == RT5640_IF_223)
			ret |= RT5640_U_IF3;
		break;
#endif

	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int get_clk_info(int sclk, int rate)
{
	int i, pd[] = {1, 2, 3, 4, 6, 8, 12, 16};

#ifdef USE_ASRC
	return 0;
#endif
	if (sclk <= 0 || rate <= 0)
		return -EINVAL;

	rate = rate << 8;
	for (i = 0; i < ARRAY_SIZE(pd); i++)
		if (sclk == rate * pd[i])
			return i;

	return -EINVAL;
}

static int rt5640_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params, struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	unsigned int val_len = 0, val_clk, mask_clk, dai_sel;
	int pre_div, bclk_ms, frame_size;
	unsigned int reg_val = 0;

	rt5640->lrck[dai->id] = params_rate(params);
	pre_div = get_clk_info(rt5640->sysclk, rt5640->lrck[dai->id]);
	if (pre_div < 0) {
		dev_err(codec->dev, "Unsupported clock setting\n");
		return -EINVAL;
	}
	frame_size = snd_soc_params_to_frame_size(params);
	if (frame_size < 0) {
		dev_err(codec->dev, "Unsupported frame size: %d\n", frame_size);
		return -EINVAL;
	}
	bclk_ms = frame_size > 32 ? 1 : 0;
	rt5640->bclk[dai->id] = rt5640->lrck[dai->id] * (32 << bclk_ms);

	dev_dbg(dai->dev, "bclk is %dHz and lrck is %dHz\n",
		rt5640->bclk[dai->id], rt5640->lrck[dai->id]);
	dev_dbg(dai->dev, "bclk_ms is %d and pre_div is %d for iis %d\n",
				bclk_ms, pre_div, dai->id);

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		break;
	case SNDRV_PCM_FORMAT_S20_3LE:
		val_len |= RT5640_I2S_DL_20;
		break;
	case SNDRV_PCM_FORMAT_S24_LE:
		val_len |= RT5640_I2S_DL_24;
		break;
	case SNDRV_PCM_FORMAT_S8:
		val_len |= RT5640_I2S_DL_8;
		break;
	default:
		return -EINVAL;
	}

	dai_sel = get_sdp_info(codec, dai->id);
	//dai_sel |= (RT5640_U_IF1 | RT5640_U_IF2);
	if (dai_sel < 0) {
		dev_err(codec->dev, "Failed to get sdp info: %d\n", dai_sel);
		return -EINVAL;
	}
	if (dai_sel & RT5640_U_IF1) {
		mask_clk = RT5640_I2S_BCLK_MS1_MASK | RT5640_I2S_PD1_MASK;
		val_clk = bclk_ms << RT5640_I2S_BCLK_MS1_SFT |
			pre_div << RT5640_I2S_PD1_SFT;
		snd_soc_update_bits(codec, RT5640_I2S1_SDP,
			RT5640_I2S_DL_MASK, val_len);
#ifndef USE_ASRC
		snd_soc_update_bits(codec, RT5640_ADDA_CLK1, mask_clk, val_clk);
#endif
	}
	if (dai_sel & RT5640_U_IF2) {
		mask_clk = RT5640_I2S_BCLK_MS2_MASK | RT5640_I2S_PD2_MASK;
		val_clk = bclk_ms << RT5640_I2S_BCLK_MS2_SFT |
			pre_div << RT5640_I2S_PD2_SFT;
		snd_soc_update_bits(codec, RT5640_I2S2_SDP,
			RT5640_I2S_DL_MASK, val_len);
#ifndef USE_ASRC
		snd_soc_update_bits(codec, RT5640_ADDA_CLK1, mask_clk, val_clk);
#endif
	}
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	if (dai_sel & RT5640_U_IF3) {
		mask_clk = RT5640_I2S_BCLK_MS3_MASK | RT5640_I2S_PD3_MASK;
		val_clk = bclk_ms << RT5640_I2S_BCLK_MS3_SFT |
			pre_div << RT5640_I2S_PD3_SFT;
		snd_soc_update_bits(codec, RT5640_I2S3_SDP,
			RT5640_I2S_DL_MASK, val_len);
#ifndef USE_ASRC
		snd_soc_update_bits(codec, RT5640_ADDA_CLK1, mask_clk, val_clk);
#endif
	}
#endif
#ifdef USE_ASRC
	snd_soc_write(codec, RT5640_ADDA_CLK1, 0x0014);
#endif
	
	return 0;
}

static int rt5640_prepare(struct snd_pcm_substream *substream,
				struct snd_soc_dai *dai)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);

	rt5640->aif_pu = dai->id;
	return 0;
}

static int rt5640_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	unsigned int reg_val = 0, dai_sel;

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBM_CFM:
		rt5640->master[dai->id] = 1;
		break;
	case SND_SOC_DAIFMT_CBS_CFS:
		reg_val |= RT5640_I2S_MS_S;
		rt5640->master[dai->id] = 0;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		break;
	case SND_SOC_DAIFMT_IB_NF:
		reg_val |= RT5640_I2S_BP_INV;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		reg_val |= RT5640_I2S_DF_LEFT;
		break;
	case SND_SOC_DAIFMT_DSP_A:
		reg_val |= RT5640_I2S_DF_PCM_A;
		break;
	case SND_SOC_DAIFMT_DSP_B:
		reg_val |= RT5640_I2S_DF_PCM_B;
		break;
	default:
		return -EINVAL;
	}

	dai_sel = get_sdp_info(codec, dai->id);
	if (dai_sel < 0) {
		dev_err(codec->dev, "Failed to get sdp info: %d\n", dai_sel);
		return -EINVAL;
	}
	if (dai_sel & RT5640_U_IF1) {
		snd_soc_update_bits(codec, RT5640_I2S1_SDP,
			RT5640_I2S_MS_MASK | RT5640_I2S_BP_MASK |
			RT5640_I2S_DF_MASK, reg_val);
	}
	if (dai_sel & RT5640_U_IF2) {
		snd_soc_update_bits(codec, RT5640_I2S2_SDP,
			RT5640_I2S_MS_MASK | RT5640_I2S_BP_MASK |
			RT5640_I2S_DF_MASK, reg_val);
	}
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	if (dai_sel & RT5640_U_IF3) {
		snd_soc_update_bits(codec, RT5640_I2S3_SDP,
			RT5640_I2S_MS_MASK | RT5640_I2S_BP_MASK |
			RT5640_I2S_DF_MASK, reg_val);
	}
#endif
	return 0;
}

static int rt5640_set_dai_sysclk(struct snd_soc_dai *dai,
		int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = dai->codec;
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	unsigned int reg_val = 0;

	if (freq == rt5640->sysclk && clk_id == rt5640->sysclk_src)
		return 0;

	switch (clk_id) {
	case RT5640_SCLK_S_MCLK:
		reg_val |= RT5640_SCLK_SRC_MCLK;
		break;
	case RT5640_SCLK_S_PLL1:
		reg_val |= RT5640_SCLK_SRC_PLL1;
		break;
	case RT5640_SCLK_S_RCCLK:
		reg_val |= RT5640_SCLK_SRC_RCCLK;
		break;
	default:
		dev_err(codec->dev, "Invalid clock id (%d)\n", clk_id);
		return -EINVAL;
	}
	snd_soc_update_bits(codec, RT5640_GLB_CLK,
		RT5640_SCLK_SRC_MASK, reg_val);
	rt5640->sysclk = freq;
	rt5640->sysclk_src = clk_id;

	dev_dbg(dai->dev, "Sysclk is %dHz and clock id is %d\n", freq, clk_id);

	return 0;
}

/**
 * rt5640_pll_calc - Calcualte PLL M/N/K code.
 * @freq_in: external clock provided to codec.
 * @freq_out: target clock which codec works on.
 * @pll_code: Pointer to structure with M, N, K and bypass flag.
 *
 * Calcualte M/N/K code to configure PLL for codec. And K is assigned to 2
 * which make calculation more efficiently.
 *
 * Returns 0 for success or negative error code.
 */
static int rt5640_pll_calc(const unsigned int freq_in,
	const unsigned int freq_out, struct rt5640_pll_code *pll_code)
{
	int max_n = RT5640_PLL_N_MAX, max_m = RT5640_PLL_M_MAX;
	int k, n, m, red, n_t, m_t, in_t, out_t, red_t = abs(freq_out - freq_in);
	bool bypass = false;

	if (RT5640_PLL_INP_MAX < freq_in || RT5640_PLL_INP_MIN > freq_in)
		return -EINVAL;

	k=100000000/freq_out-2;
	if(k>31)
		k = 31;
	for (n_t = 0; n_t <= max_n; n_t++) {
		in_t = (2*freq_in / (k+2)) + (freq_in / (k+2)) * n_t;
		if (in_t < 0)
			continue;
		if (in_t == freq_out) {
			bypass = true;
			n = n_t;
			goto code_find;
		}
		red = abs(in_t - freq_out); //m bypass
		if (red < red_t) {
			bypass = true;
			n = n_t;
			m = m_t;
			if (red == 0)
				goto code_find;
			red_t = red;
		}
		for (m_t = 0; m_t <= max_m; m_t++) {
			out_t = in_t / (m_t + 2);
			red = abs(out_t - freq_out);
			if (red < red_t) {
				bypass = false;
				n = n_t;
				m = m_t;
				if (red == 0)
					goto code_find;
				red_t = red;
			}
		}
	}
	pr_debug("Only get approximation about PLL\n");

code_find:

	pll_code->m_bp = bypass;
	pll_code->m_code = m;
	pll_code->n_code = n;
	pll_code->k_code = k;
	return 0;
}

static int rt5640_set_dai_pll(struct snd_soc_dai *dai, int pll_id, int source,
			unsigned int freq_in, unsigned int freq_out)
{
	struct snd_soc_codec *codec = dai->codec;
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	struct rt5640_pll_code pll_code;
	int ret, dai_sel;

	if (source == rt5640->pll_src && freq_in == rt5640->pll_in &&
	    freq_out == rt5640->pll_out)
		return 0;

	if (!freq_in || !freq_out) {
		dev_dbg(codec->dev, "PLL disabled\n");

		rt5640->pll_in = 0;
		rt5640->pll_out = 0;
		snd_soc_update_bits(codec, RT5640_GLB_CLK,
			RT5640_SCLK_SRC_MASK, RT5640_SCLK_SRC_MCLK);
		return 0;
	}

	switch (source) {
	case RT5640_PLL1_S_MCLK:
		snd_soc_update_bits(codec, RT5640_GLB_CLK,
			RT5640_PLL1_SRC_MASK, RT5640_PLL1_SRC_MCLK);
		break;
	case RT5640_PLL1_S_BCLK1:
	case RT5640_PLL1_S_BCLK2:
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	case RT5640_PLL1_S_BCLK3:
#endif
		dai_sel = get_sdp_info(codec, dai->id);
		if (dai_sel < 0) {
			dev_err(codec->dev,
				"Failed to get sdp info: %d\n", dai_sel);
			return -EINVAL;
		}
		if (dai_sel & RT5640_U_IF1) {
			snd_soc_update_bits(codec, RT5640_GLB_CLK,
				RT5640_PLL1_SRC_MASK, RT5640_PLL1_SRC_BCLK1);
		}
		if (dai_sel & RT5640_U_IF2) {
			snd_soc_update_bits(codec, RT5640_GLB_CLK,
				RT5640_PLL1_SRC_MASK, RT5640_PLL1_SRC_BCLK2);
		}
		if (dai_sel & RT5640_U_IF3) {
			snd_soc_update_bits(codec, RT5640_GLB_CLK,
				RT5640_PLL1_SRC_MASK, RT5640_PLL1_SRC_BCLK3);
		}
		break;
	default:
		dev_err(codec->dev, "Unknown PLL source %d\n", source);
		return -EINVAL;
	}

	ret = rt5640_pll_calc(freq_in, freq_out, &pll_code);
	if (ret < 0) {
		dev_err(codec->dev, "Unsupport input clock %d\n", freq_in);
		return ret;
	}

	dev_dbg(codec->dev, "bypass=%d m=%d n=%d k=%d\n", pll_code.m_bp,
		(pll_code.m_bp ? 0 : pll_code.m_code), pll_code.n_code, pll_code.k_code);

	snd_soc_write(codec, RT5640_PLL_CTRL1,
		pll_code.n_code << RT5640_PLL_N_SFT | pll_code.k_code);
	snd_soc_write(codec, RT5640_PLL_CTRL2,
		(pll_code.m_bp ? 0 : pll_code.m_code) << RT5640_PLL_M_SFT |
		pll_code.m_bp << RT5640_PLL_M_BP_SFT);

	rt5640->pll_in = freq_in;
	rt5640->pll_out = freq_out;
	rt5640->pll_src = source;

	return 0;
}

/**
 * rt5640_index_show - Dump private registers.
 * @dev: codec device.
 * @attr: device attribute.
 * @buf: buffer for display.
 *
 * To show non-zero values of all private registers.
 *
 * Returns buffer length.
 */
static ssize_t rt5640_index_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;
	unsigned int val;
	int cnt = 0, i;

	cnt += sprintf(buf, "RT5640 index register\n");
	for (i = 0; i <= 0xb4; i++) {
		if (cnt + RT5640_REG_DISP_LEN >= PAGE_SIZE)
			break;
		val = rt5640_index_read(codec, i);
		if (!val)
			continue;
		cnt += snprintf(buf + cnt, RT5640_REG_DISP_LEN,
				"%02x: %04x\n", i, val);
	}

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	return cnt;
}

static ssize_t rt5640_index_store(struct device *dev,struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;
	unsigned int val=0,addr=0;
	int i;

	pr_debug("register \"%s\" count=%d\n",buf,count);
	for(i=0;i<count;i++) //address
	{
		if(*(buf+i) <= '9' && *(buf+i)>='0')
		{
			addr = (addr << 4) | (*(buf+i)-'0');
		}
		else if(*(buf+i) <= 'f' && *(buf+i)>='a')
		{
			addr = (addr << 4) | ((*(buf+i)-'a')+0xa);
		}
		else if(*(buf+i) <= 'F' && *(buf+i)>='A')
		{
			addr = (addr << 4) | ((*(buf+i)-'A')+0xa);
		}
		else
		{
			break;
		}
	}

	for(i=i+1 ;i<count;i++) //val
	{
		if(*(buf+i) <= '9' && *(buf+i)>='0')
		{
			val = (val << 4) | (*(buf+i)-'0');
		}
		else if(*(buf+i) <= 'f' && *(buf+i)>='a')
		{
			val = (val << 4) | ((*(buf+i)-'a')+0xa);
		}
		else if(*(buf+i) <= 'F' && *(buf+i)>='A')
		{
			val = (val << 4) | ((*(buf+i)-'A')+0xa);
		}
		else
		{
			break;
		}
	}
	pr_debug("addr=0x%x val=0x%x\n",addr,val);
	if(addr > RT5640_VENDOR_ID2 || val > 0xffff || val < 0)
		return count;

	if(i==count)
	{
		pr_debug("0x%02x = 0x%04x\n",addr,rt5640_index_read(codec, addr));
	}
	else
	{
		rt5640_index_write(codec, addr, val);
	}


	return count;
}
static DEVICE_ATTR(index_reg, 0444, rt5640_index_show, rt5640_index_store);

static ssize_t rt5640_codec_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;
	unsigned int val;
	int cnt = 0, i;

	cnt += sprintf(buf, "RT5640 codec register\n");
	for (i = 0; i <= RT5640_VENDOR_ID2; i++) {
		if (cnt + RT5640_REG_DISP_LEN >= PAGE_SIZE)
			break;
		// joe_cheng : hw_read is NULL and kernel panic is invoked.
		//             Using snd_soc_read() instead.
//		val = codec->hw_read(codec, i);
		val = snd_soc_read(codec, i);
		if (!val)
			continue;
		cnt += snprintf(buf + cnt, RT5640_REG_DISP_LEN,
				"%04x: %04x\n", i, val);
		//cnt += snprintf(buf + cnt, RT5640_REG_DISP_LEN,
		//		"#rng%02x  #rv%04x  #rd0\n", i, val);
	}

	if (cnt >= PAGE_SIZE)
		cnt = PAGE_SIZE - 1;

	return cnt;
}

static ssize_t rt5640_codec_store(struct device *dev,struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;
	unsigned int val=0,addr=0;
	int i;

	pr_debug("register \"%s\" count=%d\n",buf,count);
	for(i=0;i<count;i++) //address
	{
		if(*(buf+i) <= '9' && *(buf+i)>='0')
		{
			addr = (addr << 4) | (*(buf+i)-'0');
		}
		else if(*(buf+i) <= 'f' && *(buf+i)>='a')
		{
			addr = (addr << 4) | ((*(buf+i)-'a')+0xa);
		}
		else if(*(buf+i) <= 'F' && *(buf+i)>='A')
		{
			addr = (addr << 4) | ((*(buf+i)-'A')+0xa);
		}
		else
		{
			break;
		}
	}

	for(i=i+1 ;i<count;i++) //val
	{
		if(*(buf+i) <= '9' && *(buf+i)>='0')
		{
			val = (val << 4) | (*(buf+i)-'0');
		}
		else if(*(buf+i) <= 'f' && *(buf+i)>='a')
		{
			val = (val << 4) | ((*(buf+i)-'a')+0xa);
		}
		else if(*(buf+i) <= 'F' && *(buf+i)>='A')
		{
			val = (val << 4) | ((*(buf+i)-'A')+0xa);		
		}
		else
		{
			break;
		}
	}
	pr_debug("addr=0x%x val=0x%x\n",addr,val);
	if(addr > RT5640_VENDOR_ID2 || val > 0xffff || val < 0)
		return count;

	if(i==count)
	{
		pr_debug("0x%02x = 0x%04x\n",addr,codec->hw_read(codec, addr));
	}
	else
	{
		snd_soc_write(codec, addr, val);
	}

	return count;
}
 	
static DEVICE_ATTR(codec_reg, 0644, rt5640_codec_show, rt5640_codec_store);

static ssize_t rt5640_codec_adb_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;
	unsigned int val;
	int cnt = 0, i;

	for (i = 0; i < rt5640->adb_reg_num; i++) {
		if (cnt + RT5640_REG_DISP_LEN >= PAGE_SIZE)
			break;

		switch (rt5640->adb_reg_addr[i] & 0x30000) {
			case 0x10000:
				val = rt5640_index_read(codec, rt5640->adb_reg_addr[i] & 0xffff);
				break;
			case 0x20000:
				break;
			default:
				val = snd_soc_read(codec, rt5640->adb_reg_addr[i] & 0xffff);
		}

		cnt += snprintf(buf + cnt, RT5640_REG_DISP_LEN, "%04x: %04x\n",
				rt5640->adb_reg_addr[i], val);
	}

	return cnt;
}

static ssize_t rt5640_codec_adb_store(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;
	unsigned int value = 0;
	int i = 2, j = 0;

	if (buf[0] == 'R' || buf[0] == 'r') {
		while (j < 0x100 && i < count) {
			rt5640->adb_reg_addr[j] = 0;
			value = 0;
			for ( ; i < count; i++) {
				if (*(buf + i) <= '9' && *(buf + i) >= '0')
					value = (value << 4) | (*(buf + i) - '0');
				else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
					value = (value << 4) | ((*(buf + i) - 'a')+0xa);
				else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
					value = (value << 4) | ((*(buf + i) - 'A')+0xa);
				else
					break;
			}
			i++;

			rt5640->adb_reg_addr[j] = value;
			j++;
		}
		rt5640->adb_reg_num = j;
	} else if (buf[0] == 'W' || buf[0] == 'w') {
		while (j < 0x100 && i < count) {
			/* Get address */
			rt5640->adb_reg_addr[j] = 0;
			value = 0;
			for ( ; i < count; i++) {
				if (*(buf + i) <= '9' && *(buf + i) >= '0')
					value = (value << 4) | (*(buf + i) - '0');
				else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
					value = (value << 4) | ((*(buf + i) - 'a')+0xa);
				else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
					value = (value << 4) | ((*(buf + i) - 'A')+0xa);
				else
					break;
			}
			i++;
			rt5640->adb_reg_addr[j] = value;

			/* Get value */
			rt5640->adb_reg_value[j] = 0;
			value = 0;
			for ( ; i < count; i++) {
				if (*(buf + i) <= '9' && *(buf + i) >= '0')
					value = (value << 4) | (*(buf + i) - '0');
				else if (*(buf + i) <= 'f' && *(buf + i) >= 'a')
					value = (value << 4) | ((*(buf + i) - 'a')+0xa);
				else if (*(buf + i) <= 'F' && *(buf + i) >= 'A')
					value = (value << 4) | ((*(buf + i) - 'A')+0xa);
				else
					break;
			}
			i++;
			rt5640->adb_reg_value[j] = value;

			j++;
		}

		rt5640->adb_reg_num = j;

		for (i = 0; i < rt5640->adb_reg_num; i++) {
			switch (rt5640->adb_reg_addr[i] & 0x30000) {
			case 0x10000:
				rt5640_index_write(codec,
					rt5640->adb_reg_addr[i] & 0xffff,
					rt5640->adb_reg_value[i]);
				break;
			case 0x20000:
				break;
			default:
				snd_soc_write(codec,
					rt5640->adb_reg_addr[i] & 0xffff,
					rt5640->adb_reg_value[i]);
			}
		}

	}

	return count;
}
static DEVICE_ATTR(codec_reg_adb, 0664, rt5640_codec_adb_show, rt5640_codec_adb_store);

static int rt5640_set_bias_level(struct snd_soc_codec *codec,
			enum snd_soc_bias_level level)
{
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	switch (level) {
	case SND_SOC_BIAS_ON:
		pr_debug("In case SND_SOC_BIAS_ON:\n");
		break;

	case SND_SOC_BIAS_PREPARE:
		break;

	case SND_SOC_BIAS_STANDBY:
		pr_debug("In case SND_SOC_BIAS_STANDBY:\n");
		if (SND_SOC_BIAS_OFF == codec->dapm.bias_level) {
			pr_debug("%s change bias_level from SND_SOC_BIAS_OFF to SND_SOC_BIAS_STANDBY\n", __func__);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_VREF1 | RT5640_PWR_MB |
				RT5640_PWR_BG | RT5640_PWR_VREF2,
				RT5640_PWR_VREF1 | RT5640_PWR_MB |
				RT5640_PWR_BG | RT5640_PWR_VREF2);
			msleep(5);
			snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
				RT5640_PWR_FV1 | RT5640_PWR_FV2,
				RT5640_PWR_FV1 | RT5640_PWR_FV2);
#ifdef USE_ASRC
			snd_soc_write(codec, RT5640_GEN_CTRL1, 0x3f71); 
#else
			snd_soc_write(codec, RT5640_GEN_CTRL1, 0x3f01);
#endif
			if (0x5 <= rt5640->v_code) {
				snd_soc_update_bits(codec, RT5640_JD_CTRL,
					0x3, 0x3);
			}
			if (0x6 <= rt5640->v_code) {
				snd_soc_update_bits(codec, RT5640_GEN_CTRL1,
					0x0800, 0x0800);
			}			
			// joe_cheng
			if (headset_status == 1) {
				pr_debug("%s joe update 0x63 and 0x64\n", __func__);
				snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
						RT5640_PWR_LDO2, RT5640_PWR_LDO2);
				snd_soc_update_bits(codec, RT5640_PWR_ANLG2,
						RT5640_PWR_MB1, RT5640_PWR_MB1);
				headset_status = 0;
				msleep(30);
			} 

			codec->cache_only = false;
			codec->cache_sync = 1;
			snd_soc_cache_sync(codec);
			rt5640_index_sync(codec);
		}
		break;

	case SND_SOC_BIAS_OFF:
		//snd_soc_write(codec, RT5640_DEPOP_M1, 0x0004);
		//snd_soc_write(codec, RT5640_DEPOP_M2, 0x1100);
		snd_soc_write(codec, RT5640_PWR_DIG1, 0x0000);
		snd_soc_write(codec, RT5640_PWR_DIG2, 0x0000);
		snd_soc_write(codec, RT5640_PWR_VOL, 0x0000);
		snd_soc_write(codec, RT5640_PWR_MIXER, 0x0000);
		snd_soc_write(codec, RT5640_PWR_ANLG1, 0x0000);
		snd_soc_write(codec, RT5640_PWR_ANLG2, 0x0000);
		snd_soc_write(codec, RT5640_GEN_CTRL1, 0x3f00);
		break;

	default:
		break;
	}
	codec->dapm.bias_level = level;
	pr_debug("%s : bias_level %d\n", __func__, codec->dapm.bias_level);

	return 0;
}

static void do_hp_amp(struct work_struct *work)
{
	pr_debug("%s MX-61h %x\n", __func__, snd_soc_read(rt5640_codec, RT5640_PWR_DIG1));
	// joe_cheng : Workaround for TT#331186, TT#333088
        if (PROJ_ID != PROJ_ID_ME372CG) {
	    if ((snd_soc_read(rt5640_codec, RT5640_PWR_DIG1) & 0x18c0) == 0) {
		snd_soc_update_bits(rt5640_codec, RT5640_PWR_DIG1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1);
	    }
        }
//	msleep(20);
	snd_soc_update_bits(rt5640_codec, RT5640_DEPOP_M3,
		RT5640_CP_FQ1_MASK | RT5640_CP_FQ2_MASK | RT5640_CP_FQ3_MASK,
		(RT5640_CP_FQ_192_KHZ << RT5640_CP_FQ1_SFT) |
		(RT5640_CP_FQ_12_KHZ << RT5640_CP_FQ2_SFT) |
		(RT5640_CP_FQ_192_KHZ << RT5640_CP_FQ3_SFT));
	rt5640_index_write(rt5640_codec, RT5640_MAMP_INT_REG2, 0xfc00);
	snd_soc_update_bits(rt5640_codec, RT5640_DEPOP_M1,
		RT5640_SMT_TRIG_MASK, RT5640_SMT_TRIG_EN);
	snd_soc_update_bits(rt5640_codec, RT5640_DEPOP_M1,
		RT5640_RSTN_MASK, RT5640_RSTN_EN);
	snd_soc_update_bits(rt5640_codec, RT5640_DEPOP_M1,
		RT5640_RSTN_MASK | RT5640_HP_L_SMT_MASK | RT5640_HP_R_SMT_MASK,
		RT5640_RSTN_DIS | RT5640_HP_L_SMT_EN | RT5640_HP_R_SMT_EN);
	snd_soc_update_bits(rt5640_codec, RT5640_HP_VOL,
		RT5640_L_MUTE | RT5640_R_MUTE, 0);
	msleep(40);
	snd_soc_update_bits(rt5640_codec, RT5640_DEPOP_M1,
		RT5640_HP_SG_MASK | RT5640_HP_L_SMT_MASK |
		RT5640_HP_R_SMT_MASK, RT5640_HP_SG_DIS |
		RT5640_HP_L_SMT_DIS | RT5640_HP_R_SMT_DIS);
	/*
	pr_debug("%s MX-61h %x\n", __func__, snd_soc_read(rt5640_codec, RT5640_PWR_DIG1));
	// joe_cheng : Workaround for TT#331186, TT#333088
	if ((snd_soc_read(rt5640_codec, RT5640_PWR_DIG1) & 0x18c0) == 0) {
		snd_soc_update_bits(rt5640_codec, RT5640_PWR_DIG1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1);
	}
	msleep(5);
	snd_soc_update_bits(rt5640_codec, RT5640_HP_VOL,
		RT5640_L_MUTE | RT5640_R_MUTE, 0);
	msleep(65);
	*/
}

static void do_spk_amp_r(struct work_struct *work)
{
	pr_debug("%s\n", __func__);
	snd_soc_update_bits(rt5640_codec, RT5640_SPK_VOL,
			RT5640_R_MUTE, 0);
	msleep(10);
	snd_soc_update_bits(rt5640_codec, RT5640_PWR_DIG1,
			RT5640_PWR_CLS_D, RT5640_PWR_CLS_D);
}

static void do_spk_amp_l(struct work_struct *work)
{
	pr_debug("%s\n", __func__);
	snd_soc_update_bits(rt5640_codec, RT5640_SPK_VOL,
			RT5640_L_MUTE, 0);
	msleep(10);
	snd_soc_update_bits(rt5640_codec, RT5640_PWR_DIG1,
			RT5640_PWR_CLS_D, RT5640_PWR_CLS_D);
}

static void do_ringtone_drc(struct work_struct *work)
{
	pr_debug("%s\n", __func__);
	if (Hal_user_scenario_mode == RINGTONE && snd_soc_read(rt5640_codec, RT5640_DRC_AGC_1) != 0x6208) {
		printk("%s : Update ringtone drc\n", __func__);
		snd_soc_write(rt5640_codec, RT5640_DRC_AGC_1, 0x6208);
		snd_soc_write(rt5640_codec, RT5640_DRC_AGC_2, 0x1faa);
		snd_soc_write(rt5640_codec, RT5640_DRC_AGC_3, 0x0000);			
	}
}

static void do_DAC_unmute(struct work_struct *work)
{
        pr_info("%s\n", __func__);

        if ((snd_soc_read(rt5640_codec, RT5640_HP_VOL) & 0x8080) == 0) {
		rt5640_pmd_depop(rt5640_codec);
		// joe_cheng : Remove hp_amp_work for TT#353209, and the pop noise could be avoid by mute DAC
		snd_soc_update_bits(rt5640_codec, RT5640_PWR_DIG1, RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1,
				RT5640_PWR_DAC_L1 | RT5640_PWR_DAC_R1);
		if (Hal_user_scenario_mode == RINGTONE && hp_event_eq_val != 0) {
			snd_soc_update_bits(rt5640_codec, RT5640_EQ_CTRL2, RT5640_EQ_CTRL_MASK, hp_event_eq_val);
			snd_soc_update_bits(rt5640_codec, RT5640_EQ_CTRL1,
					RT5640_EQ_UPD, RT5640_EQ_UPD);
			snd_soc_update_bits(rt5640_codec, RT5640_EQ_CTRL1, RT5640_EQ_UPD, 0);
			hp_event_eq_val = 0;
		}
		rt5640_pmu_depop(rt5640_codec);
        }
}

static int rt5640_probe(struct snd_soc_codec *codec)
{
	struct rt5640_priv *rt5640 = snd_soc_codec_get_drvdata(codec);
	int ret;

	pr_info("Codec driver version %s\n", VERSION);
	
	// joe_cheng : fix rt5640 can't enter suspend mode
	codec->dapm.idle_bias_off = 1;
	
	PROJ_ID = Read_PROJ_ID();
        HW_ID = Read_HW_ID();

	ret = snd_soc_codec_set_cache_io(codec, 8, 16, SND_SOC_I2C);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to set cache I/O: %d\n", ret);
		return ret;
	}

        //ME175CG +3VSUS Enable
        if( PROJ_ID == PROJ_ID_ME175CG && (HW_ID == 6)) {
                ret = gpio_request_one(ME175CG_PIN_3VSUS_EN, GPIOF_DIR_OUT, "ME175CG_PIN_3VSUS_EN");
                if (ret) {
                        pr_err("%s() : Failed to request ME175CG PIN_3VSUS_EN\n", __func__);
                }
                gpio_set_value(ME175CG_PIN_3VSUS_EN, 1);
                pr_info("%s() : Enable ME175CG PIN_3VSUS_EN\n", __func__);
        } else {
                pr_info("%s() : No PIN_3VSUS_EN\n", __func__);
        }
 
	// PF400CG ER2:2, PR:6, MP:1
        if( (PROJ_ID == PROJ_ID_PF400CG && (HW_ID == 2 || HW_ID==6 || HW_ID == 1)) || PROJ_ID == PROJ_ID_A400CG) {
                ret = gpio_request_one(PF400CG_P_SPK_EN_5, GPIOF_DIR_OUT, "P_SPK_EN_5");
                if (ret) {
                        pr_err("%s() : Failed to request PF400CG P_SPK_EN_5\n", __func__);
                }
                gpio_set_value(PF400CG_P_SPK_EN_5, 1);
		pr_info("%s() : Enable P_SPK_EN_5\n", __func__);
        } else {
		pr_info("%s() : No P_SPK_EN_5\n", __func__);
	}

        // BT hardware switch
        gpio = CLV_I2S1_SEL;
        if (gpio > 0)
        {
                printk("@@ Already get CLV_I2S1_SEL name\n");
                ret = gpio_request_one(gpio, GPIOF_DIR_OUT, "CLV_I2S1_SEL");
                if (ret)
                        pr_err("gpio_request CLV_I2S1_SEL failed! \n");

                /*Set GPIO 90 O(L) to default => GPIO 90 Low:Codec; High:BT */
                gpio_direction_output(gpio, 0);
        }
        else
                pr_err("get_gpio CLV_I2S1_SEL failed! \n");

	// joe_cheng : codec reset pin 
	gpio = get_gpio_by_name("CDC_RST_PD");
	if (gpio > 0) {
		pr_debug("%s() : get CDC_RST_PD successed\n", __func__);
		ret = gpio_request_one(gpio, GPIOF_DIR_OUT, "cdc_rst_pd");
		if (ret) {
			pr_err("%s() : Request CDC_RST_PD failed\n", __func__);	
		}
		gpio_direction_output(gpio, 1);
		msleep(400);
	} else {
		pr_err("%s() : get CDC_RST_PD failed\n", __func__);
	}

	rt5640_reset(codec);
	snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
		RT5640_PWR_VREF1 | RT5640_PWR_MB |
		RT5640_PWR_BG | RT5640_PWR_VREF2,
		RT5640_PWR_VREF1 | RT5640_PWR_MB |
		RT5640_PWR_BG | RT5640_PWR_VREF2);
	msleep(10);
	snd_soc_update_bits(codec, RT5640_PWR_ANLG1,
		RT5640_PWR_FV1 | RT5640_PWR_FV2,
		RT5640_PWR_FV1 | RT5640_PWR_FV2);
	/* DMIC */
	if (rt5640->dmic_en == RT5640_DMIC1) {
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK, RT5640_GP2_PIN_DMIC1_SCL);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_1L_LH_MASK | RT5640_DMIC_1R_LH_MASK,
			RT5640_DMIC_1L_LH_RISING | RT5640_DMIC_1R_LH_FALLING);
	} else if (rt5640->dmic_en == RT5640_DMIC2) {
		snd_soc_update_bits(codec, RT5640_GPIO_CTRL1,
			RT5640_GP2_PIN_MASK, RT5640_GP2_PIN_DMIC1_SCL);
		snd_soc_update_bits(codec, RT5640_DMIC,
			RT5640_DMIC_2L_LH_MASK | RT5640_DMIC_2R_LH_MASK,
			RT5640_DMIC_2L_LH_FALLING | RT5640_DMIC_2R_LH_RISING);
	}
	snd_soc_write(codec, RT5640_GEN_CTRL2, 0x4040);
	rt5640->v_code = snd_soc_read(codec, RT5640_VENDOR_ID);
	pr_info("%s : vendor id read 0x%x=0x%x\n",__func__, RT5640_VENDOR_ID, rt5640->v_code);
	rt5640_reg_init(codec);
	if (0x5 <= rt5640->v_code) {
		snd_soc_update_bits(codec, RT5640_JD_CTRL, 0x3, 0x3);
		pr_debug("%s : RT5640_JD_CTRL %x\n", __func__, snd_soc_read(codec, RT5640_JD_CTRL));
	}
	if (0x6 <= rt5640->v_code) {
		snd_soc_update_bits(codec, RT5640_GEN_CTRL1, 0x0800, 0x0800);	
		pr_debug("%s : RT5640_GEN_CTRL1 %x\n", __func__, snd_soc_read(codec, RT5640_GEN_CTRL1));
	}
	DC_Calibrate(codec);
	// joe_cheng 20150304 : switch from BIAS_STANDBY to BIASOFF
	codec->dapm.bias_level = SND_SOC_BIAS_OFF;
	rt5640->codec = codec;

#if (CONFIG_SND_SOC_RT5642_MODULE | CONFIG_SND_SOC_RT5642)
	rt5640->dsp_sw = RT5640_DSP_AEC_NS_FENS;
	rt5640_dsp_probe(codec);
#endif

#ifdef RTK_IOCTL
#if defined(CONFIG_SND_HWDEP) || defined(CONFIG_SND_HWDEP_MODULE)
	struct rt56xx_ops *ioctl_ops = rt56xx_get_ioctl_ops();
	ioctl_ops->index_write = rt5640_index_write;
	ioctl_ops->index_read = rt5640_index_read;
	ioctl_ops->index_update_bits = rt5640_index_update_bits;
	ioctl_ops->ioctl_common = rt5640_ioctl_common;
	realtek_ce_init_hwdep(codec);
#endif
#endif
//bard
//#if 0
	ret = device_create_file(codec->dev, &dev_attr_index_reg);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to create index_reg sysfs files: %d\n", ret);
		return ret;
	}
	
	ret = device_create_file(codec->dev, &dev_attr_codec_reg);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to create codex_reg sysfs files: %d\n", ret);
		return ret;
	}
	
	ret = device_create_file(codec->dev, &dev_attr_codec_reg_adb);
	if (ret != 0) {
		dev_err(codec->dev,
			"Failed to create codec_reg_adb sysfs files: %d\n", ret);
		return ret;
	}
//#endif
//bard
	// joe_cheng : for ATD audio_codec_status
	ret = device_create_file(codec->dev, &dev_attr_audio_codec_status);
        if (ret < 0)
                pr_err("%s(): Failed to create audio_codec_status: %d\n", __func__, ret);
	ret_codec_status = 1;
	
#ifdef CONFIG_EEPROM_PADSTATION
	ret = device_create_file(codec->dev, &dev_attr_p72g_device_status);
        if (ret < 0)
                pr_err("%s(): Failed to create p72g_device_status: %d\n", __func__, ret);
#endif
	
	rt5640_codec = codec;
	// joe_cheng : temporarily disable mclk sw solution
//	setup_timer( &mclk_check_timer, mclk_check_timer_callback, 0 );
//	INIT_WORK(&mclk_check_work, mclk_check_work_handler);

	INIT_DELAYED_WORK(&hp_amp_work, do_hp_amp);
        INIT_DELAYED_WORK(&spk_amp_l_work, do_spk_amp_l);
        INIT_DELAYED_WORK(&spk_amp_r_work, do_spk_amp_r);
        INIT_DELAYED_WORK(&DAC_unmute_work, do_DAC_unmute);
        INIT_DELAYED_WORK(&ringtone_drc_work, do_ringtone_drc);
	
	pr_info("rt5640_probe return 0\n"); //bard

	return 0;
}

static int rt5640_remove(struct snd_soc_codec *codec)
{
	rt5640_set_bias_level(codec, SND_SOC_BIAS_OFF);
	return 0;
}

#ifdef CONFIG_PM
static int rt5640_suspend(struct snd_soc_codec *codec, pm_message_t state)
{
#if (CONFIG_SND_SOC_RT5642_MODULE | CONFIG_SND_SOC_RT5642)
	/* After opening LDO of DSP, then close LDO of codec.
	 * (1) DSP LDO power on
	 * (2) DSP core power off
	 * (3) DSP IIS interface power off
	 * (4) Toggle pin of codec LDO1 to power off
	 */
	rt5640_dsp_suspend(codec, state);
#endif
	printk("%s()\n", __func__);
	rt5640_set_bias_level(codec, SND_SOC_BIAS_OFF);

#if defined(CONFIG_ME302C_SPC_PWR_SET) 
	if (PROJ_ID == PROJ_ID_ME302C || PROJ_ID == PROJ_ID_ME372CG || PROJ_ID == PROJ_ID_GEMINI || PROJ_ID == PROJ_ID_ME372CL) {
		gpio_set_value(PIN_5VSUS_EN, 0);
		pr_debug("%s : 5v pull low %d\n", __func__, gpio_get_value(PIN_5VSUS_EN));
	}
        if( (PROJ_ID == PROJ_ID_PF400CG && (HW_ID == 2 || HW_ID==6 || HW_ID == 1)) || PROJ_ID == PROJ_ID_A400CG) {
	        gpio_set_value(PF400CG_P_SPK_EN_5, 0);
		pr_debug("%s : 5v pull low %d\n", __func__, gpio_get_value(PF400CG_P_SPK_EN_5));
	}
#endif	
	
	pr_debug("%s : jack detect %d\n", __func__, gpio_get_value(JACK_DETECT_PIN));
	// joe_cheng :  shutdown rt5640 
	if ( gpio != 0 ) {
		if (gpio_get_value(JACK_DETECT_PIN) != 0 && (snd_soc_read(codec, RT5640_PWR_ANLG1) & 0x4) && (snd_soc_read(codec, RT5640_PWR_ANLG2) & 0x0800)) {
			pr_err("%s headset is connected, rt5640 is not shutdown\n", __func__);
		} else {
			gpio_direction_output(gpio, 0);
			pr_info("%s : CDC_RST_PD %d\n", __func__, gpio_get_value(gpio));
			msleep(400);
		}
	}
	
	return 0;
}

static int rt5640_resume(struct snd_soc_codec *codec)
{
	// joe_cheng : 400ms is necessary for rt5640 activated. 
	pr_debug("%s : jack detect %d\n", __func__, gpio_get_value(JACK_DETECT_PIN));
	if (gpio != 0 ) {
		if (gpio_get_value(JACK_DETECT_PIN) != 0 && (snd_soc_read(codec, RT5640_PWR_ANLG1) & 0x4) && (snd_soc_read(codec, RT5640_PWR_ANLG2) & 0x0800)) {
			pr_err("%s : headset is connected, rt5640 is not shutdown\n", __func__);
		} else {
			gpio_direction_output(gpio, 1);
			pr_info("%s : CDC_RST_PD %d\n", __func__, gpio_get_value(gpio));
			msleep(400);
		}
	}
	
	rt5640_set_bias_level(codec, SND_SOC_BIAS_STANDBY);
#if defined(CONFIG_ME302C_SPC_PWR_SET) 	
	if (PROJ_ID == PROJ_ID_ME302C || PROJ_ID == PROJ_ID_ME372CG || PROJ_ID == PROJ_ID_GEMINI || PROJ_ID == PROJ_ID_ME372CL) {
		gpio_set_value(PIN_5VSUS_EN, 1);
		pr_debug("%s : 5v pull high %d\n", __func__, gpio_get_value(PIN_5VSUS_EN));
	}
        if( (PROJ_ID == PROJ_ID_PF400CG && (HW_ID == 2 || HW_ID==6 || HW_ID == 1)) || PROJ_ID == PROJ_ID_A400CG) {
	        gpio_set_value(PF400CG_P_SPK_EN_5, 1);
		pr_debug("%s : 5v pull low %d\n", __func__, gpio_get_value(PF400CG_P_SPK_EN_5));
	}
#endif	
	DC_Calibrate(codec);

#if (CONFIG_SND_SOC_RT5642_MODULE | CONFIG_SND_SOC_RT5642)
	/* After opening LDO of codec, then close LDO of DSP. */
	rt5640_dsp_resume(codec);
#endif
	return 0;
}
#else
#define rt5640_suspend NULL
#define rt5640_resume NULL
#endif

#define RT5640_STEREO_RATES SNDRV_PCM_RATE_8000_96000
#define RT5640_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE | \
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S8)

struct snd_soc_dai_ops rt5640_aif_dai_ops = {
	.hw_params = rt5640_hw_params,
	.prepare = rt5640_prepare,
	.set_fmt = rt5640_set_dai_fmt,
	.set_sysclk = rt5640_set_dai_sysclk,
	.set_pll = rt5640_set_dai_pll,
};

struct snd_soc_dai_driver rt5640_dai[] = {
	{
		.name = "rt5640-aif1",
		.id = RT5640_AIF1,
		.playback = {
			.stream_name = "AIF1 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5640_STEREO_RATES,
			.formats = RT5640_FORMATS,
		},
		.capture = {
			.stream_name = "AIF1 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5640_STEREO_RATES,
			.formats = RT5640_FORMATS,
		},
		.ops = &rt5640_aif_dai_ops,
	},
	{
		.name = "rt5640-aif2",
		.id = RT5640_AIF2,
		.playback = {
			.stream_name = "AIF2 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5640_STEREO_RATES,
			.formats = RT5640_FORMATS,
		},
		.capture = {
			.stream_name = "AIF2 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5640_STEREO_RATES,
			.formats = RT5640_FORMATS,
		},
		.ops = &rt5640_aif_dai_ops,
		.symmetric_rates = 1,
	},
#if (CONFIG_SND_SOC_RT5643_MODULE | CONFIG_SND_SOC_RT5643 | \
	CONFIG_SND_SOC_RT5646_MODULE | CONFIG_SND_SOC_RT5646 )
	{
		.name = "rt5640-aif3",
		.id = RT5640_AIF3,
		.playback = {
			.stream_name = "AIF3 Playback",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5640_STEREO_RATES,
			.formats = RT5640_FORMATS,
		},
		.capture = {
			.stream_name = "AIF3 Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = RT5640_STEREO_RATES,
			.formats = RT5640_FORMATS,
		},
		.ops = &rt5640_aif_dai_ops,
	},
#endif
};

static struct snd_soc_codec_driver soc_codec_dev_rt5640 = {
	.probe = rt5640_probe,
	.remove = rt5640_remove,
	.suspend = rt5640_suspend,
	.resume = rt5640_resume,
	.set_bias_level = rt5640_set_bias_level,
	.reg_cache_size = RT5640_VENDOR_ID2 + 1,
	.reg_word_size = sizeof(u16),
	.reg_cache_default = rt5640_reg,
	.volatile_register = rt5640_volatile_register,
	.readable_register = rt5640_readable_register,
	.reg_cache_step = 1,
	.controls = rt5640_snd_controls,
	.num_controls = ARRAY_SIZE(rt5640_snd_controls),
	.dapm_widgets = rt5640_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(rt5640_dapm_widgets),
	.dapm_routes = rt5640_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(rt5640_dapm_routes),
};

static const struct i2c_device_id rt5640_i2c_id[] = {
	{ "rt5640", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt5640_i2c_id);

static int rt5640_i2c_probe(struct i2c_client *i2c,
		    const struct i2c_device_id *id)
{
	struct rt5640_priv *rt5640;
	int ret;

	rt5640 = kzalloc(sizeof(struct rt5640_priv), GFP_KERNEL);
	if (NULL == rt5640)
		return -ENOMEM;

	i2c_set_clientdata(i2c, rt5640);

	ret = snd_soc_register_codec(&i2c->dev, &soc_codec_dev_rt5640,
			rt5640_dai, ARRAY_SIZE(rt5640_dai));
	if (ret < 0)
		kfree(rt5640);

	return ret;
}

static int rt5640_i2c_remove(struct i2c_client *i2c)
{
	snd_soc_unregister_codec(&i2c->dev);
	kfree(i2c_get_clientdata(i2c));
	return 0;
}

static int rt5640_i2c_shutdown(struct i2c_client *client)
{
	struct rt5640_priv *rt5640 = i2c_get_clientdata(client);
	struct snd_soc_codec *codec = rt5640->codec;

	if (codec != NULL)
		rt5640_set_bias_level(codec, SND_SOC_BIAS_OFF);

	return 0;
}

struct i2c_driver rt5640_i2c_driver = {
	.driver = {
		.name = "rt5640",
		.owner = THIS_MODULE,
	},
	.probe = rt5640_i2c_probe,
	.remove   = rt5640_i2c_remove,
	.shutdown = rt5640_i2c_shutdown,
	.id_table = rt5640_i2c_id,
};

static int __init rt5640_modinit(void)
{
	return i2c_add_driver(&rt5640_i2c_driver);
}
module_init(rt5640_modinit);

static void __exit rt5640_modexit(void)
{
	i2c_del_driver(&rt5640_i2c_driver);
}
module_exit(rt5640_modexit);

MODULE_DESCRIPTION("ASoC RT5640 driver");
MODULE_AUTHOR("Johnny Hsu <johnnyhsu@realtek.com>");
MODULE_LICENSE("GPL");
