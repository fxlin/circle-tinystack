//
// dwhcidevice.cpp
//
// Supports:
//	internal DMA only,
//	no ISO transfers
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2020  R. Stange <rsta2@o2online.de>
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
#include <circle/usb/dwhcidevice.h>
#include <circle/usb/dwhciframeschednper.h>
#include <circle/usb/dwhciframeschednsplit.h>
#include <circle/usb/dwhciframeschedper.h>
#include <circle/bcmpropertytags.h>
#include <circle/bcm2835.h>
#include <circle/synchronize.h>
#include <circle/logger.h>
#include <circle/koptions.h>
#include <circle/sysconfig.h>
#include <circle/debug.h>
#include <circle/tracer.h>	// xzl
#include <assert.h>

// xzl the low level code for usb host controller...

//
// Configuration
//
#define DWC_CFG_DYNAMIC_FIFO				// re-program FIFOs with these sizes:
	#define DWC_CFG_HOST_RX_FIFO_SIZE	1024	// number of 32 bit words
	#define DWC_CFG_HOST_NPER_TX_FIFO_SIZE	1024	// number of 32 bit words
	#define DWC_CFG_HOST_PER_TX_FIFO_SIZE	1024	// number of 32 bit words

enum TStageState
{
	StageStateNoSplitTransfer,
	StageStateStartSplit,
	StageStateCompleteSplit,
	StageStatePeriodicDelay,
	StageStateUnknown
};

enum TStageSubState
{
	StageSubStateWaitForChannelDisable,
	StageSubStateWaitForTransactionComplete,
	StageSubStateUnknown
};

static const char FromDWHCI[] = "dwhci"; // xzl: designware host controller

CDWHCIDevice::CDWHCIDevice (CInterruptSystem *pInterruptSystem, CTimer *pTimer, boolean bPlugAndPlay)
:	CUSBHostController (bPlugAndPlay),
	m_pInterruptSystem (pInterruptSystem),
	m_pTimer (pTimer),
	m_nChannels (0),
	m_nChannelAllocated (0),
	m_nWaitBlockAllocated (0),
	m_WaitBlockSpinLock (TASK_LEVEL),
	m_RootPort (this),
	m_bRootPortEnabled (FALSE),
	m_bShutdown (FALSE)
{
	assert (m_pInterruptSystem != 0);
	assert (m_pTimer != 0);

	for (unsigned nChannel = 0; nChannel < DWHCI_MAX_CHANNELS; nChannel++)
	{
		m_pStageData[nChannel] = 0;
	}

	for (unsigned nWaitBlock = 0; nWaitBlock < DWHCI_WAIT_BLOCKS; nWaitBlock++)
	{
		m_bWaiting[nWaitBlock] = FALSE;
	}
}

CDWHCIDevice::~CDWHCIDevice (void)
{
	m_bShutdown = TRUE;

	assert (m_pTimer != 0);
	m_pTimer->MsDelay (200);	// wait for completion of all transactions

	assert (m_pInterruptSystem != 0);
	m_pInterruptSystem->DisconnectIRQ (ARM_IRQ_USB);

	Reset ();

	CBcmPropertyTags Tags;
	TPropertyTagPowerState PowerState;
	PowerState.nDeviceId = DEVICE_ID_USB_HCD;
	PowerState.nState = POWER_STATE_OFF | POWER_STATE_WAIT;
	Tags.GetTag (PROPTAG_SET_POWER_STATE, &PowerState, sizeof PowerState);

	m_pInterruptSystem = 0;
	m_pTimer = 0;
}

// xzl: the init entry...
boolean CDWHCIDevice::Initialize (boolean bScanDevices)
{
#ifndef USE_USB_SOF_INTR
	if (IsPlugAndPlay ())
	{
		CLogger::Get ()->Write (FromDWHCI, LogWarning,
					"Using plug-and-play without USE_USB_SOF_INTR "
					"is not recommended");
	}
#endif

	// init class-specific allocators in USB library
	INIT_PROTECTED_CLASS_ALLOCATOR (CUSBRequest, DWHCI_MAX_CHANNELS*2, IRQ_LEVEL);
	INIT_PROTECTED_CLASS_ALLOCATOR (CDWHCITransferStageData, DWHCI_MAX_CHANNELS, IRQ_LEVEL);
	INIT_PROTECTED_CLASS_ALLOCATOR (CDWHCIFrameSchedulerNonPeriodic, DWHCI_MAX_CHANNELS, IRQ_LEVEL);
	INIT_PROTECTED_CLASS_ALLOCATOR (CDWHCIFrameSchedulerPeriodic, DWHCI_MAX_CHANNELS, IRQ_LEVEL);
	INIT_PROTECTED_CLASS_ALLOCATOR (CDWHCIFrameSchedulerNoSplit, DWHCI_MAX_CHANNELS, IRQ_LEVEL);

	PeripheralEntry ();

	assert (m_pInterruptSystem != 0);
	assert (m_pTimer != 0);

	CDWHCIRegister VendorId (DWHCI_CORE_VENDOR_ID);
	if (VendorId.Read () != 0x4F54280A)
	{
		CLogger::Get ()->Write (FromDWHCI, LogError, "Unknown vendor 0x%0X", VendorId.Get ());
		return FALSE;
	}

	if (!PowerOn ())
	{
		CLogger::Get ()->Write (FromDWHCI, LogError, "Cannot power on");
		return FALSE;
	}
	
	// Disable all interrupts
	CDWHCIRegister AHBConfig (DWHCI_CORE_AHB_CFG);
	AHBConfig.Read ();
	AHBConfig.And (~DWHCI_CORE_AHB_CFG_GLOBALINT_MASK);
	AHBConfig.Write ();
	
	assert (m_pInterruptSystem != 0);
	m_pInterruptSystem->ConnectIRQ (ARM_IRQ_USB, InterruptStub, this);

	if (!InitCore ())
	{
		CLogger::Get ()->Write (FromDWHCI, LogError, "Cannot initialize core");
		return FALSE;
	}
	
	EnableGlobalInterrupts ();
	
	if (!InitHost ())
	{
		CLogger::Get ()->Write (FromDWHCI, LogError, "Cannot initialize host");
		return FALSE;
	}

	PeripheralExit ();

	if (   !IsPlugAndPlay ()
	    || bScanDevices)
	{
		ReScanDevices ();
	}

	return TRUE;
}

