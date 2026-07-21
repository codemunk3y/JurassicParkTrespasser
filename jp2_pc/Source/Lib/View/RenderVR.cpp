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
#include <stdio.h>

// The OpenXR platform header exposes the D3D11 graphics-binding + requirements types only when
// these are defined ahead of it, and it needs <d3d11.h> (above) already included.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include "openxr/openxr.h"
#include "openxr/openxr_platform.h"

#include "RenderVR.hpp"

namespace
{
	inline void Log(const char* psz) { OutputDebugStringA(psz); }

	// ---- State --------------------------------------------------------------------------
	int         s_i_enabled   = -1;		// -1 = TRESPASS_VR not yet read
	bool        s_b_init_tried = false;
	bool        s_b_active    = false;

	HMODULE     s_h_loader    = 0;
	XrInstance  s_instance    = XR_NULL_HANDLE;
	XrSystemId  s_system      = XR_NULL_SYSTEM_ID;

	int         s_eye_w       = 0;
	int         s_eye_h       = 0;

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

	void Shutdown_Internal()
	{
		if (s_instance != XR_NULL_HANDLE && s_xrDestroyInstance)
			s_xrDestroyInstance(s_instance);
		s_instance = XR_NULL_HANDLE;
		s_system   = XR_NULL_SYSTEM_ID;
		if (s_h_loader) { FreeLibrary(s_h_loader); s_h_loader = 0; }
		s_b_active = false;
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

	int  iRecommendedEyeWidth()  { return s_eye_w; }
	int  iRecommendedEyeHeight() { return s_eye_h; }

	bool bInit()
	{
		if (!bEnabled())    return false;
		if (s_b_init_tried) return s_b_active;
		s_b_init_tried = true;

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
		uint32_t u_ext_count = 0;
		s_xrEnumerateInstanceExtensionProperties(0, 0, &u_ext_count, 0);
		bool b_have_d3d11 = false;
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
				for (uint32_t i = 0; i < u_ext_count; ++i)
					if (strcmp(pa_ext[i].extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0)
						b_have_d3d11 = true;
			}
			delete[] pa_ext;
		}
		if (!b_have_d3d11)
		{
			Log("TRESPASS_VR: runtime lacks " XR_KHR_D3D11_ENABLE_EXTENSION_NAME " - cannot use the D3D11 renderer with it\n");
			Shutdown_Internal();
			return false;
		}

		// ---- 3. Create the instance ------------------------------------------------------
		const char* apsz_ext[] = { XR_KHR_D3D11_ENABLE_EXTENSION_NAME };
		XrInstanceCreateInfo ici;
		memset(&ici, 0, sizeof(ici));
		ici.type = XR_TYPE_INSTANCE_CREATE_INFO;
		ici.enabledExtensionCount = 1;
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
		    !LOAD(s_instance, xrGetD3D11GraphicsRequirementsKHR))
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
				s_eye_w = (int)pa_v[0].recommendedImageRectWidth;
				s_eye_h = (int)pa_v[0].recommendedImageRectHeight;
				char buf[160];
				sprintf(buf, "TRESPASS_VR: %u views, recommended per-eye %dx%d (samples %u)\n",
					u_view_count, s_eye_w, s_eye_h, pa_v[0].recommendedSwapchainSampleCount);
				Log(buf);
			}
			delete[] pa_v;
		}

		s_b_active = true;
		Log("TRESPASS_VR: OpenXR bring-up OK (instance + system + requirements)\n");
		return true;
	}

	void Shutdown()
	{
		Shutdown_Internal();
	}
}
