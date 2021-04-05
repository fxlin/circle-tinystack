//
// myclass.cpp
//
#include <circle/bcm2835.h>
#include <circle/bcm2836.h>
#include <circle/bcm2711.h>
#include <circle/memio.h>
#include <circle/bcmpropertytags.h>

#include <linux/kernel.h>
#include <linux/printk.h>

#include "v3d.h"
#include "v3d_regs.h"

static const char * FromKernel = "v3d";

CV3D::CV3D (void)
{
}

CV3D::~CV3D (void)
{
}

// cf: bcm2835_asb_enable
int CV3D::asb_enable(u32 reg)
{
	if (!reg)
		return 0;

	/* Enable the module's async AXI bridges. */
	ASB_WRITE(reg, ASB_READ(reg) & ~ASB_REQ_STOP);
	CTimer::Get()->usDelay(1000);
	if (ASB_READ(reg) & ASB_ACK)
			return -1;
	else
		return 0;
}

int CV3D::asb_disable(u32 reg)
{
	if (!reg)
		return 0;

	/* Enable the module's async AXI bridges. */
	ASB_WRITE(reg, ASB_READ(reg) | ASB_REQ_STOP);
	CTimer::Get()->usDelay(1000);
	if ((!ASB_READ(reg)) & ASB_ACK)
			return -1;
	else
		return 0;
}

void CV3D::set_clock(bool enable)
{
		CBcmPropertyTags Tags;
		TPropertyTagClockState st;

		st.nClockId = CLOCK_ID_V3D;
		st.nState = enable ? 1 : 0;

		if (!Tags.GetTag (PROPTAG_SET_CLOCK_STATE, &st, sizeof st))
			CLogger::Get()->Write (FromKernel, LogNotice, "set tag failed");
		else
			CLogger::Get()->Write (FromKernel, LogNotice, "set clock state %u", st.nState);
}

// cf: bcm2835_asb_power_on
void CV3D::asb_power_on(void)
{
	u32 pm_reg = PM_GRAFX;
	u32 asb_m_reg = ASB_V3D_M_CTRL;
	u32 asb_s_reg = ASB_V3D_S_CTRL;
	u32 reset_flags = PM_V3DRSTN;

	// can succeed...
//	u32 pm_reg = PM_IMAGE;
//	u32 asb_m_reg = ASB_ISP_S_CTRL;
//	u32 asb_s_reg = ASB_ISP_M_CTRL;
//	u32 reset_flags = PM_ISPRSTN;

	// also can succeed
//		asb_m_reg = ASB_H264_S_CTRL;
//		asb_s_reg = ASB_H264_M_CTRL;

#if 1

	set_clock(true);
	CTimer::Get()->usDelay(100);
	set_clock(false);

	/* Deassert the resets. */
	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~reset_flags);

	CLogger::Get()->Write (FromKernel, LogNotice, "before reset %08x. ARM_PM_BASE %08x",
			PM_READ(pm_reg), ARM_PM_BASE);

	PM_WRITE(pm_reg, PM_READ(pm_reg) | reset_flags);

	CLogger::Get()->Write (FromKernel, LogNotice, "after reset %08x",
				PM_READ(pm_reg)); // no change? still 0000_1000

	set_clock(true);
#endif

	CTimer::Get()->usDelay(100);

	// master
	int ret = asb_enable(asb_m_reg);
	if (!ret)
		CLogger::Get()->Write (FromKernel, LogNotice, "enable master ok");
	else
		CLogger::Get()->Write (FromKernel, LogNotice, "enable master failed. ret %d", ret);

	// slave
	ret = asb_enable(asb_s_reg);
	if (!ret)
		CLogger::Get()->Write (FromKernel, LogNotice, "enable slave ok");
	else
		CLogger::Get()->Write (FromKernel, LogNotice, "enable slave failed. ret %d", ret);

	CTimer::Get()->usDelay(100);
}

void CV3D::asb_power_off(void)
{
	u32 pm_reg = PM_GRAFX;
	u32 asb_m_reg = ASB_V3D_M_CTRL;
	u32 asb_s_reg = ASB_V3D_S_CTRL;
	u32 reset_flags = PM_V3DRSTN;

	int ret = asb_disable(asb_m_reg);
	if (!ret)
		CLogger::Get()->Write (FromKernel, LogNotice, "disable master ok");
	else
		CLogger::Get()->Write (FromKernel, LogNotice, "disable master failed. ret %d", ret);

	// slave
	ret = asb_disable(asb_s_reg);
	if (!ret)
		CLogger::Get()->Write (FromKernel, LogNotice, "disable slave ok");
	else
		CLogger::Get()->Write (FromKernel, LogNotice, "disable slave failed. ret %d", ret);

	PM_WRITE(pm_reg, PM_READ(pm_reg) & ~reset_flags);
}


void CV3D::power_on(void)
{
	asb_power_on();
}

