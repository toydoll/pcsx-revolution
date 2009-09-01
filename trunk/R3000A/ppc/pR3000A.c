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

#include <gccore.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include "../PsxCommon.h"
#include "../PsxMem.h"
#include "../PsxCounters.h"
#include "ppc.h"
#include "reguse.h"
#include "../R3000A.h"
#include "../R3000AOpcodeTable.h"
#include "../PsxHLE.h"

//#define NO_CONSTANT

u32 *psxRecLUT;

#undef PC_REC
#undef PC_REC8
#undef PC_REC16
#undef PC_REC32
#define PC_REC(x)	(psxRecLUT[x >> 16] + (x & 0xffff))
#define PC_REC8(x)	(*(u8 *)PC_REC(x))
#define PC_REC16(x) (*(u16*)PC_REC(x))
#define PC_REC32(x) (*(u32*)PC_REC(x))

#define OFFSET(X,Y) ((u32)(Y)-(u32)(X))

#if defined(HW_DOL)
#define RECMEM_SIZE		(7*1024*1024)
#elif defined(HW_RVL)
#define RECMEM_SIZE		(8*1024*1024)
#endif

static char *recMem;	/* the recompiled blocks will be here */
static char *recRAM;	/* and the ptr to the blocks here */
static char *recROM;	/* and here */

static u32 pc;			/* recompiler pc */
static u32 pcold;		/* recompiler oldpc */
static int count;		/* recompiler intruction count */
static int branch;		/* set for branch */
static u32 target;		/* branch target */
static u32 resp;

u32 cop2readypc = 0;
u32 idlecyclecount = 0;

#define NUM_REGISTERS	34
typedef struct {
	int state;
	u32 k;
	int reg;
} iRegisters;

static iRegisters iRegs[34];

#define ST_UNK      0x00
#define ST_CONST    0x01
#define ST_MAPPED   0x02

#ifdef NO_CONSTANT
#define IsConst(reg) 0
#else
#define IsConst(reg)  (iRegs[reg].state & ST_CONST)
#endif
#define IsMapped(reg) (iRegs[reg].state & ST_MAPPED)

static void (*recBSC[64])();
static void (*recSPC[64])();
static void (*recREG[32])();
static void (*recCP0[32])();
static void (*recCP2[64])();
static void (*recCP2BSC[32])();

#define REG_LO			32
#define REG_HI			33

// Hardware register usage
#define HWUSAGE_NONE     0x00

#define HWUSAGE_READ     0x01
#define HWUSAGE_WRITE    0x02
#define HWUSAGE_CONST    0x04
#define HWUSAGE_ARG      0x08	/* used as an argument for a function call */

#define HWUSAGE_RESERVED 0x10	/* won't get flushed when flushing all regs */
#define HWUSAGE_SPECIAL  0x20	/* special purpose register */
#define HWUSAGE_HARDWIRED 0x40	/* specific hardware register mapping that is never disposed */
#define HWUSAGE_INITED    0x80
#define HWUSAGE_PSXREG    0x100

// Remember to invalidate the special registers if they are modified by compiler
enum {
    ARG1 = 3,
    ARG2 = 4,
    ARG3 = 5,
    PSXREGS,	// ptr
	 PSXMEM,		// ptr
    CYCLECOUNT,	// ptr
    PSXPC,	// ptr
    TARGETPTR,	// ptr
    TARGET,	// ptr
    RETVAL,
    REG_RZERO,
    REG_WZERO
};

typedef struct {
    int code;
    u32 k;
    int usage;
    int lastUsed;
    
    void (*flush)(int hwreg);
    int private;
} HWRegister;
static HWRegister HWRegisters[NUM_HW_REGISTERS];
static int HWRegUseCount;
static int DstCPUReg;
static int UniqueRegAlloc;

static int GetFreeHWReg();
static void InvalidateCPURegs();
static void DisposeHWReg(int index);
static void FlushHWReg(int index);
static void FlushAllHWReg();
static void MapPsxReg32(int reg);
static void FlushPsxReg32(int hwreg);
static int UpdateHWRegUsage(int hwreg, int usage);
static int GetHWReg32(int reg);
static int PutHWReg32(int reg);
static int GetSpecialIndexFromHWRegs(int which);
static int GetHWRegFromCPUReg(int cpureg);
static int MapRegSpecial(int which);
static void FlushRegSpecial(int hwreg);
static int GetHWRegSpecial(int which);
static int PutHWRegSpecial(int which);
static void recRecompile();
static void recError();

/* --- Generic register mapping --- */

static int GetFreeHWReg()
{
	int i, least, index;
	
	if (DstCPUReg != -1) {
		index = GetHWRegFromCPUReg(DstCPUReg);
		DstCPUReg = -1;
	} else {
	    // LRU algorith with a twist ;)
	    for (i=0; i<NUM_HW_REGISTERS; i++) {
		    if (!(HWRegisters[i].usage & HWUSAGE_RESERVED)) {
			    break;
		    }
	    }
    
	    least = HWRegisters[i].lastUsed; index = i;
	    for (; i<NUM_HW_REGISTERS; i++) {
		    if (!(HWRegisters[i].usage & HWUSAGE_RESERVED)) {
			    if (HWRegisters[i].usage == HWUSAGE_NONE && HWRegisters[i].code >= 13) {
				    index = i;
				    break;
			    }
			    else if (HWRegisters[i].lastUsed < least) {
				    least = HWRegisters[i].lastUsed;
				    index = i;
			    }
		    }
	    }
		 
		 // Cycle the registers
		 if (HWRegisters[index].usage == HWUSAGE_NONE) {
			for (; i<NUM_HW_REGISTERS; i++) {
				if (!(HWRegisters[i].usage & HWUSAGE_RESERVED)) {
					if (HWRegisters[i].usage == HWUSAGE_NONE && 
						 HWRegisters[i].code >= 13 && 
						 HWRegisters[i].lastUsed < least) {
						least = HWRegisters[i].lastUsed;
						index = i;
						break;
					}
				}
			}
		 }
	}
	
/*	if (HWRegisters[index].code < 13 && HWRegisters[index].code > 3) {
		SysPrintf("Allocating volatile register %i\n", HWRegisters[index].code);
	}
	if (HWRegisters[index].usage != HWUSAGE_NONE) {
		SysPrintf("RegUse too big. Flushing %i\n", HWRegisters[index].code);
	}*/
	if (HWRegisters[index].usage & (HWUSAGE_RESERVED | HWUSAGE_HARDWIRED)) {
		if (HWRegisters[index].usage & HWUSAGE_RESERVED) {
			SysPrintf("Error! Trying to map a new register to a reserved register (r%i)", 
						HWRegisters[index].code);
		}
		if (HWRegisters[index].usage & HWUSAGE_HARDWIRED) {
			SysPrintf("Error! Trying to map a new register to a hardwired register (r%i)", 
						HWRegisters[index].code);
		}
	}
	
	if (HWRegisters[index].lastUsed != 0) {
		UniqueRegAlloc = 0;
	}
	
	// Make sure the register is really flushed!
	FlushHWReg(index);
	HWRegisters[index].usage = HWUSAGE_NONE;
	HWRegisters[index].flush = NULL;
	
	return index;
}

static void FlushHWReg(int index)
{
	if (index < 0) return;
	if (HWRegisters[index].usage == HWUSAGE_NONE) return;
	
	if (HWRegisters[index].flush) {
		HWRegisters[index].usage |= HWUSAGE_RESERVED;
		HWRegisters[index].flush(index);
		HWRegisters[index].flush = NULL;
	}
	
	if (HWRegisters[index].usage & HWUSAGE_HARDWIRED) {
		HWRegisters[index].usage &= ~(HWUSAGE_READ | HWUSAGE_WRITE);
	} else {
		HWRegisters[index].usage = HWUSAGE_NONE;
	}
}

// get rid of a mapped register without flushing the contents to the memory
static void DisposeHWReg(int index)
{
	if (index < 0) return;
	if (HWRegisters[index].usage == HWUSAGE_NONE) return;
	
	HWRegisters[index].usage &= ~(HWUSAGE_READ | HWUSAGE_WRITE);
	if (HWRegisters[index].usage == HWUSAGE_NONE) {
		SysPrintf("Error! not correctly disposing register (r%i)", HWRegisters[index].code);
	}
	
	FlushHWReg(index);
}

// operated on cpu registers
__inline static void FlushCPURegRange(int start, int end)
{
	int i;
	
	if (end <= 0) end = 31;
	if (start <= 0) start = 0;
	
	for (i=0; i<NUM_HW_REGISTERS; i++) {
		if (HWRegisters[i].code >= start && HWRegisters[i].code <= end)
			if (HWRegisters[i].flush)
				FlushHWReg(i);
	}

	for (i=0; i<NUM_HW_REGISTERS; i++) {
		if (HWRegisters[i].code >= start && HWRegisters[i].code <= end)
			FlushHWReg(i);
	}
}

static void FlushAllHWReg()
{
	FlushCPURegRange(0,31);
}

static void InvalidateCPURegs()
{
	FlushCPURegRange(0,12);
}

/* --- Mapping utility functions --- */

static void MoveHWRegToCPUReg(int cpureg, int hwreg)
{
	int dstreg;
	
	if (HWRegisters[hwreg].code == cpureg)
		return;
	
	dstreg = GetHWRegFromCPUReg(cpureg);
	
	HWRegisters[dstreg].usage &= ~(HWUSAGE_HARDWIRED | HWUSAGE_ARG);
	if (HWRegisters[hwreg].usage & (HWUSAGE_READ | HWUSAGE_WRITE)) {
		FlushHWReg(dstreg);
		MR(HWRegisters[dstreg].code, HWRegisters[hwreg].code);
	} else {
		if (HWRegisters[dstreg].usage & (HWUSAGE_READ | HWUSAGE_WRITE)) {
			MR(HWRegisters[hwreg].code, HWRegisters[dstreg].code);
		}
		else if (HWRegisters[dstreg].usage != HWUSAGE_NONE) {
			FlushHWReg(dstreg);
		}
	}
	
	HWRegisters[dstreg].code = HWRegisters[hwreg].code;
	HWRegisters[hwreg].code = cpureg;
}

static int UpdateHWRegUsage(int hwreg, int usage)
{
	HWRegisters[hwreg].lastUsed = ++HWRegUseCount;    
	if (usage & HWUSAGE_WRITE) {
		HWRegisters[hwreg].usage &= ~HWUSAGE_CONST;
	}
	if (!(usage & HWUSAGE_INITED)) {
		HWRegisters[hwreg].usage &= ~HWUSAGE_INITED;
	}
	HWRegisters[hwreg].usage |= usage;
	
	return HWRegisters[hwreg].code;
}

static int GetHWRegFromCPUReg(int cpureg)
{
	int i;
	for (i=0; i<NUM_HW_REGISTERS; i++) {
		if (HWRegisters[i].code == cpureg) {
			return i;
		}
	}
	
	SysPrintf("Error! Register location failure (r%i)", cpureg);
	return 0;
}

// this function operates on cpu registers
void SetDstCPUReg(int cpureg)
{
	DstCPUReg = cpureg;
}

static void ReserveArgs(int args)
{
	int index, i;
	
	for (i=0; i<args; i++) {
		index = GetHWRegFromCPUReg(3+i);
		HWRegisters[index].usage |= HWUSAGE_RESERVED | HWUSAGE_HARDWIRED | HWUSAGE_ARG;
	}
}

static void ReleaseArgs()
{
	int i;
	
	for (i=0; i<NUM_HW_REGISTERS; i++) {
		if (HWRegisters[i].usage & HWUSAGE_ARG) {
			//HWRegisters[i].usage = HWUSAGE_NONE;
			//HWRegisters[i].flush = NULL;
			HWRegisters[i].usage &= ~(HWUSAGE_RESERVED | HWUSAGE_HARDWIRED | HWUSAGE_ARG);
			FlushHWReg(i);
		}
	}
}

