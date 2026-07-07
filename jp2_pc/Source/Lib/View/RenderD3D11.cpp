/***********************************************************************************************
 *
 * Copyright � 2026.  Experimental modernization.
 *
 * Implementation of RenderD3D11.hpp.
 *
 * SLICE 1 (done): D3D11 device + DXGI swap chain on the game window, clear + present.
 *
 * SLICE 2a (this file, current): passthrough vertex shader + flat pixel shader, a dynamic
 * vertex buffer, and a real SubmitPolygon that fan-triangulates the engine's screen-space
 * polygon stream.  Each polygon is drawn FLAT-shaded in its texture's representative colour
 * (CTexture::d3dpixColour) - no texture sampling yet.  Polygons arrive already depth-sorted
 * (back-to-front) from the software pipeline, so we draw them in order with no depth buffer;
 * painter's-order compositing matches the software renderer.  This validates the vertex
 * buffer, input layout, screen->clip transform and draw path before textures/perspective.
 *
 * SLICE 2b (next): reconstruct w = 1/rhw in the VS for perspective-correct UV, sample the
 * texture in the PS (texture upload + SRV cache), and add a depth buffer so the software
 * depth sort can eventually be dropped.
 *
 **********************************************************************************************/

// Modern Windows SDK Direct3D 11, NOT the 1998 DirectX headers in Inc/DirectX.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <vector>

