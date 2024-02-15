//
// kernel.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "kernel.h"
#include <circle/string.h>
#include <circle/usb/usbmassdevice.h> // xzl

#define PARTITION	"umsd1-1"
#define FILENAME	"circle.txt"

static const char FromKernel[] = "kernel";

#define TRACE_DEPTH 1024 * 1024 // xzl

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer),
	m_Tracer (TRACE_DEPTH, TRUE) // xzl
{
	m_ActLED.Blink (5);	// show we are alive
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

	m_Tracer.Start(); // xzl

	if (bOK)
	{
		bOK = m_USBHCI.Initialize ();
	}

//	m_Tracer.Stop(); // xzl

	return bOK;
}

struct TCHSAddress
{
	unsigned char Head;
	unsigned char Sector	   : 6,
		      CylinderHigh : 2;
	unsigned char CylinderLow;
}
PACKED;

struct TPartitionEntry
{
	unsigned char	Status;
	TCHSAddress	FirstSector;
	unsigned char	Type;
	TCHSAddress	LastSector;
	unsigned	LBAFirstSector;
	unsigned	NumberOfSectors;
}
PACKED;

struct TMasterBootRecord
{
	unsigned char	BootCode[0x1BE];
	TPartitionEntry	Partition[4];
	unsigned short	BootSignature;
	#define BOOT_SIGNATURE		0xAA55
}
PACKED;


TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "Compile time: " __DATE__ " " __TIME__);

	CTracer::Get()->Event(TRACE_READ_START);
	// load & parse MBR
	{
		CDevice *pUMSD1 = m_DeviceNameService.GetDevice ("umsd1", TRUE);
		if (pUMSD1 == 0) {
			m_Logger.Write (FromKernel, LogError, "USB mass storage device not found");
			return ShutdownHalt;
		}

		u64 ullOffset = 0 * UMSD_BLOCK_SIZE;
		if (pUMSD1->Seek (ullOffset) != ullOffset) {
			m_Logger.Write (FromKernel, LogError, "Seek error");
			return ShutdownHalt;
		}

		TMasterBootRecord MBR;
		if (pUMSD1->Read (&MBR, sizeof MBR) != (int) sizeof MBR) {
			m_Logger.Write (FromKernel, LogError, "Read error");
			return ShutdownHalt;
		}

		if (MBR.BootSignature != BOOT_SIGNATURE) {
			m_Logger.Write (FromKernel, LogError, "Boot signature not found");
			return ShutdownHalt;
		}

		m_Logger.Write (FromKernel, LogNotice, "Dumping the partition table");
		m_Logger.Write (FromKernel, LogNotice, "# Status Type  1stSector    Sectors");

		for (unsigned nPartition = 0; nPartition < 4; nPartition++) {
			m_Logger.Write (FromKernel, LogNotice, "%u %02X     %02X   %10u %10u",
					nPartition+1,
					(unsigned) MBR.Partition[nPartition].Status,
					(unsigned) MBR.Partition[nPartition].Type,
					MBR.Partition[nPartition].LBAFirstSector,
					MBR.Partition[nPartition].NumberOfSectors);
		}
	}
	CTracer::Get()->Event(TRACE_READ_END);
	m_Tracer.Stop(); // xzl

	// Mount file system
	CDevice *pPartition = m_DeviceNameService.GetDevice (PARTITION, TRUE);
	if (pPartition == 0)
	{
		m_Logger.Write (FromKernel, LogPanic, "Partition not found: %s", PARTITION);
	}

	if (!m_FileSystem.Mount (pPartition))
	{
		m_Logger.Write (FromKernel, LogPanic, "Cannot mount partition: %s", PARTITION);
	}

	// Show contents of root directory
	TDirentry Direntry;
	TFindCurrentEntry CurrentEntry;
	unsigned nEntry = m_FileSystem.RootFindFirst (&Direntry, &CurrentEntry);
	for (unsigned i = 0; nEntry != 0; i++)
	{
		if (!(Direntry.nAttributes & FS_ATTRIB_SYSTEM))
		{
			CString FileName;
			FileName.Format ("%-14s", Direntry.chTitle);

			m_Screen.Write ((const char *) FileName, FileName.GetLength ());

			if (i % 5 == 4)
			{
				m_Screen.Write ("\n", 1);
			}
		}

		nEntry = m_FileSystem.RootFindNext (&Direntry, &CurrentEntry);
	}
	m_Screen.Write ("\n", 1);

	// Create file and write to it
	unsigned hFile = m_FileSystem.FileCreate (FILENAME);
	if (hFile == 0)
	{
		m_Logger.Write (FromKernel, LogPanic, "Cannot create file: %s", FILENAME);
	}

	for (unsigned nLine = 1; nLine <= 5; nLine++)
	{
		CString Msg;
		Msg.Format ("Hello File! (Line %u)\n", nLine);

		if (m_FileSystem.FileWrite (hFile, (const char *) Msg, Msg.GetLength ()) != Msg.GetLength ())
		{
			m_Logger.Write (FromKernel, LogError, "Write error");
			break;
		}
	}

	for (unsigned i = 0; i < m_Tracer.Count(); i++) {
		CString Msg = m_Tracer.DumpString(i);
		if (m_FileSystem.FileWrite (hFile,
				(const char *) Msg, Msg.GetLength ()) != Msg.GetLength ()) {
			m_Logger.Write (FromKernel, LogError, "Write error line %u", i);
			break;
		}
	}

	if (!m_FileSystem.FileClose (hFile))
	{
		m_Logger.Write (FromKernel, LogPanic, "Cannot close file");
	} else
		m_Logger.Write (FromKernel, LogDebug, "done.");

	return ShutdownHalt;  // xzl





	// Reopen file, read it and display its contents
	hFile = m_FileSystem.FileOpen (FILENAME);
	if (hFile == 0)
	{
		m_Logger.Write (FromKernel, LogPanic, "Cannot open file: %s", FILENAME);
	}

	char Buffer[100];
	unsigned nResult;
	while ((nResult = m_FileSystem.FileRead (hFile, Buffer, sizeof Buffer)) > 0)
	{
		if (nResult == FS_ERROR)
		{
			m_Logger.Write (FromKernel, LogError, "Read error");
			break;
		}

		m_Screen.Write (Buffer, nResult);
	}
	
	if (!m_FileSystem.FileClose (hFile))
	{
		m_Logger.Write (FromKernel, LogPanic, "Cannot close file");
	}

	return ShutdownHalt;
}