/* --- Psx register mapping --- */

static void MapPsxReg32(int reg)
{
    int hwreg = GetFreeHWReg();
    HWRegisters[hwreg].flush = FlushPsxReg32;
    HWRegisters[hwreg].private = reg;
    
    if (iRegs[reg].reg != -1) {
        SysPrintf("error: double mapped psx register");
    }
    
    iRegs[reg].reg = hwreg;
    iRegs[reg].state |= ST_MAPPED;
}

static void FlushPsxReg32(int hwreg)
{
	int reg = HWRegisters[hwreg].private;
	
	if (iRegs[reg].reg == -1) {
		SysPrintf("error: flushing unmapped psx register");
	}
	
	if (HWRegisters[hwreg].usage & HWUSAGE_WRITE) {
		if (branch) {
			/*int reguse = nextPsxRegUse(pc-8, reg);
			if (reguse == REGUSE_NONE || (reguse & REGUSE_READ))*/ {
				STW(HWRegisters[hwreg].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.GPR.r[reg]));
			}
		} else {
			int reguse = nextPsxRegUse(pc-4, reg);
			if (reguse == REGUSE_NONE || (reguse & REGUSE_READ)) {
				STW(HWRegisters[hwreg].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.GPR.r[reg]));
			}
		}
	}
	
	iRegs[reg].reg = -1;
	iRegs[reg].state = ST_UNK;
}

static int GetHWReg32(int reg)
{
	int usage = HWUSAGE_PSXREG | HWUSAGE_READ;
	
	if (reg == 0) {
		return GetHWRegSpecial(REG_RZERO);
	}
	if (!IsMapped(reg)) {
		usage |= HWUSAGE_INITED;
		MapPsxReg32(reg);
		
		HWRegisters[iRegs[reg].reg].usage |= HWUSAGE_RESERVED;
		if (IsConst(reg)) {
			LIW(HWRegisters[iRegs[reg].reg].code, iRegs[reg].k);
			usage |= HWUSAGE_WRITE | HWUSAGE_CONST;
			//iRegs[reg].state &= ~ST_CONST;
		}
		else {
			LWZ(HWRegisters[iRegs[reg].reg].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.GPR.r[reg]));
		}
		HWRegisters[iRegs[reg].reg].usage &= ~HWUSAGE_RESERVED;
	}
	else if (DstCPUReg != -1) {
		int dst = DstCPUReg;
		DstCPUReg = -1;
		
		if (HWRegisters[iRegs[reg].reg].code < 13) {
			MoveHWRegToCPUReg(dst, iRegs[reg].reg);
		} else {
			MR(DstCPUReg, HWRegisters[iRegs[reg].reg].code);
		}
	}
	
	DstCPUReg = -1;
	
	return UpdateHWRegUsage(iRegs[reg].reg, usage);
}

static int PutHWReg32(int reg)
{
	int usage = HWUSAGE_PSXREG | HWUSAGE_WRITE;
	if (reg == 0) {
		return PutHWRegSpecial(REG_WZERO);
	}
	
	if (DstCPUReg != -1 && IsMapped(reg)) {
		if (HWRegisters[iRegs[reg].reg].code != DstCPUReg) {
			int tmp = DstCPUReg;
			DstCPUReg = -1;
			DisposeHWReg(iRegs[reg].reg);
			DstCPUReg = tmp;
		}
	}
	if (!IsMapped(reg)) {
		usage |= HWUSAGE_INITED;
		MapPsxReg32(reg);
	}
	
	DstCPUReg = -1;
	iRegs[reg].state &= ~ST_CONST;

	return UpdateHWRegUsage(iRegs[reg].reg, usage);
}

/* --- Special register mapping --- */

static int GetSpecialIndexFromHWRegs(int which)
{
	int i;
	for (i=0; i<NUM_HW_REGISTERS; i++) {
		if (HWRegisters[i].usage & HWUSAGE_SPECIAL) {
			if (HWRegisters[i].private == which) {
				return i;
			}
		}
	}
	return -1;
}

static int MapRegSpecial(int which)
{
	int hwreg = GetFreeHWReg();
	HWRegisters[hwreg].flush = FlushRegSpecial;
	HWRegisters[hwreg].private = which;
	
	return hwreg;
}

static void FlushRegSpecial(int hwreg)
{
	int which = HWRegisters[hwreg].private;
	
	if (!(HWRegisters[hwreg].usage & HWUSAGE_WRITE))
		return;
	
	switch (which) {
		case CYCLECOUNT:
			STW(HWRegisters[hwreg].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.evtCycleCountdown));
			break;
		case PSXPC:
			STW(HWRegisters[hwreg].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.pc));
			break;
		case TARGET:
			STW(HWRegisters[hwreg].code, GetHWRegSpecial(TARGETPTR), 0);
			break;
	}
}

static int GetHWRegSpecial(int which)
{
	int index = GetSpecialIndexFromHWRegs(which);
	int usage = HWUSAGE_READ | HWUSAGE_SPECIAL;
	
	if (index == -1) {
		usage |= HWUSAGE_INITED;
		index = MapRegSpecial(which);
		
		HWRegisters[index].usage |= HWUSAGE_RESERVED;
		switch (which) {
			case PSXREGS:
			case PSXMEM:
				SysPrintf("error! shouldn't be here!\n");
				//HWRegisters[index].flush = NULL;
				//LIW(HWRegisters[index].code, (u32)&psxRegs);
				break;
			case TARGETPTR:
				HWRegisters[index].flush = NULL;
				LIW(HWRegisters[index].code, (u32)&target);
				break;
			case REG_RZERO:
				HWRegisters[index].flush = NULL;
				LIW(HWRegisters[index].code, 0);
				break;
			case RETVAL:
				MoveHWRegToCPUReg(3, index);
				/*reg = GetHWRegFromCPUReg(3);
				HWRegisters[reg].code = HWRegisters[index].code;
				HWRegisters[index].code = 3;*/
				HWRegisters[index].flush = NULL;
				
				usage |= HWUSAGE_RESERVED;
				break;

			case CYCLECOUNT:
				LWZ(HWRegisters[index].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.evtCycleCountdown));
				break;
			case PSXPC:
				LWZ(HWRegisters[index].code, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.pc));
				break;
			case TARGET:
				LWZ(HWRegisters[index].code, GetHWRegSpecial(TARGETPTR), 0);
				break;
			default:
				SysPrintf("Error: Unknown special register in GetHWRegSpecial()\n");
				break;
		}
		HWRegisters[index].usage &= ~HWUSAGE_RESERVED;
	}
	else if (DstCPUReg != -1) {
		int dst = DstCPUReg;
		DstCPUReg = -1;
		
		MoveHWRegToCPUReg(dst, index);
	}

	return UpdateHWRegUsage(index, usage);
}

static int PutHWRegSpecial(int which)
{
	int index = GetSpecialIndexFromHWRegs(which);
	int usage = HWUSAGE_WRITE | HWUSAGE_SPECIAL;
	
	if (DstCPUReg != -1 && index != -1) {
		if (HWRegisters[index].code != DstCPUReg) {
			int tmp = DstCPUReg;
			DstCPUReg = -1;
			DisposeHWReg(index);
			DstCPUReg = tmp;
		}
	}
	switch (which) {
		case PSXREGS:
		case TARGETPTR:
			SysPrintf("Error: Read-only special register in PutHWRegSpecial()\n");
		case REG_WZERO:
			if (index >= 0) {
					if (HWRegisters[index].usage & HWUSAGE_WRITE)
						break;
			}
			index = MapRegSpecial(which);
			HWRegisters[index].flush = NULL;
			break;
		default:
			if (index == -1) {
				usage |= HWUSAGE_INITED;
				index = MapRegSpecial(which);
				
				HWRegisters[index].usage |= HWUSAGE_RESERVED;
				switch (which) {
					case ARG1:
					case ARG2:
					case ARG3:
						MoveHWRegToCPUReg(3+(which-ARG1), index);
						/*reg = GetHWRegFromCPUReg(3+(which-ARG1));
						
						if (HWRegisters[reg].usage != HWUSAGE_NONE) {
							HWRegisters[reg].usage &= ~(HWUSAGE_HARDWIRED | HWUSAGE_ARG);
							if (HWRegisters[reg].flush != NULL && HWRegisters[reg].usage & (HWUSAGE_WRITE | HWUSAGE_READ)) {
								MR(HWRegisters[index].code, HWRegisters[reg].code);
							} else {
								FlushHWReg(reg);
							}
						}
						HWRegisters[reg].code = HWRegisters[index].code;
						if (!(HWRegisters[index].code >= 3 && HWRegisters[index].code <=31))
							SysPrintf("Error! Register allocation");
						HWRegisters[index].code = 3+(which-ARG1);*/
						HWRegisters[index].flush = NULL;
						
						usage |= HWUSAGE_RESERVED | HWUSAGE_HARDWIRED | HWUSAGE_ARG;
						break;
				}
			}
			HWRegisters[index].usage &= ~HWUSAGE_RESERVED;
			break;
	}
	
	DstCPUReg = -1;

	return UpdateHWRegUsage(index, usage);
}

static void MapConst(int reg, u32 _const) {
	if (reg == 0)
		return;
	if (IsConst(reg) && iRegs[reg].k == _const)
		return;
	
	DisposeHWReg(iRegs[reg].reg);
	iRegs[reg].k = _const;
	iRegs[reg].state = ST_CONST;
}

static void MapCopy(int dst, int src)
{
    // do it the lazy way for now
    MR(PutHWReg32(dst), GetHWReg32(src));
}

static void iFlushReg(u32 nextpc, int reg) {
	if (!IsMapped(reg) && IsConst(reg)) {
		GetHWReg32(reg);
	}
	if (IsMapped(reg)) {
		if (nextpc) {
			int use = nextPsxRegUse(nextpc, reg);
			if ((use & REGUSE_RW) == REGUSE_WRITE) {
				DisposeHWReg(iRegs[reg].reg);
			} else {
				FlushHWReg(iRegs[reg].reg);
			}
		} else {
			FlushHWReg(iRegs[reg].reg);
		}
	}
}

static void iFlushRegs(u32 nextpc) {
	int i;

	for (i=1; i<NUM_REGISTERS; i++) {
		iFlushReg(nextpc, i);
	}
}

static void Return()
{
	iFlushRegs(0);
	FlushAllHWReg();
	if (((u32)returnPC & 0x1fffffc) == (u32)returnPC) {
		BA((u32)returnPC);
	}
	else {
		LIW(0, (u32)returnPC);
		MTLR(0);
		BLR();
	}
}

static void iRet() {
    /* store cycle */
	count = idlecyclecount + (pc - pcold) / 4;
	ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -count);
	ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -resp);
	Return();
}

static int iLoadTest() {
	u32 tmp;

	// check for load delay
	tmp = psxRegs.code >> 26;
	switch (tmp) {
		case 0x10: // COP0
			switch (_Rs_) {
				case 0x00: // MFC0
				case 0x02: // CFC0
					return 1;
			}
			break;
		case 0x12: // COP2
			switch (_Funct_) {
				case 0x00:
					switch (_Rs_) {
						case 0x00: // MFC2
						case 0x02: // CFC2
							return 1;
					}
					break;
			}
			break;
		case 0x32: // LWC2
			return 1;
		default:
			if (tmp >= 0x20 && tmp <= 0x26) { // LB/LH/LWL/LW/LBU/LHU/LWR
				return 1;
			}
			break;
	}
	return 0;
}

#define REC_TEST_BRANCH() \
	CALLFunc((u32)psxBranchTest);