#include "RenderD3D11.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	// ---- Device / swap chain (slice 1) --------------------------------------------------
	HWND                     s_hwnd           = 0;
	ID3D11Device*            s_pd3dDevice     = 0;
	ID3D11DeviceContext*     s_pd3dContext    = 0;
	IDXGISwapChain*          s_pSwapChain     = 0;
	ID3D11RenderTargetView*  s_pBackBufferRTV = 0;

	// ---- Pipeline (slice 2a) ------------------------------------------------------------
	ID3D11VertexShader*      s_pVS            = 0;
	ID3D11PixelShader*       s_pPS            = 0;
	ID3D11InputLayout*       s_pLayout        = 0;
	ID3D11Buffer*            s_pVB            = 0;	// dynamic vertex buffer
	ID3D11Buffer*            s_pCB            = 0;	// view-params constant buffer
	ID3D11RasterizerState*   s_pRaster        = 0;
	UINT                     s_vb_capacity    = 0;	// s_pVB size in vertices

	int   s_i_enabled    = -1;
	bool  s_b_init_tried = false;
	bool  s_b_active     = false;	// device + swap chain live
	bool  s_b_pipeline   = false;	// shaders/layout/state built

	UINT  s_u_width      = 0;		// swap-chain back-buffer size
	UINT  s_u_height     = 0;

	// CPU-side accumulator for this frame's triangles (expanded from fans).
	std::vector<RenderD3D11::SVert> s_verts;

	// Passthrough VS: screen pixels -> clip space.  gViewParams = (2/renderW, 2/renderH).
	// Slice 2a uses a flat depth of 0.5 and w = 1 (no perspective yet - polys are already
	// sorted, so painter's order suffices, and flat colour needs no perspective-correct
	// interpolation).  Slice 2b will reconstruct w = 1/rhw here for correct UVs.
	const char* k_psz_vs =
		"cbuffer CbView : register(b0) { float4 gViewParams; }\n"
		"struct VSIn  { float4 sr : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"VSOut main(VSIn i) {\n"
		"  VSOut o;\n"
		"  float ndcx = i.sr.x * gViewParams.x - 1.0;\n"
		"  float ndcy = 1.0 - i.sr.y * gViewParams.y;\n"
		"  o.pos = float4(ndcx, ndcy, 0.5, 1.0);\n"
		"  o.col = i.col;\n"
		"  o.uv  = i.uv;\n"
		"  return o;\n"
		"}\n";

	const char* k_psz_ps =
		"struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"float4 main(VSOut i) : SV_Target { return i.col; }\n";

	inline void Log(const char* psz)  { OutputDebugStringA(psz); }

	void Shutdown_Internal();		// forward decl

	// Create the device + swap chain, sized to the window client rect.  (Slice 1.)
	void TryInit()
	{
		s_b_init_tried = true;

		if (!s_hwnd) { Log("TRESPASS_D3D11: no window set\n"); return; }

		RECT rc;
		GetClientRect(s_hwnd, &rc);
		UINT u_w = rc.right - rc.left, u_h = rc.bottom - rc.top;
		if (u_w < 8 || u_h < 8) { s_b_init_tried = false; return; }

		DXGI_SWAP_CHAIN_DESC scd;
		ZeroMemory(&scd, sizeof(scd));
		scd.BufferCount        = 1;
		scd.BufferDesc.Width   = u_w;
		scd.BufferDesc.Height  = u_h;
		scd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;
		scd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.OutputWindow       = s_hwnd;
		scd.SampleDesc.Count   = 1;
		scd.Windowed           = TRUE;
		scd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;

		D3D_FEATURE_LEVEL fl_got;
		const D3D_FEATURE_LEVEL afl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

		HRESULT hr = D3D11CreateDeviceAndSwapChain(
			0, D3D_DRIVER_TYPE_HARDWARE, 0, 0,
			afl, (UINT)(sizeof(afl) / sizeof(afl[0])), D3D11_SDK_VERSION,
			&scd, &s_pSwapChain, &s_pd3dDevice, &fl_got, &s_pd3dContext);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateDeviceAndSwapChain FAILED\n"); Shutdown_Internal(); return; }

		ID3D11Texture2D* p_bb = 0;
		hr = s_pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&p_bb);
		if (FAILED(hr) || !p_bb) { Log("TRESPASS_D3D11: GetBuffer FAILED\n"); Shutdown_Internal(); return; }
		hr = s_pd3dDevice->CreateRenderTargetView(p_bb, 0, &s_pBackBufferRTV);
		p_bb->Release();
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateRenderTargetView FAILED\n"); Shutdown_Internal(); return; }

		s_u_width = u_w; s_u_height = u_h;
		s_b_active = true;
		Log("TRESPASS_D3D11: device + swap chain created\n");
	}

	// Compile shaders and create layout / constant buffer / raster state.  (Slice 2a.)
	void TryBuildPipeline()
	{
		s_b_pipeline = true;		// only attempt once

		ID3DBlob* p_vsblob = 0; ID3DBlob* p_psblob = 0; ID3DBlob* p_err = 0;

		HRESULT hr = D3DCompile(k_psz_vs, strlen(k_psz_vs), "vs", 0, 0, "main", "vs_4_0", 0, 0, &p_vsblob, &p_err);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: VS compile FAILED\n"); if (p_err) { Log((const char*)p_err->GetBufferPointer()); p_err->Release(); } return; }
		if (p_err) { p_err->Release(); p_err = 0; }

		hr = D3DCompile(k_psz_ps, strlen(k_psz_ps), "ps", 0, 0, "main", "ps_4_0", 0, 0, &p_psblob, &p_err);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: PS compile FAILED\n"); if (p_err) { Log((const char*)p_err->GetBufferPointer()); p_err->Release(); } p_vsblob->Release(); return; }
		if (p_err) { p_err->Release(); p_err = 0; }

		hr = s_pd3dDevice->CreateVertexShader(p_vsblob->GetBufferPointer(), p_vsblob->GetBufferSize(), 0, &s_pVS);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateVertexShader FAILED\n"); p_vsblob->Release(); p_psblob->Release(); return; }
		hr = s_pd3dDevice->CreatePixelShader(p_psblob->GetBufferPointer(), p_psblob->GetBufferSize(), 0, &s_pPS);
		p_psblob->Release();
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreatePixelShader FAILED\n"); p_vsblob->Release(); return; }

		// Input layout matches SVert: pos+rhw (16B), BGRA colour (4B), uv (8B) = 28B.
		D3D11_INPUT_ELEMENT_DESC a_elem[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		hr = s_pd3dDevice->CreateInputLayout(a_elem, 3, p_vsblob->GetBufferPointer(), p_vsblob->GetBufferSize(), &s_pLayout);
		p_vsblob->Release();
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateInputLayout FAILED\n"); return; }

		// Constant buffer: float4 view params.
		D3D11_BUFFER_DESC cbd; ZeroMemory(&cbd, sizeof(cbd));
		cbd.ByteWidth      = 16;
		cbd.Usage          = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		hr = s_pd3dDevice->CreateBuffer(&cbd, 0, &s_pCB);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateBuffer(cb) FAILED\n"); return; }

		// Raster state: no back-face culling (the pipeline already culled), solid fill.
		D3D11_RASTERIZER_DESC rd; ZeroMemory(&rd, sizeof(rd));
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		hr = s_pd3dDevice->CreateRasterizerState(&rd, &s_pRaster);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateRasterizerState FAILED\n"); return; }

		Log("TRESPASS_D3D11: pipeline built (slice 2a: flat-shaded geometry)\n");
	}

	// Ensure the dynamic vertex buffer holds at least u_needed vertices.
	bool bEnsureVB(UINT u_needed)
	{
		if (s_pVB && s_vb_capacity >= u_needed)
			return true;
		if (s_pVB) { s_pVB->Release(); s_pVB = 0; }

		UINT u_cap = s_vb_capacity ? s_vb_capacity : 8192;
		while (u_cap < u_needed) u_cap *= 2;

		D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
		bd.ByteWidth      = u_cap * sizeof(RenderD3D11::SVert);
		bd.Usage          = D3D11_USAGE_DYNAMIC;
		bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = s_pd3dDevice->CreateBuffer(&bd, 0, &s_pVB);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateBuffer(vb) FAILED\n"); s_vb_capacity = 0; return false; }
		s_vb_capacity = u_cap;
		return true;
	}

	void Shutdown_Internal()
	{
		if (s_pd3dContext) s_pd3dContext->ClearState();
		if (s_pRaster)        { s_pRaster->Release();        s_pRaster        = 0; }
		if (s_pCB)            { s_pCB->Release();            s_pCB            = 0; }
		if (s_pVB)            { s_pVB->Release();            s_pVB            = 0; }
		if (s_pLayout)        { s_pLayout->Release();        s_pLayout        = 0; }
		if (s_pPS)            { s_pPS->Release();            s_pPS            = 0; }
		if (s_pVS)            { s_pVS->Release();            s_pVS            = 0; }
		if (s_pBackBufferRTV) { s_pBackBufferRTV->Release(); s_pBackBufferRTV = 0; }
		if (s_pSwapChain)     { s_pSwapChain->Release();     s_pSwapChain     = 0; }
		if (s_pd3dContext)    { s_pd3dContext->Release();    s_pd3dContext    = 0; }
		if (s_pd3dDevice)     { s_pd3dDevice->Release();     s_pd3dDevice     = 0; }
		s_vb_capacity = 0;
		s_b_active = false;
		s_b_pipeline = false;
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

	bool bActive() { return s_b_active && s_b_pipeline && s_pVS != 0; }

	void SetWindow(void* p_hwnd)
	{
		if (!bEnabled()) return;
		s_hwnd = (HWND)p_hwnd;
	}

	bool bBeginFrame(int i_width, int i_height)
	{
		if (!bEnabled()) return false;

		if (!s_b_init_tried) TryInit();
		if (!s_b_active)     return false;
		if (!s_b_pipeline)   TryBuildPipeline();
		if (!s_pVS || !s_pPS || !s_pLayout || !s_pCB || !s_pRaster)
			return false;

		s_verts.clear();

		s_pd3dContext->OMSetRenderTargets(1, &s_pBackBufferRTV, 0);

		// Aspect-correct viewport: the engine renders 4:3 (i_width x i_height) but the
		// swap chain matches the window (often 16:9), so fit the render inside the back
		// buffer preserving aspect and centre it (pillar/letter-box).
		float f_scale = (float)s_u_width / (float)i_width;
		float f_sh    = (float)s_u_height / (float)i_height;
		if (f_sh < f_scale) f_scale = f_sh;

		D3D11_VIEWPORT vp;
		vp.Width    = i_width  * f_scale;
		vp.Height   = i_height * f_scale;
		vp.TopLeftX = (s_u_width  - vp.Width)  * 0.5f;
		vp.TopLeftY = (s_u_height - vp.Height) * 0.5f;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		s_pd3dContext->RSSetViewports(1, &vp);
		s_pd3dContext->RSSetState(s_pRaster);

		// Clear the whole back buffer (teal shows in the pillar-box bars).
		const float af_clear[4] = { 0.10f, 0.35f, 0.45f, 1.0f };
		s_pd3dContext->ClearRenderTargetView(s_pBackBufferRTV, af_clear);

		// Update view params (2/renderW, 2/renderH) for the screen->clip transform.
		D3D11_MAPPED_SUBRESOURCE ms;
		if (SUCCEEDED(s_pd3dContext->Map(s_pCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
		{
			float* pf = (float*)ms.pData;
			pf[0] = 2.0f / (float)i_width;
			pf[1] = 2.0f / (float)i_height;
			pf[2] = 0.0f;
			pf[3] = 0.0f;
			s_pd3dContext->Unmap(s_pCB, 0);
		}
		return true;
	}

	void SubmitPolygon(const SVert* pav_verts, int i_count, void* p_texture)
	{
		(void)p_texture;		// slice 2b
		if (i_count < 3) return;

		// Fan (v0, v1, ... vn-1) -> triangle list (v0, vk-1, vk), matching the engine's
		// own polygon triangulation.
		for (int k = 2; k < i_count; ++k)
		{
			s_verts.push_back(pav_verts[0]);
			s_verts.push_back(pav_verts[k - 1]);
			s_verts.push_back(pav_verts[k]);
		}
	}

	void Present()
	{
		if (!bActive() || !s_pSwapChain) return;

		if (!s_verts.empty())
		{
			UINT u_count = (UINT)s_verts.size();
			if (bEnsureVB(u_count))
			{
				D3D11_MAPPED_SUBRESOURCE ms;
				if (SUCCEEDED(s_pd3dContext->Map(s_pVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
				{
					memcpy(ms.pData, &s_verts[0], u_count * sizeof(SVert));
					s_pd3dContext->Unmap(s_pVB, 0);

					UINT u_stride = sizeof(SVert), u_offset = 0;
					s_pd3dContext->IASetInputLayout(s_pLayout);
					s_pd3dContext->IASetVertexBuffers(0, 1, &s_pVB, &u_stride, &u_offset);
					s_pd3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					s_pd3dContext->VSSetShader(s_pVS, 0, 0);
					s_pd3dContext->VSSetConstantBuffers(0, 1, &s_pCB);
					s_pd3dContext->PSSetShader(s_pPS, 0, 0);
					s_pd3dContext->Draw(u_count, 0);
				}
			}
		}

		s_pSwapChain->Present(0, 0);
	}

	void Shutdown() { Shutdown_Internal(); }
}
