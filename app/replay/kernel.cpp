//
// kernel.cpp
//
#include <circle/bcm2835.h>
#include <circle/bcm2711.h>
#include <circle/memio.h>

#include <linux/printk.h>
#include <linux/bug.h>

#include "kernel.h"

static const char FromKernel[] = "kernel";

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer)
#ifdef V3D_LOAD_FROM_FILE
	,m_USBHCI (&m_Interrupt, &m_Timer)
#endif
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK)
	{
		bOK = m_Screen.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		if (pTarget == 0)
		{
			pTarget = &m_Screen;
		}

		bOK = m_Logger.Initialize (pTarget);
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
	}

	// TODO: call Initialize () of added members here (if required)
//	m_v3d.Init();

	return bOK;
}

#ifdef V3D_LOAD_FROM_FILE
#include <fatfs/ff.h>
extern FATFS the_fs;
//extern CFATFileSystem * the_fs; // v3d.cpp
#endif

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "Compile time: " __DATE__ " " __TIME__);

#ifdef V3D_LOAD_FROM_FILE
	// Mount file system
//	CDevice *pPartition = m_DeviceNameService.GetDevice ("umsd1-1", TRUE);
//	assert(pPartition);
//	if (!m_FileSystem.Mount (pPartition)) {
//		printk("cannot mount fs partition"); BUG();
//	}
//
//	the_fs = &m_FileSystem;
	int ret;
	ret = f_mount (&the_fs, "USB:", 1);
	BUG_ON(ret != FR_OK);
	printk("mount ok");
#endif

	m_Logger.Write (FromKernel, LogNotice, "hello world -- and bye! %08x", m_random.GetNumber());
	m_Logger.Write (FromKernel, LogNotice, "hello world -- and bye! %08x", m_random.GetNumber());
	m_Logger.Write (FromKernel, LogNotice, "hello world -- and bye! %08x", m_random.GetNumber());

//	m_Timer.MsDelay(1000);
	m_v3d.Init();

//	int *p = new int(20);
//	delete p;

	// test memory
	printk("memory size is %lld MB", m_Memory.GetMemSize()/1024/1024);

	int secs = 10;
	printk("to reboot in %d secs...", secs);
	m_Timer.MsDelay(secs * 1000);

	return ShutdownReboot;	// will cause reboot

	return ShutdownHalt;
}