/* set a pending branch */
static void SetBranch() {
	int treg;
	branch = 1;
	psxRegs.code = PSXMu32(pc);
	pc+=4;

	if (iLoadTest() == 1) {
		iFlushRegs(0);
		LIW(0, psxRegs.code);
		STW(0, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.code));
		/* store cycle */
		count = idlecyclecount + (pc - pcold) / 4;
		ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -count);

		treg = GetHWRegSpecial(TARGET);
		MR(PutHWRegSpecial(ARG2), treg);
		DisposeHWReg(GetHWRegFromCPUReg(treg));
		LIW(PutHWRegSpecial(ARG1), _Rt_);
		//LIW(GetHWRegSpecial(PSXPC), pc);
		FlushAllHWReg();
		CALLFunc((u32)psxDelayTest);
		ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -resp);
		FlushAllHWReg();
		Return();
		return;
	}

	recBSC[psxRegs.code>>26]();

	iFlushRegs(0);
	treg = GetHWRegSpecial(TARGET);
	MR(PutHWRegSpecial(PSXPC), treg); // FIXME: this line should not be needed
	DisposeHWReg(GetHWRegFromCPUReg(treg));
	FlushAllHWReg();

	REC_TEST_BRANCH();
	
	// TODO: don't return if target is compiled
	//Return();
	iRet();
}

static void iJump(u32 branchPC) {
	//u32 *b1, *b2;
	branch = 1;
	psxRegs.code = PSXMu32(pc);
	pc+=4;

	if (iLoadTest() == 1) {
		iFlushRegs(0);
		LIW(0, psxRegs.code);
		STW(0, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.code));
		/* store cycle */
		count = idlecyclecount + (pc - pcold) / 4;
		ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -count);

		LIW(PutHWRegSpecial(ARG2), branchPC);
		LIW(PutHWRegSpecial(ARG1), _Rt_);
		//LIW(GetHWRegSpecial(PSXPC), pc);
		FlushAllHWReg();
		CALLFunc((u32)psxDelayTest);
		ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -resp);
		Return();
		return;
	}

	recBSC[psxRegs.code>>26]();

	iFlushRegs(branchPC);
	LIW(PutHWRegSpecial(PSXPC), branchPC);

	FlushAllHWReg();
	REC_TEST_BRANCH();
 
	count = idlecyclecount + (pc - pcold) / 4;
	ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -count);
	ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -resp);
	FlushAllHWReg();

	// always return for now...
	Return();
		
	// maybe just happened an interruption, check so
	LIW(0, branchPC);
	CMPLW(GetHWRegSpecial(PSXPC), 0);
	BEQ_L(b32Ptr[0]);
	Return();
	
	B_DST(b32Ptr[0]);
	LIW(3, PC_REC(branchPC));
	LWZ(3, 3, 0);
	CMPLWI(3, 0);
	BNE_L(b32Ptr[1]);
	Return();

	// next bit is already compiled - jump right to it
	B_DST(b32Ptr[1]);
	MTCTR(3);
	BCTR();
}

static void iBranch(u32 branchPC, int savectx) {
	HWRegister HWRegistersS[NUM_HW_REGISTERS];
	iRegisters iRegsS[NUM_REGISTERS];
	int HWRegUseCountS = 0;
	u32 respold=0;
	u32 *b1, *b2;

	if (savectx) {
		respold = resp;
		memcpy(iRegsS, iRegs, sizeof(iRegs));
		memcpy(HWRegistersS, HWRegisters, sizeof(HWRegisters));
		HWRegUseCountS = HWRegUseCount;
	}
	
	branch = 1;
	psxRegs.code = PSXMu32(pc);

	// the delay test is only made when the branch is taken
	// savectx == 0 will mean that :)
	if (savectx == 0 && iLoadTest() == 1) {
		iFlushRegs(0);
		LIW(0, psxRegs.code);
		STW(0, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.code));
		/* store cycle */
		count = idlecyclecount + (pc + 4 - pcold) / 4;
		ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -count);

		LIW(PutHWRegSpecial(ARG2), branchPC);
		LIW(PutHWRegSpecial(ARG1), _Rt_);
        //LIW(GetHWRegSpecial(PSXPC), pc);
		FlushAllHWReg();
		CALLFunc((u32)psxDelayTest);
		ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -resp);
		Return();
		return;
	}

	pc += 4;
	recBSC[psxRegs.code>>26]();
	
	iFlushRegs(branchPC);
	LIW(PutHWRegSpecial(PSXPC), branchPC);

	FlushAllHWReg();
	REC_TEST_BRANCH();

	/* store cycle */
	count = idlecyclecount + (pc - pcold) / 4;
	ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -count);
	ADDI(PutHWRegSpecial(CYCLECOUNT), GetHWRegSpecial(CYCLECOUNT), -resp);
	FlushAllHWReg();
	LIW(0, branchPC);
	CMPLW(GetHWRegSpecial(PSXPC), 0);
	BEQ_L(b32Ptr[1]);
	Return();
	
	B_DST(b32Ptr[1]);
	LIW(3, PC_REC(branchPC));
	LWZ(3, 3, 0);
	CMPLWI(3, 0);
	BNE_L(b32Ptr[2]);
	Return();

	B_DST(b32Ptr[2]);
	MTCTR(3);
	BCTR();

	pc -= 4;
	if (savectx) {
		resp = respold;
		memcpy(iRegs, iRegsS, sizeof(iRegs));
		memcpy(HWRegisters, HWRegistersS, sizeof(HWRegisters));
		HWRegUseCount = HWRegUseCountS;
	}
}


void iDumpRegs() {
	int i, j;

	printf("%08x %08x\n", psxRegs.pc, psxRegs.evtCycleCountdown);
	for (i=0; i<4; i++) {
		for (j=0; j<8; j++)
			printf("%08x ", psxRegs.GPR.r[j*i]);
		printf("\n");
	}
}

void iDumpBlock(char *ptr) {
/*	FILE *f;
	u32 i;

	SysPrintf("dump1 %x:%x, %x\n", psxRegs.pc, pc, psxCurrentCycle);

	for (i = psxRegs.pc; i < pc; i+=4)
		SysPrintf("%s\n", disR3000AF(PSXMu32(i), i));

	fflush(stdout);
	f = fopen("dump1", "w");
	fwrite(ptr, 1, (u32)x86Ptr - (u32)ptr, f);
	fclose(f);
	system("ndisasmw -u dump1");
	fflush(stdout);*/
}

#define REC_FUNC(f) \
static void rec##f() { \
	iFlushRegs(0); \
	LIW(PutHWRegSpecial(ARG1), (u32)psxRegs.code); \
	STW(GetHWRegSpecial(ARG1), GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.code)); \
	LIW(PutHWRegSpecial(PSXPC), (u32)pc); \
	FlushAllHWReg(); \
	CALLFunc((u32)psx##f); \
}

#define REC_SYS(f) \
static void rec##f() { \
	iFlushRegs(0); \
	LIW(PutHWRegSpecial(ARG1), (u32)psxRegs.code); \
	STW(GetHWRegSpecial(ARG1), GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.code)); \
	LIW(PutHWRegSpecial(PSXPC), (u32)pc); \
	FlushAllHWReg(); \
	CALLFunc((u32)psx##f); \
	branch = 2; \
	iRet(); \
}

#define REC_BRANCH(f) \
static void rec##f() { \
	iFlushRegs(0); \
	LIW(PutHWRegSpecial(ARG1), (u32)psxRegs.code); \
	STW(GetHWRegSpecial(ARG1), GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.code)); \
	LIW(PutHWRegSpecial(PSXPC), (u32)pc); \
	FlushAllHWReg(); \
	CALLFunc((u32)psx##f); \
	branch = 2; \
	iRet(); \
}

static void freeMem(int all)
{
    if (recMem) free(recMem);
    if (recRAM) free(recRAM);
    if (recROM) free(recROM);
    recMem = recRAM = recROM = NULL;
    
    if (all && psxRecLUT) {
        free(psxRecLUT); 
		psxRecLUT = NULL;
    }
}

static int allocMem() {
	int i;

	freeMem(0);
        
	if (psxRecLUT==NULL)
		psxRecLUT = (u32*) memalign(32,0x010000 * 4);

	recMem = (char*) memalign(32,RECMEM_SIZE);
  //recMem = (char*) 0x90080000;
	recRAM = (char*) memalign(32,0x200000);
	recROM = (char*) memalign(32,0x080000);
	if (recRAM == NULL || recROM == NULL || recMem == NULL/*(void *)-1*/ || psxRecLUT == NULL) {
		freeMem(1);
		SysMessage("Error allocating memory"); return -1;
	}

	for (i=0; i<0x80; i++) psxRecLUT[i + 0x0000] = (u32)&recRAM[(i & 0x1f) << 16];
	memcpy(psxRecLUT + 0x8000, psxRecLUT, 0x80 * 4);
	memcpy(psxRecLUT + 0xa000, psxRecLUT, 0x80 * 4);

	for (i=0; i<0x08; i++) psxRecLUT[i + 0xbfc0] = (u32)&recROM[i << 16];

	return 0;
}

static int recInit() {
	return allocMem();
}

static void recReset() {
	memset(recRAM, 0, 0x200000);
	memset(recROM, 0, 0x080000);

	ppcInit();
	ppcSetPtr((u32 *)recMem);

	branch = 0;
	memset(iRegs, 0, sizeof(iRegs));
	iRegs[0].state = ST_CONST;
	iRegs[0].k     = 0;
}

static void recShutdown() {
	freeMem(1);
	ppcShutdown();
}

static void recError() {
	SysReset();
	ClosePlugins();
	SysMessage("Unrecoverable error while running recompiler\n");
	SysRunGui();
}

/*__inline*/ static void execute() {
	void (**recFunc)();
	char *p;

	p =	(char*)PC_REC(psxRegs.pc);
	/*if (p != NULL)*/ 
	/*else { recError(); return; }*/

	if (*p == 0) {
		recRecompile();
	}

	recFunc = (void (**)()) (u32)p;

	recRun(*recFunc, (u32)&psxRegs, (u32)&psxM);

}

static void recExecute() {
	for (;;) execute();
}

static void recExecuteBlock() {
	execute();
}

static void recClear(u32 Addr, u32 Size) {
	memset((void*)PC_REC(Addr), 0, Size * 4);
}

static void recNULL() {
//	SysMessage("recUNK: %8.8x\n", psxRegs.code);
}

/*********************************************************
* goes to opcodes tables...                              *
* Format:  table[something....]                          *
*********************************************************/

#if 0
REC_SYS(SPECIAL);
REC_SYS(REGIMM);
REC_SYS(COP0);
REC_SYS(COP2);
REC_SYS(BASIC);
#else
static void recSPECIAL() {
	recSPC[_Funct_]();
}

static void recREGIMM() {
	recREG[_Rt_]();
}

static void recCOP0() {
	recCP0[_Rs_]();
}

static void recCOP2() {
	recCP2[_Funct_]();
}

static void recBASIC() {
	recCP2BSC[_Rs_]();
}
#endif


//end of Tables opcodes...

/* - Arithmetic with immediate operand - */
/*********************************************************
* Arithmetic with immediate operand                      *
* Format:  OP rt, rs, immediate                          *
*********************************************************/
#if 0
REC_FUNC(ADDI);
REC_FUNC(ADDIU);
REC_FUNC(ANDI);
REC_FUNC(ORI);
REC_FUNC(XORI);
REC_FUNC(SLTI);
REC_FUNC(SLTIU);
#else
static void recADDIU()  {
// Rt = Rs + Im
	if (!_Rt_) return;

	if (IsConst(_Rs_)) {
		MapConst(_Rt_, iRegs[_Rs_].k + _Imm_);
	} else {
		if (_Imm_ == 0) {
			MapCopy(_Rt_, _Rs_);
		} else {
			ADDI(PutHWReg32(_Rt_), GetHWReg32(_Rs_), _Imm_);
		}
	}
}

static void recADDI()  {
// Rt = Rs + Im
	recADDIU();
}

