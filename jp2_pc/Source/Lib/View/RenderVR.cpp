/***********************************************************************************************
 *
 * Copyright (c) 2026.  Experimental modernization.
 *
 * Implementation of RenderVR.hpp.
 *
 * MILESTONE 1 (this file): OpenXR bring-up only.  Dynamically loads the OpenXR loader, creates
 * an XrInstance with the D3D11 graphics extension, finds the head-mounted system, and queries
 * its D3D11 graphics requirements (adapter LUID + minimum feature level) and the runtime's
 * recommended per-eye render size.  It renders nothing and creates no session yet - the point
 * is to prove the loader/header integration and to be verifiable headless against the Meta XR
 * Simulator or Monado (which report a system + view sizes without any hardware attached).
 *
 * The loader is bound at RUNTIME (LoadLibrary + xrGetInstanceProcAddr bootstrap), so there is
 * no link-time dependency on openxr_loader.lib: a box with no OpenXR runtime simply logs
 * "loader not found" and the game runs its normal path.
 *
 **********************************************************************************************/

#include <windows.h>
#include <d3d11.h>
#include <math.h>			// sqrtf/fabsf - the eye separation the runtime reports
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The OpenXR platform header exposes the D3D11 graphics-binding + requirements types only when
// these are defined ahead of it, and it needs <d3d11.h> (above) already included.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "RenderVR.hpp"
#include "RenderD3D11.hpp"

namespace
{
	//******************************************************************************************
	//
	// Log a line to trespass_render.log (next to the exe, since the launcher runs from there)
	// AND to the debugger.
	//
	// OutputDebugString alone is invisible unless a debugger or DebugView happens to be
	// attached at the time - which is exactly what you do not have when diagnosing a run after
	// the fact, and it made "did VR start?" unanswerable from a finished session.
	//
	// The file is opened and closed per line so that this module and the renderer can append to
	// the same log without sharing a handle, and so nothing is buffered away if the process is
	// killed rather than exited - the crash is usually the interesting part.  Log volume is a
	// few dozen lines per RUN (never per frame), so the cost does not matter.
	//
	// Always APPENDS: two modules write here, so whichever logged first would truncate the
	// other's lines away.  The launcher deletes the file before each run instead.
	//
	// The path is resolved from the EXE's location, not left relative: the engine calls
	// SetCurrentDirectory at startup to point at the install directory, so a relative name
	// silently writes the log into the game data folder instead of next to the exe where
	// anyone would look for it.
	void Log(const char* psz)
	{
		OutputDebugStringA(psz);

		static char s_ach_path[MAX_PATH] = { 0 };
		if (!s_ach_path[0])
		{
			GetModuleFileNameA(0, s_ach_path, MAX_PATH);
			char* psz_slash = strrchr(s_ach_path, '\\');
			if (psz_slash) strcpy(psz_slash + 1, "trespass_render.log");
			else           strcpy(s_ach_path, "trespass_render.log");
		}
		FILE* pf = fopen(s_ach_path, "a");
		if (pf) { fputs(psz, pf); fclose(pf); }
	}

	// ---- State --------------------------------------------------------------------------
	int         s_i_enabled   = -1;		// -1 = TRESPASS_VR not yet read
	bool        s_b_init_tried = false;
	bool        s_b_active    = false;

	HMODULE     s_h_loader    = 0;
	XrInstance  s_instance    = XR_NULL_HANDLE;
	XrSystemId  s_system      = XR_NULL_SYSTEM_ID;

	int         s_eye_w       = 0;
	int         s_eye_h       = 0;

	// ---- Stereo (M3) ---------------------------------------------------------------------
	int         s_i_stereo    = -1;		// -1 = TRESPASS_VR_STEREO not yet read
	int         s_i_eye       = 0;		// eye currently being rendered (0 = left, 1 = right)

	// Half the interpupillary distance, in metres, which is how far each eye sits from the head
	// centre along the camera's right axis.  Initialised to half of the 63mm adult average so the
	// eyes are ALWAYS separated by something plausible: with a headset session this is replaced
	// each frame by the runtime's own measurement (see UpdateEyeSeparation), but that cannot
	// happen until the first xrLocateViews, and a zero here means the two eyes render the
	// identical mono image - which looks like working stereo until you go looking for parallax.
	float       s_f_half_ipd  = 0.0315f;
	bool        s_b_ipd_from_env = false;	// TRESPASS_VR_STEREO gave an explicit IPD: don't override it
	float       s_f_ipd_logged   = 0.0f;	// last separation reported, to log changes and not frames

	// ---- Session / swapchains (M2) --------------------------------------------------------
	const int   k_i_eyes = 2;			// primary stereo view configuration

	XrSession     s_session       = XR_NULL_HANDLE;
	XrSpace       s_space         = XR_NULL_HANDLE;	// LOCAL reference space
	XrSessionState s_session_state = XR_SESSION_STATE_UNKNOWN;
	volatile bool s_b_session_running = false;		// between xrBeginSession and xrEndSession
	bool          s_b_frame_begun     = false;		// xrBeginFrame done, xrEndFrame owed
	bool          s_b_session_tried   = false;		// don't retry a failed session every frame
	bool          s_b_exit_requested  = false;
	bool          s_b_first_frame_logged = false;

	// ---- Frame-timing thread ---------------------------------------------------------------
	//
	// xrWaitFrame BLOCKS until the runtime wants the next frame - that is how a VR app is paced
	// to the headset's refresh rate.  This engine paints from the WINDOWS MESSAGE-PUMP THREAD,
	// so calling it there stops the pump too, and a runtime that stops issuing frames (the user
	// alt-tabbing to the simulator, the headset going idle, the runtime dying) took the whole
	// game down with it: Windows declares an unpumped process hung after ~5s (AppHangB1), the
	// player sees a freeze, and the one enormous frame delta on resume gets integrated by the
	// mouse look into a violent spin.  A watchdog used to notice this AFTER the fact and tear VR
	// down; that cured the hang by giving up VR entirely.
	//
	// The fix is structural: xrWaitFrame - and ONLY xrWaitFrame - runs on this dedicated thread.
	// The pump thread then picks up the frame it produced without ever blocking, so a stalled
	// runtime costs nothing but VR frames, and the game keeps painting flat until it recovers.
	//
	// EVERYTHING ELSE STAYS ON THE PUMP THREAD - xrBeginFrame/xrEndFrame, the swapchain calls,
	// and all rendering.  That is deliberate: those touch the D3D11 immediate context, which is
	// not thread-safe, and OpenXR explicitly permits xrWaitFrame to be called from a different
	// thread than the begin/end pair.  Moving one call is the whole change.
	//
	// Exactly one xrWaitFrame is in flight at a time and each one feeds exactly one xrBeginFrame,
	// so the runtime still sees the strict Wait -> Begin -> End sequence it saw single-threaded.
	// s_l_want is the request ("go get the next frame"), raised after each xrEndFrame; the event
	// is only a wake-up hint, so a missed signal costs at most one poll interval, never a
	// deadlock.
	HANDLE           s_h_wait_thread = 0;
	HANDLE           s_h_want_frame  = 0;		// auto-reset wake-up hint for the worker
	CRITICAL_SECTION s_cs_frame;				// guards s_frame_pending / s_b_frame_pending
	bool             s_b_cs_ready    = false;
	volatile LONG    s_l_want        = 0;		// 1 = the worker should wait for another frame
	volatile LONG    s_l_quit        = 0;		// 1 = the worker should exit
	XrFrameState     s_frame_pending;			// written by the worker, read by the pump thread
	bool             s_b_frame_pending = false;	// a waited-for frame is ready to be begun

	// How long xrWaitFrame may block before the runtime is worth complaining about.  A healthy
	// 90Hz runtime returns in ~11ms.  Neither threshold is fatal any more - nothing is hanging
	// while we wait - so these only decide what gets logged.
	const ULONGLONG k_u_stall_ms = 2000;
	const ULONGLONG k_u_slow_ms  = 250;		// merely noteworthy

	XrFrameState  s_frame_state;
	XrView        s_a_views[k_i_eyes];
	XrViewConfigurationView s_a_view_cfg[k_i_eyes];