void CDWHCIDevice::ReScanDevices (void)
{
	PeripheralEntry ();

	if (!m_bRootPortEnabled)
	{
		if (EnableRootPort ())
		{
			m_bRootPortEnabled = TRUE;

			if (!m_RootPort.Initialize ())
			{
				CLogger::Get ()->Write (FromDWHCI, LogWarning,
							"Cannot initialize root port");
			}
		}
		else
		{
			CLogger::Get ()->Write (FromDWHCI, LogWarning,
						"No device connected to root port");
		}
	}
	else
	{
		m_RootPort.ReScanDevices ();
	}

	PeripheralExit ();
}
// xzl: blocking == sync
// can see each xfer has up to 3 stages.
boolean CDWHCIDevice::SubmitBlockingRequest (CUSBRequest *pURB, unsigned nTimeoutMs)
{
	if (m_bShutdown)
	{
		return FALSE;
	}

	PeripheralEntry ();

	assert (pURB != 0);

	pURB->SetStatus (0);
	
	if (pURB->GetEndpoint ()->GetType () == EndpointTypeControl) // xzl:a control xfer
	{
		assert (nTimeoutMs == USB_TIMEOUT_NONE);

		TSetupData *pSetup = pURB->GetSetupData ();
		assert (pSetup != 0);

		if (pSetup->bmRequestType & REQUEST_IN) // xzl: data to host
		{
			assert (pURB->GetBufLen () > 0);
			
			if (   !TransferStage (pURB, FALSE, FALSE)
			    || !TransferStage (pURB, TRUE,  FALSE)
			    || !TransferStage (pURB, FALSE, TRUE))
			{
				return FALSE;
			}
		}
		else
		{
			if (pURB->GetBufLen () == 0)
			{
				if (   !TransferStage (pURB, FALSE, FALSE)
				    || !TransferStage (pURB, TRUE,  TRUE))
				{
					return FALSE;
				}
			}
			else
			{
				if (   !TransferStage (pURB, FALSE, FALSE)
				    || !TransferStage (pURB, FALSE, FALSE)
				    || !TransferStage (pURB, TRUE,  TRUE))
				{
					return FALSE;
				}
			}
		}
	}
	else // xzl: otherwise (bulk, interrupt) no iso
	{
		assert (   pURB->GetEndpoint ()->GetType () == EndpointTypeBulk
		        || pURB->GetEndpoint ()->GetType () == EndpointTypeInterrupt);
		assert (pURB->GetBufLen () > 0);
		
		if (!TransferStage (pURB, pURB->GetEndpoint ()->IsDirectionIn (), FALSE, nTimeoutMs))
		{
			return FALSE;
		}
	}
	
	PeripheralExit ();

	return TRUE;
}

boolean CDWHCIDevice::SubmitAsyncRequest (CUSBRequest *pURB, unsigned nTimeoutMs)
{
	if (m_bShutdown)
	{
		return FALSE;
	}

	PeripheralEntry ();

	assert (pURB != 0);
	assert (   pURB->GetEndpoint ()->GetType () == EndpointTypeBulk
		|| pURB->GetEndpoint ()->GetType () == EndpointTypeInterrupt);
	assert (pURB->GetBufLen () > 0);
	
	pURB->SetStatus (0);
	
	boolean bOK = TransferStageAsync (pURB, pURB->GetEndpoint ()->IsDirectionIn (),
					  FALSE, nTimeoutMs);

	PeripheralExit ();

	return bOK;
}

void CDWHCIDevice::CancelDeviceTransactions (CUSBDevice *pUSBDevice)
{
#ifdef USE_USB_SOF_INTR
	m_TransactionQueue.FlushDevice (pUSBDevice);
#endif
}

boolean CDWHCIDevice::DeviceConnected (void)
{
	CDWHCIRegister HostPort (DWHCI_HOST_PORT);

	return HostPort.Read () & DWHCI_HOST_PORT_CONNECT ? TRUE : FALSE;
}

TUSBSpeed CDWHCIDevice::GetPortSpeed (void)
{
	TUSBSpeed Result = USBSpeedUnknown;
	
	CDWHCIRegister HostPort (DWHCI_HOST_PORT);

	switch (DWHCI_HOST_PORT_SPEED (HostPort.Read ()))
	{
	case DWHCI_HOST_PORT_SPEED_HIGH:
		Result = USBSpeedHigh;
		break;

	case DWHCI_HOST_PORT_SPEED_FULL:
		Result = USBSpeedFull;
		break;

	case DWHCI_HOST_PORT_SPEED_LOW:
		Result = USBSpeedLow;
		break;

	default:
		break;
	}

	return Result;
}

boolean CDWHCIDevice::OvercurrentDetected (void)
{
	CDWHCIRegister HostPort (DWHCI_HOST_PORT);

	if (HostPort.Read () & DWHCI_HOST_PORT_OVERCURRENT)
	{
		return TRUE;
	}

	return FALSE;
}

void CDWHCIDevice::DisableRootPort (boolean bPowerOff)
{
	m_bRootPortEnabled = FALSE;

	CDWHCIRegister HostPort (DWHCI_HOST_PORT);
	HostPort.Read ();

	HostPort.And (~DWHCI_HOST_PORT_ENABLE);

	if (bPowerOff)
	{
		HostPort.And (~DWHCI_HOST_PORT_POWER);
	}

	HostPort.Write ();

#ifdef USE_USB_SOF_INTR
	m_TransactionQueue.Flush ();
#endif
}

boolean CDWHCIDevice::InitCore (void)
{
	CDWHCIRegister USBConfig (DWHCI_CORE_USB_CFG);
	USBConfig.Read ();
	if (CKernelOptions::Get ()->GetUSBFullSpeed ())
	{
		USBConfig.Or (DWHCI_CORE_USB_CFG_PHY_SEL_FS);
	}
	USBConfig.And (~DWHCI_CORE_USB_CFG_ULPI_EXT_VBUS_DRV);
	USBConfig.And (~DWHCI_CORE_USB_CFG_TERM_SEL_DL_PULSE);
	USBConfig.Write ();

	if (!Reset ())
	{
		CLogger::Get ()->Write (FromDWHCI, LogError, "Reset failed");
		return FALSE;
	}

	USBConfig.Read ();
	USBConfig.And (~DWHCI_CORE_USB_CFG_ULPI_UTMI_SEL);	// select UTMI+
	USBConfig.And (~DWHCI_CORE_USB_CFG_PHYIF);		// UTMI width is 8
	USBConfig.Write ();

	// Internal DMA mode only
	CDWHCIRegister HWConfig2 (DWHCI_CORE_HW_CFG2);
	HWConfig2.Read ();
	assert (DWHCI_CORE_HW_CFG2_ARCHITECTURE (HWConfig2.Get ()) == 2);

	USBConfig.Read ();
	if (   DWHCI_CORE_HW_CFG2_HS_PHY_TYPE (HWConfig2.Get ()) == DWHCI_CORE_HW_CFG2_HS_PHY_TYPE_ULPI
	    && DWHCI_CORE_HW_CFG2_FS_PHY_TYPE (HWConfig2.Get ()) == DWHCI_CORE_HW_CFG2_FS_PHY_TYPE_DEDICATED)
	{
		USBConfig.Or (DWHCI_CORE_USB_CFG_ULPI_FSLS);
		USBConfig.Or (DWHCI_CORE_USB_CFG_ULPI_CLK_SUS_M);
	}
	else
	{
		USBConfig.And (~DWHCI_CORE_USB_CFG_ULPI_FSLS);
		USBConfig.And (~DWHCI_CORE_USB_CFG_ULPI_CLK_SUS_M);
	}
	USBConfig.Write ();

	assert (m_nChannels == 0);
	m_nChannels = DWHCI_CORE_HW_CFG2_NUM_HOST_CHANNELS (HWConfig2.Get ());
	assert (4 <= m_nChannels && m_nChannels <= DWHCI_MAX_CHANNELS);

	CDWHCIRegister AHBConfig (DWHCI_CORE_AHB_CFG);
	AHBConfig.Read ();
	AHBConfig.Or (DWHCI_CORE_AHB_CFG_DMAENABLE);
	//AHBConfig.Or (DWHCI_CORE_AHB_CFG_AHB_SINGLE);		// if DMA single mode should be used
	AHBConfig.Or (DWHCI_CORE_AHB_CFG_WAIT_AXI_WRITES);
	AHBConfig.And (~DWHCI_CORE_AHB_CFG_MAX_AXI_BURST__MASK);
	//AHBConfig.Or (0 << DWHCI_CORE_AHB_CFG_MAX_AXI_BURST__SHIFT);	// max. AXI burst length 4
	AHBConfig.Write ();

	// HNP and SRP are not used
	USBConfig.Read ();
	USBConfig.And (~DWHCI_CORE_USB_CFG_HNP_CAPABLE);
	USBConfig.And (~DWHCI_CORE_USB_CFG_SRP_CAPABLE);
	USBConfig.Write ();

	EnableCommonInterrupts ();

	return TRUE;
}