//CR0:	SIGN      | POSITIVE | ZERO  | SOVERFLOW | SOVERFLOW | OVERFLOW | CARRY
static void recSLTI() {
// Rt = Rs < Im (signed)
	if (!_Rt_) return;

	if (IsConst(_Rs_)) {
		MapConst(_Rt_, (s32)iRegs[_Rs_].k < _Imm_);
	} else {
		if (_Imm_ == 0) {
			SRWI(PutHWReg32(_Rt_), GetHWReg32(_Rs_), 31);
		} else {
			int reg;
			CMPWI(GetHWReg32(_Rs_), _Imm_);
			reg = PutHWReg32(_Rt_);
			LI(reg, 1);
			BLT(1);
			LI(reg, 0);
		}
	}
}

static void recSLTIU() {
// Rt = Rs < Im (unsigned)
	if (!_Rt_) return;

	if (IsConst(_Rs_)) {
		MapConst(_Rt_, iRegs[_Rs_].k < _ImmU_);
	} else {
		int reg;
		CMPLWI(GetHWReg32(_Rs_), _Imm_);
		reg = PutHWReg32(_Rt_);
		LI(reg, 1);
		BLT(1);
		LI(reg, 0);
	}
}

static void recANDI() {
// Rt = Rs And Im
    if (!_Rt_) return;

    if (IsConst(_Rs_)) {
        MapConst(_Rt_, iRegs[_Rs_].k & _ImmU_);
    } else {
        ANDI_(PutHWReg32(_Rt_), GetHWReg32(_Rs_), _ImmU_);
    }
}

static void recORI() {
// Rt = Rs Or Im
	if (!_Rt_) return;

	if (IsConst(_Rs_)) {
		MapConst(_Rt_, iRegs[_Rs_].k | _ImmU_);
	} else {
		if (_Imm_ == 0) {
			MapCopy(_Rt_, _Rs_);
		} else {
			ORI(PutHWReg32(_Rt_), GetHWReg32(_Rs_), _ImmU_);
		}
	}
}

static void recXORI() {
// Rt = Rs Xor Im
    if (!_Rt_) return;

    if (IsConst(_Rs_)) {
        MapConst(_Rt_, iRegs[_Rs_].k ^ _ImmU_);
    } else {
        XORI(PutHWReg32(_Rt_), GetHWReg32(_Rs_), _ImmU_);
    }
}
#endif
//end of * Arithmetic with immediate operand  

/*********************************************************
* Load higher 16 bits of the first word in GPR with imm  *
* Format:  OP rt, immediate                              *
*********************************************************/

static void recLUI()  {
// Rt = Imm << 16
	if (!_Rt_) return;

	MapConst(_Rt_, _Imm_ << 16);
}

//End of Load Higher .....

/* - Register arithmetic - */
/*********************************************************
* Register arithmetic                                    *
* Format:  OP rd, rs, rt                                 *
*********************************************************/

#if 0
REC_FUNC(ADD);
REC_FUNC(ADDU);
REC_FUNC(SUB);
REC_FUNC(SUBU);
REC_FUNC(AND);
REC_FUNC(OR);
REC_FUNC(XOR);
REC_FUNC(NOR);
REC_FUNC(SLT);
REC_FUNC(SLTU);
#else
static void recADDU() {
// Rd = Rs + Rt 
	if (!_Rd_) return;

	if (IsConst(_Rs_) && IsConst(_Rt_)) {
		MapConst(_Rd_, iRegs[_Rs_].k + iRegs[_Rt_].k);
	} else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
		if ((s32)(s16)iRegs[_Rs_].k == (s32)iRegs[_Rs_].k) {
			ADDI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), (s16)iRegs[_Rs_].k);
		} else if ((iRegs[_Rs_].k & 0xffff) == 0) {
			ADDIS(PutHWReg32(_Rd_), GetHWReg32(_Rt_), iRegs[_Rs_].k>>16);
		} else {
			ADD(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
	} else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
		if ((s32)(s16)iRegs[_Rt_].k == (s32)iRegs[_Rt_].k) {
			ADDI(PutHWReg32(_Rd_), GetHWReg32(_Rs_), (s16)iRegs[_Rt_].k);
		} else if ((iRegs[_Rt_].k & 0xffff) == 0) {
			ADDIS(PutHWReg32(_Rd_), GetHWReg32(_Rs_), iRegs[_Rt_].k>>16);
		} else {
			ADD(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
	} else {
		ADD(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
	}
}

static void recADD() {
// Rd = Rs + Rt
	recADDU();
}

static void recSUBU() {
// Rd = Rs - Rt
    if (!_Rd_) return;

    if (IsConst(_Rs_) && IsConst(_Rt_)) {
        MapConst(_Rd_, iRegs[_Rs_].k - iRegs[_Rt_].k);
    } else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
        if ((s32)(s16)(-iRegs[_Rt_].k) == (s32)(-iRegs[_Rt_].k)) {
            ADDI(PutHWReg32(_Rd_), GetHWReg32(_Rs_), -iRegs[_Rt_].k);
        } else if (((-iRegs[_Rt_].k) & 0xffff) == 0) {
            ADDIS(PutHWReg32(_Rd_), GetHWReg32(_Rs_), (-iRegs[_Rt_].k)>>16);
        } else {
            SUB(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
        }
    } else {
        SUB(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
    }
}   

static void recSUB() {
// Rd = Rs - Rt
	recSUBU();
}

static void recAND() {
// Rd = Rs And Rt
    if (!_Rd_) return;
    
    if (IsConst(_Rs_) && IsConst(_Rt_)) {
        MapConst(_Rd_, iRegs[_Rs_].k & iRegs[_Rt_].k);
    } else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
        // TODO: implement shifted (ANDIS) versions of these
        if ((iRegs[_Rs_].k & 0xffff) == iRegs[_Rs_].k) {
            ANDI_(PutHWReg32(_Rd_), GetHWReg32(_Rt_), iRegs[_Rs_].k);
        } else {
            AND(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
        }
    } else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
        if ((iRegs[_Rt_].k & 0xffff) == iRegs[_Rt_].k) {
            ANDI_(PutHWReg32(_Rd_), GetHWReg32(_Rs_), iRegs[_Rt_].k);
        } else {
            AND(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
        }
    } else {
        AND(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
    }
}   

static void recOR() {
// Rd = Rs Or Rt
    if (!_Rd_) return;
    
    if (IsConst(_Rs_) && IsConst(_Rt_)) {
        MapConst(_Rd_, iRegs[_Rs_].k | iRegs[_Rt_].k);
    }
    else {
        if (_Rs_ == _Rt_) {
            MapCopy(_Rd_, _Rs_);
        }
        else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
            if ((iRegs[_Rs_].k & 0xffff) == iRegs[_Rs_].k) {
                ORI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), iRegs[_Rs_].k);
            } else {
                OR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
            }
        }
        else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
            if ((iRegs[_Rt_].k & 0xffff) == iRegs[_Rt_].k) {
                ORI(PutHWReg32(_Rd_), GetHWReg32(_Rs_), iRegs[_Rt_].k);
            } else {
                OR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
            }
        } else {
            OR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
        }
    }
}

static void recXOR() {
// Rd = Rs Xor Rt
    if (!_Rd_) return;
    
    if (IsConst(_Rs_) && IsConst(_Rt_)) {
        MapConst(_Rd_, iRegs[_Rs_].k ^ iRegs[_Rt_].k);
    } else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
        if ((iRegs[_Rs_].k & 0xffff) == iRegs[_Rs_].k) {
            XORI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), iRegs[_Rs_].k);
        } else {
            XOR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
        }
    } else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
        if ((iRegs[_Rt_].k & 0xffff) == iRegs[_Rt_].k) {
            XORI(PutHWReg32(_Rd_), GetHWReg32(_Rs_), iRegs[_Rt_].k);
        } else {
            XOR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
        }
    } else {
        XOR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
    }
}

static void recNOR() {
// Rd = Rs Nor Rt
	if (!_Rd_) return;
    
	if (IsConst(_Rs_) && IsConst(_Rt_)) {
		MapConst(_Rd_, ~(iRegs[_Rs_].k | iRegs[_Rt_].k));
	} else if (IsConst(_Rs_)) {
		LI(0, iRegs[_Rs_].k);
		NOR(PutHWReg32(_Rd_), GetHWReg32(_Rt_), 0);
	} else if (IsConst(_Rt_)) {
		LI(0, iRegs[_Rt_].k);
		NOR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), 0);
	} else {
		NOR(PutHWReg32(_Rd_), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
	}
}

static void recSLT() {
// Rd = Rs < Rt (signed)
	if (!_Rd_) return;

	if (IsConst(_Rs_) && IsConst(_Rt_)) {
		MapConst(_Rd_, (s32)iRegs[_Rs_].k < (s32)iRegs[_Rt_].k);
	} else { // TODO: add immidiate cases
		int reg;
		CMPW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		reg = PutHWReg32(_Rd_);
		LI(reg, 1);
		BLT(1);
		LI(reg, 0);
	}
}

static void recSLTU() { 
// Rd = Rs < Rt (unsigned)
	if (!_Rd_) return;

	if (IsConst(_Rs_) && IsConst(_Rt_)) {
		MapConst(_Rd_, iRegs[_Rs_].k < iRegs[_Rt_].k);
	} else { // TODO: add immidiate cases
		SUBFC(PutHWReg32(_Rd_), GetHWReg32(_Rt_), GetHWReg32(_Rs_));
		SUBFE(PutHWReg32(_Rd_), GetHWReg32(_Rd_), GetHWReg32(_Rd_));
		NEG(PutHWReg32(_Rd_), GetHWReg32(_Rd_));
	}
}
#endif
//End of * Register arithmetic

/* - mult/div & Register trap logic - */
/*********************************************************
* Register mult/div & Register trap logic                *
* Format:  OP rs, rt                                     *
*********************************************************/

#if 0
REC_FUNC(MULT);
REC_FUNC(MULTU);
REC_FUNC(DIV);
REC_FUNC(DIVU);
#else
int DoShift(u32 k)
{
	u32 i;
	for (i=0; i<30; i++) {
		if (k == (1ul << i))
			return i;
	}
	return -1;
}

