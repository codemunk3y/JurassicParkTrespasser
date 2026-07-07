/***********************************************************************************************
 *
 * Copyright � 2026.  Experimental modernization.
 *
 * Implementation of RenderD3D11.hpp.
 *
 * SLICE 1 (this file, current): stand up a real D3D11 device + DXGI swap chain on the game
 * window, and each frame clear the back buffer and Present.  Geometry submission and the
 * HLSL shaders are stubbed - when enabled the window shows a solid clear colour, which
 * proves the device / swap chain / present infrastructure works on the game's HWND before
 * any shader or vertex-buffer work.
 *
 * SLICE 2 (next): passthrough vertex shader + textured pixel shader, a dynamic vertex
 * buffer fed from SubmitPolygon, texture upload, and a depth buffer - turning the clear
 * colour into the actual rendered scene through the GPU.
 *
 **********************************************************************************************/

// This module talks to the modern Windows SDK Direct3D 11, NOT the 1998 DirectX headers in
// Inc/DirectX.  Include only the SDK headers here.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "RenderD3D11.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace
{
	// ---- Module state -------------------------------------------------------------------
	HWND                     s_hwnd          = 0;
	ID3D11Device*            s_pd3dDevice    = 0;
	ID3D11DeviceContext*     s_pd3dContext   = 0;
	IDXGISwapChain*          s_pSwapChain    = 0;
	ID3D11RenderTargetView*  s_pBackBufferRTV= 0;

	int   s_i_enabled   = -1;		// -1 = unresolved, 0 = off, 1 = on (cached env check).
	bool  s_b_init_tried= false;	// Have we attempted device creation yet?
	bool  s_b_active    = false;	// Device + swap chain live and presenting.

	UINT  s_u_width     = 0;		// Swap-chain back-buffer size.
	UINT  s_u_height    = 0;

	void Shutdown_Internal();		// forward decl (used by TryInit on failure)

	inline void Log(const char* psz)
	{
		OutputDebugStringA(psz);
	}

	// Create the device + swap chain sized to the window's current client rect.  Sets
	// s_b_active on success.  Called once, lazily, from bBeginFrame.
	void TryInit()
	{
		s_b_init_tried = true;

		if (!s_hwnd)
		{
			Log("TRESPASS_D3D11: no window set, cannot init\n");
			return;
		}

		RECT rc;
		GetClientRect(s_hwnd, &rc);
		UINT u_w = rc.right  - rc.left;
		UINT u_h = rc.bottom - rc.top;
		if (u_w < 8 || u_h < 8)
		{
			// Window not sized yet; try again next frame.
			s_b_init_tried = false;
			return;
		}

		DXGI_SWAP_CHAIN_DESC scd;
		ZeroMemory(&scd, sizeof(scd));
		scd.BufferCount        = 1;
		scd.BufferDesc.Width   = u_w;
		scd.BufferDesc.Height  = u_h;
		scd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
		scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.OutputWindow       = s_hwnd;
		scd.SampleDesc.Count   = 1;
		scd.SampleDesc.Quality = 0;
		scd.Windowed           = TRUE;
		// Blt-model (DISCARD) coexists with a window that GDI/DirectDraw also reference,
		// which matters while the software path still owns the same HWND.
		scd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

		D3D_FEATURE_LEVEL fl_got;
		const D3D_FEATURE_LEVEL afl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

		HRESULT hr = D3D11CreateDeviceAndSwapChain(
			0,							// default adapter
			D3D_DRIVER_TYPE_HARDWARE,
			0,							// no software rasterizer module
			0,							// flags
			afl, (UINT)(sizeof(afl) / sizeof(afl[0])),
			D3D11_SDK_VERSION,
			&scd,
			&s_pSwapChain,
			&s_pd3dDevice,
			&fl_got,
			&s_pd3dContext);

		if (FAILED(hr))
		{
			Log("TRESPASS_D3D11: D3D11CreateDeviceAndSwapChain FAILED\n");
			Shutdown_Internal();
			return;
		}

		ID3D11Texture2D* p_backbuffer = 0;
		hr = s_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&p_backbuffer);
		if (FAILED(hr) || !p_backbuffer)
		{
			Log("TRESPASS_D3D11: GetBuffer(backbuffer) FAILED\n");
			Shutdown_Internal();
			return;
		}

		hr = s_pd3dDevice->CreateRenderTargetView(p_backbuffer, 0, &s_pBackBufferRTV);
		p_backbuffer->Release();
		if (FAILED(hr))
		{
			Log("TRESPASS_D3D11: CreateRenderTargetView FAILED\n");
			Shutdown_Internal();
			return;
		}

		s_u_width  = u_w;
		s_u_height = u_h;
		s_b_active = true;
		Log("TRESPASS_D3D11: device + swap chain created (slice 1: clear + present only)\n");
	}

	// Release everything.  Declared here, exposed through Shutdown() below.
	void Shutdown_Internal()
	{
		if (s_pd3dContext) s_pd3dContext->ClearState();
		if (s_pBackBufferRTV) { s_pBackBufferRTV->Release(); s_pBackBufferRTV = 0; }
		if (s_pSwapChain)     { s_pSwapChain->Release();     s_pSwapChain     = 0; }
		if (s_pd3dContext)    { s_pd3dContext->Release();    s_pd3dContext    = 0; }
		if (s_pd3dDevice)     { s_pd3dDevice->Release();     s_pd3dDevice     = 0; }
		s_b_active = false;
	}
}

namespace RenderD3D11
{
	bool bEnabled()
	{
		if (s_i_enabled < 0)
			s_i_enabled = GetEnvironmentVariableA("TRESPASS_D3D11", 0, 0) > 0 ? 1 : 0;
		return s_i_enabled != 0;
	}

	bool bActive()
	{
		return s_b_active;
	}

	void SetWindow(void* p_hwnd)
	{
		if (!bEnabled())
			return;
		s_hwnd = (HWND)p_hwnd;
	}

	bool bBeginFrame(int i_width, int i_height)
	{
		if (!bEnabled())
			return false;

		if (!s_b_init_tried)
			TryInit();

		if (!s_b_active)
			return false;

		// Bind the back buffer and clear it.  A distinctive teal makes it obvious that the
		// D3D11 swap chain - not the software blit - owns the window.
		s_pd3dContext->OMSetRenderTargets(1, &s_pBackBufferRTV, 0);

		D3D11_VIEWPORT vp;
		vp.TopLeftX = 0.0f;
		vp.TopLeftY = 0.0f;
		vp.Width    = (float)s_u_width;
		vp.Height   = (float)s_u_height;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		s_pd3dContext->RSSetViewports(1, &vp);

		const float af_clear[4] = { 0.10f, 0.35f, 0.45f, 1.0f };
		s_pd3dContext->ClearRenderTargetView(s_pBackBufferRTV, af_clear);

		// i_width / i_height (the engine's render size) will drive the screen->clip transform
		// in slice 2's vertex shader.  Unused in slice 1.
		(void)i_width;
		(void)i_height;
		return true;
	}

	void SubmitPolygon(const SVert* pav_verts, int i_count, void* p_texture)
	{
		// SLICE 2: append the fan to a dynamic vertex buffer, bind p_texture, draw through
		// the passthrough VS + textured PS.  No-op for now.
		(void)pav_verts;
		(void)i_count;
		(void)p_texture;
	}

	void Present()
	{
		if (!s_b_active || !s_pSwapChain)
			return;
		// Present interval 0: don't block on vsync (the game does its own pacing).
		s_pSwapChain->Present(0, 0);
	}

	void Shutdown()
	{
		Shutdown_Internal();
	}
}
