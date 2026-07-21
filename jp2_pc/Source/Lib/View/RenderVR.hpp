/***********************************************************************************************
 *
 * Copyright (c) 2026.  Experimental modernization.
 *
 * Contents:
 *		RenderVR - an experimental OpenXR bring-up layer (env TRESPASS_VR).
 *
 *		This is the VR sibling of RenderD3D11.  The D3D11 backend already renders the engine's
 *		screen-space polygon stream through a modern GPU; this module adds the OpenXR runtime on
 *		top of that same D3D11 device so the frame can be delivered to a head-mounted display.
 *
 *		Staged, so each step is verifiable headless (against the Meta XR Simulator / Monado, no
 *		hardware) before the next:
 *		  M1 (this slice): load the OpenXR loader, create an XrInstance with the D3D11 graphics
 *		     extension, find the HMD system, and query its D3D11 requirements + recommended
 *		     per-eye render size.  Pure bring-up; renders nothing.
 *		  M2: create the session on RenderD3D11's device + per-eye swapchains + the frame loop.
 *		  M3: stereo - drive the engine camera to each eye pose/projection, run the CPU pipeline
 *		     twice, submit each pass to an eye swapchain.
 *		  M4: head pose -> engine camera.
 *
 *		The engine's D3D11 path receives geometry ALREADY projected to 2D screen space (the CPU
 *		pipeline bakes in a single mono projection), so a second eye cannot be produced with a
 *		shader matrix - stereo is done by re-running the CPU pipeline per eye (M3).  That is why
 *		this layer sits beside the renderer rather than inside the pixel shader.
 *
 *		This header pulls in NO OpenXR or Direct3D headers, so it is safe to include from any
 *		engine translation unit.  All OpenXR detail lives in RenderVR.cpp, and the loader is
 *		bound at runtime via LoadLibrary("openxr_loader.dll") - there is NO link-time dependency
 *		on the OpenXR loader, so a build/box without one is unaffected.
 *
 *		Everything is gated by the TRESPASS_VR environment variable and is a complete no-op when
 *		it is unset - the existing (mono, D3D11 or software) path is untouched.
 *
 **********************************************************************************************/

#ifndef HEADER_LIB_VIEW_RENDERVR_HPP
#define HEADER_LIB_VIEW_RENDERVR_HPP

namespace RenderVR
{
	//******************************************************************************************
	//
	// Returns true if the TRESPASS_VR environment variable is set (cached).  When false, every
	// other entry point in this module is a no-op and the game runs its normal mono path.
	//
	bool bEnabled();

	//******************************************************************************************
	//
	// Attempt OpenXR bring-up (M1): load the loader, create the instance, find the HMD system,
	// query D3D11 graphics requirements and the recommended per-eye render size.  Idempotent -
	// the first call does the work, later calls are no-ops.  Returns true if an OpenXR runtime
	// was found and a head-mounted system is available; false (cleanly, no crash) when VR is
	// disabled, no loader/runtime is installed, or no HMD is connected - the caller then just
	// runs the normal path.  Everything it does is logged via OutputDebugString.
	//
	bool bInit();

	//******************************************************************************************
	//
	// True once bInit has succeeded and an HMD system is available.  (M1 stops here; later
	// slices add a bSessionRunning() once the session/swapchains exist.)
	//
	bool bActive();

	//******************************************************************************************
	//
	// The runtime's recommended per-eye render target size, valid after a successful bInit.
	// Zero when VR is inactive.  Exposed so the renderer/camera work (M2/M3) can size the
	// per-eye targets and set each eye's aspect.
	//
	int  iRecommendedEyeWidth();
	int  iRecommendedEyeHeight();

	//******************************************************************************************
	//
	// Release all OpenXR objects and free the loader.  Safe to call when never initialised.
	//
	void Shutdown();
}

#endif // HEADER_LIB_VIEW_RENDERVR_HPP
