//
// kernel.h
//
#ifndef _kernel_h
#define _kernel_h

#include <circle/memory.h>
#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/bcmrandom.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/types.h>

#ifdef V3D_LOAD_FROM_FILE
#include <circle/usb/usbhcidevice.h>
#include <circle/fs/fat/fatfs.h>
#endif

#include "v3d.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	// do not change this order
	CMemorySystem		m_Memory;
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CV3D				m_v3d;
	CBcmRandomNumberGenerator m_random;

#ifdef V3D_LOAD_FROM_FILE
	CUSBHCIDevice		m_USBHCI; // xzl: may bloat. to remove later.
//	CFATFileSystem		m_FileSystem;
//	CEMMCDevice		m_EMMC;
#endif

	// TODO: add more members here
//	CMyClass		m_MyObject;
};

#endif
