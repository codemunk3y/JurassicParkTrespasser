/**********************************************************************************************
 *
 * Copyright © DreamWorks Interactive, 1996
 *
 * Notes:
 *
 * This file should not contain any source code, it just inlcudes another source file
 * based on the selected target build processor. If code is included within this file it
 * will be common to all processors.
 *
 * All source files that are included by this file should be complete C/C++ files that will
 * compile without this file so that this can easily be reversed if required.
 *
 **********************************************************************************************
 *
 * $Log:: /JP2_PC/Source/Lib/Renderer/Primitives/DrawSubTriangle.cpp                          $
 * 
 * 24    98.10.03 11:11p Mmouni
 * Put self moding code in code segment "SelfMod".
 * 
 * 23    10/01/98 12:36a Asouth
 * Rearranged includes to ensure proper config
 * 
 * 22    8/27/98 9:13p Asouth
 * fixed data type explicitly scoped
 * 
 * 21    98.01.22 3:15p Mmouni
 * Changed bump-map depth conditional compilation.
 * 
 * 20    1/19/98 7:34p Pkeet
 * Added support for 16 bit bumpmaps.
 * 
 * 19    97.12.01 4:25p Mmouni
 * P6 build now includes its own version of ScanlineAsmMacros.
 * 
 * 18    11/16/97 3:56a Rwycko
 * Now includes P6 version of water and terrain primitives.
 * 
 * 17    97.11.11 9:51p Mmouni
 * Now includes AMDK6 version of DrawSubTriangleTerrain and DrawSubTriangleWater.
 * 
 * 16    97/11/10 11:26a Pkeet
 * Added a water version of 'DrawSubTriangle.'
 * 
 * 15    97/11/06 4:51p Pkeet
 * Added the terrain fogging module.
 * 
 * 14    97.10.27 1:27p Mmouni
 * Made changes to support the K6-3D.
 * 
 * 13    97/10/23 10:56a Pkeet
 * Added a K6 3D switch that uses the current pentium subtriangle.
 * 
 * 12    97.10.15 7:36p Mmouni
 * Now include DrawSubTriangle.hpp before DrawTriangle.hpp.
 * 
 * 11    97/09/26 16:31 Speter
 * Okay, REALLY turned off bloody warnings.
 * 
 * 10    97/09/24 12:29 Speter
 * Turned off the bloody warning.
 * 
 * 9     9/15/97 1:58p Mmouni
 * Added pvBaseOfLine, d_temp_a, and d_temp_b to statics.
 * 
 * 8     8/28/97 4:07p Agrant
 * Source Safe Restored to Tuesday, August 26, 1997
 * 
 * 8     8/25/97 5:15p Rwyatt
 * Added headers for performance counters.
 * 
 * 7     8/19/97 12:12a Rwyatt
 * Added 32 bit shadow primitive
 * 
 * 6     8/17/97 12:23a Rwyatt
 * Added new includes for alpha primitive and the associated globals.
 * 
 * 5     8/15/97 12:47a Rwyatt
 * New includes for terrain primitives
 * 
 * 4     97/07/16 16:01 Speter
 * Reinstated VER_ASM switch for primitives.  Now this file includes all DrawSubTriangle
 * sub-modules; the processor gating is done only here.
 * 
 * 3     7/07/97 11:43p Rwyatt
 * Shell for different processors
 *
 *********************************************************************************************/

#include "Lib/W95/WinInclude.hpp"
#include "Config.hpp"
#include "Common.hpp"
#include "DrawTriangle.hpp"
#include "DrawSubTriangle.hpp"

#include "Lib/Sys/PerformanceCount.hpp"
#include "Lib/Sys/DebugConsole.hpp"

#if VER_ASM

static int			i_x_from;
static int			i_x_to;
static int			i_screen_index;
static int			i_pixel;

static ::fixed		fx_inc;
static ::fixed		fx_diff;
static UBigFixed	bf_u_inc;
static CWalk1D		w1d_v_inc;

static float		f_inc_uinvz;
static float		f_inc_vinvz;
static float		f_inc_invz;

static uint32		u4ShadowColour;

static float f_u;
static float f_v;
static float f_z;
static float f_next_z;

static int32 i4_temp_a;
static int32 i4_temp_b;