boolean CDWHCIDevice::InitHost (void)
{
	// Restart the PHY clock
	CDWHCIRegister Power (ARM_USB_POWER, 0);
	Power.Write ();

	CDWHCIRegister HostConfig (DWHCI_HOST_CFG);
	HostConfig.Read ();
	HostConfig.And (~DWHCI_HOST_CFG_FSLS_PCLK_SEL__MASK);

	CDWHCIRegister HWConfig2 (DWHCI_CORE_HW_CFG2);
	CDWHCIRegister USBConfig (DWHCI_CORE_USB_CFG);
	if (   DWHCI_CORE_HW_CFG2_HS_PHY_TYPE (HWConfig2.Read ()) == DWHCI_CORE_HW_CFG2_HS_PHY_TYPE_ULPI
	    && DWHCI_CORE_HW_CFG2_FS_PHY_TYPE (HWConfig2.Get ()) == DWHCI_CORE_HW_CFG2_FS_PHY_TYPE_DEDICATED
	    && (USBConfig.Read () & DWHCI_CORE_USB_CFG_ULPI_FSLS))
	{
		HostConfig.Or (DWHCI_HOST_CFG_FSLS_PCLK_SEL_48_MHZ);
	}
	else
	{
		HostConfig.Or (DWHCI_HOST_CFG_FSLS_PCLK_SEL_30_60_MHZ);
	}

	HostConfig.Write ();

#ifdef DWC_CFG_DYNAMIC_FIFO
	CDWHCIRegister RxFIFOSize (DWHCI_CORE_RX_FIFO_SIZ, DWC_CFG_HOST_RX_FIFO_SIZE);
	RxFIFOSize.Write ();
	
	CDWHCIRegister NonPeriodicTxFIFOSize (DWHCI_CORE_NPER_TX_FIFO_SIZ, 0);
	NonPeriodicTxFIFOSize.Or (DWC_CFG_HOST_RX_FIFO_SIZE);
	NonPeriodicTxFIFOSize.Or (DWC_CFG_HOST_NPER_TX_FIFO_SIZE << 16);
	NonPeriodicTxFIFOSize.Write ();
	
	CDWHCIRegister HostPeriodicTxFIFOSize (DWHCI_CORE_HOST_PER_TX_FIFO_SIZ, 0);
	HostPeriodicTxFIFOSize.Or (DWC_CFG_HOST_RX_FIFO_SIZE + DWC_CFG_HOST_NPER_TX_FIFO_SIZE);
	HostPeriodicTxFIFOSize.Or (DWC_CFG_HOST_PER_TX_FIFO_SIZE << 16);
	HostPeriodicTxFIFOSize.Write ();
#endif

	FlushTxFIFO (0x10);	 	// Flush all TX FIFOs
	FlushRxFIFO ();

	CDWHCIRegister HostPort (DWHCI_HOST_PORT);
	HostPort.Read ();
	HostPort.And (~DWHCI_HOST_PORT_DEFAULT_MASK);
	if (!(HostPort.Get () & DWHCI_HOST_PORT_POWER))
	{
		HostPort.Or (DWHCI_HOST_PORT_POWER);
		HostPort.Write ();
	}
	
	EnableHostInterrupts ();

	return TRUE;
}

boolean CDWHCIDevice::EnableRootPort (void)
{
	unsigned nMsDelay = 510;
	CKernelOptions *pOptions = CKernelOptions::Get ();
	if (pOptions != 0)
	{
		unsigned nUSBPowerDelay = pOptions->GetUSBPowerDelay ();
		if (nUSBPowerDelay != 0)
		{
			nMsDelay = nUSBPowerDelay;
		}
	}

	CDWHCIRegister HostPort (DWHCI_HOST_PORT);
	if (!WaitForBit (&HostPort, DWHCI_HOST_PORT_CONNECT, TRUE, nMsDelay))
	{
		return FALSE;
	}
	
	m_pTimer->MsDelay (100);	// see USB 2.0 spec

	HostPort.Read ();
	HostPort.And (~DWHCI_HOST_PORT_DEFAULT_MASK);
	HostPort.Or (DWHCI_HOST_PORT_RESET);
	HostPort.Write ();
	
	m_pTimer->MsDelay (50);		// see USB 2.0 spec (tDRSTR)

	HostPort.Read ();
	HostPort.And (~DWHCI_HOST_PORT_DEFAULT_MASK);
	HostPort.And (~DWHCI_HOST_PORT_RESET);
	HostPort.Write ();

	// normally 10ms, seems to be too short for some devices
	m_pTimer->MsDelay (20);		// see USB 2.0 spec (tRSTRCY)

	return TRUE;
}

boolean CDWHCIDevice::PowerOn (void)
{
	CBcmPropertyTags Tags;
	TPropertyTagPowerState PowerState;
	PowerState.nDeviceId = DEVICE_ID_USB_HCD;
	PowerState.nState = POWER_STATE_ON | POWER_STATE_WAIT;
	if (   !Tags.GetTag (PROPTAG_SET_POWER_STATE, &PowerState, sizeof PowerState)
	    ||  (PowerState.nState & POWER_STATE_NO_DEVICE)
	    || !(PowerState.nState & POWER_STATE_ON))
	{
		return FALSE;
	}
	
	return TRUE;
}

boolean CDWHCIDevice::Reset (void)
{
	CDWHCIRegister Reset (DWHCI_CORE_RESET, 0);
	
	// wait for AHB master IDLE state
	if (!WaitForBit (&Reset, DWHCI_CORE_RESET_AHB_IDLE, TRUE, 100))
	{
		return FALSE;
	}
	
	// core soft reset
	Reset.Or (DWHCI_CORE_RESET_SOFT_RESET);
	Reset.Write ();

	if (!WaitForBit (&Reset, DWHCI_CORE_RESET_SOFT_RESET, FALSE, 10))
	{
		return FALSE;
	}
	
	m_pTimer->MsDelay (100);

	return TRUE;
}

void CDWHCIDevice::EnableGlobalInterrupts (void)
{
	CDWHCIRegister AHBConfig (DWHCI_CORE_AHB_CFG);
	AHBConfig.Read ();
	AHBConfig.Or (DWHCI_CORE_AHB_CFG_GLOBALINT_MASK);
	AHBConfig.Write ();
}

void CDWHCIDevice::EnableCommonInterrupts (void)
{
	CDWHCIRegister IntStatus (DWHCI_CORE_INT_STAT);		// Clear any pending interrupts
	IntStatus.SetAll ();
	IntStatus.Write ();

#if 0	// not used
	CDWHCIRegister IntMask (DWHCI_CORE_INT_MASK,   DWHCI_CORE_INT_MASK_MODE_MISMATCH
						     | DWHCI_CORE_INT_MASK_USB_SUSPEND
						     | DWHCI_CORE_INT_MASK_CON_ID_STS_CHNG
						     | DWHCI_CORE_INT_MASK_SESS_REQ_INTR
						     | DWHCI_CORE_INT_MASK_WKUP_INTR);
	IntMask.Write ();
#endif
}