void CV3D::dump_v3d_regs(void)
{
	printk("V3D_HUB_IDENT0: %08x", V3D_READ(V3D_HUB_IDENT0));
	printk("V3D_HUB_IDENT1: %08x", V3D_READ(V3D_HUB_IDENT1));
	printk("V3D_HUB_IDENT2: %08x", V3D_READ(V3D_HUB_IDENT2));
	printk("V3D_HUB_IDENT3: %08x", V3D_READ(V3D_HUB_IDENT3));
	printk("V3D_MMU_DEBUG_INFO: %08x", V3D_READ(V3D_MMU_DEBUG_INFO));
}

void CV3D::dump_asb_regs(void)
{
	printk("ASB_BRDG_VERSION: %08x (should be: 0)", ASB_READ(ASB_BRDG_VERSION));
	/* 0x62726467, "BRDG". cf bcm2835-power.c */
	printk("ASB_AXI_BRDG_ID: %08x (should be: 0x62726467)", ASB_READ(ASB_AXI_BRDG_ID));
}

/* cf: linux clk-raspberrypi.c */
enum rpi_firmware_clk_id {
	RPI_FIRMWARE_EMMC_CLK_ID = 1,
	RPI_FIRMWARE_UART_CLK_ID,
	RPI_FIRMWARE_ARM_CLK_ID,
	RPI_FIRMWARE_CORE_CLK_ID,
	RPI_FIRMWARE_V3D_CLK_ID,
	RPI_FIRMWARE_H264_CLK_ID,
	RPI_FIRMWARE_ISP_CLK_ID,
	RPI_FIRMWARE_SDRAM_CLK_ID,
	RPI_FIRMWARE_PIXEL_CLK_ID,
	RPI_FIRMWARE_PWM_CLK_ID,
	RPI_FIRMWARE_HEVC_CLK_ID,
	RPI_FIRMWARE_EMMC2_CLK_ID,
	RPI_FIRMWARE_M2MC_CLK_ID,
	RPI_FIRMWARE_PIXEL_BVB_CLK_ID,
	RPI_FIRMWARE_NUM_CLK_ID,
};

void CV3D::dump_clk_states(void)
{
	CBcmPropertyTags Tags;
	TPropertyTagClockState st;
	int len = 256, cnt;
	char *buf = new char(len), *p = buf;
	assert(buf);

	cnt = snprintf(buf, len, "clock:state ");
	buf += cnt; len -= cnt;

	for (int i = 1; i < RPI_FIRMWARE_NUM_CLK_ID; i++) {
		st.nClockId = i;
		if (!Tags.GetTag (PROPTAG_GET_CLOCK_STATE, &st, sizeof st)) {
			printk("get tag failed");
			break;
		} else {
			cnt = snprintf(buf, len, "clk_%d:%u ", i, st.nState);
			buf += cnt; len -= cnt;
		}
	}

	printk("%s\n", p);
	delete p;
//	free(buf);
}



/* These power domain indices are the firmware interface's indices
 * minus one.
 * linux: raspbeerrypi-power.h
 */
#define RPI_POWER_DOMAIN_I2C0		0
#define RPI_POWER_DOMAIN_I2C1		1
#define RPI_POWER_DOMAIN_I2C2		2
#define RPI_POWER_DOMAIN_VIDEO_SCALER	3
#define RPI_POWER_DOMAIN_VPU1		4
#define RPI_POWER_DOMAIN_HDMI		5
#define RPI_POWER_DOMAIN_USB		6
#define RPI_POWER_DOMAIN_VEC		7
#define RPI_POWER_DOMAIN_JPEG		8
#define RPI_POWER_DOMAIN_H264		9
#define RPI_POWER_DOMAIN_V3D		10
#define RPI_POWER_DOMAIN_ISP		11
#define RPI_POWER_DOMAIN_UNICAM0	12
#define RPI_POWER_DOMAIN_UNICAM1	13
#define RPI_POWER_DOMAIN_CCP2RX		14
#define RPI_POWER_DOMAIN_CSI2		15
#define RPI_POWER_DOMAIN_CPI		16
#define RPI_POWER_DOMAIN_DSI0		17
#define RPI_POWER_DOMAIN_DSI1		18
#define RPI_POWER_DOMAIN_TRANSPOSER	19
#define RPI_POWER_DOMAIN_CCP2TX		20
#define RPI_POWER_DOMAIN_CDP		21
#define RPI_POWER_DOMAIN_ARM		22

#define RPI_POWER_DOMAIN_COUNT		23


void CV3D::dump_pd_states(void)
{
		CBcmPropertyTags Tags;
		TPropertyTagPowerState r;
		int len = 256, cnt;
		char *buf = new char(len), *p = buf;
		assert(buf);

		for (int i = 0; i < RPI_POWER_DOMAIN_COUNT; i++) {
			r.nDeviceId = i + 1;
			if (!Tags.GetTag (PROPTAG_GET_DOMAIN_STATE, &r, sizeof r))
				CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
			else {
				cnt = snprintf(buf, len, "%d:%u ", i, r.nState);
				buf += cnt; len -= cnt;
			}
		}

		printk("pd:state = %s\n", p);
		delete p;
}

