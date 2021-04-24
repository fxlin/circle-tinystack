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


/**
 * __wait_for - magic wait macro
 *
 * Macro to help avoid open coding check/wait/timeout patterns. Note that it's
 * important that we check the condition again after having timed out, since the
 * timeout could be due to preemption or similar and we've never had a chance to
 * check the condition before the timeout.
 *
 * xzl: @US: total timeout
 * @Wmin/Wmax. wait intervals. start from @Wmin, exponential growth until @WMax
 * will check OP before wait
 * return: 0 if success, otherwise error code
 */
#if 0
#define __wait_for(OP, COND, US, Wmin, Wmax) ({ \
	const ktime_t end__ = ktime_add_ns(ktime_get_raw(), 1000ll * (US)); \
	long wait__ = (Wmin); /* recommended min for usleep is 10 us */	\
	int ret__;							\
	might_sleep();							\
	for (;;) {							\
		const bool expired__ = ktime_after(ktime_get_raw(), end__); \
		OP;							\
		/* Guarantee COND check prior to timeout */		\
		barrier();						\
		if (COND) {						\
			ret__ = 0;					\
			break;						\
		}							\
		if (expired__) {					\
			ret__ = -ETIMEDOUT;				\
			break;						\
		}							\
		usleep_range(wait__, wait__ * 2);			\
		if (wait__ < (Wmax))					\
			wait__ <<= 1;					\
	}								\
	ret__;								\
})

#define _wait_for(COND, US, Wmin, Wmax)	__wait_for(, (COND), (US), (Wmin), \
						   (Wmax))
//#define wait_for(COND, MS)		_wait_for((COND), (MS) * 1000, 10, 1000)
#endif

// XXX should sleep instead of delay??
#define wait_for(COND, MS)		\
	({			\
		u32 u = 0, us = MS * 1000;		\
		u32 ret = 0; 										\
		while (!(COND) && (u += 100) < us) {	\
			CTimer::Get()->usDelay(100);	\
		}	\
		if (!(COND))	\
			ret = -1; 	\
	ret; 	\
})

class CV3D : public CDevice
{
public:
	CV3D (CInterruptSystem *pInterruptSystem = 0);
	~CV3D (void);

	boolean Init(void);
	void power_on(bool on = true);
	int Replay(void);

private:
//	uintptr  m_nBaseAddress;
	void asb_power_on(void);
	void asb_power_off(void);
	void clock_on(bool enable =  true);
//	CLogger			m_Logger;

	CInterruptSystem *m_pInterruptSystem;
};


#endif