void CDWHCIDevice::EnableHostInterrupts (void)
{
	CDWHCIRegister IntMask (DWHCI_CORE_INT_MASK, 0);
	IntMask.Write ();					// Disable all interrupts

	EnableCommonInterrupts ();

	IntMask.Read ();
	IntMask.Or (  DWHCI_CORE_INT_MASK_HC_INTR
#ifdef USE_USB_SOF_INTR
		    | DWHCI_CORE_INT_MASK_SOF_INTR
#endif
		   );
	if (IsPlugAndPlay ())
	{
		IntMask.Or (  DWHCI_CORE_INT_MASK_PORT_INTR
			    | DWHCI_CORE_INT_MASK_DISCONNECT);
	}
	IntMask.Write ();
}

void CDWHCIDevice::EnableChannelInterrupt (unsigned nChannel)
{
	CDWHCIRegister AllChanInterruptMask (DWHCI_HOST_ALLCHAN_INT_MASK);

	m_IntMaskSpinLock.Acquire ();

	AllChanInterruptMask.Read ();
	AllChanInterruptMask.Or (1 << nChannel);
	AllChanInterruptMask.Write ();

	m_IntMaskSpinLock.Release ();
}

void CDWHCIDevice::DisableChannelInterrupt (unsigned nChannel)
{
	CDWHCIRegister AllChanInterruptMask (DWHCI_HOST_ALLCHAN_INT_MASK);

	m_IntMaskSpinLock.Acquire ();

	AllChanInterruptMask.Read ();
	AllChanInterruptMask.And (~(1 << nChannel));
	AllChanInterruptMask.Write ();

	m_IntMaskSpinLock.Release ();
}

void CDWHCIDevice::FlushTxFIFO (unsigned nFIFO)
{
	CDWHCIRegister Reset (DWHCI_CORE_RESET, 0);
	Reset.Or (DWHCI_CORE_RESET_TX_FIFO_FLUSH);
	Reset.And (~DWHCI_CORE_RESET_TX_FIFO_NUM__MASK);
	Reset.Or (nFIFO << DWHCI_CORE_RESET_TX_FIFO_NUM__SHIFT);
	Reset.Write ();

	if (!WaitForBit (&Reset, DWHCI_CORE_RESET_TX_FIFO_FLUSH, FALSE, 10))
	{
		return;
	}
	
	m_pTimer->usDelay (1);		// Wait for 3 PHY clocks
}

void CDWHCIDevice::FlushRxFIFO (void)
{
	CDWHCIRegister Reset (DWHCI_CORE_RESET, 0);
	Reset.Or (DWHCI_CORE_RESET_RX_FIFO_FLUSH);
	Reset.Write ();

	if (!WaitForBit (&Reset, DWHCI_CORE_RESET_RX_FIFO_FLUSH, FALSE, 10))
	{
		return;
	}
	
	m_pTimer->usDelay (1);		// Wait for 3 PHY clocks
}
// xzl:  seems a core func. wrapper around TransferStageAsync()
// xzl: bIn: is it input? bStatus; is it status?
// xfer stage -- part of a transaction?
boolean CDWHCIDevice::TransferStage (CUSBRequest *pURB, boolean bIn, boolean bStatusStage,
				     unsigned nTimeoutMs)
{
	unsigned nWaitBlock = AllocateWaitBlock ();
	if (nWaitBlock >= DWHCI_WAIT_BLOCKS)
	{
		return FALSE;
	}
	
	assert (pURB != 0);
	pURB->SetCompletionRoutine (CompletionRoutine, (void *) (uintptr) nWaitBlock, this);

	assert (!m_bWaiting[nWaitBlock]);
	m_bWaiting[nWaitBlock] = TRUE;

	if (!TransferStageAsync (pURB, bIn, bStatusStage, nTimeoutMs))
	{
		m_bWaiting[nWaitBlock] = FALSE;
		FreeWaitBlock (nWaitBlock);

		return FALSE;
	}

	while (m_bWaiting[nWaitBlock])
	{
		// do nothing
	}

	FreeWaitBlock (nWaitBlock);

	return pURB->GetStatus ();
}
// xzl: callback of stage (??)
void CDWHCIDevice::CompletionRoutine (CUSBRequest *pURB, void *pParam, void *pContext)
{
	CDWHCIDevice *pThis = (CDWHCIDevice *) pContext;
	assert (pThis != 0);

	unsigned nWaitBlock = (unsigned) (uintptr) pParam;
	assert (nWaitBlock < DWHCI_WAIT_BLOCKS);

	pThis->m_bWaiting[nWaitBlock] = FALSE;
}
// xzl: a stage of a xfer. can have multiple tx. can be a setup/data/status stage.
// grab a new channel for it
boolean CDWHCIDevice::TransferStageAsync (CUSBRequest *pURB, boolean bIn, boolean bStatusStage,
					  unsigned nTimeoutMs)
{
	assert (pURB != 0);
	
#ifndef USE_USB_SOF_INTR
	unsigned nChannel = AllocateChannel ();
	if (nChannel >= m_nChannels)
	{
		return FALSE;
	}
#else
	unsigned nChannel = DWHCI_MAX_CHANNELS;		// unused
#endif

	CDWHCITransferStageData *pStageData =
		new CDWHCITransferStageData (nChannel, pURB, bIn, bStatusStage, nTimeoutMs);
	assert (pStageData != 0);

#ifndef USE_USB_SOF_INTR
	assert (m_pStageData[nChannel] == 0);
	m_pStageData[nChannel] = pStageData;

	EnableChannelInterrupt (nChannel);
#endif

	if (!pStageData->IsSplit ())
	{
		pStageData->SetState (StageStateNoSplitTransfer);
	}
	else
	{
		if (!pStageData->BeginSplitCycle ())
		{
#ifndef USE_USB_SOF_INTR
			DisableChannelInterrupt (nChannel);
#endif

			delete pStageData;
#ifndef USE_USB_SOF_INTR
			m_pStageData[nChannel] = 0;
			
			FreeChannel (nChannel);
#endif

			return FALSE;
		}

		pStageData->SetState (StageStateStartSplit);
		pStageData->SetSplitComplete (FALSE);
		pStageData->GetFrameScheduler ()->StartSplit ();
	}

#ifndef USE_USB_SOF_INTR
	StartTransaction (pStageData); // xzl: kickoff the 1st tx for the stage
#else
	QueueTransaction (pStageData); // xzl: if SOF, queue transaction until upcoming SOF?
#endif
	
	return TRUE;
}

#ifdef USE_USB_SOF_INTR

void CDWHCIDevice::QueueTransaction (CDWHCITransferStageData *pStageData)
{
	u16 usFrameNumber;

	assert (pStageData != 0);
	CDWHCIFrameScheduler *pFrameScheduler = pStageData->GetFrameScheduler ();
	if (pFrameScheduler != 0)
	{
		usFrameNumber = pFrameScheduler->GetFrameNumber ();
	}
	else
	{
		CDWHCIRegister FrameNumber (DWHCI_HOST_FRM_NUM);
		usFrameNumber = DWHCI_HOST_FRM_NUM_NUMBER (FrameNumber.Read ());

		usFrameNumber = (usFrameNumber+1) & DWHCI_MAX_FRAME_NUMBER;
	}

	m_TransactionQueue.Enqueue (pStageData, usFrameNumber);
}