// FIXME: doesn't work in GT - wrong way marker
static void recMULT() {
// Lo/Hi = Rs * Rt (signed)
	s32 k; int r;
	int usehi, uselo;
	
	if ((IsConst(_Rs_) && iRegs[_Rs_].k == 0) ||
		(IsConst(_Rt_) && iRegs[_Rt_].k == 0)) {
		MapConst(REG_LO, 0);
		MapConst(REG_HI, 0);
		return;
	}
	
	if (IsConst(_Rs_) && IsConst(_Rt_)) {
		u64 res = (s64)((s64)(s32)iRegs[_Rs_].k * (s64)(s32)iRegs[_Rt_].k);
		MapConst(REG_LO, (res & 0xffffffff));
		MapConst(REG_HI, ((res >> 32) & 0xffffffff));
		return;
	}
	
	if (IsConst(_Rs_)) {
		k = (s32)iRegs[_Rs_].k;
		r = _Rt_;
	} else if (IsConst(_Rt_)) {
		k = (s32)iRegs[_Rt_].k;
		r = _Rs_;
	} else {
		r = -1;
		k = 0;
	}
	
	// FIXME: this should not be needed!!!
	uselo = 1; //isPsxRegUsed(pc, REG_LO);
	usehi = 1; //isPsxRegUsed(pc, REG_HI);


	if (r != -1) {
		int shift = DoShift(k);
		if (shift != -1) {
			if (uselo) {
				SLWI(PutHWReg32(REG_LO), GetHWReg32(r), shift)
			}
			if (usehi) {
				SRAWI(PutHWReg32(REG_HI), GetHWReg32(r), 31-shift);
			}
		} else {
			//if ((s32)(s16)k == k) {
			//	MULLWI(PutHWReg32(REG_LO), GetHWReg32(r), k);
			//	MULHWI(PutHWReg32(REG_HI), GetHWReg32(r), k);
			//} else
			{
				if (uselo) {
					MULLW(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
				}
				if (usehi) {
					MULHW(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
				}
			}
		}
	} else {
		if (uselo) {
			MULLW(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
		if (usehi) {
			MULHW(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
	}
}

static void recMULTU() {
// Lo/Hi = Rs * Rt (unsigned)
	u32 k; int r;
	int usehi, uselo;
	
	if ((IsConst(_Rs_) && iRegs[_Rs_].k == 0) ||
		(IsConst(_Rt_) && iRegs[_Rt_].k == 0)) {
		MapConst(REG_LO, 0);
		MapConst(REG_HI, 0);
		return;
	}
	
	if (IsConst(_Rs_) && IsConst(_Rt_)) {
		u64 res = (u64)((u64)(u32)iRegs[_Rs_].k * (u64)(u32)iRegs[_Rt_].k);
		MapConst(REG_LO, (res & 0xffffffff));
		MapConst(REG_HI, ((res >> 32) & 0xffffffff));
		return;
	}
	
	if (IsConst(_Rs_)) {
		k = (s32)iRegs[_Rs_].k;
		r = _Rt_;
	} else if (IsConst(_Rt_)) {
		k = (s32)iRegs[_Rt_].k;
		r = _Rs_;
	} else {
		r = -1;
		k = 0;
	}
	
	uselo = isPsxRegUsed(pc, REG_LO);
	usehi = isPsxRegUsed(pc, REG_HI);

	if (r != -1) {
		int shift = DoShift(k);
		if (shift != -1) {
			if (uselo) {
				SLWI(PutHWReg32(REG_LO), GetHWReg32(r), shift);
			}
			if (usehi) {
				SRWI(PutHWReg32(REG_HI), GetHWReg32(r), 31-shift);
			}
		} else {
			{
				if (uselo) {
					MULLW(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
				}
				if (usehi) {
					MULHWU(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
				}
			}
		}
	} else {
		if (uselo) {
			MULLW(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
		if (usehi) {
			MULHWU(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
	}
}

static void recDIV() {
// Lo/Hi = Rs / Rt (signed)

	int usehi;

	if (IsConst(_Rs_) && iRegs[_Rs_].k == 0) {
		MapConst(REG_LO, 0);
		MapConst(REG_HI, 0);
		return;
	}
	if (IsConst(_Rt_) && IsConst(_Rs_)) {
		MapConst(REG_LO, (s32)iRegs[_Rs_].k / (s32)iRegs[_Rt_].k);
		MapConst(REG_HI, (s32)iRegs[_Rs_].k % (s32)iRegs[_Rt_].k);
		return;
	}
	
	usehi = isPsxRegUsed(pc, REG_HI);
	
	if (IsConst(_Rt_)) {
		int shift = DoShift(iRegs[_Rt_].k);
		if (shift != -1) {
			SRAWI(PutHWReg32(REG_LO), GetHWReg32(_Rs_), shift);
			ADDZE(PutHWReg32(REG_LO), GetHWReg32(REG_LO));
			if (usehi) {
				RLWINM(PutHWReg32(REG_HI), GetHWReg32(_Rs_), 0, (31-shift), 31);
			}
		} else if (iRegs[_Rt_].k == 3) {
			// http://the.wall.riscom.net/books/proc/ppc/cwg/code2.html
			LIS(PutHWReg32(REG_HI), 0x5555);
			ADDI(PutHWReg32(REG_HI), GetHWReg32(REG_HI), 0x5556);
			MULHW(PutHWReg32(REG_LO), GetHWReg32(REG_HI), GetHWReg32(_Rs_));
			SRWI(PutHWReg32(REG_HI), GetHWReg32(_Rs_), 31);
			ADD(PutHWReg32(REG_LO), GetHWReg32(REG_LO), GetHWReg32(REG_HI));
			if (usehi) {
				MULLI(PutHWReg32(REG_HI), GetHWReg32(REG_LO), 3);
				SUB(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(REG_HI));
			}
		} else {
			DIVW(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
			if (usehi) {
				if ((iRegs[_Rt_].k & 0x7fff) == iRegs[_Rt_].k) {
					MULLI(PutHWReg32(REG_HI), GetHWReg32(REG_LO), iRegs[_Rt_].k);
				} else {
					MULLW(PutHWReg32(REG_HI), GetHWReg32(REG_LO), GetHWReg32(_Rt_));
				}
				SUB(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(REG_HI));
			}
		}
	} else {
		DIVW(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		if (usehi) {
			MULLW(PutHWReg32(REG_HI), GetHWReg32(REG_LO), GetHWReg32(_Rt_));
			SUB(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(REG_HI));
		}
	}
}

static void recDIVU() {
// Lo/Hi = Rs / Rt (unsigned)
	int usehi;

	if (IsConst(_Rs_) && iRegs[_Rs_].k == 0) {
		MapConst(REG_LO, 0);
		MapConst(REG_HI, 0);
		return;
	}
	if (IsConst(_Rt_) && IsConst(_Rs_)) {
		MapConst(REG_LO, (u32)iRegs[_Rs_].k / (u32)iRegs[_Rt_].k);
		MapConst(REG_HI, (u32)iRegs[_Rs_].k % (u32)iRegs[_Rt_].k);
		return;
	}
	
	usehi = isPsxRegUsed(pc, REG_HI);

	if (IsConst(_Rt_)) {
		int shift = DoShift(iRegs[_Rt_].k);
		if (shift != -1) {
			SRWI(PutHWReg32(REG_LO), GetHWReg32(_Rs_), shift);
			if (usehi) {
				RLWINM(PutHWReg32(REG_HI), GetHWReg32(_Rs_), 0, (31-shift), 31);
			}
		} else {
			DIVWU(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
			if (usehi) {
				MULLW(PutHWReg32(REG_HI), GetHWReg32(_Rt_), GetHWReg32(REG_LO));
				SUB(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(REG_HI));
			}
		}
	} else {
		DIVWU(PutHWReg32(REG_LO), GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		if (usehi) {
			MULLW(PutHWReg32(REG_HI), GetHWReg32(_Rt_), GetHWReg32(REG_LO));
			SUB(PutHWReg32(REG_HI), GetHWReg32(_Rs_), GetHWReg32(REG_HI));
		}
	}
}
#endif
//End of * Register mult/div & Register trap logic  

/* - memory access - */

static void preMemRead()
{
	ReserveArgs(1);
	if (_Rs_ != _Rt_) {
		DisposeHWReg(iRegs[_Rt_].reg);
	}

	ADDI(PutHWRegSpecial(ARG1), GetHWReg32(_Rs_), _Imm_);

	if (_Rs_ == _Rt_) {
		DisposeHWReg(iRegs[_Rt_].reg);
	}

	InvalidateCPURegs();
	//resp += 4;
}

static void preMemWrite(int size)
{
	ReserveArgs(2);
	ADDI(PutHWRegSpecial(ARG1), GetHWReg32(_Rs_), _Imm_);

	switch(size)
	{
		case 1:
			RLWINM(PutHWRegSpecial(ARG2), GetHWReg32(_Rt_), 0, 24, 31);
			break;
		case 2:
			RLWINM(PutHWRegSpecial(ARG2), GetHWReg32(_Rt_), 0, 16, 31);
			break;
		default:
			MR(PutHWRegSpecial(ARG2), GetHWReg32(_Rt_));
	}

	InvalidateCPURegs();

	//resp += 8;
}

#if 1
REC_FUNC(LWL);	//TODO
REC_FUNC(LWR);	//TODO
REC_FUNC(SWL);	//TODO
REC_FUNC(SWR);	//TODO
#else
static void recLWL() {
/*
const u32 addr = _rRs_ + _Imm_;
const u32 shift = (addr & 3) << 3;
const u32 mem = psxMemRead32( addr & 0xfffffffc );

if (!_Rt_) return;
_rRt_ = (_rRt_ & (0x00ffffff >> shift)) | (mem << (24 - shift));
///
0x800449cc <psxLWL+0>:  mflr    r0
0x800449d0 <psxLWL+4>:  stwu    r1,-16(r1)
0x800449d4 <psxLWL+8>:  stw     r30,8(r1)
0x800449d8 <psxLWL+12>: stw     r31,12(r1)
0x800449dc <psxLWL+16>: lis     r31,-32735
0x800449e0 <psxLWL+20>: stw     r0,20(r1)
0x800449e4 <psxLWL+24>: addi    r31,r31,24808
0x800449e8 <psxLWL+28>: lwz     r0,524(r31)
0x800449ec <psxLWL+32>: rlwinm  r9,r0,13,25,29
0x800449f0 <psxLWL+36>: extsh   r30,r0
0x800449f4 <psxLWL+40>: lwzx    r9,r31,r9
0x800449f8 <psxLWL+44>: add     r30,r30,r9
0x800449fc <psxLWL+48>: rlwinm  r3,r30,0,0,29
0x80044a00 <psxLWL+52>: bl      0x80042ce4 <psxMemRead32>
0x80044a04 <psxLWL+56>: lhz     r0,524(r31)
0x80044a08 <psxLWL+60>: andi.   r0,r0,31
0x80044a0c <psxLWL+64>: beq-    0x80044a3c <psxLWL+112>
0x80044a10 <psxLWL+68>: rlwinm  r0,r0,2,0,29
0x80044a14 <psxLWL+72>: lis     r9,255
0x80044a18 <psxLWL+76>: rlwinm  r30,r30,3,27,28
0x80044a1c <psxLWL+80>: lwzx    r11,r31,r0
0x80044a20 <psxLWL+84>: ori     r9,r9,65535
0x80044a24 <psxLWL+88>: sraw    r9,r9,r30
0x80044a28 <psxLWL+92>: subfic  r30,r30,24
0x80044a2c <psxLWL+96>: slw     r3,r3,r30
0x80044a30 <psxLWL+100>:        and     r9,r9,r11
0x80044a34 <psxLWL+104>:        or      r3,r3,r9
0x80044a38 <psxLWL+108>:        stwx    r3,r31,r0
0x80044a3c <psxLWL+112>:        lwz     r0,20(r1)
0x80044a40 <psxLWL+116>:        lwz     r30,8(r1)
0x80044a44 <psxLWL+120>:        mtlr    r0
0x80044a48 <psxLWL+124>:        lwz     r31,12(r1)
0x80044a4c <psxLWL+128>:        addi    r1,r1,16
0x80044a50 <psxLWL+132>:        blr
*/

	iFlushRegs(0);
	if (IsConst(_Rs_)) {
		LIW(31, iRegs[_Rs_].k);
		ADDI(31, 31, _Imm_);
	}
	else
		ADDI(31, GetHWReg32(_Rs_), _Imm_);
	

}
#endif

#if 0
REC_FUNC(LB);
REC_FUNC(LBU);
REC_FUNC(LH);
REC_FUNC(LHU);
REC_FUNC(LW);
#else
static void recLB() {
// Rt = mem[Rs + Im] (signed)
	
    if (IsConst(_Rs_)) {
        u32 addr = iRegs[_Rs_].k + _Imm_;
        int t = addr >> 16;
    
        if ((t & 0xfff0)  == 0xbfc0) {
            if (!_Rt_) return;
            // since bios is readonly it won't change
            MapConst(_Rt_, psxRs8(addr));
            return;
        }
        if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
            if (!_Rt_) return;
                
            LIW(PutHWReg32(_Rt_), (u32)&psxM[addr & 0x1fffff]);
            LBZ(PutHWReg32(_Rt_), GetHWReg32(_Rt_), 0);
            EXTSB(PutHWReg32(_Rt_), GetHWReg32(_Rt_));
            return;
        }
        if (t == 0x1f80 && addr < 0x1f801000) {
            if (!_Rt_) return;
    
            LIW(PutHWReg32(_Rt_), (u32)&psxH[addr & 0xfff]);
            LBZ(PutHWReg32(_Rt_), GetHWReg32(_Rt_), 0);
            EXTSB(PutHWReg32(_Rt_), GetHWReg32(_Rt_));
            return;
        }
    //	SysPrintf("unhandled r8 %x\n", addr);
    }
	
	preMemRead();
	CALLFunc((u32)psxMemRead8);
	if (_Rt_) {
		EXTSB(PutHWReg32(_Rt_), GetHWRegSpecial(RETVAL));
		DisposeHWReg(GetSpecialIndexFromHWRegs(RETVAL));
	}
}

static void recLBU() {
// Rt = mem[Rs + Im] (unsigned)

    if (IsConst(_Rs_)) {
        u32 addr = iRegs[_Rs_].k + _Imm_;
        int t = addr >> 16;
    
        if ((t & 0xfff0)  == 0xbfc0) {
            if (!_Rt_) return;
            // since bios is readonly it won't change
            MapConst(_Rt_, psxRu8(addr));
            return;
        }
        if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
            if (!_Rt_) return;
                
            LIW(PutHWReg32(_Rt_), (u32)&psxM[addr & 0x1fffff]);
            LBZ(PutHWReg32(_Rt_), GetHWReg32(_Rt_), 0);
            return;
        }
        if (t == 0x1f80 && addr < 0x1f801000) {
            if (!_Rt_) return;
    
            LIW(PutHWReg32(_Rt_), (u32)&psxH[addr & 0xfff]);
            LBZ(PutHWReg32(_Rt_), GetHWReg32(_Rt_), 0);
            return;
        }
    //	SysPrintf("unhandled r8 %x\n", addr);
    }
        
	preMemRead();
	CALLFunc((u32)psxMemRead8);
	
	if (_Rt_) {
		SetDstCPUReg(3);
		PutHWReg32(_Rt_);
	}
}

static void recLH() {
// Rt = mem[Rs + Im] (signed)

	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].k + _Imm_;
		int t = addr >> 16;
	
		if ((t & 0xfff0)  == 0xbfc0) {
			if (!_Rt_) return;
			// since bios is readonly it won't change
			MapConst(_Rt_, psxRs16(addr));
			return;
		}
		if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
			if (!_Rt_) return;

			LIW(PutHWReg32(_Rt_), (u32)&psxM[addr & 0x1fffff]);
			LHBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
			EXTSH(PutHWReg32(_Rt_), GetHWReg32(_Rt_));
			return;
		}
		if (t == 0x1f80 && addr < 0x1f801000) {
			if (!_Rt_) return;

			LIW(PutHWReg32(_Rt_), (u32)&psxH[addr & 0xfff]);
			LHBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
			EXTSH(PutHWReg32(_Rt_), GetHWReg32(_Rt_));
			return;
		}
	//	SysPrintf("unhandled r16 %x\n", addr);
	}
    
	preMemRead();
	CALLFunc((u32)psxMemRead16);
	if (_Rt_) {
		EXTSH(PutHWReg32(_Rt_), GetHWRegSpecial(RETVAL));
		DisposeHWReg(GetSpecialIndexFromHWRegs(RETVAL));
	}
}

static void recLHU() {
// Rt = mem[Rs + Im] (unsigned)

	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].k + _Imm_;
		int t = addr >> 16;
	
		if ((t & 0xfff0) == 0xbfc0) {
			if (!_Rt_) return;
			// since bios is readonly it won't change
			MapConst(_Rt_, psxRu16(addr));
			return;
		}
		if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
			if (!_Rt_) return;

			LIW(PutHWReg32(_Rt_), (u32)&psxM[addr & 0x1fffff]);
			LHBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
			return;
		}
		if (t == 0x1f80 && addr < 0x1f801000) {
			if (!_Rt_) return;

			LIW(PutHWReg32(_Rt_), (u32)&psxH[addr & 0xfff]);
			LHBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
			return;
		}
		if (t == 0x1f80) {
			if (addr >= 0x1f801c00 && addr < 0x1f801e00) {
					if (!_Rt_) return;
					
					ReserveArgs(1);
					LIW(PutHWRegSpecial(ARG1), addr);
					DisposeHWReg(iRegs[_Rt_].reg);
					InvalidateCPURegs();
					CALLFunc((u32)SPU_readRegister);
					
					SetDstCPUReg(3);
					PutHWReg32(_Rt_);
					resp+= 4;
					return;
			}
			switch (addr) {
					case 0x1f801100: case 0x1f801110: case 0x1f801120:
						if (!_Rt_) return;
						
						ReserveArgs(1);
						LIW(PutHWRegSpecial(ARG1), (addr >> 4) & 0x3);
						DisposeHWReg(iRegs[_Rt_].reg);
						InvalidateCPURegs();
						CALLFunc((u32)psxRcntRcount);
						
						SetDstCPUReg(3);
						PutHWReg32(_Rt_);
						resp+= 4;
						return;

					case 0x1f801104: case 0x1f801114: case 0x1f801124:
						if (!_Rt_) return;

						ReserveArgs(1);
						LIW(PutHWRegSpecial(ARG1), (addr >> 4) & 0x3);
						DisposeHWReg(iRegs[_Rt_].reg);
						InvalidateCPURegs();
						CALLFunc((u32)psxRcntRmode);

						SetDstCPUReg(3);
						PutHWReg32(_Rt_);
						resp+= 4;
/*
						LIW(PutHWReg32(_Rt_), (u32)&psxCounters[(addr >> 4) & 0x3].mode);
						LWZ(PutHWReg32(_Rt_), GetHWReg32(_Rt_), 0);
*/
						return;
	
					case 0x1f801108: case 0x1f801118: case 0x1f801128:
						if (!_Rt_) return;

						LIW(PutHWReg32(_Rt_), (u32)&psxCounters[(addr >> 4) & 0x3].target);
						LWZ(PutHWReg32(_Rt_), GetHWReg32(_Rt_), 0);
						return;
					}
		}
	//	SysPrintf("unhandled r16u %x\n", addr);
	}
	
	preMemRead();
	CALLFunc((u32)psxMemRead16);
	if (_Rt_) {
		SetDstCPUReg(3);
		PutHWReg32(_Rt_);
	}
}

static void recLW() {
// Rt = mem[Rs + Im] (unsigned)

	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].k + _Imm_;
		int t = addr >> 16;

		if ((t & 0xfff0) == 0xbfc0) {
			if (!_Rt_) return;
			// since bios is readonly it won't change
			MapConst(_Rt_, psxRu32(addr));
			return;
		}
		if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
			if (!_Rt_) return;

			LIW(PutHWReg32(_Rt_), (u32)&psxM[addr & 0x1fffff]);
			LWBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
			return;
		}
		if (t == 0x1f80 && addr < 0x1f801000) {
			if (!_Rt_) return;

			LIW(PutHWReg32(_Rt_), (u32)&psxH[addr & 0xfff]);
			LWBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
			return;
		}
		if (t == 0x1f80) {
			switch (addr) {
				case 0x1f801080: case 0x1f801084: case 0x1f801088: 
				case 0x1f801090: case 0x1f801094: case 0x1f801098: 
				case 0x1f8010a0: case 0x1f8010a4: case 0x1f8010a8: 
				case 0x1f8010b0: case 0x1f8010b4: case 0x1f8010b8: 
				case 0x1f8010c0: case 0x1f8010c4: case 0x1f8010c8: 
				case 0x1f8010d0: case 0x1f8010d4: case 0x1f8010d8: 
				case 0x1f8010e0: case 0x1f8010e4: case 0x1f8010e8: 
				case 0x1f801070: case 0x1f801074:
				case 0x1f8010f0: case 0x1f8010f4:
					if (!_Rt_) return;
					
					LIW(PutHWReg32(_Rt_), (u32)&psxH[addr & 0xffff]);
					LWBRX(PutHWReg32(_Rt_), 0, GetHWReg32(_Rt_));
					return;

				case 0x1f801810:
					if (!_Rt_) return;

					DisposeHWReg(iRegs[_Rt_].reg);
					InvalidateCPURegs();
					CALLFunc((u32)GPU_readData);

					SetDstCPUReg(3);
					PutHWReg32(_Rt_);
					return;

				case 0x1f801814:
					if (!_Rt_) return;

					DisposeHWReg(iRegs[_Rt_].reg);
					InvalidateCPURegs();
					CALLFunc((u32)GPU_readStatus);
					
					SetDstCPUReg(3);
					PutHWReg32(_Rt_);
					return;
			}
		}
//		SysPrintf("unhandled r32 %x\n", addr);
	}

	preMemRead();
	CALLFunc((u32)psxMemRead32);
	if (_Rt_) {
		SetDstCPUReg(3);
		PutHWReg32(_Rt_);
	}
}
#endif

#if 0
REC_FUNC(SB);
REC_FUNC(SH);
REC_FUNC(SW);
#else
static void recSB() {
// mem[Rs + Im] = Rt
/*
	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].k + _Imm_;
		int t = addr >> 16;

		if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
			if (IsConst(_Rt_)) {
				MOV8ItoM((uptr)&psxM[addr & 0x1fffff], (u8)iRegs[_Rt_].k);
			} else {
				MOV8MtoR(EAX, (uptr)&psxRegs.GPR.r[_Rt_]);
				MOV8RtoM((uptr)&psxM[addr & 0x1fffff], EAX);
			}
			return;
		}
		if (t == 0x1f80 && addr < 0x1f801000) {
			if (IsConst(_Rt_)) {
				MOV8ItoM((uptr)&psxH[addr & 0xfff], (u8)iRegs[_Rt_].k);
			} else {
				MOV8MtoR(EAX, (uptr)&psxRegs.GPR.r[_Rt_]);
				MOV8RtoM((uptr)&psxH[addr & 0xfff], EAX);
			}
			return;
		}
//		SysPrintf("unhandled w8 %x\n", addr);
	}
*/
	preMemWrite(1);
	CALLFunc((u32)psxMemWrite8);
}

static void recSH() {
// mem[Rs + Im] = Rt

	/*if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].k + _Imm_;
		int t = addr >> 16;

		if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
			if (IsConst(_Rt_)) {
				MOV16ItoM((u32)&psxM[addr & 0x1fffff], (u16)iRegs[_Rt_].k);
			} else {
				LIW(0, GetHWReg32(_Rt_));
				MOV16RtoM((u32)&psxM[addr & 0x1fffff], EAX);
			}
			return;
		}
		if (t == 0x1f80 && addr < 0x1f801000) {
			if (IsConst(_Rt_)) {
				MOV16ItoM((u32)&psxH[addr & 0xfff], (u16)iRegs[_Rt_].k);
			} else {
				MOV16MtoR(EAX, (u32)&psxRegs.GPR.r[_Rt_]);
				MOV16RtoM((u32)&psxH[addr & 0xfff], EAX);
			}
			return;
		}
		if (t == 0x1f80) {
			if (addr >= 0x1f801c00 && addr < 0x1f801e00) {
				if (IsConst(_Rt_)) {
					PUSH32I(iRegs[_Rt_].k);
				} else {
					PUSH32M((u32)&psxRegs.GPR.r[_Rt_]);
				}
				PUSH32I  (addr);
				CALL32M  ((u32)&SPU_writeRegister);
#ifndef __WIN32__
				resp+= 8;
#endif
				return;
			}
		}
//		SysPrintf("unhandled w16 %x\n", addr);
	}*/

	preMemWrite(2);
	CALLFunc((u32)psxMemWrite16);
}

static void recSW() {
// mem[Rs + Im] = Rt
	//u32 *b1, *b2;
#if 0
	if (IsConst(_Rs_)) {
		u32 addr = iRegs[_Rs_].k + _Imm_;
		int t = addr >> 16;

		if ((t & 0x1fe0) == 0 && (t & 0x1fff) != 0) {
			LIW(0, addr & 0x1fffff);
			STWBRX(GetHWReg32(_Rt_), GetHWRegSpecial(PSXMEM), 0);
			return;
		}
		if (t == 0x1f80 && addr < 0x1f801000) {
			LIW(0, (u32)&psxH[addr & 0xfff]);
			STWBRX(GetHWReg32(_Rt_), 0, 0);
			return;
		}
		if (t == 0x1f80) {
			switch (addr) {
				case 0x1f801080: case 0x1f801084: 
				case 0x1f801090: case 0x1f801094: 
				case 0x1f8010a0: case 0x1f8010a4: 
				case 0x1f8010b0: case 0x1f8010b4: 
				case 0x1f8010c0: case 0x1f8010c4: 
				case 0x1f8010d0: case 0x1f8010d4: 
				case 0x1f8010e0: case 0x1f8010e4: 
				case 0x1f801074:
				case 0x1f8010f0:
					LIW(0, (u32)&psxH[addr & 0xffff]);
					STWBRX(GetHWReg32(_Rt_), 0, 0);
					return;

/*				case 0x1f801810:
					if (IsConst(_Rt_)) {
						PUSH32I(iRegs[_Rt_].k);
					} else {
						PUSH32M((u32)&psxRegs.GPR.r[_Rt_]);
					}
					CALL32M((u32)&GPU_writeData);
#ifndef __WIN32__
					resp+= 4;
#endif
					return;

				case 0x1f801814:
					if (IsConst(_Rt_)) {
						PUSH32I(iRegs[_Rt_].k);
					} else {
						PUSH32M((u32)&psxRegs.GPR.r[_Rt_]);
					}
					CALL32M((u32)&GPU_writeStatus);
#ifndef __WIN32__
					resp+= 4;
#endif*/
			}
		}
//		SysPrintf("unhandled w32 %x\n", addr);
	}
	
/*	LIS(0, 0x0079 + ((_Imm_ <= 0) ? 1 : 0));
	CMPLW(GetHWReg32(_Rs_), 0);
	BGE_L(b1);
	
	//SaveContext();
	ADDI(0, GetHWReg32(_Rs_), _Imm_);
	RLWINM(0, GetHWReg32(_Rs_), 0, 11, 31);
	STWBRX(GetHWReg32(_Rt_), GetHWRegSpecial(PSXMEM), 0);
	B_L(b2);
	
	B_DST(b1);*/
#endif
	preMemWrite(4);
	CALLFunc((u32)psxMemWrite32);
	//B_DST(b2);
}

#endif

#if 0
REC_FUNC(SLL);
REC_FUNC(SRL);
REC_FUNC(SRA);
#else
static void recSLL() {
// Rd = Rt << Sa
    if (!_Rd_) return;

    if (IsConst(_Rt_)) {
        MapConst(_Rd_, iRegs[_Rt_].k << _Sa_);
    } else {
        SLWI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), _Sa_);
    }
}

static void recSRL() {
// Rd = Rt >> Sa
    if (!_Rd_) return;

    if (IsConst(_Rt_)) {
        MapConst(_Rd_, iRegs[_Rt_].k >> _Sa_);
    } else {
        SRWI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), _Sa_);
    }
}

static void recSRA() {
// Rd = Rt >> Sa
    if (!_Rd_) return;

    if (IsConst(_Rt_)) {
        MapConst(_Rd_, (s32)iRegs[_Rt_].k >> _Sa_);
    } else {
        SRAWI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), _Sa_);
    }
}
#endif

