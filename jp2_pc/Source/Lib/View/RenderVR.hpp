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
	// STEREO (M3).
	//
	// Because the D3D11 path is handed geometry that is ALREADY projected to 2D screen space,
	// a second eye cannot come from a shader matrix - the 3D information is gone by then.  The
	// only way to get one is to run the engine's CPU pipeline again with the camera moved to
	// the other eye.  So the renderer loop asks iEyeCount() how many passes to make, calls
	// SetEye() before each, and everything downstream (the camera offset here, the render
	// target in RenderD3D11) keys off that.
	//
	// This is deliberately INDEPENDENT of bEnabled()/bInit(): stereo is driven by its own
	// TRESPASS_VR_STEREO environment variable so that the whole per-eye path can be developed
	// and verified on an ordinary desktop, side by side in the game window, with no headset and
	// no OpenXR runtime installed at all.  When the OpenXR session lands (M2/M4) the same
	// switch flips on automatically and the offsets below come from the runtime's real view
	// poses instead of the fixed IPD.
	//
	// TRESPASS_VR_STEREO may optionally be set to the desired interpupillary distance in
	// MILLIMETRES (e.g. "64"); unset or unparseable means the 63mm adult average.
	//
	bool bStereoActive();

	//******************************************************************************************
	//
	// Number of camera passes the renderer must make this frame: 2 when stereo is active, 1
	// otherwise.  A non-stereo build/run therefore takes exactly the path it always did.
	//
	int  iEyeCount();

	//******************************************************************************************
	//
	// Select the eye about to be rendered (0 = left, 1 = right).  Set by the render loop before
	// each camera pass; read by the camera offset and by the render backend when it decides
	// which target/viewport the pass lands in.
	//
	void SetEye(int i_eye);
	int  iEye();

	//******************************************************************************************
	//
	// Lateral offset of eye i_eye from the head centre, in world units along the camera's LOCAL
	// X (right) axis - negative for the left eye, positive for the right.  Trespasser's world
	// unit is the metre (its gravity constant is 9.8), so this is a true physical IPD/2 with no
	// scale conversion.  Zero when stereo is inactive.
	//
	float fEyeOffsetX(int i_eye);

	//******************************************************************************************
	//
	// Release all OpenXR objects and free the loader.  Safe to call when never initialised.
	//
	void Shutdown();
}

#endif // HEADER_LIB_VIEW_RENDERVR_HPP
