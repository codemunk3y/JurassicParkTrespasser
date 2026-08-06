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
	// HEAD TRACKING (M4).
	//
	// Where the runtime says this eye is, and what it can see from there, for the frame that
	// FrameBegin most recently picked up.
	//
	// EVERYTHING HERE IS ALREADY IN ENGINE AXES AND ENGINE UNITS - the OpenXR-to-Trespasser
	// conversion happens once, inside this module, rather than at each call site.  OpenXR is
	// X = right, Y = up, Z = backwards; the engine's camera space is X = right, Y = forward,
	// Z = up (Camera.cpp's view-normalising transform), so the map is (x, -z, y).  That is a
	// proper rotation, so it preserves handedness and applies unchanged to the quaternion's
	// vector part.  Trespasser's world unit is the metre, so positions need no scaling.
	//
	// The position is relative to the LOCAL reference space, whose origin is roughly where the
	// player's head was when the session started - so it is a small room-scale offset to add to
	// the game's own camera position, not an absolute world position.
	//
	// The pose is the EYE's, not the head's: it already includes that eye's half of the IPD.
	// Anything using it must NOT also apply fEyeOffsetX, or the eyes separate twice.
	//
	// Prefix: eyev
	//
	struct SEyeView
	{
		float af_pos[3];		// eye position, engine axes, metres, LOCAL space
		float f_rot_w;			// eye orientation as a quaternion, engine axes:
		float af_rot_v[3];		//   scalar part, then vector part

		// Tangents of the four half-angles of this eye's frustum.  Left and down are normally
		// negative: an HMD's frustum is ASYMMETRIC, which is why these are four numbers and not
		// one field of view.  Tangents rather than angles because that is the form the
		// projection maths wants, and computing them once here keeps tanf out of the frame loop.
		float f_tan_left, f_tan_right, f_tan_up, f_tan_down;
	};

	//******************************************************************************************
	//
	// Fill in where eye i_eye is for this frame.  Returns false - leaving eyev untouched - when
	// there is no tracked pose to give: VR inactive, no frame picked up yet, or the runtime
	// reporting its position or orientation as invalid (which it does legitimately, e.g. while
	// the headset is being put on).  A false return means "carry on with the flat camera", so
	// every caller needs a path that does not use head tracking at all.
	//
	bool bEyeView(int i_eye, SEyeView& eyev);

	//******************************************************************************************
	//
	// CONTROLLERS (input).
	//
	// Per-hand controller state for the frame FrameBegin most recently picked up.  Hand 0 = left,
	// 1 = right.  Like SEyeView, the pose is ALREADY in engine axes (X right, Y forward, Z up),
	// metres, and the same reference space + recentre + standing-height reconciliation as the eyes -
	// so it is directly comparable to the head/eye positions and can be added to the game camera the
	// same way.  The pose is the GRIP pose (palm), which is the natural one for holding objects.
	//
	// Analogue axes are in their OpenXR ranges: thumbstick components in [-1, 1] (Y is up-positive),
	// trigger and grip in [0, 1].  The b_* booleans are the digital buttons plus thresholded
	// trigger/grip, for convenience.
	//
	// Prefix: hs
	//
	struct SHandState
	{
		bool  b_active;			// controller present / its actions are live this frame
		bool  b_pose_valid;		// the grip pose was located this frame (af_pos/af_rot usable)
		float af_pos[3];		// grip position, engine axes, metres, reference space
		float f_rot_w;			// grip orientation quaternion, engine axes: scalar, then
		float af_rot_v[3];		//   vector part
		float af_stick[2];		// thumbstick x, y  in [-1, 1]
		float f_trigger;		// trigger pull      in [0, 1]
		float f_grip;			// grip/squeeze      in [0, 1]
		bool  b_trigger;		// trigger past half
		bool  b_grip;			// grip past half
		bool  b_a;				// primary button   (A / X / Vive+Simple menu)
		bool  b_b;				// secondary button (B / Y)
		bool  b_stick_click;	// thumbstick pressed in
	};

	//******************************************************************************************
	//
	// Fill in hand i_hand's state for this frame.  Returns false - leaving hs untouched - when VR
	// is inactive or the controllers are not attached yet.  A true return means the state is fresh;
	// callers that need the POSE must still check hs.b_pose_valid (a controller can be reporting
	// buttons and sticks while its 6-DoF pose is momentarily untracked).
	//
	bool bHandState(int i_hand, SHandState& hs);

	//******************************************************************************************
	//
	// Declare the frustum this eye was ACTUALLY rendered with, as tangents of its four half
	// angles, so that the projection layer describes the image being handed over rather than the
	// one the runtime suggested.
	//
	// This matters because the engine cannot render the runtime's frustum exactly.  An HMD's is
	// asymmetric (four different angles); Trespasser's camera is symmetric, so the closest it
	// can do is a symmetric frustum that CONTAINS the asymmetric one.  Submitting the runtime's
	// angles for an image drawn with different ones makes the compositor reproject it wrongly -
	// the world comes out at the wrong scale and skewed, which reads as bad tracking rather than
	// as a projection error.  Telling the truth here is what makes it correct; the cost of the
	// containing frustum is some overdraw at the edges, not accuracy.
	//
	// Call once per eye per frame, after the camera for that eye is built and before Present.
	// Not called = fall back to the runtime's own angles.
	//
	//******************************************************************************************
	//
	// Make whichever way the player is facing RIGHT NOW count as straight ahead.
	//
	// The LOCAL reference space anchors "forward" to wherever the headset pointed when the session
	// began, which for seated play is routinely nowhere near where the player's body faces - 90
	// degrees off in testing.  This removes that reference yaw from every pose bEyeView returns.
	//
	// Yaw only: recentring while looking up, or with the head tilted, must not leave the whole
	// world pitched or rolled.  No-op with a warning if there is no tracked pose yet.
	//
	void Recentre();

	//******************************************************************************************
	//
	// Re-arm the automatic recentre so it fires again on the next tracked frame.  Call this when a
	// level finishes loading (and the player is placed) so the player always starts facing forward
	// and at the game camera, instead of wherever the headset happened to point at session start.
	// No-op if VR is inactive.
	//
	void OnLevelLoad();

	void SetRenderedFov(int i_eye, float f_tan_left, float f_tan_right,
	                               float f_tan_up,   float f_tan_down);

	//******************************************************************************************
	//
	// SESSION + FRAME LOOP (M2).
	//
	// FrameBegin is called once at the top of each rendered frame, before the engine paints.
	// It lazily creates the session (which cannot happen until RenderD3D11 has a device - that
	// is built on the first frame, so this cannot be done in bInit), pumps the OpenXR event
	// queue and drives the session state machine, then, if a frame is ready, does xrBeginFrame
	// and locates the eye views for it.
	//
	// IT NEVER BLOCKS.  The blocking part of the OpenXR frame loop - xrWaitFrame, which is how a
	// VR app is paced to the headset's refresh rate - runs on a dedicated thread inside this
	// module, because the engine calls this from the WINDOWS MESSAGE-PUMP THREAD and a runtime
	// that stops issuing frames would otherwise stop the pump and hang the game.  When no frame
	// is ready this simply returns and the pass does not go to the headset.
	//
	void FrameBegin();

	//******************************************************************************************
	//
	// SubmitFrame is called at the end of the frame, BEFORE RenderD3D11::Present consumes it -
	// the eye images are drawn from the same accumulated geometry the desktop mirror uses, and
	// Present is what throws that away.
	//
	// It acquires each eye's swapchain image, renders that eye into it, releases it, and hands
	// the runtime a projection layer via xrEndFrame.  Every frame that began MUST be ended,
	// even one with nothing to draw, or the runtime's frame pipeline stalls - so this still
	// calls xrEndFrame (with no layers) when the session is not currently visible.
	//
	void SubmitFrame();

	//******************************************************************************************
	//
	// True once the session is running and frames are being submitted to the runtime.  This is
	// what makes stereo engage automatically when a headset session comes up, without the
	// TRESPASS_VR_STEREO override.
	//
	bool bSessionRunning();

	//******************************************************************************************
	//
	// Append a line to trespass_render.log (next to the exe) and to the debugger.
	//
	// Exposed because the engine's own dprintf is compiled to an empty inline in release builds
	// and OutputDebugString needs a debugger attached, so both of the obvious ways to leave a
	// diagnostic breadcrumb in this codebase are silently discarded in exactly the build being
	// tested.  This one always writes.
	//
	void LogLine(const char* psz);

	//******************************************************************************************
	//
	// Release all OpenXR objects and free the loader.  Safe to call when never initialised.
	//
	void Shutdown();
}

#endif // HEADER_LIB_VIEW_RENDERVR_HPP