/* - shift ops - */

#if 1
REC_FUNC(SLLV);
#else
static void recSLLV() {
// Rd = Rt << Rs
	if (!_Rd_) return;

	if (IsConst(_Rt_) && IsConst(_Rs_)) {
		MapConst(_Rd_, iRegs[_Rt_].k << (iRegs[_Rs_].k & 0x1f));
	} else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
		SLWI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), (iRegs[_Rs_].k & 0x1f));
	} else {
		RLWINM(PutHWReg32(_Rd_), GetHWReg32(_Rs_), 0, 27, 31); // &0x1f
		SLW(PutHWReg32(_Rd_), GetHWReg32(_Rt_), GetHWReg32(_Rd_));
	}
}
#endif

#if 1
REC_FUNC(SRLV);
#else
static void recSRLV() {
// Rd = Rt >> Rs
	if (!_Rd_) return;

	if (IsConst(_Rt_) && IsConst(_Rs_)) {
		MapConst(_Rd_, iRegs[_Rt_].k >> (iRegs[_Rs_].k & 0x1f));
	} else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
		SRWI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), (iRegs[_Rs_].k & 0x1f));
	} else {
		RLWINM(PutHWReg32(_Rd_), GetHWReg32(_Rs_), 0, 27, 31); // &0x1f
		SRW(PutHWReg32(_Rd_), GetHWReg32(_Rt_), GetHWReg32(_Rd_));
	}
}
#endif