void CDWHCIDevice::QueueDelayedTransaction (CDWHCITransferStageData *pStageData)
{
	assert (pStageData != 0);
	CUSBRequest *pURB = pStageData->GetURB ();
	assert (pURB != 0);

	u16 usFrameOffset = (u16) pURB->GetEndpoint ()->GetInterval ();
	CDWHCIRegister HostPort (DWHCI_HOST_PORT);
	if (DWHCI_HOST_PORT_SPEED (HostPort.Read ()) == DWHCI_HOST_PORT_SPEED_HIGH)
	{
		usFrameOffset *= 8;
	}
	assert (usFrameOffset < DWHCI_MAX_FRAME_NUMBER/2);

	u16 usFrameNumber;

	if (pStageData->IsSplit ())
	{
		CDWHCIFrameScheduler *pFrameScheduler = pStageData->GetFrameScheduler ();
		assert (pFrameScheduler != 0);
		pFrameScheduler->PeriodicDelay (usFrameOffset);
		usFrameNumber = pFrameScheduler->GetFrameNumber ();

		pStageData->SetState (StageStateStartSplit);
		pStageData->SetSplitComplete (FALSE);
		pFrameScheduler->StartSplit ();
	}
	else
	{
		CDWHCIRegister FrameNumber (DWHCI_HOST_FRM_NUM);
		usFrameNumber = DWHCI_HOST_FRM_NUM_NUMBER (FrameNumber.Read ());
		usFrameNumber = (usFrameNumber+usFrameOffset) & DWHCI_MAX_FRAME_NUMBER;

		pStageData->SetState (StageStateNoSplitTransfer);
	}

	m_TransactionQueue.Enqueue (pStageData, usFrameNumber);
}

#endif

