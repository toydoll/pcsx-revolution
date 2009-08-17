/*  Pcsx - Pc Psx Emulator
 *  Copyright (C) 1999-2003  Pcsx Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef __R3000A_H__
#define __R3000A_H__

#include "PsxCommon.h"

typedef struct {
	int  (*Init)();
	void (*Reset)();
	void (*Execute)();		/* executes up to a break */
	void (*ExecuteBlock)(u32 addr);	/* executes up to a jump */
	void (*Clear)(u32 Addr, u32 Size);
	void (*Shutdown)();
} R3000Acpu;

R3000Acpu *psxCpu;
extern R3000Acpu psxInt;
#ifndef __GAMECUBE__
extern R3000Acpu psxIntDbg;
#endif
#if (defined(__x86_64__) || defined(__i386__) || defined(__sh__) || defined(__ppc__)) && !defined(NOPSXREC)
extern R3000Acpu psxRec;
#define PSXREC
#endif

enum PsxEventType
{
	PsxEvt_Counter0 = 0,

	PsxEvt_Counter1,
	PsxEvt_Counter2,
	PsxEvt_Counter3,
	PsxEvt_Counter4,

	PsxEvt_Exception,		// 5
	PsxEvt_SIO,				// 6
	PsxEvt_GPU,				// 7

	PsxEvt_Cdrom,			// 8
	PsxEvt_CdromRead,		// 9
	PsxEvt_SPU,				// 10

	PsxEvt_CountNonIdle,

	// Idle state, no events scheduled.  Placed at -1 since it has no actual
	// entry in the Event System's event schedule table.
	PsxEvt_Idle = PsxEvt_CountNonIdle,

	PsxEvt_CountAll		// total number of schedulable event types in the Psx
};

typedef union 
{
	struct 
	{
		u32 
			r0, 
			at, 
			v0, v1, 
			a0, a1, a2, a3,

			t0, t1, t2, t3, 
			t4, t5, t6, t7,

			s0, s1, s2, s3, 
			s4, s5, s6, s7,

			t8, t9, k0, k1, 
			gp, sp, s8, 

			ra, // Link register

			lo, hi;
	} n;
	u32 r[34]; /* Lo, Hi in r[33] and r[34] */
} psxGPRRegs;

typedef union
{
	struct
	{
		u32
			Index,     Random,    EntryLo0,  EntryLo1,
			Context,   PageMask,  Wired,     Reserved0,
			BadVAddr,  Count,     EntryHi,   Compare,
			Status,    Cause,     EPC,       PRid,
			Config,    LLAddr,    WatchLO,   WatchHI,
			XContext,  Reserved1, Reserved2, Reserved3,
			Reserved4, Reserved5, ECC,       CacheErr,
			TagLo,     TagHi,     ErrorEPC,  Reserved6;
	} n;
	u32 r[32];
} psxCP0Regs;

typedef struct {
	short x, y;
} SVector2D;

typedef struct {
	short z, pad;
} SVector2Dz;

typedef struct {
	short x, y, z, pad;
} SVector3D;

typedef struct {
	short x, y, z, pad;
} LVector3D;

typedef struct {
	unsigned char r, g, b, c;
} CBGR;

typedef struct {
	short m11, m12, m13, m21, m22, m23, m31, m32, m33, pad;
} SMatrix3D;

typedef union {
	struct {
		SVector3D     v0, v1, v2;
		CBGR          rgb;
		s32          otz;
		s32          ir0, ir1, ir2, ir3;
		SVector2D     sxy0, sxy1, sxy2, sxyp;
		SVector2Dz    sz0, sz1, sz2, sz3;
		CBGR          rgb0, rgb1, rgb2;
		s32          reserved;
		s32          mac0, mac1, mac2, mac3;
		u32 irgb, orgb;
		s32          lzcs, lzcr;
	} n;
	u32 r[32];
} psxCP2Data;

typedef union {
	struct {
		SMatrix3D rMatrix;
		s32      trX, trY, trZ;
		SMatrix3D lMatrix;
		s32      rbk, gbk, bbk;
		SMatrix3D cMatrix;
		s32      rfc, gfc, bfc;
		s32      ofx, ofy;
		s32      h;
		s32      dqa, dqb;
		s32      zsf3, zsf4;
		s32      flag;
	} n;
	u32 r[32];
} psxCP2Ctrl;