#if 1
REC_FUNC(SRAV);
#else
static void recSRAV() {
// Rd = Rt >> Rs
	if (!_Rd_) return;

	if (IsConst(_Rt_) && IsConst(_Rs_)) {
		MapConst(_Rd_, (s32)iRegs[_Rt_].k >> (iRegs[_Rs_].k & 0x1f));
	} else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
		SRAWI(PutHWReg32(_Rd_), GetHWReg32(_Rt_), (iRegs[_Rs_].k & 0x1f));
	} else {
		RLWINM(PutHWReg32(_Rd_), GetHWReg32(_Rs_), 0, 27, 31); // &0x1f
		SRAW(PutHWReg32(_Rd_), GetHWReg32(_Rt_), GetHWReg32(_Rd_));
	}
}
#endif

#if 0
REC_SYS(SYSCALL);
REC_SYS(BREAK);
#else
static void recSYSCALL() {
//	dump=1;
	iFlushRegs(0);
	
	ReserveArgs(2);
	LIW(PutHWRegSpecial(PSXPC), pc - 4);
	LIW(PutHWRegSpecial(ARG1), 0x20);
	LIW(PutHWRegSpecial(ARG2), (branch == 1 ? 1 : 0));
	FlushAllHWReg();
	CALLFunc ((u32)psxException);

	branch = 2;
	iRet();
}

static void recBREAK() {
}
#endif

#if 0
REC_FUNC(MFHI);
REC_FUNC(MTHI);
REC_FUNC(MFLO);
REC_FUNC(MTLO);
#else
static void recMFHI() {
// Rd = Hi
	if (!_Rd_) return;
	
	if (IsConst(REG_HI)) {
		MapConst(_Rd_, iRegs[REG_HI].k);
	} else {
		MapCopy(_Rd_, REG_HI);
	}
}

static void recMTHI() {
// Hi = Rs

	if (IsConst(_Rs_)) {
		MapConst(REG_HI, iRegs[_Rs_].k);
	} else {
		MapCopy(REG_HI, _Rs_);
	}
}

static void recMFLO() {
// Rd = Lo
	if (!_Rd_) return;

	if (IsConst(REG_LO)) {
		MapConst(_Rd_, iRegs[REG_LO].k);
	} else {
		MapCopy(_Rd_, REG_LO);
	}
}

static void recMTLO() {
// Lo = Rs

	if (IsConst(_Rs_)) {
		MapConst(REG_LO, iRegs[_Rs_].k);
	} else {
		MapCopy(REG_LO, _Rs_);
	}
}
#endif
/* - branch ops - */

#if 1
REC_BRANCH(BEQ);     // *FIXME
REC_BRANCH(BNE);     // *FIXME
#else
static void recBEQ() {
// Branch if Rs == Rt
	u32 bpc = _Imm_ * 4 + pc;
	u32 *b;

	if (_Rs_ == _Rt_) {
		iJump(bpc);
	}
	else {
		if (IsConst(_Rs_) && IsConst(_Rt_)) {
			if (iRegs[_Rs_].k == iRegs[_Rt_].k) {
				iJump(bpc); return;
			} else {
				iJump(pc+4); return;
			}
		}
		else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
			if ((iRegs[_Rs_].k & 0xffff) == iRegs[_Rs_].k) {
				CMPLWI(GetHWReg32(_Rt_), iRegs[_Rs_].k);
			}
			else if ((s32)(s16)iRegs[_Rs_].k == (s32)iRegs[_Rs_].k) {
				CMPWI(GetHWReg32(_Rt_), iRegs[_Rs_].k);
			}
			else {
				CMPLW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
			}
		}
		else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
			if ((iRegs[_Rt_].k & 0xffff) == iRegs[_Rt_].k) {
				CMPLWI(GetHWReg32(_Rs_), iRegs[_Rt_].k);
			}
			else if ((s32)(s16)iRegs[_Rt_].k == (s32)iRegs[_Rt_].k) {
				CMPWI(GetHWReg32(_Rs_), iRegs[_Rt_].k);
			}
			else {
				CMPLW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
			}
		}
		else {
			CMPLW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
		
		BEQ_L(b);
		
		iBranch(pc+4, 1);
	
		B_DST(b);
		
		iBranch(bpc, 0);
		pc+=4;
	}
}

static void recBNE() {
// Branch if Rs != Rt
	u32 bpc = _Imm_ * 4 + pc;
	u32 *b;

	if (_Rs_ == _Rt_) {
		iJump(pc+4);
	}
	else {
		if (IsConst(_Rs_) && IsConst(_Rt_)) {
			if (iRegs[_Rs_].k != iRegs[_Rt_].k) {
				iJump(bpc); return;
			} else {
				iJump(pc+4); return;
			}
		}
		else if (IsConst(_Rs_) && !IsMapped(_Rs_)) {
			if ((iRegs[_Rs_].k & 0xffff) == iRegs[_Rs_].k) {
				CMPLWI(GetHWReg32(_Rt_), iRegs[_Rs_].k);
			}
			else if ((s32)(s16)iRegs[_Rs_].k == (s32)iRegs[_Rs_].k) {
				CMPWI(GetHWReg32(_Rt_), iRegs[_Rs_].k);
			}
			else {
				CMPLW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
			}
		}
		else if (IsConst(_Rt_) && !IsMapped(_Rt_)) {
			if ((iRegs[_Rt_].k & 0xffff) == iRegs[_Rt_].k) {
				CMPLWI(GetHWReg32(_Rs_), iRegs[_Rt_].k);
			}
			else if ((s32)(s16)iRegs[_Rt_].k == (s32)iRegs[_Rt_].k) {
				CMPWI(GetHWReg32(_Rs_), iRegs[_Rt_].k);
			}
			else {
				CMPLW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
			}
		}
		else {
			CMPLW(GetHWReg32(_Rs_), GetHWReg32(_Rt_));
		}
		
		BNE_L(b);
		
		iBranch(pc+4, 1);
	
		B_DST(b);
		
		iBranch(bpc, 0);
		pc+=4;
	}
}
#endif

#if 0
REC_BRANCH(BLTZ);
REC_BRANCH(BGTZ);
REC_BRANCH(BLTZAL);
REC_BRANCH(BGEZAL);

REC_BRANCH(BLEZ);
REC_BRANCH(BGEZ);
REC_BRANCH(J);
REC_BRANCH(JR);
REC_BRANCH(JAL);
REC_BRANCH(JALR);
#else
static void recBLTZ() {
// Branch if Rs < 0
	u32 bpc = _Imm_ * 4 + pc;
	u32 *b;

	if (IsConst(_Rs_)) {
		if ((s32)iRegs[_Rs_].k < 0) {
			iJump(bpc); return;
		} else {
			iJump(pc+4); return;
		}
	}

	CMPWI(GetHWReg32(_Rs_), 0);
	BLT_L(b);
	
	iBranch(pc+4, 1);

	B_DST(b);
	
	iBranch(bpc, 0);
	pc+=4;
}

static void recBGTZ() {
// Branch if Rs > 0
    u32 bpc = _Imm_ * 4 + pc;
    u32 *b;

    if (IsConst(_Rs_)) {
        if ((s32)iRegs[_Rs_].k > 0) {
            iJump(bpc); return;
        } else {
            iJump(pc+4); return;
        }
    }

    CMPWI(GetHWReg32(_Rs_), 0);
    BGT_L(b);
    
    iBranch(pc+4, 1);

    B_DST(b);

    iBranch(bpc, 0);
    pc+=4;
}