// These need to be 64 bit aligned for speed.
double d_temp_a;
double d_temp_b;

// Pointer to base of the current scanline.
static void* pvBaseOfLine;



#if TARGET_PROCESSOR == PROCESSOR_K6_3D

// Disable useless warning about no EMMS instruction in a function.
#pragma warning(disable: 4799)

// Stupid assembler complains about the size of an element of a struct,
// but assembles the instruction with the correct size anyway.
#pragma warning(disable: 4410)

//
// K6-3D specific static globals and constants.
//
const float pfFixed16Scale[2] = {65536.0f, 65536.0f};
const DWORD u4OneOne = 0x00010001;

static uint64 qIndexTemp;			// Temp for texture indicies.
static uint64 qShadeTemp;			// Temp for shading indicies.
static uint64 qMFactor;				// Temp for texture address multipliers.
static uint64 pfCurUV;				// Current u,v.
static TexVals tvCurUVZ;			// Current u,v,z.
static TexVals tvEdgeStep;			// Edge stepping values.
static uint64 qShadeValue;			// Temp for shading values.
static uint64 qShadeSlopes;			// Temp for shading slopes.

#endif // TARGET_PROCESSOR == PROCESSOR_K6_3D


//
// Static pointers to polygons and scanlines.
//
static TCopyLinearTrans*					pscanLin;
static CDrawPolygon<TCopyLinearTrans>*		pdtriLin;

static TCopyLinear*							pscanCopy;
static CDrawPolygon<TCopyLinear>*			pdtriCopy;

static TTexNoClutLinear*					pscanTer;
static CDrawPolygon<TTexNoClutLinear>*		pdtriTer;

static TTexNoClutLinearTrans*				pscanTerTrans;
static CDrawPolygon<TTexNoClutLinearTrans>*	pdtriTerTrans;

static TShadowTrans8*						pscanShadow8;
static CDrawPolygon<TShadowTrans8>*			pdtriShadow8;

static TShadowTrans32*						pscanShadow32;
static CDrawPolygon<TShadowTrans32>*		pdtriShadow32;


//**********************************************************************************************
#if TARGET_PROCESSOR == PROCESSOR_PENTIUM

#pragma code_seg("SelfMod")
	#include "P5/ScanlineAsmMacros.hpp"
	#include "P5/DrawSubTriangleEx.inl"
	#include "P5/DrawSubTriangleGourEx.inl"
	#include "P5/DrawSubTriangleTexEx.inl"
	#include "P5/DrawSubTriangleTexGourEx.inl"
	#include "P5/DrawSubTriangleBumpEx.inl"

	#if iBUMPMAP_RESOLUTION == 32
		#include "P5/DrawSubTriangleBumpTblEx.inl"
	#endif // iBUMPMAP_RESOLUTION == 32

	#include "P5/DrawSubTriangleTerrainEx.inl"
	#include "P5/DrawSubTriangleAlpha.inl"
	#include "P5/DrawSubTriangleTerrain.inl"
	#include "P5/DrawSubTriangleWater.inl"
#pragma code_seg()

#elif TARGET_PROCESSOR == PROCESSOR_PENTIUMPRO

#pragma code_seg("SelfMod")
	#include "P6/ScanlineAsmMacros.hpp"
	#include "P6/DrawSubTriangleEx.inl"
	#include "P6/DrawSubTriangleGourEx.inl"
	#include "P6/DrawSubTriangleTexEx.inl"
	#include "P6/DrawSubTriangleTexGourEx.inl"
	#include "P6/DrawSubTriangleBumpEx.inl"

	#if iBUMPMAP_RESOLUTION == 32
		#include "P6/DrawSubTriangleBumpTblEx.inl"
	#endif // iBUMPMAP_RESOLUTION == 32

	#include "P6/DrawSubTriangleTerrainEx.inl"
	#include "P6/DrawSubTriangleAlpha.inl"
	#include "P6/DrawSubTriangleTerrain.inl"
	#include "P6/DrawSubTriangleWater.inl"
#pragma code_seg()

