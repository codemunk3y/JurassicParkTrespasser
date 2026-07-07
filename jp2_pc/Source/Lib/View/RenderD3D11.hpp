/***********************************************************************************************
 *
 * Copyright � 2026.  Experimental modernization.
 *
 * Contents:
 *		RenderD3D11 - an experimental Direct3D 11 present backend (env TRESPASS_D3D11).
 *
 *		The engine's CPU pipeline already produces, per frame, a list of screen-space,
 *		pre-transformed, textured, lit polygons (the same D3DTLVERTEX-shaped stream the
 *		1998 aux-D3D path consumed).  This module takes that stream and renders it through
 *		a modern GPU (D3D11) with a passthrough shader, presenting via a DXGI swap chain.
 *		It is the first slice of the VR-enablement work: prove the transformed-vertex
 *		stream can reach a modern per-eye render target.
 *
 *		This header deliberately pulls in NO Direct3D headers, so it is safe to include
 *		from any engine translation unit.  All D3D11 detail lives in RenderD3D11.cpp.
 *
 *		Everything here is gated by the TRESPASS_D3D11 environment variable and is a
 *		complete no-op when it is unset - the software renderer path is untouched.
 *
 **********************************************************************************************/

#ifndef HEADER_LIB_VIEW_RENDERD3D11_HPP
#define HEADER_LIB_VIEW_RENDERD3D11_HPP

namespace RenderD3D11
{
	//******************************************************************************************
	//
	// A screen-space, pre-transformed vertex mirrored from the engine's SRenderVertex.
	// Matches the semantics of a 1998 D3DTLVERTEX: position is already in screen pixels,
	// fInvW (= rhw) carries 1/w for perspective-correct texture interpolation, colour is
	// the baked Gouraud/lit diffuse.
	//
	struct SVert
	{
		float        fSX, fSY;		// Screen position, pixels (top-left origin).
		float        fSZ;			// Screen depth, 0..1.
		float        fInvW;			// Reciprocal homogeneous w (rhw).
		unsigned int u4Color;		// Diffuse colour, 0xAARRGGBB.
		float        fU, fV;		// Texture coordinates, [0,1].
	};

	//******************************************************************************************
	//
	// Returns true if the TRESPASS_D3D11 environment variable is set (cached).  When false,
	// every other entry point in this module is a no-op and the software path runs normally.
	//
	bool bEnabled();

	//******************************************************************************************
	//
	// Returns true only once the device and swap chain have been successfully created and are
	// actively presenting.  The software present path (CRasterWin::Flip) suppresses itself iff
	// this is true, so a D3D11 init failure cleanly falls back to the software renderer rather
	// than leaving a black window.
	//
	bool bActive();

	//******************************************************************************************
	//
	// Hand the module the game window.  Called by the app (which owns the HWND); the device
	// and swap chain are created lazily on the first bBeginFrame using this window.  Passed
	// as void* so this header needs no <windows.h>.  Idempotent.
	//
	void SetWindow(void* p_hwnd);

	//******************************************************************************************
	//
	// Begin a frame: lazily create the device / swap chain / render-target on first use,
	// bind and clear the back buffer, set the viewport to (i_width x i_height).  Returns
	// false if D3D11 is disabled or unavailable (caller then just runs the software path).
	//
	bool bBeginFrame(int i_width, int i_height);

	//******************************************************************************************
	//
	// Submit one screen-space polygon (triangle fan of i_count vertices) with its texture.
	//
	// SLICE 2 (not yet implemented): append to a dynamic vertex buffer and issue the draw
	// through the passthrough vertex shader + textured pixel shader.  Stubbed no-op for now
	// so the device/swap-chain/present infrastructure can be brought up and verified first.
	//
	void SubmitPolygon(const SVert* pav_verts, int i_count, void* p_texture);

	//******************************************************************************************
	//
	// Present the back buffer to the window (DXGI Present).
	//
	void Present();

	//******************************************************************************************
	//
	// Release all D3D11 objects.  Safe to call when never initialised.
	//
	void Shutdown();
}

#endif // HEADER_LIB_VIEW_RENDERD3D11_HPP
