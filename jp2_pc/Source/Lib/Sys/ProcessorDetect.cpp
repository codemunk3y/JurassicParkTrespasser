/***********************************************************************************************
 *
 * Copyright © DreamWorks Interactive. 1996
 *
 * Contents:
 * Application side of the processor detection system.
 *
 * Bugs:
 *
 * To do:
 * Have some method of locating the DLL if it is no where we expected it to be.
 *
 ***********************************************************************************************
 *
 * $Log:: /JP2_PC/Source/Lib/Sys/ProcessorDetect.cpp                                           $
 * 
 * 3     7/09/98 1:11a Pkeet
 * Added the 'u4GetCPUSpeed' function.
 * 
 * 2     7/08/97 5:16p Rwyatt
 * Now loads the DLL from the current Dir
 * 
 * 1     7/08/97 4:47p Rwyatt
 * Class wrapper for the processor detection DLL
 * 
 **********************************************************************************************/


//*********************************************************************************************
//
#include "Common.hpp"
#include "Lib/W95/WinInclude.hpp"
#include "ProcessorDetect.hpp"

#include <intrin.h>
#include <string.h>


//*********************************************************************************************
// Detect the processor directly with the CPUID instruction and read its clock speed from the
// registry.  This replaces the original 'processor.dll', which fails to load / returns nothing
// useful on modern Windows and modern CPUs - producing the startup "Failed to detect processor"
// prompt and a bogus 0 MHz reading that made the game classify every machine as "slow" (lowest
// quality + smallest render size).
//
static void DetectProcessor(CPUInfo& cpu)
{
	memset(&cpu, 0, sizeof(cpu));

	int regs[4] = { 0 };

	// CPUID function 0: max standard leaf + vendor id string (EBX, EDX, ECX).
	__cpuid(regs, 0);
	int i_max_id = regs[0];
	*((int*)&cpu.strManufactureID[0])  = regs[1];
	*((int*)&cpu.strManufactureID[4])  = regs[3];
	*((int*)&cpu.strManufactureID[8])  = regs[2];
	cpu.strManufactureID[12] = '\0';
	cpu.u4MaxCPUID = (uint32)i_max_id;

	// CPUID function 1: model + standard feature flags (EDX).
	uint32 u4_features = 0;
	if (i_max_id >= 1)
	{
		__cpuid(regs, 1);
		cpu.u4Model   = (uint32)regs[0];
		cpu.u4Feature = (uint32)regs[3];
		u4_features   = (uint32)regs[3];
	}

	// Extended CPUID: brand string (0x80000002..4) and extended features (0x80000001).
	__cpuid(regs, 0x80000000);
	uint32 u4_max_ext = (uint32)regs[0];

	if (u4_max_ext >= 0x80000004)
	{
		char* pc = &cpu.strProcessor[0];
		for (uint32 u4_leaf = 0x80000002; u4_leaf <= 0x80000004; ++u4_leaf)
		{
			__cpuid(regs, (int)u4_leaf);
			memcpy(pc, regs, 16);
			pc += 16;
		}
		*pc = '\0';
	}
	else
	{
		strcpy(&cpu.strProcessor[0], "x86 processor");
	}

	// Manufacturer.
	if (memcmp(cpu.strManufactureID, "GenuineIntel", 12) == 0)
		cpu.cpumanProcessorManufacture = cpumanINTEL;
	else if (memcmp(cpu.strManufactureID, "AuthenticAMD", 12) == 0)
		cpu.cpumanProcessorManufacture = cpumanAMD;
	else
		cpu.cpumanProcessorManufacture = cpumanUNKNOWN;

	// Normalised feature flags.  Any Win32-capable modern CPU is at least a
	// Pentium Pro class device with an FPU; the game's PentiumPro build only
	// requires the CPU_PENTIUMPRO bit (CMOV etc.), which is always true here.
	uint32 u4_flags = CPU_CPUID | CPU_FPUPRESENT | CPU_PENTIUM | CPU_PENTIUMPRO;
	if (u4_features & (1u << 23)) u4_flags |= CPU_MMX;			// EDX bit 23
	if (u4_features & (1u << 15)) u4_flags |= CPU_CMOV;			// EDX bit 15
	if (u4_features & (1u <<  4)) u4_flags |= CPU_RDTSC;			// EDX bit 4
	if (u4_features & (1u <<  8)) u4_flags |= CPU_CMPXCHG8B;		// EDX bit 8

	// 3DNow! lives in extended-feature EDX bit 31 (AMD only).
	if (u4_max_ext >= 0x80000001)
	{
		__cpuid(regs, 0x80000001);
		if ((uint32)regs[3] & (1u << 31)) u4_flags |= CPU_3DNOW;
	}
	cpu.u4CPUFlags = u4_flags;

	cpu.cpufamProcessorFamily = cpufamPENTIUMPRO;
	cpu.cpufamFPUFamily       = cpufamPENTIUMPRO;

	// Clock speed: read ~MHz from the registry (set by Windows, reliable on
	// modern machines where the old RDTSC-timing method in the DLL failed).
	cpu.u4CPUSpeed = 0;
	HKEY hkey;
	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE,
	                 "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
	                 0, KEY_READ, &hkey) == ERROR_SUCCESS)
	{
		DWORD dw_mhz = 0, dw_size = sizeof(dw_mhz), dw_type = 0;
		if (RegQueryValueEx(hkey, "~MHz", NULL, &dw_type,
		                    (LPBYTE)&dw_mhz, &dw_size) == ERROR_SUCCESS &&
		    dw_type == REG_DWORD)
		{
			cpu.u4CPUSpeed = dw_mhz;
		}
		RegCloseKey(hkey);
	}

	// If the registry reading failed, assume a fast modern machine so the game
	// does not fall back to its slowest quality/render-size defaults.
	if (cpu.u4CPUSpeed < 10)
		cpu.u4CPUSpeed = 3000;
}


//*********************************************************************************************
// Class Implementation
//*********************************************************************************************

//*********************************************************************************************
// Constructor loads the DLL, does the stuff and then unloads it. We do not keep the DLL
// around until the destructor is called. This is because the DLL only returns a 100 byte
// struture which can be copied into some local storage.
//
// If the DLL proves to be a problem then meybe we could move the DLL code into this class
// but that would mean keeping it around for the whole game which is pointless.
//
CCPUDetect::CCPUDetect()
{
	// Detect the processor in-process via CPUID instead of the legacy
	// processor.dll (which no longer works on modern Windows).
	DetectProcessor(cpuCPUInfo);
	bLoaded = true;
}



//*********************************************************************************************
//
CCPUDetect::~CCPUDetect()
{
	// do nothing at the moment
}


//*********************************************************************************************
uint32 u4GetCPUSpeed()
{
	CCPUDetect det;

	// If the cpu is not reliably detected, return 0.
	if (!det.bLoaded)
		return 0;

	// Return the detected speed.
	return det.cpuCPUInfo.u4CPUSpeed;
}
