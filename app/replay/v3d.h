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

// pm/clk functions
int asb_enable(u32 reg);
int asb_disable(u32 reg);
void asb_power_on(void);
void asb_power_off(void);

void dump_v3d_regs(void);
void dump_asb_regs(void);
void dump_clk_states(void);
void set_clk_states(void);
void set_pd_states(void);
void dump_pd_states(void);

class CV3D : public CDevice
{
public:
	CV3D (void);
	~CV3D (void);

	boolean Init(void);
	void power_on(bool on = true);

private:
	uintptr  m_nBaseAddress;
	void asb_power_on(void);
	void asb_power_off(void);
	void clock_on(bool enable =  true);
//	CLogger			m_Logger;
};


#endif