	// RECENTRE (2026-07-25).  The LOCAL reference space puts "forward" wherever the headset was
	// pointing when the session began, which for seated play is often nowhere near where the
	// player's body faces - measured 90 degrees off in testing, which made the head-vs-body
	// experiments unreadable.  This is the inverse of the reference YAW, applied to every pose in
	// bEyeView.  Stored as (x, y, z, w) in OPENXR axes, identity until Recentre() is called.
	float s_a_recentre[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

	// POSE ORIGIN (2026-08-06).  The head pose the runtime gives is relative to the reference space
	// origin, which is NOT where the game wants the eye: with LOCAL it is the session-start head, with
	// LOCAL_FLOOR the point on the floor under it, and either way the player spawns rotated (measured
	// 90 degrees) and offset from the game camera - "behind the player model".  Recentre captures the
	// current head position AND yaw as the neutral origin; bEyeView then reports every pose RELATIVE to
	// it (subtract this position, then remove the yaw).  So standing at the neutral pose puts the eye
	// exactly at the game camera, physical movement offsets from there, and a crouch lowers the view
	// by the real amount.  Stored raw, in OpenXR axes/metres.  Captured automatically on the first
	// tracked frame after a level load (see s_b_auto_recentred) so the player never spawns off-centre.
	float s_a_ref_pos[3] = { 0.0f, 0.0f, 0.0f };
	bool  s_b_auto_recentred = false;			// has the automatic on-load recentre fired yet

	// A small upward view lift baked into the neutral origin, so the on-load eye height sits a touch
	// above the game camera (the player found ~15 cm the comfortable "sweet spot").  Applied by
	// lowering the captured reference height, which raises every reported eye/hand up-offset equally,
	// keeping hand-eye alignment.  Metres.
	const float k_f_view_lift = 0.15f;

	// Which reference space the runtime actually gave us, for the log and to explain the origin.
	XrReferenceSpaceType s_ref_space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;

	// ---- Controllers / actions --------------------------------------------------------------
	const int   k_i_hands = 2;					// 0 = left, 1 = right
	XrActionSet s_action_set   = XR_NULL_HANDLE;
	XrAction    s_act_pose     = XR_NULL_HANDLE;	// grip pose (per-hand via subaction path)
	XrAction    s_act_stick    = XR_NULL_HANDLE;	// thumbstick (vector2)
	XrAction    s_act_trigger  = XR_NULL_HANDLE;	// trigger (float)
	XrAction    s_act_grip     = XR_NULL_HANDLE;	// squeeze/grip (float)
	XrAction    s_act_a        = XR_NULL_HANDLE;	// primary button (bool)
	XrAction    s_act_b        = XR_NULL_HANDLE;	// secondary button (bool)
	XrAction    s_act_stickclk = XR_NULL_HANDLE;	// thumbstick click (bool)
	XrPath      s_path_hand[2] = { XR_NULL_PATH, XR_NULL_PATH };		// /user/hand/left, /right
	XrSpace     s_space_hand[2]= { XR_NULL_HANDLE, XR_NULL_HANDLE };	// grip pose spaces
	bool        s_b_actions_created  = false;
	bool        s_b_actions_attached = false;

	// SHandState lives in namespace RenderVR (the public header); these statics are in this file's
	// anonymous namespace, so it must be named with its full scope here.
	RenderVR::SHandState s_hand[2] = {};			// cached, refreshed by SyncInput each focused frame

	// Quaternion product a*b, both (x,y,z,w) in OpenXR axes - standard right-handed convention,
	// NOT the engine's reversed "a then b" operator*.
	XrQuaternionf qMul(const float* a, const XrQuaternionf& b)
	{
		XrQuaternionf r;
		r.w = a[3]*b.w - a[0]*b.x - a[1]*b.y - a[2]*b.z;
		r.x = a[3]*b.x + a[0]*b.w + a[1]*b.z - a[2]*b.y;
		r.y = a[3]*b.y - a[0]*b.z + a[1]*b.w + a[2]*b.x;
		r.z = a[3]*b.z + a[0]*b.y - a[1]*b.x + a[2]*b.w;
		return r;
	}

	// Rotate v by quaternion q (x,y,z,w): v + 2w(q x v) + 2(q x (q x v)).
	XrVector3f v3Rotate(const float* q, const XrVector3f& v)
	{
		const float t_x = 2.0f * (q[1]*v.z - q[2]*v.y);
		const float t_y = 2.0f * (q[2]*v.x - q[0]*v.z);
		const float t_z = 2.0f * (q[0]*v.y - q[1]*v.x);

		XrVector3f r;
		r.x = v.x + q[3]*t_x + (q[1]*t_z - q[2]*t_y);
		r.y = v.y + q[3]*t_y + (q[2]*t_x - q[0]*t_z);
		r.z = v.z + q[3]*t_z + (q[0]*t_y - q[1]*t_x);
		return r;
	}

	// The frustum each eye was really rendered with this frame (tangents), and whether the
	// renderer has declared one.  Reset every frame: a stale frustum from a frame that is no
	// longer on screen would be submitted as if it described the current image.
	float s_aa_f_rendered_tan[k_i_eyes][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
	bool  s_ab_rendered_fov[k_i_eyes]      = { false, false };

	// Whether the poses in s_a_views are actually tracked this frame.  The runtime reports
	// position and orientation validity separately and either can drop out on its own (a headset
	// being put on, tracking lost, a runtime that only ever orients), so both are kept: a view
	// with a valid orientation and a stale position is still worth rendering with.
	XrViewStateFlags s_view_state_flags = 0;
	bool             s_b_views_located  = false;

	XrSwapchain   s_a_swapchain[k_i_eyes]   = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrSwapchainImageD3D11KHR* s_apa_images[k_i_eyes] = { 0, 0 };
	uint32_t      s_a_image_count[k_i_eyes] = { 0, 0 };

	// The one function fetched from the DLL by name; every other entry point comes from it.
	PFN_xrGetInstanceProcAddr           s_xrGetInstanceProcAddr = 0;

	// Instance-independent (queried with a null instance).
	PFN_xrEnumerateInstanceExtensionProperties s_xrEnumerateInstanceExtensionProperties = 0;
	PFN_xrCreateInstance                s_xrCreateInstance = 0;

	// Instance-level.
	PFN_xrDestroyInstance               s_xrDestroyInstance = 0;
	PFN_xrGetInstanceProperties         s_xrGetInstanceProperties = 0;
	PFN_xrGetSystem                     s_xrGetSystem = 0;
	PFN_xrGetSystemProperties           s_xrGetSystemProperties = 0;
	PFN_xrEnumerateViewConfigurationViews s_xrEnumerateViewConfigurationViews = 0;
	PFN_xrGetD3D11GraphicsRequirementsKHR s_xrGetD3D11GraphicsRequirementsKHR = 0;

	// Session-level (M2).
	PFN_xrCreateSession                 s_xrCreateSession = 0;
	PFN_xrDestroySession                s_xrDestroySession = 0;
	PFN_xrBeginSession                  s_xrBeginSession = 0;
	PFN_xrEndSession                    s_xrEndSession = 0;
	PFN_xrRequestExitSession            s_xrRequestExitSession = 0;
	PFN_xrPollEvent                     s_xrPollEvent = 0;
	PFN_xrCreateReferenceSpace          s_xrCreateReferenceSpace = 0;
	PFN_xrEnumerateReferenceSpaces      s_xrEnumerateReferenceSpaces = 0;
	PFN_xrDestroySpace                  s_xrDestroySpace = 0;
	PFN_xrEnumerateSwapchainFormats     s_xrEnumerateSwapchainFormats = 0;
	PFN_xrCreateSwapchain               s_xrCreateSwapchain = 0;
	PFN_xrDestroySwapchain              s_xrDestroySwapchain = 0;
	PFN_xrEnumerateSwapchainImages      s_xrEnumerateSwapchainImages = 0;
	PFN_xrAcquireSwapchainImage         s_xrAcquireSwapchainImage = 0;
	PFN_xrWaitSwapchainImage            s_xrWaitSwapchainImage = 0;
	PFN_xrReleaseSwapchainImage         s_xrReleaseSwapchainImage = 0;
	PFN_xrWaitFrame                     s_xrWaitFrame = 0;
	PFN_xrBeginFrame                    s_xrBeginFrame = 0;
	PFN_xrEndFrame                      s_xrEndFrame = 0;
	PFN_xrLocateViews                   s_xrLocateViews = 0;

	// Input / actions (controllers).
	PFN_xrStringToPath                  s_xrStringToPath = 0;
	PFN_xrCreateActionSet               s_xrCreateActionSet = 0;
	PFN_xrDestroyActionSet              s_xrDestroyActionSet = 0;
	PFN_xrCreateAction                  s_xrCreateAction = 0;
	PFN_xrDestroyAction                 s_xrDestroyAction = 0;
	PFN_xrSuggestInteractionProfileBindings s_xrSuggestInteractionProfileBindings = 0;
	PFN_xrAttachSessionActionSets       s_xrAttachSessionActionSets = 0;
	PFN_xrSyncActions                   s_xrSyncActions = 0;
	PFN_xrGetActionStateFloat           s_xrGetActionStateFloat = 0;
	PFN_xrGetActionStateBoolean         s_xrGetActionStateBoolean = 0;
	PFN_xrGetActionStateVector2f        s_xrGetActionStateVector2f = 0;
	PFN_xrGetActionStatePose            s_xrGetActionStatePose = 0;
	PFN_xrCreateActionSpace             s_xrCreateActionSpace = 0;
	PFN_xrLocateSpace                   s_xrLocateSpace = 0;

	// Fetch an OpenXR function pointer from the instance (or null instance for the bootstrap
	// three).  Logs and returns false on failure so bInit can bail cleanly.
	bool bLoad(XrInstance xr, const char* psz_name, PFN_xrVoidFunction* ppfn)
	{
		XrResult r = s_xrGetInstanceProcAddr(xr, psz_name, ppfn);
		if (XR_FAILED(r) || !*ppfn)
		{
			char buf[128];
			sprintf(buf, "TRESPASS_VR: xrGetInstanceProcAddr(%s) failed (%d)\n", psz_name, (int)r);
			Log(buf);
			return false;
		}
		return true;
	}
	#define LOAD(xr, name) bLoad((xr), #name, (PFN_xrVoidFunction*)&s_##name)

	//******************************************************************************************
	//
	// Ask the frame-timing thread for another frame.  Raised once when the thread starts and
	// again after every xrEndFrame, so there is never more than one xrWaitFrame outstanding.
	//
	void RequestFrame()
	{
		InterlockedExchange(&s_l_want, 1);
		if (s_h_want_frame) SetEvent(s_h_want_frame);
	}

	//******************************************************************************************
	//
	// The frame-timing thread: block in xrWaitFrame, hand the result to the pump thread, repeat.
	//
	DWORD WINAPI dwFrameWaitThread(LPVOID)
	{
		// A 100ms cap on the wake-up wait rather than an infinite one: the session can stop or
		// the process can quit while no frame is being asked for, and both need noticing without
		// depending on someone remembering to signal the event.
		for (;;)
		{
			WaitForSingleObject(s_h_want_frame, 100);

			if (InterlockedCompareExchange(&s_l_quit, 0, 0)) break;
			if (!s_b_session_running)                        continue;

			// Consume the request.  If none is outstanding the pump thread has not finished with
			// the last frame yet, so there is nothing to do.
			if (!InterlockedCompareExchange(&s_l_want, 0, 1)) continue;

			XrFrameState fs; memset(&fs, 0, sizeof(fs));
			fs.type = XR_TYPE_FRAME_STATE;
			XrFrameWaitInfo fwi; memset(&fwi, 0, sizeof(fwi));
			fwi.type = XR_TYPE_FRAME_WAIT_INFO;

			const ULONGLONG u_before = GetTickCount64();
			const XrResult  r_wait   = s_xrWaitFrame(s_session, &fwi, &fs);
			const ULONGLONG u_waited = GetTickCount64() - u_before;

			if (XR_FAILED(r_wait))
			{
				// Typically the session stopping underneath us.  Re-arm and let the loop settle;
				// the state check at the top is what actually stops the spinning.
				InterlockedExchange(&s_l_want, 1);
				Sleep(10);
				continue;
			}

			// A stall is now survivable, so it is reported rather than acted on - the pump thread
			// is not waiting for this and the game carries on flat.  Logged once per episode
			// because a runtime that stalls usually keeps stalling and would otherwise bury the
			// rest of the log.
			{
				static bool s_b_stalled = false;
				if (u_waited > k_u_stall_ms)
				{
					if (!s_b_stalled)
					{
						s_b_stalled = true;
						char buf[192];
						sprintf(buf, "TRESPASS_VR: xrWaitFrame blocked %llu ms - runtime stalled; the game "
						             "keeps running flat and VR resumes if it recovers\n",
							(unsigned long long)u_waited);
						Log(buf);
					}
				}
				else
				{
					if (s_b_stalled)
					{
						s_b_stalled = false;
						Log("TRESPASS_VR: runtime recovered - frames flowing again\n");
					}
					if (u_waited > k_u_slow_ms)
					{
						char buf[160];
						sprintf(buf, "TRESPASS_VR: slow frame - xrWaitFrame blocked %llu ms\n",
							(unsigned long long)u_waited);
						Log(buf);
					}
				}
			}

			EnterCriticalSection(&s_cs_frame);
			s_frame_pending   = fs;
			s_b_frame_pending = true;
			LeaveCriticalSection(&s_cs_frame);
		}
		return 0;
	}

	//******************************************************************************************
	//
	// Start the frame-timing thread.  Called once, after the session and swapchains exist.
	//
	bool bStartFrameThread()
	{
		if (s_h_wait_thread) return true;

		if (!s_b_cs_ready) { InitializeCriticalSection(&s_cs_frame); s_b_cs_ready = true; }
		memset(&s_frame_pending, 0, sizeof(s_frame_pending));
		s_b_frame_pending = false;
		InterlockedExchange(&s_l_quit, 0);

		if (!s_h_want_frame)
			s_h_want_frame = CreateEventA(0, FALSE, FALSE, 0);	// auto-reset, initially clear
		if (!s_h_want_frame) { Log("TRESPASS_VR: CreateEvent failed - VR frame loop cannot start\n"); return false; }

		s_h_wait_thread = CreateThread(0, 0, dwFrameWaitThread, 0, 0, 0);
		if (!s_h_wait_thread) { Log("TRESPASS_VR: CreateThread failed - VR frame loop cannot start\n"); return false; }

		RequestFrame();
		Log("TRESPASS_VR: frame-timing thread started (xrWaitFrame is off the message-pump thread)\n");
		return true;
	}

	//******************************************************************************************
	//
	// Stop the frame-timing thread.  Returns false if it could not be stopped, which means it is
	// still inside xrWaitFrame on a runtime that has stopped answering - in that case the caller
	// must NOT destroy the OpenXR objects that call is using.
	//
	bool bStopFrameThread()
	{
		if (!s_h_wait_thread) return true;

		InterlockedExchange(&s_l_quit, 1);
		SetEvent(s_h_want_frame);

		// Long enough to cover any healthy wait (one frame at any plausible refresh rate) plus a
		// stalled runtime's usual recovery; short enough not to hang the shutdown that this whole
		// change exists to prevent.
		if (WaitForSingleObject(s_h_wait_thread, 3000) != WAIT_OBJECT_0)
		{
			Log("TRESPASS_VR: frame-timing thread is still blocked in xrWaitFrame - leaking the "
			    "OpenXR objects rather than destroying ones it is using\n");
			return false;
		}

		CloseHandle(s_h_wait_thread);
		s_h_wait_thread = 0;
		return true;
	}

	//******************************************************************************************
	//
	// Create the session, its reference space and the per-eye swapchains.
	//
	// This cannot happen in bInit: xrCreateSession needs the D3D11 device, and the renderer
	// creates that lazily on the first painted frame.  So it is attempted once per frame until
	// the device exists, then once for real - and never retried after a genuine failure, since
	// retrying a rejected session every frame would spam the log and stall the frame loop.
	//
	bool bTryCreateSession()
	{
		if (s_session != XR_NULL_HANDLE) return true;
		if (s_b_session_tried)           return false;

		ID3D11Device* p_device = (ID3D11Device*)RenderD3D11::pGetDevice();
		if (!p_device)
		{
			// Renderer has no device yet - try again next frame.  Say so ONCE, because a log
			// that stops after bring-up otherwise looks identical to a hang here, and this is
			// the state a run gets stuck in if the D3D11 backend never starts (no TRESPASS_D3D11,
			// or its init failed) - in which case the session is never created and VR is silent.
			static bool s_b_said = false;
			if (!s_b_said) { s_b_said = true; Log("TRESPASS_VR: waiting for the D3D11 device before creating the session\n"); }
			return false;
		}

		s_b_session_tried = true;

		XrGraphicsBindingD3D11KHR gb; memset(&gb, 0, sizeof(gb));
		gb.type   = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR;
		gb.device = p_device;

		XrSessionCreateInfo sci; memset(&sci, 0, sizeof(sci));
		sci.type     = XR_TYPE_SESSION_CREATE_INFO;
		sci.next     = &gb;
		sci.systemId = s_system;

		XrResult r = s_xrCreateSession(s_instance, &sci, &s_session);
		if (XR_FAILED(r))
		{
			char buf[96];
			sprintf(buf, "TRESPASS_VR: xrCreateSession failed (%d)\n", (int)r);
			Log(buf);
			s_session = XR_NULL_HANDLE;
			return false;
		}

		// Reference space: prefer LOCAL_FLOOR so the player's real floor and standing height come
		// from the SteamVR room calibration (this is the "standing" experience the player asked for).
		// LOCAL_FLOOR is like LOCAL - origin under where the headset started, forward = the start
		// facing, so it does NOT wander the origin to the room centre the way STAGE does - but with
		// the height pinned to the real floor.  The head's resting height is then reconciled against
		// the game camera in bEyeView (see s_a_ref_pos), so it doesn't lift the whole view.
		//
		// LOCAL_FLOOR is core in OpenXR 1.1 and an extension in 1.0; we request api 1.0, so rather
		// than assume it exists, enumerate what the runtime offers and pick the best available:
		// LOCAL_FLOOR, else plain LOCAL (the previous seated behaviour, still correct - the height
		// reconciliation just becomes a no-op).  STAGE is deliberately NOT chosen: its origin is the
		// room centre, which for a camera-driven game puts the view metres from the in-game body.
		XrReferenceSpaceType e_want = XR_REFERENCE_SPACE_TYPE_LOCAL;
		{
			uint32_t u_sp_count = 0;
			if (XR_SUCCEEDED(s_xrEnumerateReferenceSpaces(s_session, 0, &u_sp_count, 0)) && u_sp_count)
			{
				XrReferenceSpaceType a_sp[16];
				if (u_sp_count > 16) u_sp_count = 16;
				if (XR_SUCCEEDED(s_xrEnumerateReferenceSpaces(s_session, u_sp_count, &u_sp_count, a_sp)))
				{
					for (uint32_t i = 0; i < u_sp_count; ++i)
						if (a_sp[i] == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR)
							{ e_want = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR; break; }
				}
			}
		}

		XrReferenceSpaceCreateInfo rsci; memset(&rsci, 0, sizeof(rsci));
		rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
		rsci.referenceSpaceType = e_want;
		rsci.poseInReferenceSpace.orientation.w = 1.0f;		// identity
		if (XR_FAILED(s_xrCreateReferenceSpace(s_session, &rsci, &s_space)))
		{
			// Fall back to LOCAL if the preferred type was refused despite being listed.
			if (e_want != XR_REFERENCE_SPACE_TYPE_LOCAL)
			{
				e_want = XR_REFERENCE_SPACE_TYPE_LOCAL;
				rsci.referenceSpaceType = e_want;
			}
			if (XR_FAILED(s_xrCreateReferenceSpace(s_session, &rsci, &s_space)))
			{
				Log("TRESPASS_VR: xrCreateReferenceSpace failed\n");
				s_space = XR_NULL_HANDLE;
				return false;
			}
		}
		s_ref_space_type = e_want;
		{
			char buf[128];
			sprintf(buf, "TRESPASS_VR: reference space = %s\n",
				e_want == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR ? "LOCAL_FLOOR (standing, real floor)"
				                                              : "LOCAL (seated - LOCAL_FLOOR unavailable)");
			Log(buf);
		}
		// (Action-set attach happens in FrameBegin, just after this succeeds - AttachActions lives in
		// namespace RenderVR and cannot be named from this anonymous-namespace function.)

		// Pick a swapchain format.  The runtime lists what it supports in ITS order of
		// preference, but we need one the renderer can actually draw into, so intersect that
		// list with ours rather than taking its first entry blindly.
		int64_t i_format = 0;
		{
			uint32_t u_fmt_count = 0;
			s_xrEnumerateSwapchainFormats(s_session, 0, &u_fmt_count, 0);
			if (u_fmt_count)
			{
				int64_t* pa_fmt = new int64_t[u_fmt_count];
				if (XR_SUCCEEDED(s_xrEnumerateSwapchainFormats(s_session, u_fmt_count, &u_fmt_count, pa_fmt)))
				{
					const int64_t a_want[] = {
						DXGI_FORMAT_R8G8B8A8_UNORM,
						DXGI_FORMAT_B8G8R8A8_UNORM,
						DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
						DXGI_FORMAT_B8G8R8A8_UNORM_SRGB };
					for (int i_w = 0; i_w < 4 && !i_format; ++i_w)
						for (uint32_t i_f = 0; i_f < u_fmt_count; ++i_f)
							if (pa_fmt[i_f] == a_want[i_w]) { i_format = a_want[i_w]; break; }
					if (!i_format) i_format = pa_fmt[0];		// nothing we know - take theirs
				}
				delete[] pa_fmt;
			}
		}
		if (!i_format)
		{
			Log("TRESPASS_VR: no swapchain formats offered\n");
			return false;
		}

		for (int i_e = 0; i_e < k_i_eyes; ++i_e)
		{
			XrSwapchainCreateInfo scci; memset(&scci, 0, sizeof(scci));
			scci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
			scci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			scci.format      = i_format;
			scci.sampleCount = 1;			// the engine does not multisample
			scci.width       = s_a_view_cfg[i_e].recommendedImageRectWidth;
			scci.height      = s_a_view_cfg[i_e].recommendedImageRectHeight;
			scci.faceCount   = 1;
			scci.arraySize   = 1;
			scci.mipCount    = 1;

			if (XR_FAILED(s_xrCreateSwapchain(s_session, &scci, &s_a_swapchain[i_e])))
			{
				Log("TRESPASS_VR: xrCreateSwapchain failed\n");
				s_a_swapchain[i_e] = XR_NULL_HANDLE;
				return false;
			}

			// The runtime owns these images and decides how many it wants to cycle through.
			uint32_t u_n = 0;
			s_xrEnumerateSwapchainImages(s_a_swapchain[i_e], 0, &u_n, 0);
			if (!u_n) { Log("TRESPASS_VR: swapchain has no images\n"); return false; }

			s_apa_images[i_e] = new XrSwapchainImageD3D11KHR[u_n];
			for (uint32_t i_img = 0; i_img < u_n; ++i_img)
			{
				s_apa_images[i_e][i_img].type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR;
				s_apa_images[i_e][i_img].next = 0;
			}
			if (XR_FAILED(s_xrEnumerateSwapchainImages(s_a_swapchain[i_e], u_n, &u_n,
				(XrSwapchainImageBaseHeader*)s_apa_images[i_e])))
			{
				Log("TRESPASS_VR: xrEnumerateSwapchainImages failed\n");
				return false;
			}
			s_a_image_count[i_e] = u_n;
		}

		char buf[160];
		sprintf(buf, "TRESPASS_VR: session + swapchains created (%ux%u per eye, %u/%u images, format %d)\n",
			s_a_view_cfg[0].recommendedImageRectWidth, s_a_view_cfg[0].recommendedImageRectHeight,
			s_a_image_count[0], s_a_image_count[1], (int)i_format);
		Log(buf);

		if (!bStartFrameThread())
		{
			// Without the timing thread nothing ever waits on a frame, so VR would sit there
			// looking initialised in the log while submitting nothing at all.  Give up openly.
			s_b_exit_requested = true;
			return false;
		}
		return true;
	}

	//******************************************************************************************
	//
	// Drain the OpenXR event queue and drive the session state machine.
	//
	// The runtime, not the application, decides when a session may start rendering and when it
	// must stop (the user removing the headset, another app taking focus, the runtime shutting
	// down).  Ignoring these events is the classic way to get a VR app that hangs on exit or
	// renders into a session the runtime has already torn down.
	//
	void PollEvents()
	{
		for (;;)
		{
			XrEventDataBuffer ev; memset(&ev, 0, sizeof(ev));
			ev.type = XR_TYPE_EVENT_DATA_BUFFER;
			if (s_xrPollEvent(s_instance, &ev) != XR_SUCCESS)
				break;

			switch (ev.type)
			{
				case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
				{
					const XrEventDataSessionStateChanged* p_ss =
						(const XrEventDataSessionStateChanged*)&ev;
					s_session_state = p_ss->state;

					char buf[96];
					sprintf(buf, "TRESPASS_VR: session state -> %d\n", (int)s_session_state);
					Log(buf);

					if (s_session_state == XR_SESSION_STATE_READY && !s_b_session_running)
					{
						XrSessionBeginInfo sbi; memset(&sbi, 0, sizeof(sbi));
						sbi.type = XR_TYPE_SESSION_BEGIN_INFO;
						sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
						if (XR_SUCCEEDED(s_xrBeginSession(s_session, &sbi)))
						{
							s_b_session_running = true;
							Log("TRESPASS_VR: session begun - submitting frames\n");
						}
						else
							Log("TRESPASS_VR: xrBeginSession failed\n");
					}
					else if (s_session_state == XR_SESSION_STATE_STOPPING && s_b_session_running)
					{
						s_xrEndSession(s_session);
						s_b_session_running = false;
						s_b_frame_begun     = false;
						Log("TRESPASS_VR: session stopped\n");
					}
					else if (s_session_state == XR_SESSION_STATE_EXITING ||
					         s_session_state == XR_SESSION_STATE_LOSS_PENDING)
					{
						s_b_session_running = false;
						s_b_frame_begun     = false;
						s_b_exit_requested  = true;
					}
					break;
				}

				case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
					// The runtime is going away (restarted, or shut down under us).  Drop
					// everything; the game carries on without VR rather than dying with it.
					Log("TRESPASS_VR: instance loss pending - shutting VR down\n");
					s_b_exit_requested = true;
					break;

				default:
					break;
			}
		}
	}

	//******************************************************************************************
	//
	// Take the eye separation from the runtime's own located views.
	//
	// The two view poses ARE the eyes: their positions are where the runtime says each pupil is,
	// so the distance between them is the real interpupillary distance of the device (or of the
	// user, on a headset with a physical IPD adjustment - which is why this is re-read every
	// frame rather than latched once: turning that wheel mid-session changes it).
	//
	// Only the MAGNITUDE is used, not the direction.  The poses are in reference space and so
	// carry the head's rotation; the engine applies the offset along the camera's own right axis
	// (RenderDB.cpp), which already accounts for where the head is pointing.  Taking the vector
	// as well would apply that rotation twice.  Feeding the pose itself into the camera is M4.
	//
	void UpdateEyeSeparation(const XrViewState& vs, uint32_t u_views)
	{
		// An explicit TRESPASS_VR_STEREO is a deliberate instruction (comfort, experiment,
		// exaggerated parallax for a screenshot) and must not be quietly overwritten.
		if (s_b_ipd_from_env || u_views < 2) return;

		if (!(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) return;

		const float f_dx = s_a_views[1].pose.position.x - s_a_views[0].pose.position.x;
		const float f_dy = s_a_views[1].pose.position.y - s_a_views[0].pose.position.y;
		const float f_dz = s_a_views[1].pose.position.z - s_a_views[0].pose.position.z;
		const float f_ipd = sqrtf(f_dx * f_dx + f_dy * f_dy + f_dz * f_dz);

		// Human IPDs run about 52-78mm; headsets clamp to roughly that.  A value outside it means
		// the runtime is reporting something we do not understand (a simulator with the eyes
		// collapsed, metres-vs-centimetres confusion), and the 63mm default is a better guess
		// than an obviously wrong measurement - a too-large separation is actively unpleasant to
		// look at, and a zero silently disables stereo altogether.
		if (f_ipd < 0.040f || f_ipd > 0.090f)
		{
			if (s_f_ipd_logged != -1.0f)
			{
				s_f_ipd_logged = -1.0f;		// sentinel: implausible, reported once
				char buf[192];
				sprintf(buf, "TRESPASS_VR: runtime reports an implausible eye separation (%.1f mm) - "
				             "using the %.0f mm default instead\n",
					f_ipd * 1000.0f, s_f_half_ipd * 2000.0f);
				Log(buf);
			}
			return;
		}

		s_f_half_ipd = f_ipd * 0.5f;

		// Logged on first sight and whenever it moves by more than a millimetre, so an IPD
		// adjustment during play is visible in the log without a line every frame.
		if (s_f_ipd_logged < 0.0f || fabsf(f_ipd - s_f_ipd_logged) > 0.001f)
		{
			s_f_ipd_logged = f_ipd;
			char buf[160];
			sprintf(buf, "TRESPASS_VR: eye separation from runtime = %.1f mm (half-offset %.4f world units)\n",
				f_ipd * 1000.0f, s_f_half_ipd);
			Log(buf);
		}
	}

	void Shutdown_Internal()
	{
		// The frame-timing thread has to be gone BEFORE anything it touches is destroyed - it
		// holds s_session inside a blocking xrWaitFrame.  If it will not come back (a runtime that
		// has stopped answering, which is exactly the case that gets us here) then destroying the
		// session under it would turn a survivable stall into a crash, so instead we abandon the
		// OpenXR objects and let process exit reclaim them.  Leaking on the way out is the cheap
		// half of that trade; the game itself carries on flat either way.
		// Only speak up if there is actually something to tear down: this runs on every quit,
		// including runs where VR was never enabled, and a log line there would create a
		// trespass_render.log for a plain flat run that never used any of this.
		const bool b_anything = (s_instance != XR_NULL_HANDLE) || s_b_active || s_h_wait_thread;
		if (b_anything)
			Log("TRESPASS_VR: shutting down - stopping the frame thread, then the session\n");

		const bool b_thread_stopped = bStopFrameThread();

		// Taken before the flags are cleared - xrEndSession below still needs to know whether a
		// session was ever begun.
		const bool b_was_running = s_b_session_running;

		s_b_frame_begun     = false;
		s_b_session_running = false;
		s_b_active          = false;

		// The render targets cached against the swapchain images are now dangling - the runtime
		// may hand the same addresses back for different textures - so they go either way.
		RenderD3D11::PurgeEyeTargets();

		if (!b_thread_stopped)
			return;

		// Tear down inside-out: swapchains and space belong to the session, the session belongs
		// to the instance.  Destroying the instance first orphans the rest.
		if (b_was_running && s_xrEndSession)
			s_xrEndSession(s_session);

		for (int i_e = 0; i_e < k_i_eyes; ++i_e)
		{
			if (s_a_swapchain[i_e] != XR_NULL_HANDLE && s_xrDestroySwapchain)
				s_xrDestroySwapchain(s_a_swapchain[i_e]);
			s_a_swapchain[i_e] = XR_NULL_HANDLE;
			delete[] s_apa_images[i_e];
			s_apa_images[i_e]   = 0;
			s_a_image_count[i_e] = 0;
		}

		if (s_space != XR_NULL_HANDLE && s_xrDestroySpace)
			s_xrDestroySpace(s_space);
		s_space = XR_NULL_HANDLE;

		if (s_session != XR_NULL_HANDLE && s_xrDestroySession)
			s_xrDestroySession(s_session);
		s_session = XR_NULL_HANDLE;

		if (s_instance != XR_NULL_HANDLE && s_xrDestroyInstance)
			s_xrDestroyInstance(s_instance);
		s_instance = XR_NULL_HANDLE;
		s_system   = XR_NULL_SYSTEM_ID;
		if (s_h_loader) { FreeLibrary(s_h_loader); s_h_loader = 0; }

		// Only safe on this path: the early return above leaves these alone precisely because the
		// worker that uses them is still alive.
		if (s_h_want_frame) { CloseHandle(s_h_want_frame); s_h_want_frame = 0; }
		if (s_b_cs_ready)   { DeleteCriticalSection(&s_cs_frame); s_b_cs_ready = false; }

		if (b_anything)
			Log("TRESPASS_VR: shutdown complete - OpenXR released cleanly\n");
	}
}

namespace RenderVR
{
	bool bEnabled()
	{
		if (s_i_enabled < 0)
			s_i_enabled = GetEnvironmentVariableA("TRESPASS_VR", 0, 0) > 0 ? 1 : 0;
		return s_i_enabled != 0;
	}

	bool bActive() { return s_b_active; }

	// Forward declarations - defined further down this namespace but called from FrameBegin above them.
	void CreateActions();
	void AttachActions();
	void SyncInput();

	int  iRecommendedEyeWidth()  { return s_eye_w; }
	int  iRecommendedEyeHeight() { return s_eye_h; }

	bool bStereoActive()
	{
		if (s_i_stereo < 0)
		{
			char ach[32] = { 0 };
			int  i_len = GetEnvironmentVariableA("TRESPASS_VR_STEREO", ach, sizeof(ach));
			s_i_stereo = i_len > 0 ? 1 : 0;
			if (s_i_stereo)
			{
				// Optional value = interpupillary distance in millimetres.  Anything outside a
				// plausible human range (40..80mm) is treated as "not specified" rather than
				// obeyed - a typo there would otherwise produce a wildly wrong stereo effect
				// that looks like a bug in the projection rather than a bad input.
				int i_ipd_mm = atoi(ach);
				if (i_ipd_mm < 40 || i_ipd_mm > 80)
					i_ipd_mm = 63;			// adult average
				s_f_half_ipd     = (float)i_ipd_mm * 0.0005f;	// mm -> metres, halved
				s_b_ipd_from_env = true;	// an explicit request outranks the runtime's measurement

				char buf[128];
				sprintf(buf, "TRESPASS_VR: stereo ON, IPD %dmm (half-offset %.4f world units)\n",
					i_ipd_mm, s_f_half_ipd);
				Log(buf);
			}
		}
		// A live headset session implies stereo whether or not the override was set - the eyes
		// are what the runtime is being handed.  The env var stays as the way to get the same
		// per-eye path with no runtime at all, for desktop work.
		if (!s_i_stereo && !s_b_session_running)
			return false;

		// Stereo needs the D3D11 backend.  It is the only path that can keep the two eyes
		// apart: it tags each submitted batch with its eye and gives each one its own viewport.
		// The software rasteriser has a single screen raster, so a second pass would simply
		// overpaint the first and the result would be one eye's view with the other's smeared
		// through it.  Checked live rather than cached - the device is created lazily on the
		// first frame, so an early call must not latch stereo off for the whole run.
		return RenderD3D11::bActive();
	}

	bool bSessionRunning() { return s_b_session_running; }

	void LogLine(const char* psz) { Log(psz); }

	void FrameBegin()
	{
		if (!bEnabled() || !s_b_active || s_b_exit_requested) return;

		if (!bTryCreateSession()) { PollEvents(); return; }
		PollEvents();

		// The runtime has asked us to go away (session exiting, or the instance being lost).  Tear
		// down here, on the pump thread, rather than leaving VR half-alive: the stereo eye loop
		// keys off the session still being "running" and would go on paying for two camera passes
		// per frame for a runtime that is no longer listening.
		if (s_b_exit_requested) { Shutdown_Internal(); return; }

		if (!s_b_session_running) return;

		// Controllers: attach the action set + create the hand pose spaces now the session exists.
		// Idempotent (guards on s_b_actions_attached), so calling it every frame is fine; it only
		// does work once, and must happen before the first SyncInput below.
		AttachActions();

		// A frame begun on a previous pass that was never submitted must be closed before a new
		// one can start - xrBeginFrame/xrEndFrame have to pair exactly.  This happens whenever
		// the engine paints something that is not a 3D scene (the menu, a UI-only frame): this
		// function runs on EVERY painted frame, but SubmitFrame only runs when the renderer
		// actually produced a frame, so the two are not called the same number of times.
		//
		// Without this the very first menu frame strands s_b_frame_begun at true and every
		// later call returns here - the session stays alive and keeps reporting state changes,
		// but not one further frame is ever waited on or submitted, and VR is silently dead
		// while looking healthy in the log.
		if (s_b_frame_begun)
			SubmitFrame();

		// Collect the frame the timing thread has been waiting on.  THIS NEVER BLOCKS - that is
		// the entire point of the thread (see its definition above).  No frame ready means the
		// runtime has not released the next one yet, so this painting pass simply does not go to
		// the headset; the engine still draws its normal desktop frame and the next pass picks up
		// where this one left off.  A stalled runtime therefore costs VR frames and nothing else,
		// where it used to freeze the message pump and hang the game.
		bool b_have_frame = false;
		EnterCriticalSection(&s_cs_frame);
		if (s_b_frame_pending)
		{
			s_frame_state     = s_frame_pending;
			s_b_frame_pending = false;
			b_have_frame      = true;
		}
		LeaveCriticalSection(&s_cs_frame);

		if (!b_have_frame) return;

		// One line the first time a frame actually flows, so the log distinguishes "the loop
		// never started" from "the loop ran and then something else went wrong".
		if (!s_b_first_frame_logged)
		{
			s_b_first_frame_logged = true;
			char buf[160];
			sprintf(buf, "TRESPASS_VR: frame loop running (shouldRender=%d)\n",
				(int)s_frame_state.shouldRender);
			Log(buf);
		}

		// Frame RATE, reported periodically.  The timing thread's wait time only says how long the
		// runtime made US wait - it says nothing about how long we took, and a renderer that has
		// just gone from 640x480 mono to two 1680x1760 eyes can be slow enough to break the
		// simulation without the runtime ever looking slow.  That matters beyond smoothness:
		// the engine integrates player rotation against frame delta, so a collapsed frame rate
		// turns a small mouse movement into an uncontrollable spin (pitch is clamped to +/-90
		// and still looks fine, which is what makes the symptom so misleading).
		//
		// Counted here rather than on the timing thread because this is where a frame is actually
		// TAKEN UP by the renderer - frames the runtime released while the engine was busy are
		// dropped, and this number is meant to show that.
		{
			static ULONGLONG s_u_window_start = 0;
			static int       s_i_frames       = 0;
			if (!s_u_window_start) s_u_window_start = GetTickCount64();
			if (++s_i_frames >= 100)
			{
				const ULONGLONG u_elapsed = GetTickCount64() - s_u_window_start;
				char buf[192];
				sprintf(buf, "TRESPASS_VR: %d frames in %llu ms = %.1f fps (%.1f ms/frame)\n",
					s_i_frames, (unsigned long long)u_elapsed,
					u_elapsed ? (1000.0 * s_i_frames / (double)u_elapsed) : 0.0,
					s_i_frames ? (double)u_elapsed / s_i_frames : 0.0);
				Log(buf);
				s_i_frames = 0;
				s_u_window_start = GetTickCount64();
			}
		}

		XrFrameBeginInfo fbi; memset(&fbi, 0, sizeof(fbi));
		fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
		if (XR_FAILED(s_xrBeginFrame(s_session, &fbi)))
		{
			// The frame just taken from the timing thread dies here, and only xrEndFrame re-arms
			// the request - so without this the worker would never be asked for another frame and
			// VR would go quiet for the rest of the run.
			RequestFrame();
			return;
		}
		s_b_frame_begun = true;

		// Locate the eyes for THIS frame's predicted display time.  M2 does not yet drive the
		// engine camera ORIENTATION from these (that is M4) - but the poses and field of view are
		// needed for the projection layer so the runtime can reproject correctly, and the
		// separation between the two positions is where the stereo IPD comes from.
		for (int i_e = 0; i_e < k_i_eyes; ++i_e)
		{
			memset(&s_a_views[i_e], 0, sizeof(s_a_views[i_e]));
			s_a_views[i_e].type = XR_TYPE_VIEW;
		}
		XrViewLocateInfo vli; memset(&vli, 0, sizeof(vli));
		vli.type                  = XR_TYPE_VIEW_LOCATE_INFO;
		vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		vli.displayTime           = s_frame_state.predictedDisplayTime;
		vli.space                 = s_space;

		XrViewState vs; memset(&vs, 0, sizeof(vs));
		vs.type = XR_TYPE_VIEW_STATE;
		uint32_t u_got = 0;
		s_b_views_located = false;
		s_ab_rendered_fov[0] = s_ab_rendered_fov[1] = false;
		if (XR_SUCCEEDED(s_xrLocateViews(s_session, &vli, &vs, k_i_eyes, &u_got, s_a_views)))
		{
			s_view_state_flags = vs.viewStateFlags;
			s_b_views_located  = (u_got >= (uint32_t)k_i_eyes);
			UpdateEyeSeparation(vs, u_got);
		}

		// AUTO-RECENTRE ON LOAD.  The reference space anchors "forward" and "here" to wherever the
		// headset was when the space was created, so the player otherwise spawns rotated (~90 degrees
		// in testing) and offset from the game camera.  The first frame with a genuinely tracked pose
		// after a level load, snap the neutral origin to it so the player starts facing forward, at
		// the camera.  OnLevelLoad() re-arms this for the next level.  The manual Recentre (Ctrl+R)
		// still works to re-zero at any time.
		if (!s_b_auto_recentred && s_b_views_located &&
		    (s_view_state_flags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
		    (s_view_state_flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
		{
			Recentre();
			s_b_auto_recentred = true;
		}

		// Refresh controller state for this frame (sticks, buttons, hand poses).  Cheap; guards
		// internally on being attached + focused.  Uses this frame's predicted display time for the
		// hand pose location, so it must run after the frame is picked up.
		SyncInput();
	}

	//******************************************************************************************
	//
	// Report a failure in the per-frame submit path - ONCE per call site, because anything wrong
	// here is wrong every frame and would otherwise bury the log at 45 lines a second.
	//
	// These calls all used to have their results discarded, which made the one failure mode that
	// matters invisible: every frame "succeeds", the desktop mirror looks perfect, the log shows
	// frames flowing, and the headset shows nothing at all.
	//
	void LogSubmitFailure(const char* psz_what, int i_eye, int i_result)
	{
		static bool s_ab_logged[8] = { false, false, false, false, false, false, false, false };

		// Key on the call site's name pointer, which is a literal, so each site reports once.
		static const char* s_apsz_seen[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
		int i_slot = -1;
		for (int i = 0; i < 8; ++i)
		{
			if (s_apsz_seen[i] == psz_what) { i_slot = i; break; }
			if (!s_apsz_seen[i])            { s_apsz_seen[i] = psz_what; i_slot = i; break; }
		}
		if (i_slot < 0 || s_ab_logged[i_slot]) return;
		s_ab_logged[i_slot] = true;

		char buf[224];
		if (i_result)
			sprintf(buf, "TRESPASS_VR: %s FAILED for eye %d (result %d) - the headset will show\n"
			             "TRESPASS_VR: nothing even though frames are being submitted\n",
				psz_what, i_eye, i_result);
		else
			sprintf(buf, "TRESPASS_VR: %s FAILED for eye %d - the eye image was never drawn, so the\n"
			             "TRESPASS_VR: headset shows whatever was left in the swapchain (usually black)\n",
				psz_what, i_eye);
		Log(buf);
	}

	void SubmitFrame()
	{
		if (!s_b_frame_begun) return;
		s_b_frame_begun = false;		// every begun frame is ended exactly once, below

		XrCompositionLayerProjectionView a_proj_views[k_i_eyes];
		XrCompositionLayerProjection     layer;
		memset(a_proj_views, 0, sizeof(a_proj_views));
		memset(&layer, 0, sizeof(layer));

		bool b_rendered = false;

		// shouldRender is false when the session is running but not visible (headset off the
		// head, another app in front).  The frame must still be ended - just with nothing in it.
		//
		// The renderer also has to HAVE a frame: this is reached on UI-only frames too (see the
		// pairing note in FrameBegin), and there is no geometry to put in an eye image then.
		if (s_frame_state.shouldRender && RenderD3D11::bFrameOpen())
		{
			b_rendered = true;
			for (int i_e = 0; i_e < k_i_eyes && b_rendered; ++i_e)
			{
				uint32_t u_index = 0;
				XrSwapchainImageAcquireInfo ai; memset(&ai, 0, sizeof(ai));
				ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
				XrResult r_acq = s_xrAcquireSwapchainImage(s_a_swapchain[i_e], &ai, &u_index);
				if (XR_FAILED(r_acq))
				{ LogSubmitFailure("xrAcquireSwapchainImage", i_e, (int)r_acq); b_rendered = false; break; }

				XrSwapchainImageWaitInfo wi; memset(&wi, 0, sizeof(wi));
				wi.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
				wi.timeout = XR_INFINITE_DURATION;
				XrResult r_wait_img = s_xrWaitSwapchainImage(s_a_swapchain[i_e], &wi);
				if (XR_FAILED(r_wait_img))
				{ LogSubmitFailure("xrWaitSwapchainImage", i_e, (int)r_wait_img); b_rendered = false; break; }

				const int i_w = (int)s_a_view_cfg[i_e].recommendedImageRectWidth;
				const int i_h = (int)s_a_view_cfg[i_e].recommendedImageRectHeight;

				// The RESULT MATTERS: if this fails, the eye image is whatever the runtime last
				// left in that swapchain slot, and it gets submitted as though it were a rendered
				// frame.  The compositor then has nothing to show and the headset stays dark
				// while every log line still says frames are flowing - which is exactly the state
				// that produced "the mirror moves but the headset is black".
				if (!RenderD3D11::bRenderEyeToTexture(i_e, s_apa_images[i_e][u_index].texture, i_w, i_h))
					LogSubmitFailure("RenderD3D11::bRenderEyeToTexture", i_e, 0);

				// Release even if the render failed - an acquired image that is never released
				// deadlocks the swapchain on the next acquire.
				XrSwapchainImageReleaseInfo ri; memset(&ri, 0, sizeof(ri));
				ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
				s_xrReleaseSwapchainImage(s_a_swapchain[i_e], &ri);

				a_proj_views[i_e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
				a_proj_views[i_e].pose = s_a_views[i_e].pose;

				// Describe the image being handed over, not the one that was suggested - see
				// SetRenderedFov.  Only the renderer knows what it managed to draw.
				if (s_ab_rendered_fov[i_e])
				{
					a_proj_views[i_e].fov.angleLeft  = atanf(s_aa_f_rendered_tan[i_e][0]);
					a_proj_views[i_e].fov.angleRight = atanf(s_aa_f_rendered_tan[i_e][1]);
					a_proj_views[i_e].fov.angleUp    = atanf(s_aa_f_rendered_tan[i_e][2]);
					a_proj_views[i_e].fov.angleDown  = atanf(s_aa_f_rendered_tan[i_e][3]);
				}
				else
				{
					a_proj_views[i_e].fov = s_a_views[i_e].fov;
				}
				a_proj_views[i_e].subImage.swapchain            = s_a_swapchain[i_e];
				a_proj_views[i_e].subImage.imageArrayIndex      = 0;
				a_proj_views[i_e].subImage.imageRect.offset.x   = 0;
				a_proj_views[i_e].subImage.imageRect.offset.y   = 0;
				a_proj_views[i_e].subImage.imageRect.extent.width  = i_w;
				a_proj_views[i_e].subImage.imageRect.extent.height = i_h;
			}
		}

		layer.type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
		layer.space     = s_space;
		layer.viewCount = k_i_eyes;
		layer.views     = a_proj_views;

		const XrCompositionLayerBaseHeader* ap_layers[1] =
			{ (const XrCompositionLayerBaseHeader*)&layer };

		XrFrameEndInfo fei; memset(&fei, 0, sizeof(fei));
		fei.type                 = XR_TYPE_FRAME_END_INFO;
		fei.displayTime          = s_frame_state.predictedDisplayTime;
		fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		fei.layerCount           = b_rendered ? 1 : 0;
		fei.layers               = b_rendered ? ap_layers : 0;

		// A rejected layer is silent otherwise: the runtime simply shows nothing, and every other
		// signal (frames counted, session FOCUSED, mirror correct) says the frame was fine.
		XrResult r_end = s_xrEndFrame(s_session, &fei);
		if (XR_FAILED(r_end))
			LogSubmitFailure("xrEndFrame", -1, (int)r_end);
		else if (b_rendered)
		{
			// Positive confirmation that a frame with a layer was accepted - once.  Its ABSENCE
			// from the log is then just as informative as its presence.
			static bool s_b_first = false;
			if (!s_b_first)
			{
				s_b_first = true;
				Log("TRESPASS_VR: first projection layer accepted by the runtime\n");
			}
		}

		// This frame is finished, so the timing thread may go and wait for the next one.  Asking
		// only here is what keeps exactly one xrWaitFrame in flight and preserves the strict
		// Wait -> Begin -> End order the runtime saw when all three ran on one thread.
		RequestFrame();
	}

	int  iEyeCount() { return bStereoActive() ? 2 : 1; }

	void SetEye(int i_eye) { s_i_eye = (i_eye == 1) ? 1 : 0; }
	int  iEye()            { return s_i_eye; }

	float fEyeOffsetX(int i_eye)
	{
		if (!bStereoActive()) return 0.0f;
		return (i_eye == 1) ? s_f_half_ipd : -s_f_half_ipd;
	}

	bool bEyeView(int i_eye, SEyeView& eyev)
	{
		if (!s_b_session_running || !s_b_views_located) return false;
		if (i_eye < 0 || i_eye >= k_i_eyes)             return false;

		// Both bits, not either: a pose with a valid orientation but an unknown position would
		// otherwise be used with a garbage position, which is far worse than not tracking at all
		// - the view would be somewhere random in the level rather than merely not turning.
		const XrViewStateFlags k_needed =
			XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
		if ((s_view_state_flags & k_needed) != k_needed) return false;

		const XrView& view = s_a_views[i_eye];

		// RECENTRE.  Remove the reference yaw captured by Recentre(), so that whichever way the
		// player was facing then becomes "straight ahead" and lines up with the body.  Done HERE,
		// in OpenXR space, before the axis remap below: the maths is unambiguous in the frame the
		// runtime gave us, and every consumer of bEyeView gets the corrected pose for free.
		//
		// Yaw only, so recentring while looking up or with the head tilted does not leave the
		// whole world pitched or rolled.
		// Re-origin to the neutral pose captured by Recentre: subtract the reference POSITION, then
		// remove the reference YAW.  This order makes the neutral pose map to (0,0,0) with zero yaw,
		// so the eye sits at the game camera there; physical movement/crouch then offsets from it.
		XrQuaternionf q_or = view.pose.orientation;
		XrVector3f    v_ps = view.pose.position;
		v_ps.x -= s_a_ref_pos[0];
		v_ps.y -= s_a_ref_pos[1];
		v_ps.z -= s_a_ref_pos[2];

		if (s_a_recentre[3] != 1.0f || s_a_recentre[1] != 0.0f)
		{
			q_or = qMul(s_a_recentre, q_or);
			v_ps = v3Rotate(s_a_recentre, v_ps);
		}

		// OpenXR (X right, Y up, Z back) -> engine (X right, Y forward, Z up) is (x, -z, y).
		eyev.af_pos[0]   =  v_ps.x;
		eyev.af_pos[1]   = -v_ps.z;
		eyev.af_pos[2]   =  v_ps.y;

		// The same remap applies to the quaternion's vector part with the scalar untouched:
		// the map is a proper rotation (determinant +1), so it carries the rotation into the
		// new frame without flipping its sense.  Both conventions are right-handed - the engine's
		// "clockwise looking along the axis" (Rotate.hpp) is the right-hand rule stated from the
		// other end, and its rotation matrix (Rotate.cpp) is the standard one.
		eyev.f_rot_w     =  q_or.w;
		eyev.af_rot_v[0] =  q_or.x;
		eyev.af_rot_v[1] = -q_or.z;
		eyev.af_rot_v[2] =  q_or.y;

		eyev.f_tan_left  = tanf(view.fov.angleLeft);		// normally negative

		eyev.f_tan_right = tanf(view.fov.angleRight);
		eyev.f_tan_up    = tanf(view.fov.angleUp);
		eyev.f_tan_down  = tanf(view.fov.angleDown);	// normally negative

		return true;
	}

	void Recentre()
	{
		if (!s_b_session_running || !s_b_views_located)
		{
			Log("TRESPASS_VR: recentre ignored - no tracked pose yet\n");
			return;
		}

		// Twist-about-up of the current head orientation: in OpenXR, up is +Y, so the yaw part of
		// (w,x,y,z) is (w,0,y,0) renormalised.  Store its INVERSE - conjugate, which for a unit
		// quaternion just negates the vector part - so that composing it in bEyeView cancels the
		// yaw the player is facing right now.
		const XrQuaternionf& q = s_a_views[0].pose.orientation;

		float f_len = sqrtf(q.w * q.w + q.y * q.y);
		if (f_len < 1e-6f)
		{
			// Looking straight up or straight down - no yaw to extract.  Leave it alone rather
			// than snapping the world to something arbitrary.
			Log("TRESPASS_VR: recentre ignored - head has no measurable yaw (looking vertically?)\n");
			return;
		}

		s_a_recentre[0] = 0.0f;
		s_a_recentre[1] = -q.y / f_len;		// conjugate: negate the vector part
		s_a_recentre[2] = 0.0f;
		s_a_recentre[3] =  q.w / f_len;

		// Capture the current head POSITION as the neutral origin too - recentring means "treat my
		// current comfortable pose as here", which is position (including height) as well as facing.
		// bEyeView / the hand poses subtract this before removing the yaw.
		s_a_ref_pos[0] = s_a_views[0].pose.position.x;
		s_a_ref_pos[1] = s_a_views[0].pose.position.y - k_f_view_lift;	// lift the neutral eye height
		s_a_ref_pos[2] = s_a_views[0].pose.position.z;

		char buf[192];
		sprintf(buf, "TRESPASS_VR: recentred - yaw removed (inv yaw y=%.4f w=%.4f), origin=(%.2f,%.2f,%.2f)\n",
			s_a_recentre[1], s_a_recentre[3], s_a_ref_pos[0], s_a_ref_pos[1], s_a_ref_pos[2]);
		Log(buf);
	}

	void OnLevelLoad()
	{
		// Just re-arm; the actual recentre happens on the next tracked frame in FrameBegin, when a
		// valid head pose is available (the pose is not ready at the instant a level finishes loading).
		s_b_auto_recentred = false;
		Log("TRESPASS_VR: level load - will recentre on the next tracked frame\n");
	}

	void SetRenderedFov(int i_eye, float f_tan_left, float f_tan_right,
	                               float f_tan_up,   float f_tan_down)
	{
		if (i_eye < 0 || i_eye >= k_i_eyes) return;

		s_aa_f_rendered_tan[i_eye][0] = f_tan_left;
		s_aa_f_rendered_tan[i_eye][1] = f_tan_right;
		s_aa_f_rendered_tan[i_eye][2] = f_tan_up;
		s_aa_f_rendered_tan[i_eye][3] = f_tan_down;
		s_ab_rendered_fov[i_eye]      = true;
	}

	//******************************************************************************************
	//
	// CONTROLLERS.  OpenXR's action system: declare abstract actions (a trigger pull, a grip pose),
	// suggest how each maps to real controller inputs for the profiles we know, and let the runtime
	// bind whatever hardware is present.  All of this needs only the instance except the action
	// SPACES and the attach, which need the session (done in AttachActions from bTryCreateSession).
	//

	XrPath xrPath(const char* psz)
	{
		XrPath p = XR_NULL_PATH;
		if (s_xrStringToPath) s_xrStringToPath(s_instance, psz, &p);
		return p;
	}

	// Create one action, bound to BOTH hands via the two subaction paths.
	XrAction MakeAction(const char* psz_name, const char* psz_loc, XrActionType e_type)
	{
		XrActionCreateInfo aci; memset(&aci, 0, sizeof(aci));
		aci.type = XR_TYPE_ACTION_CREATE_INFO;
		strncpy(aci.actionName, psz_name, sizeof(aci.actionName) - 1);
		strncpy(aci.localizedActionName, psz_loc, sizeof(aci.localizedActionName) - 1);
		aci.actionType          = e_type;
		aci.countSubactionPaths = 2;
		aci.subactionPaths      = s_path_hand;
		XrAction a = XR_NULL_HANDLE;
		XrResult r = s_xrCreateAction(s_action_set, &aci, &a);
		if (XR_FAILED(r)) { char b[96]; sprintf(b, "TRESPASS_VR: create action '%s' failed (%d)\n", psz_name, (int)r); Log(b); }
		return a;
	}

	// A growable list of suggested bindings for one interaction profile.
	struct SBindList { XrActionSuggestedBinding a[64]; uint32_t n; };

	void Bind(SBindList& bl, XrAction act, const char* psz_path)
	{
		if (act == XR_NULL_HANDLE || bl.n >= 64) return;
		XrPath p = xrPath(psz_path);
		if (p == XR_NULL_PATH) return;
		bl.a[bl.n].action  = act;
		bl.a[bl.n].binding = p;
		++bl.n;
	}

	void Suggest(const char* psz_profile, SBindList& bl)
	{
		if (!bl.n) return;
		XrInteractionProfileSuggestedBinding s; memset(&s, 0, sizeof(s));
		s.type                   = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
		s.interactionProfile     = xrPath(psz_profile);
		s.suggestedBindings      = bl.a;
		s.countSuggestedBindings = bl.n;
		XrResult r = s_xrSuggestInteractionProfileBindings(s_instance, &s);
		char b[128]; sprintf(b, "TRESPASS_VR: suggest %s -> %d (%u bindings)\n", psz_profile, (int)r, bl.n);
		Log(b);
	}

	// The common analogue-stick controller layout (Index / Oculus Touch), parameterised only where
	// they differ: the squeeze axis path and the two face-button paths per hand.
	void BindStickController(const char* psz_profile,
	                         const char* psz_squeeze,
	                         const char* psz_l_primary, const char* psz_l_secondary,
	                         const char* psz_r_primary, const char* psz_r_secondary)
	{
		SBindList bl; bl.n = 0;
		Bind(bl, s_act_pose,    "/user/hand/left/input/grip/pose");
		Bind(bl, s_act_pose,    "/user/hand/right/input/grip/pose");
		Bind(bl, s_act_stick,   "/user/hand/left/input/thumbstick");
		Bind(bl, s_act_stick,   "/user/hand/right/input/thumbstick");
		Bind(bl, s_act_stickclk,"/user/hand/left/input/thumbstick/click");
		Bind(bl, s_act_stickclk,"/user/hand/right/input/thumbstick/click");
		Bind(bl, s_act_trigger, "/user/hand/left/input/trigger/value");
		Bind(bl, s_act_trigger, "/user/hand/right/input/trigger/value");
		char l_sq[64], r_sq[64];
		sprintf(l_sq, "/user/hand/left/input/%s",  psz_squeeze);
		sprintf(r_sq, "/user/hand/right/input/%s", psz_squeeze);
		Bind(bl, s_act_grip, l_sq);
		Bind(bl, s_act_grip, r_sq);
		Bind(bl, s_act_a, psz_l_primary);
		Bind(bl, s_act_b, psz_l_secondary);
		Bind(bl, s_act_a, psz_r_primary);
		Bind(bl, s_act_b, psz_r_secondary);
		Suggest(psz_profile, bl);
	}

	void CreateActions()
	{
		if (s_b_actions_created) return;
		if (!s_xrCreateActionSet) return;

		XrActionSetCreateInfo asci; memset(&asci, 0, sizeof(asci));
		asci.type = XR_TYPE_ACTION_SET_CREATE_INFO;
		strcpy(asci.actionSetName, "gameplay");
		strcpy(asci.localizedActionSetName, "Gameplay");
		asci.priority = 0;
		XrResult r = s_xrCreateActionSet(s_instance, &asci, &s_action_set);
		if (XR_FAILED(r)) { char b[96]; sprintf(b, "TRESPASS_VR: xrCreateActionSet failed (%d)\n", (int)r); Log(b); return; }

		s_path_hand[0] = xrPath("/user/hand/left");
		s_path_hand[1] = xrPath("/user/hand/right");

		s_act_pose     = MakeAction("hand_pose",  "Hand pose",        XR_ACTION_TYPE_POSE_INPUT);
		s_act_stick    = MakeAction("stick",      "Thumbstick",       XR_ACTION_TYPE_VECTOR2F_INPUT);
		s_act_trigger  = MakeAction("trigger",    "Trigger",          XR_ACTION_TYPE_FLOAT_INPUT);
		s_act_grip     = MakeAction("grip",       "Grip",             XR_ACTION_TYPE_FLOAT_INPUT);
		s_act_a        = MakeAction("primary",    "Primary button",   XR_ACTION_TYPE_BOOLEAN_INPUT);
		s_act_b        = MakeAction("secondary",  "Secondary button", XR_ACTION_TYPE_BOOLEAN_INPUT);
		s_act_stickclk = MakeAction("stick_click","Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);

		// Valve Index ("knuckles") - the user's lighthouse controllers are most likely these.
		BindStickController("/interaction_profiles/valve/index_controller",
			"squeeze/value",
			"/user/hand/left/input/a/click",  "/user/hand/left/input/b/click",
			"/user/hand/right/input/a/click", "/user/hand/right/input/b/click");

		// Oculus Touch - left has X/Y, right has A/B.
		BindStickController("/interaction_profiles/oculus/touch_controller",
			"squeeze/value",
			"/user/hand/left/input/x/click",  "/user/hand/left/input/y/click",
			"/user/hand/right/input/a/click", "/user/hand/right/input/b/click");

		// Khronos simple controller - the guaranteed fallback.  Only a pose and two clicks; no
		// analogue axes, so trigger/stick/grip stay zero on it.  Map select+menu to the buttons and
		// route select to the trigger boolean too, so "pull to act" still works.
		{
			SBindList bl; bl.n = 0;
			Bind(bl, s_act_pose, "/user/hand/left/input/grip/pose");
			Bind(bl, s_act_pose, "/user/hand/right/input/grip/pose");
			Bind(bl, s_act_a,    "/user/hand/left/input/menu/click");
			Bind(bl, s_act_a,    "/user/hand/right/input/menu/click");
			Bind(bl, s_act_b,    "/user/hand/left/input/select/click");
			Bind(bl, s_act_b,    "/user/hand/right/input/select/click");
			Suggest("/interaction_profiles/khr/simple_controller", bl);
		}

		s_b_actions_created = true;
		Log("TRESPASS_VR: input actions created\n");
	}

	void AttachActions()
	{
		if (s_b_actions_attached || s_action_set == XR_NULL_HANDLE || s_session == XR_NULL_HANDLE)
			return;

		// A pose action needs an action SPACE to be locatable; one per hand, at the grip pose.
		for (int h = 0; h < k_i_hands; ++h)
		{
			XrActionSpaceCreateInfo aspci; memset(&aspci, 0, sizeof(aspci));
			aspci.type                       = XR_TYPE_ACTION_SPACE_CREATE_INFO;
			aspci.action                     = s_act_pose;
			aspci.subactionPath              = s_path_hand[h];
			aspci.poseInActionSpace.orientation.w = 1.0f;
			if (XR_FAILED(s_xrCreateActionSpace(s_session, &aspci, &s_space_hand[h])))
				Log("TRESPASS_VR: xrCreateActionSpace(hand) failed\n");
		}

		XrSessionActionSetsAttachInfo ai; memset(&ai, 0, sizeof(ai));
		ai.type            = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
		ai.countActionSets = 1;
		ai.actionSets      = &s_action_set;
		XrResult r = s_xrAttachSessionActionSets(s_session, &ai);
		char b[96]; sprintf(b, "TRESPASS_VR: attach action sets -> %d\n", (int)r); Log(b);
		s_b_actions_attached = XR_SUCCEEDED(r);
	}

	// Small readers - each returns 0/false when the input is not active on this controller.
	float GetFloat(XrAction act, XrPath hand)
	{
		XrActionStateFloat st; memset(&st, 0, sizeof(st)); st.type = XR_TYPE_ACTION_STATE_FLOAT;
		XrActionStateGetInfo gi; memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
		gi.action = act; gi.subactionPath = hand;
		if (act != XR_NULL_HANDLE && XR_SUCCEEDED(s_xrGetActionStateFloat(s_session, &gi, &st)) && st.isActive)
			return st.currentState;
		return 0.0f;
	}
	bool GetBool(XrAction act, XrPath hand, bool* pb_active = 0)
	{
		XrActionStateBoolean st; memset(&st, 0, sizeof(st)); st.type = XR_TYPE_ACTION_STATE_BOOLEAN;
		XrActionStateGetInfo gi; memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
		gi.action = act; gi.subactionPath = hand;
		if (act != XR_NULL_HANDLE && XR_SUCCEEDED(s_xrGetActionStateBoolean(s_session, &gi, &st)))
		{
			if (pb_active && st.isActive) *pb_active = true;
			return st.isActive && st.currentState;
		}
		return false;
	}

	void SyncInput()
	{
		if (!s_b_actions_attached) return;
		if (s_session_state != XR_SESSION_STATE_FOCUSED) return;	// input only flows when focused

		XrActiveActionSet aas; memset(&aas, 0, sizeof(aas));
		aas.actionSet    = s_action_set;
		aas.subactionPath = XR_NULL_PATH;
		XrActionsSyncInfo si; memset(&si, 0, sizeof(si));
		si.type                  = XR_TYPE_ACTIONS_SYNC_INFO;
		si.countActiveActionSets = 1;
		si.activeActionSets      = &aas;
		if (XR_FAILED(s_xrSyncActions(s_session, &si))) return;

		for (int h = 0; h < k_i_hands; ++h)
		{
			SHandState hs; memset(&hs, 0, sizeof(hs));
			XrPath hand = s_path_hand[h];
			bool b_any = false;

			XrActionStateVector2f v; memset(&v, 0, sizeof(v)); v.type = XR_TYPE_ACTION_STATE_VECTOR2F;
			XrActionStateGetInfo gi; memset(&gi, 0, sizeof(gi)); gi.type = XR_TYPE_ACTION_STATE_GET_INFO;
			gi.action = s_act_stick; gi.subactionPath = hand;
			if (XR_SUCCEEDED(s_xrGetActionStateVector2f(s_session, &gi, &v)) && v.isActive)
			{ hs.af_stick[0] = v.currentState.x; hs.af_stick[1] = v.currentState.y; b_any = true; }

			hs.f_trigger = GetFloat(s_act_trigger, hand);
			hs.f_grip    = GetFloat(s_act_grip,    hand);
			hs.b_trigger = hs.f_trigger > 0.5f;
			hs.b_grip    = hs.f_grip    > 0.5f;
			hs.b_a           = GetBool(s_act_a,        hand, &b_any);
			hs.b_b           = GetBool(s_act_b,        hand, &b_any);
			hs.b_stick_click = GetBool(s_act_stickclk, hand, &b_any);
			if (hs.f_trigger != 0.0f || hs.f_grip != 0.0f) b_any = true;

			// Grip pose, located in the reference space at this frame's display time, then put
			// through the SAME recentre + axis remap + standing-height reconciliation as the eyes,
			// so a hand position is directly comparable to the head/eye positions.
			if (s_space_hand[h] != XR_NULL_HANDLE)
			{
				XrActionStatePose ps; memset(&ps, 0, sizeof(ps)); ps.type = XR_TYPE_ACTION_STATE_POSE;
				gi.action = s_act_pose; gi.subactionPath = hand;
				if (XR_SUCCEEDED(s_xrGetActionStatePose(s_session, &gi, &ps)) && ps.isActive)
				{
					b_any = true;
					XrSpaceLocation sl; memset(&sl, 0, sizeof(sl)); sl.type = XR_TYPE_SPACE_LOCATION;
					if (XR_SUCCEEDED(s_xrLocateSpace(s_space_hand[h], s_space,
						s_frame_state.predictedDisplayTime, &sl)) &&
						(sl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
						(sl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
					{
						XrQuaternionf q = sl.pose.orientation;
						XrVector3f    p = sl.pose.position;
						p.x -= s_a_ref_pos[0];
						p.y -= s_a_ref_pos[1];
						p.z -= s_a_ref_pos[2];
						if (s_a_recentre[3] != 1.0f || s_a_recentre[1] != 0.0f)
						{ q = qMul(s_a_recentre, q); p = v3Rotate(s_a_recentre, p); }
						hs.af_pos[0]   =  p.x;
						hs.af_pos[1]   = -p.z;
						hs.af_pos[2]   =  p.y;
						hs.f_rot_w     =  q.w;
						hs.af_rot_v[0] =  q.x;
						hs.af_rot_v[1] = -q.z;
						hs.af_rot_v[2] =  q.y;
						hs.b_pose_valid = true;
					}
				}
			}

			hs.b_active = b_any;
			s_hand[h] = hs;
		}

		// One line the first time a controller reports anything, to confirm bindings took.
		static bool s_b_logged = false;
		if (!s_b_logged && (s_hand[0].b_active || s_hand[1].b_active))
		{
			s_b_logged = true;
			char b[192];
			sprintf(b, "TRESPASS_VR: controllers live - L active=%d pose=%d, R active=%d pose=%d\n",
				s_hand[0].b_active, s_hand[0].b_pose_valid, s_hand[1].b_active, s_hand[1].b_pose_valid);
			Log(b);
		}
	}

	bool bHandState(int i_hand, SHandState& hs)
	{
		if (!s_b_active || i_hand < 0 || i_hand >= k_i_hands) return false;
		if (!s_b_actions_attached) return false;
		hs = s_hand[i_hand];
		return true;
	}

	bool bInit()
	{
		if (!bEnabled())    return false;
		if (s_b_init_tried) return s_b_active;
		s_b_init_tried = true;

		// Log unconditionally on the first attempt: a silent log is ambiguous between "VR was
		// off" and "VR was on and failed before it could say anything".
		{
			char ach[512] = { 0 };
			GetEnvironmentVariableA("XR_RUNTIME_JSON", ach, sizeof(ach));
			char buf[640];
			sprintf(buf, "TRESPASS_VR: enabled; XR_RUNTIME_JSON=%s\n", ach[0] ? ach : "(unset - system default runtime)");
			Log(buf);
		}

		// ---- 1. Load the OpenXR loader and its single bootstrap entry point --------------
		s_h_loader = LoadLibraryA("openxr_loader.dll");
		if (!s_h_loader)
		{
			Log("TRESPASS_VR: openxr_loader.dll not found - no OpenXR runtime installed; running normal path\n");
			return false;
		}
		s_xrGetInstanceProcAddr =
			(PFN_xrGetInstanceProcAddr)GetProcAddress(s_h_loader, "xrGetInstanceProcAddr");
		if (!s_xrGetInstanceProcAddr)
		{
			Log("TRESPASS_VR: xrGetInstanceProcAddr missing from loader\n");
			Shutdown_Internal();
			return false;
		}

		// The three functions callable before an instance exists come from a null instance.
		if (!LOAD(XR_NULL_HANDLE, xrEnumerateInstanceExtensionProperties) ||
		    !LOAD(XR_NULL_HANDLE, xrCreateInstance))
		{
			Shutdown_Internal();
			return false;
		}

		// ---- 2. Require the D3D11 graphics extension -------------------------------------
		// Log the RESULT of the count query, not just the count.  Three very different failures
		// all end up at the same "lacks D3D11" message otherwise: the runtime DLL failing to
		// load at all (XR_ERROR_RUNTIME_UNAVAILABLE, count 0), the runtime loading but offering
		// nothing, and the runtime genuinely not supporting D3D11.  Only the last one is about
		// graphics APIs; the first is an installation problem.
		uint32_t u_ext_count = 0;
		XrResult r_enum = s_xrEnumerateInstanceExtensionProperties(0, 0, &u_ext_count, 0);
		{
			char buf[160];
			sprintf(buf, "TRESPASS_VR: xrEnumerateInstanceExtensionProperties -> result %d, count %u\n",
				(int)r_enum, u_ext_count);
			Log(buf);
			if (r_enum == XR_ERROR_RUNTIME_UNAVAILABLE)
			{
				// XR_ERROR_RUNTIME_UNAVAILABLE is reported both for "no runtime installed" and
				// for "the runtime is there and refused us", which are diagnosed completely
				// differently - so list the causes actually seen rather than guessing at one.
				//
				// ELEVATION IS FIRST BECAUSE IT IS THE LEAST OBVIOUS.  SteamVR will not talk to a
				// process at a different integrity level: launched from an administrator shell
				// while SteamVR runs normally (or the reverse), its client DLL loads and then
				// cannot reach vrserver, so everything looks installed and correct and the
				// session simply never comes up.
				Log("TRESPASS_VR: the runtime is installed but would not start.  In order of how\n"
				    "TRESPASS_VR: often this is the cause:\n"
				    "TRESPASS_VR:  1. ELEVATION MISMATCH - this game and the runtime must run at the\n"
				    "TRESPASS_VR:     same level.  SteamVR refuses a process launched as administrator\n"
				    "TRESPASS_VR:     when SteamVR itself is not (and vice versa).\n"
				    "TRESPASS_VR:  2. the runtime's own service is not running (start SteamVR / the\n"
				    "TRESPASS_VR:     Meta simulator's companion app first)\n"
				    "TRESPASS_VR:  3. the headset is asleep, off, or not plugged in\n"
				    "TRESPASS_VR:  4. ActiveRuntime points at a manifest that is missing, or whose\n"
				    "TRESPASS_VR:     library_path does not resolve\n");
			}
		}
		bool b_have_d3d11 = false;
		bool b_have_local_floor = false;
		if (u_ext_count)
		{
			XrExtensionProperties* pa_ext = new XrExtensionProperties[u_ext_count];
			for (uint32_t i = 0; i < u_ext_count; ++i)
			{
				pa_ext[i].type = XR_TYPE_EXTENSION_PROPERTIES;
				pa_ext[i].next = 0;
			}
			if (XR_SUCCEEDED(s_xrEnumerateInstanceExtensionProperties(0, u_ext_count, &u_ext_count, pa_ext)))
			{
				// List every extension the runtime offers.  "Lacks D3D11" on its own cannot be
				// acted on - what matters is which graphics API it DOES support, since that
				// decides whether this is a runtime we can never use or one we reach through an
				// interop path.  A few lines once per run, and it makes the difference between
				// a guess and a decision.
				char buf[256];
				sprintf(buf, "TRESPASS_VR: runtime offers %u extensions:\n", u_ext_count);
				Log(buf);
				for (uint32_t i = 0; i < u_ext_count; ++i)
				{
					sprintf(buf, "TRESPASS_VR:   %s (v%u)\n",
						pa_ext[i].extensionName, pa_ext[i].extensionVersion);
					Log(buf);
					if (strcmp(pa_ext[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
						b_have_d3d11 = true;
					if (strcmp(pa_ext[i].extensionName, XR_EXT_LOCAL_FLOOR_EXTENSION_NAME) == 0)
						b_have_local_floor = true;
				}
			}
			delete[] pa_ext;
		}
		if (!b_have_d3d11)
		{
			// Do not accuse the runtime of lacking D3D11 when the truth is that nothing was
			// enumerated at all - that reads as "your headset is incompatible" and sends the
			// reader after a graphics problem that does not exist.  An empty list after a failed
			// query means the runtime never answered; the real cause was logged above.
			if (u_ext_count == 0)
				Log("TRESPASS_VR: no extensions were enumerated, so D3D11 support is unknown - VR off\n"
				    "TRESPASS_VR: (this is the failure above, NOT a graphics-API incompatibility)\n");
			else
				Log("TRESPASS_VR: runtime lacks " XR_KHR_D3D11_ENABLE_EXTENSION_NAME " - cannot use the D3D11 renderer with it\n");
			Shutdown_Internal();
			return false;
		}

		// ---- 3. Create the instance ------------------------------------------------------
		// Enable XR_EXT_local_floor when offered, so a floor-referenced reference space becomes
		// available (SteamVR does not expose LOCAL_FLOOR without it).  D3D11 is mandatory and always
		// first; local_floor is optional and only appended when present, so a runtime without it is
		// unaffected and just falls back to the seated LOCAL space.
		const char* apsz_ext[2] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME, 0 };
		uint32_t u_ext_enabled = 1;
		if (b_have_local_floor)
			apsz_ext[u_ext_enabled++] = XR_EXT_LOCAL_FLOOR_EXTENSION_NAME;
		XrInstanceCreateInfo ici;
		memset(&ici, 0, sizeof(ici));
		ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
		ici.enabledExtensionCount = u_ext_enabled;
		ici.enabledExtensionNames = apsz_ext;
		strcpy(ici.applicationInfo.applicationName, "Trespasser");
		ici.applicationInfo.applicationVersion = 1;
		strcpy(ici.applicationInfo.engineName, "Trespasser");
		ici.applicationInfo.engineVersion = 1;
		// Request 1.0.0 (not the header's 1.1.x XR_CURRENT_API_VERSION): a runtime that only
		// speaks OpenXR 1.0 rejects a 1.1 apiVersion with XR_ERROR_API_VERSION_UNSUPPORTED, and
		// nothing in M1-M4 needs 1.1.  The 1.1 headers are backward-compatible, so this is safe.
		ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);

		XrResult r = s_xrCreateInstance(&ici, &s_instance);
		if (XR_FAILED(r))
		{
			char buf[96];
			sprintf(buf, "TRESPASS_VR: xrCreateInstance failed (%d)\n", (int)r);
			Log(buf);
			Shutdown_Internal();
			return false;
		}

		// ---- 4. Bind the instance-level functions we need --------------------------------
		if (!LOAD(s_instance, xrDestroyInstance) ||
		    !LOAD(s_instance, xrGetInstanceProperties) ||
		    !LOAD(s_instance, xrGetSystem) ||
		    !LOAD(s_instance, xrGetSystemProperties) ||
		    !LOAD(s_instance, xrEnumerateViewConfigurationViews) ||
		    !LOAD(s_instance, xrGetD3D11GraphicsRequirementsKHR) ||
		    !LOAD(s_instance, xrCreateSession) ||
		    !LOAD(s_instance, xrDestroySession) ||
		    !LOAD(s_instance, xrBeginSession) ||
		    !LOAD(s_instance, xrEndSession) ||
		    !LOAD(s_instance, xrRequestExitSession) ||
		    !LOAD(s_instance, xrPollEvent) ||
		    !LOAD(s_instance, xrCreateReferenceSpace) ||
		    !LOAD(s_instance, xrEnumerateReferenceSpaces) ||
		    !LOAD(s_instance, xrDestroySpace) ||
		    !LOAD(s_instance, xrEnumerateSwapchainFormats) ||
		    !LOAD(s_instance, xrCreateSwapchain) ||
		    !LOAD(s_instance, xrDestroySwapchain) ||
		    !LOAD(s_instance, xrEnumerateSwapchainImages) ||
		    !LOAD(s_instance, xrAcquireSwapchainImage) ||
		    !LOAD(s_instance, xrWaitSwapchainImage) ||
		    !LOAD(s_instance, xrReleaseSwapchainImage) ||
		    !LOAD(s_instance, xrWaitFrame) ||
		    !LOAD(s_instance, xrBeginFrame) ||
		    !LOAD(s_instance, xrEndFrame) ||
		    !LOAD(s_instance, xrLocateViews) ||
		    !LOAD(s_instance, xrStringToPath) ||
		    !LOAD(s_instance, xrCreateActionSet) ||
		    !LOAD(s_instance, xrDestroyActionSet) ||
		    !LOAD(s_instance, xrCreateAction) ||
		    !LOAD(s_instance, xrDestroyAction) ||
		    !LOAD(s_instance, xrSuggestInteractionProfileBindings) ||
		    !LOAD(s_instance, xrAttachSessionActionSets) ||
		    !LOAD(s_instance, xrSyncActions) ||
		    !LOAD(s_instance, xrGetActionStateFloat) ||
		    !LOAD(s_instance, xrGetActionStateBoolean) ||
		    !LOAD(s_instance, xrGetActionStateVector2f) ||
		    !LOAD(s_instance, xrGetActionStatePose) ||
		    !LOAD(s_instance, xrCreateActionSpace) ||
		    !LOAD(s_instance, xrLocateSpace))
		{
			Shutdown_Internal();
			return false;
		}

		XrInstanceProperties ip; memset(&ip, 0, sizeof(ip)); ip.type = XR_TYPE_INSTANCE_PROPERTIES;
		if (XR_SUCCEEDED(s_xrGetInstanceProperties(s_instance, &ip)))
		{
			char buf[192];
			sprintf(buf, "TRESPASS_VR: runtime '%s' v%u.%u.%u\n", ip.runtimeName,
				XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
				XR_VERSION_PATCH(ip.runtimeVersion));
			Log(buf);
		}

		// ---- 5. Find the head-mounted system ---------------------------------------------
		XrSystemGetInfo sgi; memset(&sgi, 0, sizeof(sgi));
		sgi.type = XR_TYPE_SYSTEM_GET_INFO;
		sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		r = s_xrGetSystem(s_instance, &sgi, &s_system);
		if (XR_FAILED(r))
		{
			// XR_ERROR_FORM_FACTOR_UNAVAILABLE just means no HMD is connected right now.
			char buf[112];
			sprintf(buf, "TRESPASS_VR: no head-mounted system available (%d); running normal path\n", (int)r);
			Log(buf);
			Shutdown_Internal();
			return false;
		}

		XrSystemProperties sp; memset(&sp, 0, sizeof(sp)); sp.type = XR_TYPE_SYSTEM_PROPERTIES;
		if (XR_SUCCEEDED(s_xrGetSystemProperties(s_instance, s_system, &sp)))
		{
			char buf[192];
			sprintf(buf, "TRESPASS_VR: system '%s' (max swapchain %ux%u)\n", sp.systemName,
				sp.graphicsProperties.maxSwapchainImageWidth,
				sp.graphicsProperties.maxSwapchainImageHeight);
			Log(buf);
		}

		// ---- 6. D3D11 graphics requirements (adapter LUID + min feature level) -----------
		// M2 must create the D3D11 device on THIS adapter, else xrCreateSession is rejected.
		XrGraphicsRequirementsD3D11KHR gr; memset(&gr, 0, sizeof(gr));
		gr.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR;
		if (XR_SUCCEEDED(s_xrGetD3D11GraphicsRequirementsKHR(s_instance, s_system, &gr)))
		{
			char buf[160];
			sprintf(buf, "TRESPASS_VR: required adapter LUID %08lx:%08lx, min feature level 0x%x\n",
				(unsigned long)gr.adapterLuid.HighPart, (unsigned long)gr.adapterLuid.LowPart,
				(unsigned)gr.minFeatureLevel);
			Log(buf);

			// Hand the renderer the adapter it must build its device on.  This only works
			// because bInit runs before the first frame - the device is created lazily on that
			// frame, so the constraint arrives in time.  xrCreateSession would reject a device
			// built on any other GPU, which is the normal case on a laptop with a discrete card
			// driving the headset and an integrated one driving the display.
			RenderD3D11::SetRequiredAdapterLuid((unsigned int)gr.adapterLuid.LowPart,
			                                    (int)gr.adapterLuid.HighPart);
		}

		// ---- 7. Recommended per-eye render size ------------------------------------------
		uint32_t u_view_count = 0;
		s_xrEnumerateViewConfigurationViews(s_instance, s_system,
			XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &u_view_count, 0);
		if (u_view_count >= 1)
		{
			XrViewConfigurationView* pa_v = new XrViewConfigurationView[u_view_count];
			for (uint32_t i = 0; i < u_view_count; ++i)
			{
				pa_v[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
				pa_v[i].next = 0;
			}
			if (XR_SUCCEEDED(s_xrEnumerateViewConfigurationViews(s_instance, s_system,
				XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, u_view_count, &u_view_count, pa_v)))
			{
				// Keep the per-view configuration - the swapchains are created from it.
				//
				// TRESPASS_VR_SCALE (percent, 10..100) shrinks the per-eye render target below
				// what the runtime recommends.  A Quest 3 asks for 1680x1760 PER EYE; against a
				// renderer whose whole design point was a 640x480 software rasteriser that is
				// nearly 20x the pixels of the flat game, twice over.  The runtime scales the
				// submitted image back up itself, so this is a pure quality-for-speed trade and
				// costs nothing but sharpness.
				int i_scale = 100;
				{
					char ach[16] = { 0 };
					if (GetEnvironmentVariableA("TRESPASS_VR_SCALE", ach, sizeof(ach)) > 0)
					{
						i_scale = atoi(ach);
						if (i_scale < 10 || i_scale > 100) i_scale = 100;
					}
				}

				for (int i_e = 0; i_e < k_i_eyes && (uint32_t)i_e < u_view_count; ++i_e)
				{
					s_a_view_cfg[i_e] = pa_v[i_e];
					if (i_scale != 100)
					{
						s_a_view_cfg[i_e].recommendedImageRectWidth  =
							(pa_v[i_e].recommendedImageRectWidth  * i_scale) / 100;
						s_a_view_cfg[i_e].recommendedImageRectHeight =
							(pa_v[i_e].recommendedImageRectHeight * i_scale) / 100;
					}
				}
				if (i_scale != 100)
				{
					char buf[160];
					sprintf(buf, "TRESPASS_VR: per-eye scaled to %d%% -> %ux%u\n", i_scale,
						s_a_view_cfg[0].recommendedImageRectWidth,
						s_a_view_cfg[0].recommendedImageRectHeight);
					Log(buf);
				}

				// Report the size we will actually render, not the unscaled recommendation.
				s_eye_w = (int)s_a_view_cfg[0].recommendedImageRectWidth;
				s_eye_h = (int)s_a_view_cfg[0].recommendedImageRectHeight;
				char buf[160];
				sprintf(buf, "TRESPASS_VR: %u views, recommended per-eye %dx%d (samples %u)\n",
					u_view_count, s_eye_w, s_eye_h, pa_v[0].recommendedSwapchainSampleCount);
				Log(buf);
			}
			delete[] pa_v;
		}

		// Controllers: declare the actions and suggest bindings now (needs only the instance).  The
		// action spaces and the attach happen later, once the session exists (AttachActions).
		CreateActions();

		s_b_active = true;
		Log("TRESPASS_VR: OpenXR bring-up OK (instance + system + requirements)\n");
		return true;
	}

	void Shutdown()
	{
		Shutdown_Internal();

		// Clear the "already tried" latches, so a later bInit can bring VR up again.  This is the
		// difference between the public Shutdown and the internal one: this is a deliberate
		// teardown by the app (quitting, or the restart-with-the-render-dialog path, which loops
		// back through DoWinMain and starts everything over), whereas Shutdown_Internal also fires
		// when the RUNTIME asks us to go away - and re-initialising there would just walk back into
		// the runtime that has already said no, once per frame.
		s_b_init_tried     = false;
		s_b_session_tried  = false;
		s_b_exit_requested = false;
		s_b_first_frame_logged = false;
	}
}