// turn all power domains on.
void CV3D::set_pd_states(void)
{
	CBcmPropertyTags Tags;
	TPropertyTagPowerState r;

	for (int i = 0; i < RPI_POWER_DOMAIN_COUNT; i++) {
		r.nDeviceId = i + 1;
		r.nState = 1;

		if (!Tags.GetTag (PROPTAG_SET_DOMAIN_STATE, &r, sizeof r))
			CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
//		else
//			CLogger::Get()->Write (FromKernel, LogNotice, "set domain %d: %u", i, r.nState);
	}
}

boolean CV3D::Init(void)
{

	/* test random num gen. to show reg read works */
#if 0
	CLogger::Get()->Write (FromKernel, LogNotice, "random: %08x", read32(ARM_HW_RNG200_BASE + 0x20));
	CLogger::Get()->Write (FromKernel, LogNotice, "random: %08x", read32(ARM_HW_RNG200_BASE + 0x20));
	CLogger::Get()->Write (FromKernel, LogNotice, "random: %08x", read32(ARM_HW_RNG200_BASE + 0x20));
#endif

	// check v3d clock state
	{
		CBcmPropertyTags Tags;
		TPropertyTagClockState st;
		st.nClockId = CLOCK_ID_V3D;

		if (!Tags.GetTag (PROPTAG_GET_CLOCK_STATE, &st, sizeof st))
			CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
		else
			CLogger::Get()->Write (FromKernel, LogNotice, "v3d clock state %u", st.nState);
	}

	// check v3d clock rate
	{
		CBcmPropertyTags Tags;
		TPropertyTagClockRate r;
		r.nClockId = CLOCK_ID_V3D;

		if (!Tags.GetTag (PROPTAG_GET_CLOCK_RATE, &r, sizeof r))
			printk("get tag failed");
		else
			printk("v3d clock rate %u", r.nRate);
	}

//	asb_power_off();
//	CLogger::Get()->Write (FromKernel, LogNotice, "reset status PM_GRAFX %08x",
//			PM_READ(PM_GRAFX) & PM_V3DRSTN);
//	power_on();
//	CLogger::Get()->Write (FromKernel, LogNotice, "reset status PM_GRAFX %08x",
//				PM_READ(PM_GRAFX) & PM_V3DRSTN);



	// enable all power domain states
//	{

//	}

	// set all clock states
	{
			CBcmPropertyTags Tags;
			TPropertyTagClockState st;
			st.nState = 1;

			for (int i = 1; i <14; i++) {
				st.nClockId = i;
				if (!Tags.GetTag (PROPTAG_SET_CLOCK_STATE, &st, sizeof st))
					CLogger::Get()->Write (FromKernel, LogNotice, "set tag failed");
				else
					CLogger::Get()->Write (FromKernel, LogNotice, "clock %d state %u", i, st.nState);
			}
	}

	dump_clk_states();
	dump_pd_states();
	set_pd_states();
	dump_pd_states();

	{
		CBcmPropertyTags Tags;
		TPropertyTagPowerState r;
//		r.nDeviceId = RPI_POWER_DOMAIN_V3D;

//		if (!Tags.GetTag (PROPTAG_GET_DOMAIN_STATE, &r, sizeof r))
//			CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
//		else
//			CLogger::Get()->Write (FromKernel, LogNotice, "get domain is %u", r.nState);
//
//		// set power domain
//		CLogger::Get()->Write (FromKernel, LogNotice, "set all power domain states...");
//		r.nState = 1 | POWER_STATE_WAIT;
//		if (!Tags.GetTag (PROPTAG_SET_DOMAIN_STATE, &r, sizeof r))
//			CLogger::Get()->Write (FromKernel, LogNotice, "set tag failed");

//		// get again..
//		//r.nDeviceId = RPI_POWER_DOMAIN_HDMI;
//		r.nDeviceId = RPI_POWER_DOMAIN_V3D;
//		if (!Tags.GetTag (PROPTAG_GET_DOMAIN_STATE, &r, sizeof r))
//					CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
//		else
//			CLogger::Get()->Write (FromKernel, LogNotice, "get domain v3d tag is %u", r.nState);
//

		// get all power domain states
//		{
//			CBcmPropertyTags Tags;
//			TPropertyTagPowerState r;
//
//			for (int i =0; i < 23; i++) {
//				r.nDeviceId = i;
//				if (!Tags.GetTag (PROPTAG_GET_DOMAIN_STATE, &r, sizeof r))
//					CLogger::Get()->Write (FromKernel, LogNotice, "get tag failed");
//				else
//					CLogger::Get()->Write (FromKernel, LogNotice, "domain %d: %u", i, r.nState);
//			}
//		}

		// and read regs...
		dump_v3d_regs();
		dump_asb_regs();

		power_on();

		// read reg again
		dump_v3d_regs();
	}

	return true;
}
// ...
