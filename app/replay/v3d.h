//
// myclass.h
//
#ifndef _template_myclass_h
#define _template_myclass_h

#include <circle/device.h>
#include <circle/interrupt.h>
#include <circle/sysconfig.h>
#include <circle/types.h>
#include <circle/logger.h>

class CV3D : public CDevice
{
public:
	CV3D (void);
	~CV3D (void);

	boolean Init(void);

private:
	uintptr  m_nBaseAddress;
	void power_on(void);
	int asb_enable(u32 reg);
	int asb_disable(u32 reg);
	void asb_power_on(void);
	void asb_power_off(void);
	void set_clock(bool enable);
	void dump_v3d_regs(void);
	void dump_asb_regs(void);
//	CLogger			m_Logger;
};

#endif