void CDWHCIDevice::StartTransaction (CDWHCITransferStageData *pStageData)
{
	assert (pStageData != 0);
	unsigned nChannel = pStageData->GetChannelNumber ();
	assert (nChannel < m_nChannels);
	
	// channel must be disabled, if not already done but controller
	// xzl: the channel is allocated for / used throughout the entire stage.
	CDWHCIRegister Character (DWHCI_HOST_CHAN_CHARACTER (nChannel));
	Character.Read ();
	if (Character.IsSet (DWHCI_HOST_CHAN_CHARACTER_ENABLE))
	{ // xzl: if enabled, force the channel off - which will be notified by irq (?)
		pStageData->SetSubState (StageSubStateWaitForChannelDisable);
		
		Character.And (~DWHCI_HOST_CHAN_CHARACTER_ENABLE);
		Character.Or (DWHCI_HOST_CHAN_CHARACTER_DISABLE);
		Character.Write ();

		CDWHCIRegister ChanInterruptMask (DWHCI_HOST_CHAN_INT_MASK (nChannel));
		ChanInterruptMask.Set (DWHCI_HOST_CHAN_INT_HALTED);
		ChanInterruptMask.Write ();
	}
	else
	{
		StartChannel (pStageData);
	}
}
// xzl: start tx on a channel. the chan is already ready. may have been used for
// 		earlier stages of this stage/xfer
// called for a transaction.
// @pStageData - the context for the stage
void CDWHCIDevice::StartChannel (CDWHCITransferStageData *pStageData)
{
	assert (pStageData != 0);
	unsigned nChannel = pStageData->GetChannelNumber ();
	assert (nChannel < m_nChannels);
	
	pStageData->SetSubState (StageSubStateWaitForTransactionComplete);

	// reset all pending channel interrupts
	CDWHCIRegister ChanInterrupt (DWHCI_HOST_CHAN_INT (nChannel));
	ChanInterrupt.SetAll ();
	ChanInterrupt.Write ();
	
	// set transfer size, packet count and pid
	// xzl: the "transfer" seems for this tx. e.g. xfer size is per tx.
	// 			it's not a USB transfer (multi stages...)
	CDWHCIRegister TransferSize (DWHCI_HOST_CHAN_XFER_SIZ (nChannel), 0);
	TransferSize.Or (pStageData->GetBytesToTransfer () & DWHCI_HOST_CHAN_XFER_SIZ_BYTES__MASK);
	TransferSize.Or ((pStageData->GetPacketsToTransfer () << DWHCI_HOST_CHAN_XFER_SIZ_PACKETS__SHIFT)
			 & DWHCI_HOST_CHAN_XFER_SIZ_PACKETS__MASK);
	TransferSize.Or (pStageData->GetPID () << DWHCI_HOST_CHAN_XFER_SIZ_PID__SHIFT);
	TransferSize.Write ();

	// set DMA address
	CDWHCIRegister DMAAddress (DWHCI_HOST_CHAN_DMA_ADDR (nChannel),
				   BUS_ADDRESS (pStageData->GetDMAAddress ()));
	DMAAddress.Write ();

	CleanAndInvalidateDataCacheRange (pStageData->GetDMAAddress (), pStageData->GetBytesToTransfer ());

	// set split control (xzl: keep split tx going or finish one)
	CDWHCIRegister SplitControl (DWHCI_HOST_CHAN_SPLIT_CTRL (nChannel), 0);
	if (pStageData->IsSplit ())
	{
		SplitControl.Or (pStageData->GetHubPortAddress ());
		SplitControl.Or (   pStageData->GetHubAddress ()
				 << DWHCI_HOST_CHAN_SPLIT_CTRL_HUB_ADDRESS__SHIFT);
		SplitControl.Or (   pStageData->GetSplitPosition ()
				 << DWHCI_HOST_CHAN_SPLIT_CTRL_XACT_POS__SHIFT);
		if (pStageData->IsSplitComplete ())
		{
			SplitControl.Or (DWHCI_HOST_CHAN_SPLIT_CTRL_COMPLETE_SPLIT);
		}
		SplitControl.Or (DWHCI_HOST_CHAN_SPLIT_CTRL_SPLIT_ENABLE);
	}
	SplitControl.Write ();

	// set channel parameters
	CDWHCIRegister Character (DWHCI_HOST_CHAN_CHARACTER (nChannel));
	Character.Read ();
	Character.And (~DWHCI_HOST_CHAN_CHARACTER_MAX_PKT_SIZ__MASK);
	Character.Or (pStageData->GetMaxPacketSize () & DWHCI_HOST_CHAN_CHARACTER_MAX_PKT_SIZ__MASK);

	Character.And (~DWHCI_HOST_CHAN_CHARACTER_MULTI_CNT__MASK);
	Character.Or (1 << DWHCI_HOST_CHAN_CHARACTER_MULTI_CNT__SHIFT);		// TODO: optimize

	if (pStageData->IsDirectionIn ())
	{
		Character.Or (DWHCI_HOST_CHAN_CHARACTER_EP_DIRECTION_IN);
	}
	else
	{
		Character.And (~DWHCI_HOST_CHAN_CHARACTER_EP_DIRECTION_IN);
	}

	if (pStageData->GetSpeed () == USBSpeedLow)
	{
		Character.Or (DWHCI_HOST_CHAN_CHARACTER_LOW_SPEED_DEVICE);
	}
	else
	{
		Character.And (~DWHCI_HOST_CHAN_CHARACTER_LOW_SPEED_DEVICE);
	}

	Character.And (~DWHCI_HOST_CHAN_CHARACTER_DEVICE_ADDRESS__MASK);
	Character.Or (pStageData->GetDeviceAddress () << DWHCI_HOST_CHAN_CHARACTER_DEVICE_ADDRESS__SHIFT);

	Character.And (~DWHCI_HOST_CHAN_CHARACTER_EP_TYPE__MASK);
	Character.Or (pStageData->GetEndpointType () << DWHCI_HOST_CHAN_CHARACTER_EP_TYPE__SHIFT);

	Character.And (~DWHCI_HOST_CHAN_CHARACTER_EP_NUMBER__MASK);
	Character.Or (pStageData->GetEndpointNumber () << DWHCI_HOST_CHAN_CHARACTER_EP_NUMBER__SHIFT);

#ifndef USE_QEMU_USB_FIX
	CDWHCIFrameScheduler *pFrameScheduler = pStageData->GetFrameScheduler ();
	if (pFrameScheduler != 0)
	{
#ifndef USE_USB_SOF_INTR
		pFrameScheduler->WaitForFrame ();
#endif

		if (pFrameScheduler->IsOddFrame ())
		{
			Character.Or (DWHCI_HOST_CHAN_CHARACTER_PER_ODD_FRAME);
		}
		else
		{
			Character.And (~DWHCI_HOST_CHAN_CHARACTER_PER_ODD_FRAME);
		}
	}
#endif

	CDWHCIRegister ChanInterruptMask (DWHCI_HOST_CHAN_INT_MASK(nChannel));
	ChanInterruptMask.Set (pStageData->GetStatusMask ());
	ChanInterruptMask.Write ();
	
	Character.Or (DWHCI_HOST_CHAN_CHARACTER_ENABLE);
	Character.And (~DWHCI_HOST_CHAN_CHARACTER_DISABLE);
	Character.Write ();
}
// xzl: a channel is bound to a stage which has its FSM to drive
void CDWHCIDevice::ChannelInterruptHandler (unsigned nChannel)
{
	if (m_bShutdown)
	{
		return;
	}

	CDWHCITransferStageData *pStageData = m_pStageData[nChannel];
	assert (pStageData != 0);
	CUSBRequest *pURB = pStageData->GetURB ();
	assert (pURB != 0);

	if (!m_bRootPortEnabled)
	{
		DisableChannelInterrupt (nChannel);

		pURB->SetStatus (0);
		pURB->SetUSBError (USBErrorAborted);

		delete pStageData;
		m_pStageData[nChannel] = 0;

		FreeChannel (nChannel);

		pURB->CallCompletionRoutine ();

		return;
	}

	// xzl: drive "substate" FSM. prior to a tx? in case that we haven't get hold of this chan?
	switch (pStageData->GetSubState ())
	{
	case StageSubStateWaitForChannelDisable:// xzl: was waiting for the chan. now chan is ready
		StartChannel (pStageData);
		return;

	case StageSubStateWaitForTransactionComplete: {
		CleanAndInvalidateDataCacheRange (pStageData->GetDMAAddress (), pStageData->GetBytesToTransfer ());

		CDWHCIRegister TransferSize (DWHCI_HOST_CHAN_XFER_SIZ (nChannel));
		TransferSize.Read ();

		CDWHCIRegister ChanInterrupt (DWHCI_HOST_CHAN_INT (nChannel));

		// restart halted transaction
		if (ChanInterrupt.Read () == DWHCI_HOST_CHAN_INT_HALTED)
		{
#ifndef USE_USB_SOF_INTR
			StartTransaction (pStageData);
#else
			m_pStageData[nChannel] = 0;
			FreeChannel (nChannel); // xzl: use the channel once and then done?

			QueueTransaction (pStageData);
#endif
			return;
		}

		assert (   !pStageData->IsPeriodic ()
			||    DWHCI_HOST_CHAN_XFER_SIZ_PID (TransferSize.Get ())
			   != DWHCI_HOST_CHAN_XFER_SIZ_PID_MDATA);

		pStageData->TransactionComplete (ChanInterrupt.Read (),
			DWHCI_HOST_CHAN_XFER_SIZ_PACKETS (TransferSize.Get ()),
			TransferSize.Get () & DWHCI_HOST_CHAN_XFER_SIZ_BYTES__MASK);
		} break;
	
	default:
		assert (0);
		break;
	}

	unsigned nStatus;
	// xzl: drive FSM of this stage
	switch (pStageData->GetState ())
	{
	case StageStateNoSplitTransfer:
		nStatus = pStageData->GetTransactionStatus ();
		if (nStatus & DWHCI_HOST_CHAN_INT_ERROR_MASK)
		{
			CLogger::Get ()->Write (FromDWHCI, LogError,
						"Transaction failed (status 0x%X)", nStatus);

			pURB->SetStatus (0);
			pURB->SetUSBError (pStageData->GetUSBError ());
		}
		else if (   (nStatus & (DWHCI_HOST_CHAN_INT_NAK | DWHCI_HOST_CHAN_INT_NYET))
			 && pStageData->IsPeriodic ())
		{
			if (pStageData->IsTimeout ())
			{
				pURB->SetStatus (0);
				pURB->SetUSBError (USBErrorTimeout);
			}
			else
			{
#ifdef USE_USB_SOF_INTR
				m_pStageData[nChannel] = 0;
				FreeChannel (nChannel);

				QueueDelayedTransaction (pStageData);
#else
				pStageData->SetState (StageStatePeriodicDelay);

				unsigned nInterval = pURB->GetEndpoint ()->GetInterval ();

				m_pTimer->StartKernelTimer (MSEC2HZ (nInterval), TimerStub,
							    pStageData, this);
#endif

				break;
			}
		}
		else
		{
			if (!pStageData->IsStatusStage ())
			{
				pURB->SetResultLen (pStageData->GetResultLen ());
			}

			pURB->SetStatus (1);
		}
		
		DisableChannelInterrupt (nChannel);

		delete pStageData;
		m_pStageData[nChannel] = 0;

		FreeChannel (nChannel);

		pURB->CallCompletionRoutine ();
		break;

	case StageStateStartSplit:
		nStatus = pStageData->GetTransactionStatus ();
		if (   (nStatus & DWHCI_HOST_CHAN_INT_ERROR_MASK)
		    || (nStatus & DWHCI_HOST_CHAN_INT_NAK)
		    || (nStatus & DWHCI_HOST_CHAN_INT_NYET))
		{
			CLogger::Get ()->Write (FromDWHCI, LogError,
						"Transaction failed (status 0x%X)", nStatus);

			pURB->SetStatus (0);
			pURB->SetUSBError (pStageData->GetUSBError ());

			DisableChannelInterrupt (nChannel);

			delete pStageData;
			m_pStageData[nChannel] = 0;

			FreeChannel (nChannel);

			pURB->CallCompletionRoutine ();
			break;
		}

		pStageData->GetFrameScheduler ()->TransactionComplete (nStatus);

		pStageData->SetState (StageStateCompleteSplit);
		pStageData->SetSplitComplete (TRUE);

		if (!pStageData->GetFrameScheduler ()->CompleteSplit ())
		{
			goto LeaveCompleteSplit;
		}
		
#ifndef USE_USB_SOF_INTR
		StartTransaction (pStageData);
#else
		m_pStageData[nChannel] = 0;
		FreeChannel (nChannel);

		QueueTransaction (pStageData);
#endif
		break;
		
	case StageStateCompleteSplit:
		nStatus = pStageData->GetTransactionStatus ();
		if (nStatus & DWHCI_HOST_CHAN_INT_ERROR_MASK)
		{
			CLogger::Get ()->Write (FromDWHCI, LogError,
						"Transaction failed (status 0x%X)", nStatus);

			pURB->SetStatus (0);
			pURB->SetUSBError (pStageData->GetUSBError ());

			DisableChannelInterrupt (nChannel);

			delete pStageData;
			m_pStageData[nChannel] = 0;

			FreeChannel (nChannel);

			pURB->CallCompletionRoutine ();
			break;
		}
		
		pStageData->GetFrameScheduler ()->TransactionComplete (nStatus);

		if (pStageData->GetFrameScheduler ()->CompleteSplit ())
		{
#ifndef USE_USB_SOF_INTR
			StartTransaction (pStageData);
#else
			m_pStageData[nChannel] = 0;
			FreeChannel (nChannel);

			QueueTransaction (pStageData);
#endif
			break;
		}

	LeaveCompleteSplit:
		if (!pStageData->IsStageComplete ())
		{
			if (!pStageData->BeginSplitCycle ())
			{
				pURB->SetStatus (0);
				pURB->SetUSBError (USBErrorSplit);

				DisableChannelInterrupt (nChannel);

				delete pStageData;
				m_pStageData[nChannel] = 0;

				FreeChannel (nChannel);

				pURB->CallCompletionRoutine ();
				break;
			}

			if (!pStageData->IsPeriodic ())
			{
				pStageData->SetState (StageStateStartSplit);
				pStageData->SetSplitComplete (FALSE);
				pStageData->GetFrameScheduler ()->StartSplit ();

#ifndef USE_USB_SOF_INTR
				StartTransaction (pStageData);
#else
				m_pStageData[nChannel] = 0;
				FreeChannel (nChannel);

				QueueTransaction (pStageData);
#endif
			}
			else
			{
				if (pStageData->IsTimeout ())
				{
					DisableChannelInterrupt (nChannel);

					pURB->SetStatus (0);
					pURB->SetUSBError (USBErrorTimeout);

					delete pStageData;
					m_pStageData[nChannel] = 0;

					FreeChannel (nChannel);

					pURB->CallCompletionRoutine ();
				}
				else
				{
#ifdef USE_USB_SOF_INTR
					m_pStageData[nChannel] = 0;
					FreeChannel (nChannel);

					QueueDelayedTransaction (pStageData);
#else
					pStageData->SetState (StageStatePeriodicDelay);

					unsigned nInterval = pURB->GetEndpoint ()->GetInterval ();

					m_pTimer->StartKernelTimer (MSEC2HZ (nInterval),
								    TimerStub, pStageData, this);
#endif
				}
			}
			break;
		}

		DisableChannelInterrupt (nChannel);

		if (!pStageData->IsStatusStage ())
		{
			pURB->SetResultLen (pStageData->GetResultLen ());
		}
		pURB->SetStatus (1);

		delete pStageData;
		m_pStageData[nChannel] = 0;

		FreeChannel (nChannel);

		pURB->CallCompletionRoutine ();
		break;

	default:
		assert (0);
		break;
	}
}