static void recBLTZAL() {
// Branch if Rs < 0
    u32 bpc = _Imm_ * 4 + pc;
    u32 *b;

    if (IsConst(_Rs_)) {
        if ((s32)iRegs[_Rs_].k < 0) {
            MapConst(31, pc + 4);

            iJump(bpc); return;
        } else {
            iJump(pc+4); return;
        }
    }

    CMPWI(GetHWReg32(_Rs_), 0);
    BLT_L(b);
    
    iBranch(pc+4, 1);

    B_DST(b);
    
    MapConst(31, pc + 4);

    iBranch(bpc, 0);
    pc+=4;
}

static void recBGEZAL() {
// Branch if Rs >= 0
    u32 bpc = _Imm_ * 4 + pc;
    u32 *b;

    if (IsConst(_Rs_)) {
        if ((s32)iRegs[_Rs_].k >= 0) {
            MapConst(31, pc + 4);

            iJump(bpc); return;
        } else {
            iJump(pc+4); return;
        }
    }

    CMPWI(GetHWReg32(_Rs_), 0);
    BGE_L(b);
    
    iBranch(pc+4, 1);

    B_DST(b);
    
    MapConst(31, pc + 4);

    iBranch(bpc, 0);
    pc+=4;
}

static void recBLEZ() {
// Branch if Rs <= 0
	u32 bpc = _Imm_ * 4 + pc;
	u32 *b;

	if (IsConst(_Rs_)) {
		if ((s32)iRegs[_Rs_].k <= 0) {
			iJump(bpc); return;
		} else {
			iJump(pc+4); return;
		}
	}

	CMPWI(GetHWReg32(_Rs_), 0);
	BLE_L(b);
	
	iBranch(pc+4, 1);

	B_DST(b);
	
	iBranch(bpc, 0);
	pc+=4;
}

static void recBGEZ() {
// Branch if Rs >= 0
	u32 bpc = _Imm_ * 4 + pc;
	u32 *b;

	if (IsConst(_Rs_)) {
		if ((s32)iRegs[_Rs_].k >= 0) {
			iJump(bpc); return;
		} else {
			iJump(pc+4); return;
		}
	}

	CMPWI(GetHWReg32(_Rs_), 0);
	BGE_L(b);
	
	iBranch(pc+4, 1);

	B_DST(b);
	
	iBranch(bpc, 0);
	pc+=4;
}

static void recJ() {
// j target

	iJump(_Target_ * 4 + (pc & 0xf0000000));
}

static void recJAL() {
// jal target
	MapConst(31, pc + 4);

	iJump(_Target_ * 4 + (pc & 0xf0000000));
}

static void recJR() {
// jr Rs

	if (IsConst(_Rs_)) {
		LIW(PutHWRegSpecial(TARGET), iRegs[_Rs_].k);
	} else {
		MR(PutHWRegSpecial(TARGET), GetHWReg32(_Rs_));
	}
	SetBranch();
}

static void recJALR() {
// jalr Rs

	
	if (IsConst(_Rs_)) {
		//iJump(iRegs[_Rs_].k);
		LIW(PutHWRegSpecial(TARGET), iRegs[_Rs_].k);
	} else {
		MR(PutHWRegSpecial(TARGET), GetHWReg32(_Rs_));
	}
	
	if (_Rd_) {
		MapConst(_Rd_, pc + 4);
	}
	
	SetBranch();
}
#endif

#if 0
REC_FUNC(RFE);
#else
static void recRFE() {
	iFlushRegs(0);
	LWZ(0, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.CP0.n.Status));
	RLWINM(11, 0, 0, 0, 27);
	RLWINM(0, 0, 30, 28, 31);
	OR(0, 0, 11);
	STW(0, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.CP0.n.Status));

	LIW(PutHWRegSpecial(PSXPC), (u32)pc);
	FlushAllHWReg();
	if (branch == 0) {
		branch = 2;
		iRet();
	}
}
#endif

#if 0
REC_FUNC(MFC0);
REC_SYS(MTC0);
REC_FUNC(CFC0);
REC_SYS(CTC0);
#else
static void recMFC0() {
// Rt = Cop0->Rd
	if (!_Rt_) return;
	LWZ(PutHWReg32(_Rt_), GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.CP0.r[_Rd_]));
}

static void recCFC0() {
// Rt = Cop0->Rd

	recMFC0();
}

static void recMTC0() {
// Cop0->Rd = Rt

	if (IsConst(_Rt_)) {
		LIW(0, (u32)iRegs[_Rt_].k);
		STW(0, GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.CP0.r[_Rd_]));
	}
	else
	{
		STW(GetHWReg32(_Rt_), GetHWRegSpecial(PSXREGS), OFFSET(&psxRegs, &psxRegs.CP0.r[_Rd_]));
	}
}

static void recCTC0() {
// Cop0->Rd = Rt

	recMTC0();
}
#endif
#include "pGte.h"

static void recHLE() {
	CALLFunc((u32)psxHLEt[psxRegs.code & 0xffff]);
	branch = 2;
	iRet();
}

static void (*recBSC[64])() = {
	recSPECIAL, recREGIMM, recJ   , recJAL  , recBEQ , recBNE , recBLEZ, recBGTZ,
	recADDI   , recADDIU , recSLTI, recSLTIU, recANDI, recORI , recXORI, recLUI ,
	recCOP0   , recNULL  , recCOP2, recNULL , recNULL, recNULL, recNULL, recNULL,
	recNULL   , recNULL  , recNULL, recNULL , recNULL, recNULL, recNULL, recNULL,
	recLB     , recLH    , recLWL , recLW   , recLBU , recLHU , recLWR , recNULL,
	recSB     , recSH    , recSWL , recSW   , recNULL, recNULL, recSWR , recNULL,
	recNULL   , recNULL  , recLWC2, recNULL , recNULL, recNULL, recNULL, recNULL,
	recNULL   , recNULL  , recSWC2, recHLE  , recNULL, recNULL, recNULL, recNULL
};

static void (*recSPC[64])() = {
	recSLL , recNULL, recSRL , recSRA , recSLLV   , recNULL , recSRLV, recSRAV,
	recJR  , recJALR, recNULL, recNULL, recSYSCALL, recBREAK, recNULL, recNULL,
	recMFHI, recMTHI, recMFLO, recMTLO, recNULL   , recNULL , recNULL, recNULL,
	recMULT, recMULTU, recDIV, recDIVU, recNULL   , recNULL , recNULL, recNULL,
	recADD , recADDU, recSUB , recSUBU, recAND    , recOR   , recXOR , recNOR ,
	recNULL, recNULL, recSLT , recSLTU, recNULL   , recNULL , recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL   , recNULL , recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL   , recNULL , recNULL, recNULL
};

static void (*recREG[32])() = {
	recBLTZ  , recBGEZ  , recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL  , recNULL  , recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recBLTZAL, recBGEZAL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL  , recNULL  , recNULL, recNULL, recNULL, recNULL, recNULL, recNULL
};

static void (*recCP0[32])() = {
	recMFC0, recNULL, recCFC0, recNULL, recMTC0, recNULL, recCTC0, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recRFE , recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL
};

static void (*recCP2[64])() = {
	recBASIC, recRTPS , recNULL , recNULL, recNULL, recNULL , recNCLIP, recNULL, // 00
	recNULL , recNULL , recNULL , recNULL, recOP  , recNULL , recNULL , recNULL, // 08
	recDPCS , recINTPL, recMVMVA, recNCDS, recCDP , recNULL , recNCDT , recNULL, // 10
	recNULL , recNULL , recNULL , recNCCS, recCC  , recNULL , recNCS  , recNULL, // 18
	recNCT  , recNULL , recNULL , recNULL, recNULL, recNULL , recNULL , recNULL, // 20
	recSQR  , recDCPL , recDPCT , recNULL, recNULL, recAVSZ3, recAVSZ4, recNULL, // 28 
	recRTPT , recNULL , recNULL , recNULL, recNULL, recNULL , recNULL , recNULL, // 30
	recNULL , recNULL , recNULL , recNULL, recNULL, recGPF  , recGPL  , recNCCT  // 38
};

static void (*recCP2BSC[32])() = {
	recMFC2, recNULL, recCFC2, recNULL, recMTC2, recNULL, recCTC2, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL,
	recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL, recNULL
};

static void recRecompile() {
	//static int recCount = 0;
	char *p;
	u32 *ptr;
	int i;
	
	cop2readypc = 0;
	idlecyclecount = 0;
	resp = 0;

	// initialize state variables
	UniqueRegAlloc = 1;
	HWRegUseCount = 0;
	DstCPUReg = -1;
	memset(HWRegisters, 0, sizeof(HWRegisters));
	for (i=0; i<NUM_HW_REGISTERS; i++)
		HWRegisters[i].code = cpuHWRegisters[NUM_HW_REGISTERS-i-1];
	
	// reserve the special psxReg register
	HWRegisters[0].usage = HWUSAGE_SPECIAL | HWUSAGE_RESERVED | HWUSAGE_HARDWIRED;
	HWRegisters[0].private = PSXREGS;
	HWRegisters[0].k = (u32)&psxRegs;

	HWRegisters[1].usage = HWUSAGE_SPECIAL | HWUSAGE_RESERVED | HWUSAGE_HARDWIRED;
	HWRegisters[1].private = PSXMEM;
	HWRegisters[1].k = (u32)&psxM;

	// reserve the special psxRegs.cycle register
	//HWRegisters[1].usage = HWUSAGE_SPECIAL | HWUSAGE_RESERVED | HWUSAGE_HARDWIRED;
	//HWRegisters[1].private = CYCLECOUNT;
	
	//memset(iRegs, 0, sizeof(iRegs));
	for (i=0; i<NUM_REGISTERS; i++) {
		iRegs[i].state = ST_UNK;
		iRegs[i].reg = -1;
	}
	iRegs[0].k = 0;
	iRegs[0].state = ST_CONST;
	
	/* if ppcPtr reached the mem limit reset whole mem */
	if (((u32)ppcPtr - (u32)recMem) >= (RECMEM_SIZE - 0x10000))
		recReset();

	ppcAlign(4);
	ptr = ppcPtr;
	
	// give us write access
	//mprotect(recMem, RECMEM_SIZE, PROT_EXEC|PROT_READ|PROT_WRITE);
	
	// tell the LUT where to find us
	PC_REC32(psxRegs.pc) = (u32)ppcPtr;

	pcold = pc = psxRegs.pc;
	
	//SysPrintf("RecCount: %i\n", recCount++);
	
	for (count=0; count<500;) {
		p = (char *)PSXM(pc);
		if (p == NULL) recError();
		psxRegs.code = GETLE32((u32 *)p);

		pc+=4; count++;
//		iFlushRegs(0); // test
		recBSC[psxRegs.code>>26]();

		if (branch) {
			branch = 0;
			//if (dump) iDumpBlock(ptr);
			goto done;
		}
	}

	iFlushRegs(pc);
	
	LIW(PutHWRegSpecial(PSXPC), pc);

	iRet();

done:;
	u32 a = (u32)(u8*)ptr;
	while(a < (u32)(u8*)ppcPtr) {
	  __asm__ __volatile__("icbi 0,%0" : : "r" (a));
	  __asm__ __volatile__("dcbst 0,%0" : : "r" (a));
	  a += 4;
	}
	__asm__ __volatile__("sync");
	__asm__ __volatile__("isync");

	sprintf((char *)ppcPtr, "PC=%08x", pcold);
	ppcPtr += strlen((char *)ppcPtr);
}

R3000Acpu psxRec = {
	recInit,
	recReset,
	recExecute,
	recExecuteBlock,
	recClear,
	recShutdown
};
