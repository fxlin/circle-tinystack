//
// tracer.h
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
#ifndef _circle_tracer_h
#define _circle_tracer_h

#include <circle/types.h>
#include <circle/string.h> // xzl

struct TTraceEntry
{
	unsigned nClockTicks;
	unsigned nEventID;
#define TRACER_EVENT_STOP	0
	unsigned nParam[4];
};

class CTracer
{
public:
	CTracer (unsigned nDepth, boolean bStopIfFull);
	~CTracer (void);

	void Start (void);
	void Stop (void);

	// not reentrant, use spin lock if required
	void Event (unsigned nID, unsigned nParam1 = 0, unsigned nParam2 = 0, unsigned nParam3 = 0, unsigned nParam4 = 0);

	void Dump (void);
	unsigned Count(void); // xzl
	CString DumpString (unsigned); // xzl

	static CTracer *Get (void);

private:
	unsigned	 m_nDepth;		// size of ring buffer
	boolean		 m_bStopIfFull;
	boolean		 m_bActive;
	unsigned	 m_nStartTicks;
	TTraceEntry	*m_pEntry;		// array used as ring buffer
	unsigned	 m_nEntries;		// valid entries in ring buffer
	unsigned	 m_nCurrent;		// write index into ring buffer

	static CTracer *s_pThis;
};

// xzl
#define TRACE_REG_READ			1
#define TRACE_REG_WRITE			2
#define TRACE_IRQ_START			3
#define TRACE_IRQ_END				4
#define TRACE_SOFIRQ_START	5
#define TRACE_SOFIRQ_END		6
#define TRACE_WRITE_START		7
#define TRACE_WRITE_END			8
#define TRACE_READ_START		9
#define TRACE_READ_END			10


#endif