#elif TARGET_PROCESSOR == PROCESSOR_K6_3D
	#include "AMDK6/ScanlineAsmMacros.hpp"
	#include "AMDK6/DrawSubTriangleEx.inl"
	#include "AMDK6/DrawSubTriangleGourEx.inl"
	#include "AMDK6/DrawSubTriangleTexEx.inl"
	#include "AMDK6/DrawSubTriangleTexGourEx.inl"
	#include "AMDK6/DrawSubTriangleBumpEx.inl"

	#if iBUMPMAP_RESOLUTION == 32
		#include "AMDK6/DrawSubTriangleBumpTblEx.inl"
	#endif // iBUMPMAP_RESOLUTION == 32

	#include "AMDK6/DrawSubTriangleTerrainEx.inl"
	#include "AMDK6/DrawSubTriangleAlpha.inl"
	#include "AMDK6/DrawSubTriangleTerrain.inl"
	#include "AMDK6/DrawSubTriangleWater.inl"
#else
	#error Invalid [No] target processor specified
#endif

#include "DrawSubTriangleFlat.inl"
//**********************************************************************************************

// #if VER_ASM
#else

//**********************************************************************************************
//
// Terrain lighting, without assembly.
//
// A terrain page texel is not a colour but a packed CLUT index: (intensity << 8) | palette
// index, which CClut::iGetIndex reassembles as 'i_index | (i_ramp << iShiftRamp)'.  The two
// halves are rasterised by separate passes - TTexNoClutLinear writes the palette index into
// the low byte, and this primitive Gouraud-shades the intensity into the high byte - so this
// pass must leave the low byte of every pixel it touches alone.
//
// The generic template cannot do that.  It composes a whole destination pixel and assigns it,
// and for TShadeTerrain (CMap<uint8> + CIndexNone + CColLookupOff) the pixel it composes is a
// constant zero: nothing is ever indexed, so the source pixel stays 0 and no CLUT is applied.
// It therefore wiped both the intensity and the texture index, leaving a page of index 0 -
// which is why a non-asm build drew the terrain flat blue.  Shadows avoid this by way of an
// Assign() overload keyed on CMapShadow::TDummy (MapT.hpp), which writes the high byte only;
// that trick does not extend here because the value written is per-pixel, not constant.
//
// Mirrors the assembly in P5/P6/AMDK6 DrawSubTriangleGourEx.inl, which rasterises this
// primitive left to right only.
//
void DrawSubtriangle(TShadeTerrain* pscan, CDrawPolygon<TShadeTerrain>* pdtri)
{
	Assert(pscan);
	Assert(pdtri);
	Assert(pdtri->prasScreen->iPixelBytes() == 2);

	// Iterate through the scanlines that intersect the subtriangle.
	do
	{
		int i_x_from = pscan->fxX.i4Fx >> 16;
		int i_x_to   = (pscan->fxX.i4Fx + pdtri->fxLineLength.i4Fx) >> 16;

		// Draw if there are pixels to draw, and this scanline is not being skipped.
		// bEvenScanlinesOnly is 0 or 1, so 'iY & bEvenScanlinesOnly' drops the odd lines,
		// as the assembly does.  (The generic template's version of this test keeps the odd
		// lines and drops the even ones instead - it disagrees with both the assembly and
		// the name, but it has never been compiled.  Left alone here: that template still
		// serves primitives which have no assembly version, including in VER_ASM builds.)
		if (i_x_to > i_x_from && !(pdtri->iY & bEvenScanlinesOnly))
		{
			TShadeTerrain::TGouraud gour(pscan->gourIntensity);

			// Address the intensity byte of the pixel, not the pixel.
			uint8* pu1_intensity = (uint8*)
				((uint16*)pdtri->prasScreen->pSurface + pdtri->iLineStartIndex + i_x_to) + 1;

			// As elsewhere in the rasteriser, the pixel count runs negative up to zero.
			for (int i_pixel = i_x_from - i_x_to; i_pixel; i_pixel++)
			{
				pu1_intensity[i_pixel * 2] = uint8(gour.fxIntensity.i4Fx >> 16);

				++gour;
			}
		}

		// Increment the base edge.
		++*pdtri->pedgeBase;

		// Set the new length of the line to be rasterized.
		pdtri->fxLineLength.i4Fx += pdtri->fxDeltaLineLength.i4Fx;

		// Get new starting pixel TIndex for the scanline.
		pdtri->iLineStartIndex += pdtri->prasScreen->iLinePixels;
	}
	while (++pdtri->iY < pdtri->iYTo);
}

#endif