typedef struct {
	psxGPRRegs GPR;		/* General Purpose Registers */
	psxCP0Regs CP0;		/* Coprocessor0 Registers */
	psxCP2Data CP2D; 	/* Cop2 data registers */
	psxCP2Ctrl CP2C; 	/* Cop2 control registers */
	u32 pc;				/* Program counter */
	u32 code;			/* The instruction */
	u32 cycle;
	u32 IsDelaySlot:1;

	// marks the original duration of time for the current pending event.  This is
	// typically used to determine the amount of time passed since the last update
	// to psxRegs.cycle:
	//  currentcycle = cycle + ( evtCycleDuration - evtCycleCountdown );
	s32 evtCycleDuration;

	// marks the *current* duration of time until the current pending event. In
	// other words: counts down from evtCycleDuration to 0; event is raised when 0
	// is reached.
	s32 evtCycleCountdown;

	// number of cycles pending on the current div unit instruction (mult and div both run in the same
	// unit).  Any zero-or-negative values mean the unit is free and no stalls incurred for executing
	// a new instruction on the pipeline.  Negative values are flushed to 0 during PendingEvent executions.
	// (faster than flushing to zero on every cycle update).
	s32 DivUnitCycles;
} psxRegisters;

extern psxRegisters psxRegs;

#if defined(__BIGENDIAN__) || defined(__GAMECUBE__)

#define _i32(x) *(s32 *)&x
#define _u32(x) x

#define _i16(x) (((short *)&x)[1])
#define _u16(x) (((unsigned short *)&x)[1])

#define _i8(x) (((char *)&x)[3])
#define _u8(x) (((unsigned char *)&x)[3])

#else

#define _i32(x) *(s32 *)&x
#define _u32(x) x

#define _i16(x) *(short *)&x
#define _u16(x) *(unsigned short *)&x

#define _i8(x) *(char *)&x
#define _u8(x) *(unsigned char *)&x

#endif

/**** R3000A Instruction Macros ****/
#define _PC_       psxRegs.pc       // The next PC to be executed

#define _fOp_(code)		((code >> 26)       )  // The opcode part of the instruction register 
#define _fFunct_(code)	((code      ) & 0x3F)  // The funct part of the instruction register 
#define _fSa_(code)		((code >>  6) & 0x1F)  // The sa part of the instruction register
#define _fRd_(code)		((code >> 11) & 0x1F)  // The rd part of the instruction register 
#define _fRt_(code)		((code >> 16) & 0x1F)  // The rt part of the instruction register 
#define _fRs_(code)		((code >> 21) & 0x1F)  // The rs part of the instruction register 
#define _fIm_(code)		((u16)code)            // The immediate part of the instruction register
#define _fTarget_(code)	(code & 0x03ffffff)    // The target part of the instruction register

#define _fImm_(code)	((s16)code)            // sign-extended immediate
#define _fImmU_(code)	((u16)code)          // zero-extended immediate

#define _Op_     _fOp_(psxRegs.code)
#define _Funct_  _fFunct_(psxRegs.code)
#define _Rd_     _fRd_(psxRegs.code)
#define _Rt_     _fRt_(psxRegs.code)
#define _Rs_     _fRs_(psxRegs.code)
#define _Sa_     _fSa_(psxRegs.code)
#define _Im_     _fIm_(psxRegs.code)
#define _Target_ _fTarget_(psxRegs.code)

#define _Imm_	 _fImm_(psxRegs.code)
#define _ImmU_	 _fImmU_(psxRegs.code)

#define _rRs_   psxRegs.GPR.r[_Rs_]   // Rs register
#define _rRt_   psxRegs.GPR.r[_Rt_]   // Rt register
#define _rRd_   psxRegs.GPR.r[_Rd_]   // Rd register
#define _rSa_   psxRegs.GPR.r[_Sa_]   // Sa register
#define _rFs_   psxRegs.CP0.r[_Rd_]   // Fs register

#define _c2dRs_ psxRegs.CP2D.r[_Rs_]  // Rs cop2 data register
#define _c2dRt_ psxRegs.CP2D.r[_Rt_]  // Rt cop2 data register
#define _c2dRd_ psxRegs.CP2D.r[_Rd_]  // Rd cop2 data register
#define _c2dSa_ psxRegs.CP2D.r[_Sa_]  // Sa cop2 data register

#define _rHi_   psxRegs.GPR.n.hi   // The HI register
#define _rLo_   psxRegs.GPR.n.lo   // The LO register

#define _JumpTarget_    ((_Target_ << 2) + ((_PC_+4) & 0xf0000000))   // Calculates the target during a jump instruction
#define _BranchTarget_  (_Imm_ * 4 + _PC_)                 // Calculates the target during a branch instruction

#define _SetLink(x)     psxRegs.GPR.r[x] = _PC_ + 4;       // Sets the return address in the link register

int  psxInit();
void psxReset();
void psxShutdown();
void psxException(u32 code, u32 bd);
void psxBranchTest();
void psxExecuteBios();
int  psxTestLoadDelay(int reg, u32 tmp);
void psxDelayTest(int reg, u32 bpc);
void psxTestSWInts();
void psxJumpTest();

u32 psxGetCycle();
void AddCycles( int amount );

void psx_int_add(int n, s32 ecycle);
void psx_int_remove(int n);
void psxRaiseExtInt( uint irq );

#endif /* __R3000A_H__ */