#ifdef USE_USB_SOF_INTR
// xzl: fire every 1ms???
void CDWHCIDevice::SOFInterruptHandler (void)
{
	if (m_bShutdown)
	{
		return;
	}

	CTracer::Get()->Event(TRACE_SOFIRQ_START);

	CDWHCIRegister FrameNumber (DWHCI_HOST_FRM_NUM);
	u16 usFrameNumber = DWHCI_HOST_FRM_NUM_NUMBER (FrameNumber.Read ());

	// xzl: tries to issue prev enqueued tx for this frame?
	CDWHCITransferStageData *pStageData;
	while ((pStageData = m_TransactionQueue.Dequeue (usFrameNumber)) != 0)
	{
		// xzl: grab a channel and exec a tx
		unsigned nChannel = AllocateChannel ();
		if (nChannel >= m_nChannels)
		{
			CLogger::Get ()->Write (FromDWHCI, LogPanic, "Too many parallel USB transactions");
		}
		pStageData->SetChannelNumber (nChannel);

		assert (m_pStageData[nChannel] == 0);
		m_pStageData[nChannel] = pStageData;

		EnableChannelInterrupt (nChannel);

		StartTransaction (pStageData);
	}

	CTracer::Get()->Event(TRACE_SOFIRQ_END);
}

#endif
// xzl: dispatch irq to individual chans
void CDWHCIDevice::InterruptHandler (void)
{
#ifndef NDEBUG
	//debug_click ();
#endif

	CTracer::Get()->Event(TRACE_IRQ_START);

	PeripheralEntry ();

	CDWHCIRegister IntStatus (DWHCI_CORE_INT_STAT);
	IntStatus.Read ();

	if (IntStatus.Get () & DWHCI_CORE_INT_STAT_HC_INTR)
	{
		CDWHCIRegister AllChanInterrupt (DWHCI_HOST_ALLCHAN_INT);
		AllChanInterrupt.Read ();
		AllChanInterrupt.Write ();
		
		unsigned nChannelMask = 1;
		for (unsigned nChannel = 0; nChannel < m_nChannels; nChannel++)
		{
			if (AllChanInterrupt.Get () & nChannelMask)
			{
				CDWHCIRegister ChanInterruptMask (DWHCI_HOST_CHAN_INT_MASK(nChannel), 0);
				ChanInterruptMask.Write ();
				
				ChannelInterruptHandler (nChannel);
			}
			
			nChannelMask <<= 1;
		}
	}

#ifdef USE_USB_SOF_INTR
	if (IntStatus.Get () & DWHCI_CORE_INT_STAT_SOF_INTR)
	{
		SOFInterruptHandler ();
	}
#endif

	if (IsPlugAndPlay ())
	{
		if (IntStatus.Get () & DWHCI_CORE_INT_STAT_PORT_INTR)
		{
			CDWHCIRegister HostPort (DWHCI_HOST_PORT);
			HostPort.Read ();

			//CLogger::Get ()->Write (FromDWHCI, LogDebug,
			//			"Port interrupt (status 0x%08X)", HostPort.Get ());

			if (HostPort.Get () & DWHCI_HOST_PORT_CONNECT_CHANGED)
			{
				PortStatusChanged (&m_RootPort);
			}

			HostPort.And (~DWHCI_HOST_PORT_ENABLE);
			HostPort.Or (  DWHCI_HOST_PORT_CONNECT_CHANGED
				     | DWHCI_HOST_PORT_ENABLE_CHANGED
				     | DWHCI_HOST_PORT_OVERCURRENT_CHANGED);
			HostPort.Write ();
		}

		if (IntStatus.Get () & DWHCI_CORE_INT_MASK_DISCONNECT)
		{
			//CLogger::Get ()->Write (FromDWHCI, LogDebug, "Disconnect interrupt");

			PortStatusChanged (&m_RootPort);
		}
	}

	IntStatus.Write ();

	PeripheralExit ();

	CTracer::Get()->Event(TRACE_IRQ_END);
}

void CDWHCIDevice::InterruptStub (void *pParam)
{
	CDWHCIDevice *pThis = (CDWHCIDevice *) pParam;
	assert (pThis != 0);

	pThis->InterruptHandler ();
}

#ifndef USE_USB_SOF_INTR

void CDWHCIDevice::TimerHandler (CDWHCITransferStageData *pStageData)
{
	PeripheralEntry ();

	assert (pStageData != 0);

	if (!m_bRootPortEnabled)
	{
		unsigned nChannel = pStageData->GetChannelNumber ();
		assert (nChannel < m_nChannels);
		CUSBRequest *pURB = pStageData->GetURB ();
		assert (pURB != 0);

		DisableChannelInterrupt (nChannel);

		pURB->SetStatus (0);
		pURB->SetUSBError (USBErrorAborted);

		delete pStageData;
		m_pStageData[nChannel] = 0;

		FreeChannel (nChannel);

		PeripheralExit ();

		pURB->CallCompletionRoutine ();

		return;
	}

	assert (pStageData->GetState () == StageStatePeriodicDelay);

	if (pStageData->IsSplit ())
	{
		pStageData->SetState (StageStateStartSplit);
	
		pStageData->SetSplitComplete (FALSE);
		pStageData->GetFrameScheduler ()->StartSplit ();
	}
	else
	{
		pStageData->SetState (StageStateNoSplitTransfer);
	}

	StartTransaction (pStageData);

	PeripheralExit ();
}

void CDWHCIDevice::TimerStub (TKernelTimerHandle /* hTimer */, void *pParam, void *pContext)
{
	CDWHCIDevice *pThis = (CDWHCIDevice *) pContext;
	assert (pThis != 0);
	
	CDWHCITransferStageData *pStageData = (CDWHCITransferStageData *) pParam;
	assert (pParam != 0);
	
	pThis->TimerHandler (pStageData);
}

#endif

unsigned CDWHCIDevice::AllocateChannel (void)
{
	m_ChannelSpinLock.Acquire ();

	unsigned nChannelMask = 1;
	for (unsigned nChannel = 0; nChannel < m_nChannels; nChannel++)
	{
		if (!(m_nChannelAllocated & nChannelMask))
		{
			m_nChannelAllocated |= nChannelMask;

			m_ChannelSpinLock.Release ();
			
			return nChannel;
		}
		
		nChannelMask <<= 1;
	}
	
	m_ChannelSpinLock.Release ();
	
	return DWHCI_MAX_CHANNELS;
}

void CDWHCIDevice::FreeChannel (unsigned nChannel)
{
	assert (nChannel < m_nChannels);
	unsigned nChannelMask = 1 << nChannel; 
	
	m_ChannelSpinLock.Acquire ();
	
	assert (m_nChannelAllocated & nChannelMask);
	m_nChannelAllocated &= ~nChannelMask;
	
	m_ChannelSpinLock.Release ();
}

unsigned CDWHCIDevice::AllocateWaitBlock (void)
{
	m_WaitBlockSpinLock.Acquire ();

	unsigned nWaitBlockMask = 1;
	for (unsigned nWaitBlock = 0; nWaitBlock < DWHCI_WAIT_BLOCKS; nWaitBlock++)
	{
		if (!(m_nWaitBlockAllocated & nWaitBlockMask))
		{
			m_nWaitBlockAllocated |= nWaitBlockMask;

			m_WaitBlockSpinLock.Release ();
			
			return nWaitBlock;
		}
		
		nWaitBlockMask <<= 1;
	}
	
	m_WaitBlockSpinLock.Release ();
	
	return DWHCI_WAIT_BLOCKS;
}

void CDWHCIDevice::FreeWaitBlock (unsigned nWaitBlock)
{
	assert (nWaitBlock < DWHCI_WAIT_BLOCKS);
	unsigned nWaitBlockMask = 1 << nWaitBlock;
	
	m_WaitBlockSpinLock.Acquire ();
	
	assert (m_nWaitBlockAllocated & nWaitBlockMask);
	m_nWaitBlockAllocated &= ~nWaitBlockMask;
	
	m_WaitBlockSpinLock.Release ();
}

boolean CDWHCIDevice::WaitForBit (CDWHCIRegister *pRegister,
				  u32		  nMask,
				  boolean	  bWaitUntilSet,
				  unsigned	  nMsTimeout)
{
	assert (pRegister != 0);
	assert (nMask != 0);
	assert (nMsTimeout > 0);

	while ((pRegister->Read () & nMask) ? !bWaitUntilSet : bWaitUntilSet)
	{
		assert (m_pTimer != 0);
		m_pTimer->MsDelay (1);

		if (--nMsTimeout == 0)
		{
			//CLogger::Get ()->Write (FromDWHCI, LogWarning, "Timeout");
#ifndef NDEBUG
			//pRegister->Dump ();
#endif
			return FALSE;
		}
	}
	
	return TRUE;
}

#ifndef NDEBUG

void CDWHCIDevice::DumpRegister (const char *pName, u32 nAddress)
{
	CDWHCIRegister Register (nAddress);

	DataMemBarrier ();

	CLogger::Get ()->Write (FromDWHCI, LogDebug, "0x%08X %s", Register.Read (), pName);
}

void CDWHCIDevice::DumpStatus (unsigned nChannel)
{
	DumpRegister ("OTG_CTRL",                DWHCI_CORE_OTG_CTRL);
	DumpRegister ("AHB_CFG",                 DWHCI_CORE_AHB_CFG);
	DumpRegister ("USB_CFG",                 DWHCI_CORE_USB_CFG);
	DumpRegister ("RESET",                   DWHCI_CORE_RESET);
	DumpRegister ("INT_STAT",                DWHCI_CORE_INT_STAT);
	DumpRegister ("INT_MASK",                DWHCI_CORE_INT_MASK);
	DumpRegister ("RX_FIFO_SIZ",             DWHCI_CORE_RX_FIFO_SIZ);
	DumpRegister ("NPER_TX_FIFO_SIZ",        DWHCI_CORE_NPER_TX_FIFO_SIZ);
	DumpRegister ("NPER_TX_STAT",            DWHCI_CORE_NPER_TX_STAT);
	DumpRegister ("HOST_PER_TX_FIFO_SIZ",    DWHCI_CORE_HOST_PER_TX_FIFO_SIZ);

	DumpRegister ("HOST_CFG",                DWHCI_HOST_CFG);
	DumpRegister ("HOST_PER_TX_FIFO_STAT",   DWHCI_HOST_PER_TX_FIFO_STAT);
	DumpRegister ("HOST_ALLCHAN_INT",        DWHCI_HOST_ALLCHAN_INT);
	DumpRegister ("HOST_ALLCHAN_INT_MASK",   DWHCI_HOST_ALLCHAN_INT_MASK);
	DumpRegister ("HOST_PORT",               DWHCI_HOST_PORT);

	DumpRegister ("HOST_CHAN_CHARACTER(n)",  DWHCI_HOST_CHAN_CHARACTER (nChannel));
	DumpRegister ("HOST_CHAN_SPLIT_CTRL(n)", DWHCI_HOST_CHAN_SPLIT_CTRL (nChannel));
	DumpRegister ("HOST_CHAN_INT(n)",        DWHCI_HOST_CHAN_INT (nChannel));
	DumpRegister ("HOST_CHAN_INT_MASK(n)",   DWHCI_HOST_CHAN_INT_MASK (nChannel));
	DumpRegister ("HOST_CHAN_XFER_SIZ(n)",   DWHCI_HOST_CHAN_XFER_SIZ (nChannel));
	DumpRegister ("HOST_CHAN_DMA_ADDR(n)",   DWHCI_HOST_CHAN_DMA_ADDR (nChannel));
}

#endif
