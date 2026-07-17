/***********************************************************************************************
 *
 * Copyright � 2026.  Experimental modernization.
 *
 * Implementation of RenderD3D11.hpp.
 *
 * SLICE 1 (done): D3D11 device + DXGI swap chain on the game window, clear + present.
 * SLICE 2a (done): passthrough VS + flat PS, dynamic vertex buffer, fan-triangulated stream.
 *
 * SLICE 2b (this file, current): perspective-correct UV (reconstruct w = 1/rhw in the VS),
 * texture upload + SRV cache (caller supplies BGRA + a CTexture key), textured pixel shader,
 * a linear/wrap sampler, and batching by texture.  Untextured polygons bind a 1x1 white
 * texture so their flat vertex colour shows through the same shader.  Still no depth buffer:
 * polygons arrive depth-sorted, so painter's order matches the software renderer.  (Depth
 * buffer + dropping the software sort is the next step.)
 *
 **********************************************************************************************/

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <vector>
#include <unordered_map>

#include "RenderD3D11.hpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace
{
	// ---- Device / swap chain ------------------------------------------------------------
	HWND                     s_hwnd           = 0;
	ID3D11Device*            s_pd3dDevice     = 0;
	ID3D11DeviceContext*     s_pd3dContext    = 0;
	IDXGISwapChain*          s_pSwapChain     = 0;
	ID3D11RenderTargetView*  s_pBackBufferRTV = 0;
	ID3D11Texture2D*         s_pDepthTex      = 0;
	ID3D11DepthStencilView*  s_pDSV           = 0;
	ID3D11DepthStencilState* s_pDepthState    = 0;

	// ---- Pipeline -----------------------------------------------------------------------
	ID3D11VertexShader*      s_pVS            = 0;
	ID3D11PixelShader*       s_pPS            = 0;
	ID3D11InputLayout*       s_pLayout        = 0;
	ID3D11Buffer*            s_pVB            = 0;
	ID3D11Buffer*            s_pCB            = 0;
	ID3D11RasterizerState*   s_pRaster        = 0;
	ID3D11SamplerState*      s_pSampWrap      = 0;	// tiling textures
	ID3D11SamplerState*      s_pSampClamp     = 0;	// non-tileable textures
	ID3D11BlendState*        s_pBlend         = 0;	// alpha blend (colour-key + translucency)
	ID3D11ShaderResourceView* s_pWhiteSRV     = 0;	// 1x1 white for untextured polys
	UINT                     s_vb_capacity    = 0;

	// ---- Bump pipeline (separate so the main path is untouched) --------------------------
	ID3D11VertexShader*      s_pBumpVS        = 0;
	ID3D11PixelShader*       s_pBumpPS        = 0;
	ID3D11InputLayout*       s_pBumpLayout    = 0;
	ID3D11Buffer*            s_pBumpVB        = 0;
	UINT                     s_bumpvb_capacity = 0;
	ID3D11ShaderResourceView* s_pFlatNormSRV  = 0;	// 1x1 (0,0,1) fallback normal

	// ---- GPU sky (software sky image blitted behind the geometry) -------------------------
	// The software renderer already draws a correct cloud sky into the 16-bit main raster each
	// frame (ClearMemSurfaces -> DrawSkyToHorizon) before any geometry.  Rather than re-project
	// clouds on the GPU (which streaks at grazing angles), we upload that finished screen-space
	// image and blit it full-screen behind the geometry: pixel-exact, and it cannot streak.
	ID3D11VertexShader*      s_pSkyVS         = 0;
	ID3D11PixelShader*       s_pSkyPS         = 0;
	ID3D11DepthStencilState* s_pSkyDepthState = 0;	// no depth test/write for the sky
	ID3D11Texture2D*         s_pSkyImgTex     = 0;	// dynamic, screen-sized sky image
	ID3D11ShaderResourceView* s_pSkyImgSRV    = 0;
	int                      s_sky_img_w      = 0;
	int                      s_sky_img_h      = 0;
	bool                     s_b_sky_565      = false;	// sky texture is B5G6R5, not BGRA
	bool                     s_sky_valid      = false;	// SetSkyImage called this frame

	int   s_i_enabled    = -1;
	bool  s_b_init_tried = false;
	bool  s_b_active     = false;
	bool  s_b_pipeline   = false;
	bool  s_frame_open   = false;	// a frame is in progress (cleared, accumulating submits)

	UINT  s_u_width      = 0;
	UINT  s_u_height     = 0;

	// Backbuffer clear colour.  Defaults to the old diagnostic teal; the caller overrides it
	// each frame with the sky's fog colour (SetClearColour) so the sky region isn't teal.
	float s_clear_r      = 0.10f;
	float s_clear_g      = 0.35f;
	float s_clear_b      = 0.45f;

	// Diagnostic stats (written to d3d11_stats.txt): peak per-frame vertex/upload counts and
	// any device-removed event, to find what a pathological object does to the GPU path.
	unsigned int s_stat_max_verts     = 0;
	unsigned int s_stat_max_bumpverts = 0;
	unsigned int s_stat_dyn_bytes     = 0;	// dynamic-texture bytes uploaded this frame
	unsigned int s_stat_max_dyn_bytes = 0;
	int          s_stat_vb_fail       = 0;
	long         s_stat_dev_removed   = 0;	// GetDeviceRemovedReason (0 = ok)
	int          s_stat_frames        = 0;

	// This frame's triangles, expanded from fans, and one draw batch per texture run.
	std::vector<RenderD3D11::SVert> s_verts;
	struct SBatch { ID3D11ShaderResourceView* pSRV; UINT u_start; UINT u_count; bool b_clamp; };
	std::vector<SBatch> s_batches;

	// CTexture* -> SRV.  Static-world textures are persistent, so a grow-only cache is fine.
	std::unordered_map<const void*, ID3D11ShaderResourceView*> s_tex_cache;

	// CTexture* -> normal-map SRV, for bump surfaces (decoded once, reused).
	std::unordered_map<const void*, ID3D11ShaderResourceView*> s_norm_cache;

	// This frame's bump triangles + batches (one per colour+normal+sampler run).
	std::vector<RenderD3D11::SBumpVert> s_bump_verts;
	struct SBumpBatch { ID3D11ShaderResourceView* pCol; ID3D11ShaderResourceView* pNrm; UINT u_start; UINT u_count; bool b_clamp; };
	std::vector<SBumpBatch> s_bump_batches;

	// Dynamic (terrain) textures: contents change per frame and CTexture objects recycle,
	// so keep a persistent DYNAMIC gpu texture per key and re-upload its pixels each frame.
	struct SDynTex { ID3D11Texture2D* pTex; ID3D11ShaderResourceView* pSRV; int i_w; int i_h; unsigned u_frame; bool b_565; };
	std::unordered_map<const void*, SDynTex> s_dyn_cache;
	unsigned s_frame_no = 0;	// bumped each bBeginFrame; used to upload each dyn texture once/frame

	// VS: screen pixels -> clip space, reconstructing w = 1/rhw so UV/colour interpolate
	// perspective-correctly.  gViewParams = (2/renderW, 2/renderH).  Flat depth 0.5 (no
	// depth buffer yet; painter's order from the sorted input).
	const char* k_psz_vs =
		"cbuffer CbView : register(b0) { float4 gViewParams; }\n"
		"struct VSIn  { float4 sr : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"VSOut main(VSIn i) {\n"
		"  float w = 1.0 / max(i.sr.w, 1e-6);\n"          // sr.w = rhw = 1/z
		"  float ndcx = i.sr.x * gViewParams.x - 1.0;\n"
		"  float ndcy = 1.0 - i.sr.y * gViewParams.y;\n"
		"  float d = clamp(i.sr.w, 0.0, 0.99);\n"         // depth = rhw, closer = larger
		"  VSOut o;\n"
		"  o.pos = float4(ndcx * w, ndcy * w, d * w, w);\n"  // final depth = d
		"  o.col = i.col;\n"
		"  o.uv  = i.uv;\n"
		"  return o;\n"
		"}\n";

	// PS: sample the texture, modulate by the (flat) vertex colour.
	const char* k_psz_ps =
		"Texture2D    gTex : register(t0);\n"
		"SamplerState gSmp : register(s0);\n"
		"struct VSOut { float4 pos : SV_Position; float4 col : COLOR0; float2 uv : TEXCOORD0; };\n"
		"float4 main(VSOut i) : SV_Target {\n"
		"  float4 c = gTex.Sample(gSmp, i.uv) * i.col;\n"
		"  clip(c.a - 0.003);\n"     // discard colour-key holes so they don't write depth
		"  return c;\n"
		"}\n";

	// Bump VS: same screen->clip transform as the main VS, but also carries the per-polygon
	// object/texture-space light: light1 = (unit dir.xyz, strength), light2 = (ambient, spec).
	const char* k_psz_bump_vs =
		"cbuffer CbView : register(b0) { float4 gViewParams; }\n"
		"struct VSIn  { float4 sr : POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; float4 light1 : TEXCOORD1; float4 light2 : TEXCOORD2; };\n"
		"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 light1 : TEXCOORD1; float4 light2 : TEXCOORD2; };\n"
		"VSOut main(VSIn i) {\n"
		"  float w = 1.0 / max(i.sr.w, 1e-6);\n"
		"  float ndcx = i.sr.x * gViewParams.x - 1.0;\n"
		"  float ndcy = 1.0 - i.sr.y * gViewParams.y;\n"
		"  float d = clamp(i.sr.w, 0.0, 0.99);\n"
		"  VSOut o;\n"
		"  o.pos = float4(ndcx * w, ndcy * w, d * w, w);\n"
		"  o.uv = i.uv;\n"
		"  o.light1 = i.light1;\n"
		"  o.light2 = i.light2;\n"
		"  return o;\n"
		"}\n";

	// Bump PS: sample base colour (t0) + object-space normal (t1).  Diffuse = ambient +
	// strength*N.L.  The engine folds the eye half-vector into the light direction for
	// specular materials (Light.cpp), so N.L along that direction reads as specular.
	//
	// The specular FALLOFF is the engine's own: CMaterial::fSpecular -> fAngularStrength is a
	// linear ramp between two cone cosines - full strength at cos >= the light's angular size
	// (light2.z), zero below light2.z * the material's sharpness (light2.w), linear between -
	// NOT a Phong lobe.  This shader used pow(N.L, 16), an invented curve with a fabricated
	// exponent, which made every shiny material glint identically regardless of what the
	// artist authored.  Both terms are cosines, so 1 = zero angular width: a mirror-sharp
	// material collapses inner onto outer, and the max() below turns that into a hard step
	// rather than a divide by zero.
	//
	// The rvSpecular > rvDiffuse gate the engine applies is done on the CPU, by passing a
	// specular of 0 - so matte surfaces (rock) stay untouched here.
	const char* k_psz_bump_ps =
		"cbuffer CbView : register(b0) { float4 gViewParams; }\n"	// .z = bump debug tint
		"Texture2D    gTex : register(t0);\n"
		"Texture2D    gNrm : register(t1);\n"
		"SamplerState gSmp : register(s0);\n"
		"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 light1 : TEXCOORD1; float4 light2 : TEXCOORD2; };\n"
		"float4 main(VSOut i) : SV_Target {\n"
		"  float4 base = gTex.Sample(gSmp, i.uv);\n"
		"  clip(base.a - 0.003);\n"
		"  float3 n = normalize(gNrm.Sample(gSmp, i.uv).rgb * 2.0 - 1.0);\n"
		"  float ndl = max(0.0, dot(n, i.light1.xyz));\n"
		"  float lit = saturate(i.light2.x + i.light1.w * ndl);\n"
		"  float inner = saturate(i.light2.z);\n"					// cos of the light's angular size
		"  float outer = saturate(i.light2.w * i.light2.z);\n"		// scaled by material sharpness
		"  float s = saturate((ndl - outer) / max(inner - outer, 1e-4));\n"
		"  float spec = i.light2.y * s;\n"
		"  if (gViewParams.z > 0.5) {\n"				// debug: green = bump relief, red = specular material
		"    return float4(saturate(i.light2.y * 4.0), ndl, 0.0, 1.0);\n"
		"  }\n"
		"  return float4(saturate(base.rgb * lit + spec), base.a);\n"
		"}\n";

	// Sky VS: a full-screen triangle generated from the vertex id (no vertex buffer), passing a
	// 0..1 screen UV (top-left = (0,0), bottom-right = (1,1)) to sample the sky image top-down.
	const char* k_psz_sky_vs =
		"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
		"VSOut main(uint id : SV_VertexID) {\n"
		"  VSOut o;\n"
		"  o.uv  = float2((id << 1) & 2, id & 2);\n"
		"  o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
		"  return o;\n"
		"}\n";

	// Sky PS: straight blit of the software-rendered sky image (which already has clouds + fog
	// baked in).  A 1:1 screen-space copy, so there is nothing to alias or streak.
	const char* k_psz_sky_ps =
		"Texture2D    gSkyTex : register(t0);\n"
		"SamplerState gSmp    : register(s0);\n"
		"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
		"float4 main(VSOut i) : SV_Target {\n"
		"  return float4(gSkyTex.Sample(gSmp, i.uv).rgb, 1.0);\n"
		"}\n";

	inline void Log(const char* psz) { OutputDebugStringA(psz); }

	void Shutdown_Internal();

	void TryInit()
	{
		s_b_init_tried = true;
		if (!s_hwnd) { Log("TRESPASS_D3D11: no window set\n"); return; }

		RECT rc; GetClientRect(s_hwnd, &rc);
		UINT u_w = rc.right - rc.left, u_h = rc.bottom - rc.top;
		if (u_w < 8 || u_h < 8) { s_b_init_tried = false; return; }

		DXGI_SWAP_CHAIN_DESC scd; ZeroMemory(&scd, sizeof(scd));
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

		// Depth buffer, matching the back-buffer size.
		D3D11_TEXTURE2D_DESC dtd; ZeroMemory(&dtd, sizeof(dtd));
		dtd.Width = u_w; dtd.Height = u_h; dtd.MipLevels = 1; dtd.ArraySize = 1;
		dtd.Format = DXGI_FORMAT_D32_FLOAT; dtd.SampleDesc.Count = 1;
		dtd.Usage = D3D11_USAGE_DEFAULT; dtd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		if (FAILED(s_pd3dDevice->CreateTexture2D(&dtd, 0, &s_pDepthTex))) { Log("TRESPASS_D3D11: CreateTexture2D(depth) FAILED\n"); Shutdown_Internal(); return; }
		if (FAILED(s_pd3dDevice->CreateDepthStencilView(s_pDepthTex, 0, &s_pDSV))) { Log("TRESPASS_D3D11: CreateDepthStencilView FAILED\n"); Shutdown_Internal(); return; }

		s_u_width = u_w; s_u_height = u_h;
		s_b_active = true;
		Log("TRESPASS_D3D11: device + swap chain created\n");
	}

	// Create a texture + SRV from a BGRA image.
	//
	// With b_mipchain the texture gets a full mip chain, generated on the GPU.  The caller
	// hands us the sharpest version of a texture it can get and lets the hardware pick and
	// filter a level per pixel; the smaller levels are what it minifies WITH, so without
	// them distant and grazing surfaces alias and shimmer.  1x1 helper textures (white,
	// flat normal) pass false - there is nothing to generate.
	ID3D11ShaderResourceView* CreateSRVFromBGRA(int i_w, int i_h, const unsigned int* pu4, bool b_mipchain = false)
	{
		// A full chain needs the levels allocated (MipLevels 0) and writable by the GPU, so
		// such a texture cannot be IMMUTABLE with its pixels supplied at creation - mip 0 is
		// uploaded separately below and the rest derived from it.
		D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
		td.Width = i_w; td.Height = i_h; td.ArraySize = 1;
		td.MipLevels = b_mipchain ? 0 : 1;
		td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage     = b_mipchain ? D3D11_USAGE_DEFAULT : D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (b_mipchain ? D3D11_BIND_RENDER_TARGET : 0);
		td.MiscFlags = b_mipchain ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

		D3D11_SUBRESOURCE_DATA sd; ZeroMemory(&sd, sizeof(sd));
		sd.pSysMem = pu4;
		sd.SysMemPitch = i_w * 4;

		ID3D11Texture2D* p_tex = 0;
		if (FAILED(s_pd3dDevice->CreateTexture2D(&td, b_mipchain ? 0 : &sd, &p_tex)) || !p_tex)
			return 0;
		ID3D11ShaderResourceView* p_srv = 0;
		HRESULT hr = s_pd3dDevice->CreateShaderResourceView(p_tex, 0, &p_srv);
		if (SUCCEEDED(hr) && b_mipchain)
		{
			// Colour-keyed texels are premultiplied-zero, which is exactly the form that
			// survives averaging - so generated levels keep clean edges around cutouts.
			s_pd3dContext->UpdateSubresource(p_tex, 0, 0, pu4, i_w * 4, 0);
			s_pd3dContext->GenerateMips(p_srv);
		}
		p_tex->Release();
		return SUCCEEDED(hr) ? p_srv : 0;
	}

	int s_i_hires = -1;		// TRESPASS_HIRES cache (-1 = not yet queried)

	// Load a 24-bit BMP into a top-down BGRA (0xFFRRGGBB) buffer.  Used for the hi-res
	// texture side-load: these are the AI-upscaled BMPs named by the source texture's
	// content hash.  Returns false (buffer untouched) if the file is missing/unreadable
	// or not a 24-bit BMP.  Opaque only (no colour key) - alpha is forced to 0xFF.
	bool LoadBmp24ToBGRA(const char* psz_path, std::vector<unsigned int>& out, int& i_w, int& i_h)
	{
		FILE* f = fopen(psz_path, "rb");
		if (!f) return false;

		BITMAPFILEHEADER bfh; BITMAPINFOHEADER bih;
		if (fread(&bfh, sizeof(bfh), 1, f) != 1 || fread(&bih, sizeof(bih), 1, f) != 1 ||
		    bfh.bfType != 0x4D42 || bih.biBitCount != 24 || bih.biCompression != BI_RGB)
		{ fclose(f); return false; }

		int  w = bih.biWidth;
		int  h = bih.biHeight < 0 ? -bih.biHeight : bih.biHeight;
		bool b_topdown = bih.biHeight < 0;
		if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { fclose(f); return false; }

		int i_rowbytes = (w * 3 + 3) & ~3;			// BMP rows are 4-byte aligned
		out.resize((size_t)w * h);
		std::vector<unsigned char> row((size_t)i_rowbytes);
		fseek(f, bfh.bfOffBits, SEEK_SET);
		bool b_ok = true;
		for (int i = 0; i < h; ++i)
		{
			if (fread(&row[0], i_rowbytes, 1, f) != 1) { b_ok = false; break; }
			int y = b_topdown ? i : (h - 1 - i);	// store top-down for D3D
			unsigned int* pd = &out[(size_t)y * w];
			for (int x = 0; x < w; ++x)
			{
				unsigned int b = row[x * 3 + 0], g = row[x * 3 + 1], r = row[x * 3 + 2];
				pd[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
			}
		}
		fclose(f);
		if (!b_ok) return false;
		i_w = w; i_h = h;
		return true;
	}

	void TryBuildPipeline()
	{
		s_b_pipeline = true;

		ID3DBlob* p_vs = 0; ID3DBlob* p_ps = 0; ID3DBlob* p_err = 0;

		HRESULT hr = D3DCompile(k_psz_vs, strlen(k_psz_vs), "vs", 0, 0, "main", "vs_4_0", 0, 0, &p_vs, &p_err);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: VS compile FAILED\n"); if (p_err) { Log((const char*)p_err->GetBufferPointer()); p_err->Release(); } return; }
		if (p_err) { p_err->Release(); p_err = 0; }

		hr = D3DCompile(k_psz_ps, strlen(k_psz_ps), "ps", 0, 0, "main", "ps_4_0", 0, 0, &p_ps, &p_err);
		if (FAILED(hr)) { Log("TRESPASS_D3D11: PS compile FAILED\n"); if (p_err) { Log((const char*)p_err->GetBufferPointer()); p_err->Release(); } p_vs->Release(); return; }
		if (p_err) { p_err->Release(); p_err = 0; }

		if (FAILED(s_pd3dDevice->CreateVertexShader(p_vs->GetBufferPointer(), p_vs->GetBufferSize(), 0, &s_pVS))) { Log("TRESPASS_D3D11: CreateVertexShader FAILED\n"); p_vs->Release(); p_ps->Release(); return; }
		if (FAILED(s_pd3dDevice->CreatePixelShader(p_ps->GetBufferPointer(), p_ps->GetBufferSize(), 0, &s_pPS)))  { Log("TRESPASS_D3D11: CreatePixelShader FAILED\n");  p_ps->Release(); p_vs->Release(); return; }
		p_ps->Release();

		D3D11_INPUT_ELEMENT_DESC a_elem[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		};
		hr = s_pd3dDevice->CreateInputLayout(a_elem, 3, p_vs->GetBufferPointer(), p_vs->GetBufferSize(), &s_pLayout);
		p_vs->Release();
		if (FAILED(hr)) { Log("TRESPASS_D3D11: CreateInputLayout FAILED\n"); return; }

		D3D11_BUFFER_DESC cbd; ZeroMemory(&cbd, sizeof(cbd));
		cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(s_pd3dDevice->CreateBuffer(&cbd, 0, &s_pCB))) { Log("TRESPASS_D3D11: CreateBuffer(cb) FAILED\n"); return; }

		D3D11_RASTERIZER_DESC rd; ZeroMemory(&rd, sizeof(rd));
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		if (FAILED(s_pd3dDevice->CreateRasterizerState(&rd, &s_pRaster))) { Log("TRESPASS_D3D11: CreateRasterizerState FAILED\n"); return; }

		// Anisotropic: the whole point of handing mip selection to the GPU.  A polygon seen at
		// a grazing angle is minified hard along one screen axis and barely at all along the
		// other; an isotropic filter has to pick one level for both and blurs away everything
		// on the long axis (the engine's own area-based mip choice fails the same way, which
		// is why an angled surface used to read as untextured).  Anisotropy samples along the
		// footprint's long axis and keeps that detail.
		D3D11_SAMPLER_DESC smp; ZeroMemory(&smp, sizeof(smp));
		smp.Filter        = D3D11_FILTER_ANISOTROPIC;
		smp.MaxAnisotropy = 16;
		smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		smp.MaxLOD   = D3D11_FLOAT32_MAX;
		if (FAILED(s_pd3dDevice->CreateSamplerState(&smp, &s_pSampWrap))) { Log("TRESPASS_D3D11: CreateSamplerState(wrap) FAILED\n"); return; }
		smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		if (FAILED(s_pd3dDevice->CreateSamplerState(&smp, &s_pSampClamp))) { Log("TRESPASS_D3D11: CreateSamplerState(clamp) FAILED\n"); return; }

		// Alpha blend: colour-key transparency (texel 0 -> alpha 0) and future translucency.
		// Polygons arrive back-to-front sorted, so blending in submit order is correct.
		D3D11_BLEND_DESC bld; ZeroMemory(&bld, sizeof(bld));
		bld.RenderTarget[0].BlendEnable           = TRUE;
		bld.RenderTarget[0].SrcBlend              = D3D11_BLEND_ONE;			// premultiplied alpha:
		bld.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;	// no dark edge halo
		bld.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
		bld.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
		bld.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
		bld.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
		bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		if (FAILED(s_pd3dDevice->CreateBlendState(&bld, &s_pBlend))) { Log("TRESPASS_D3D11: CreateBlendState FAILED\n"); return; }

		// Depth test: rhw (1/z) is the depth, closer = larger, so compare GREATER-EQUAL
		// (matching the 1998 aux-D3D path).  Write enabled.
		D3D11_DEPTH_STENCIL_DESC dsd; ZeroMemory(&dsd, sizeof(dsd));
		dsd.DepthEnable    = TRUE;
		dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		dsd.DepthFunc      = D3D11_COMPARISON_GREATER_EQUAL;
		if (FAILED(s_pd3dDevice->CreateDepthStencilState(&dsd, &s_pDepthState))) { Log("TRESPASS_D3D11: CreateDepthStencilState FAILED\n"); return; }

		const unsigned int u_white = 0xFFFFFFFF;
		s_pWhiteSRV = CreateSRVFromBGRA(1, 1, &u_white);
		if (!s_pWhiteSRV) { Log("TRESPASS_D3D11: white texture FAILED\n"); return; }

		// ---- Bump pipeline ------------------------------------------------------------------
		// Non-fatal: if any of this fails, bump surfaces just fall back to the main path
		// (base colour, no relief) - so guard each step but don't abort the whole pipeline.
		{
			ID3DBlob* p_bvs = 0; ID3DBlob* p_bps = 0; ID3DBlob* p_berr = 0;
			if (SUCCEEDED(D3DCompile(k_psz_bump_vs, strlen(k_psz_bump_vs), "bvs", 0, 0, "main", "vs_4_0", 0, 0, &p_bvs, &p_berr)))
			{
				if (p_berr) { p_berr->Release(); p_berr = 0; }
				if (SUCCEEDED(D3DCompile(k_psz_bump_ps, strlen(k_psz_bump_ps), "bps", 0, 0, "main", "ps_4_0", 0, 0, &p_bps, &p_berr)))
				{
					if (p_berr) { p_berr->Release(); p_berr = 0; }
					if (SUCCEEDED(s_pd3dDevice->CreateVertexShader(p_bvs->GetBufferPointer(), p_bvs->GetBufferSize(), 0, &s_pBumpVS)) &&
					    SUCCEEDED(s_pd3dDevice->CreatePixelShader (p_bps->GetBufferPointer(), p_bps->GetBufferSize(), 0, &s_pBumpPS)))
					{
						D3D11_INPUT_ELEMENT_DESC a_belem[] =
						{
							{ "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
							{ "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
							{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
							{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
							// (ambient, specular, light angular size, material sharpness)
							{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0 },
						};
						s_pd3dDevice->CreateInputLayout(a_belem, 5, p_bvs->GetBufferPointer(), p_bvs->GetBufferSize(), &s_pBumpLayout);
					}
					p_bps->Release();
				}
				else if (p_berr) { Log("TRESPASS_D3D11: bump PS compile FAILED\n"); Log((const char*)p_berr->GetBufferPointer()); p_berr->Release(); }
				p_bvs->Release();
			}
			else if (p_berr) { Log("TRESPASS_D3D11: bump VS compile FAILED\n"); Log((const char*)p_berr->GetBufferPointer()); p_berr->Release(); }

			const unsigned int u_flatnorm = 0xFF8080FF;	// 0xAARRGGBB: R=0x80,G=0x80,B=0xFF -> n=(0,0,1)
			s_pFlatNormSRV = CreateSRVFromBGRA(1, 1, &u_flatnorm);

			if (s_pBumpVS && s_pBumpPS && s_pBumpLayout && s_pFlatNormSRV)
				Log("TRESPASS_D3D11: bump pipeline built\n");
			else
				Log("TRESPASS_D3D11: bump pipeline unavailable (falling back to flat bump colour)\n");
		}

		// ---- Sky pipeline (full-screen blit of the software sky image) ----------------------
		{
			ID3DBlob* p_svs = 0; ID3DBlob* p_sps = 0; ID3DBlob* p_serr = 0;
			if (SUCCEEDED(D3DCompile(k_psz_sky_vs, strlen(k_psz_sky_vs), "svs", 0, 0, "main", "vs_4_0", 0, 0, &p_svs, &p_serr)))
			{
				if (p_serr) { p_serr->Release(); p_serr = 0; }
				if (SUCCEEDED(D3DCompile(k_psz_sky_ps, strlen(k_psz_sky_ps), "sps", 0, 0, "main", "ps_4_0", 0, 0, &p_sps, &p_serr)))
				{
					if (p_serr) { p_serr->Release(); p_serr = 0; }
					s_pd3dDevice->CreateVertexShader(p_svs->GetBufferPointer(), p_svs->GetBufferSize(), 0, &s_pSkyVS);
					s_pd3dDevice->CreatePixelShader (p_sps->GetBufferPointer(), p_sps->GetBufferSize(), 0, &s_pSkyPS);
					p_sps->Release();
				}
				else if (p_serr) { Log("TRESPASS_D3D11: sky PS compile FAILED\n"); Log((const char*)p_serr->GetBufferPointer()); p_serr->Release(); }
				p_svs->Release();
			}
			else if (p_serr) { Log("TRESPASS_D3D11: sky VS compile FAILED\n"); Log((const char*)p_serr->GetBufferPointer()); p_serr->Release(); }

			D3D11_DEPTH_STENCIL_DESC sdsd; ZeroMemory(&sdsd, sizeof(sdsd));
			sdsd.DepthEnable = FALSE; sdsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			s_pd3dDevice->CreateDepthStencilState(&sdsd, &s_pSkyDepthState);

			if (s_pSkyVS && s_pSkyPS && s_pSkyDepthState)
				Log("TRESPASS_D3D11: sky pipeline built\n");
			else
				Log("TRESPASS_D3D11: sky pipeline unavailable\n");
		}

		Log("TRESPASS_D3D11: pipeline built (slice 2b: textured)\n");
	}

	bool bEnsureVB(UINT u_needed)
	{
		if (s_pVB && s_vb_capacity >= u_needed) return true;
		if (s_pVB) { s_pVB->Release(); s_pVB = 0; }
		UINT u_cap = s_vb_capacity ? s_vb_capacity : 8192;
		while (u_cap < u_needed) u_cap *= 2;
		D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
		bd.ByteWidth = u_cap * sizeof(RenderD3D11::SVert);
		bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(s_pd3dDevice->CreateBuffer(&bd, 0, &s_pVB))) { Log("TRESPASS_D3D11: CreateBuffer(vb) FAILED\n"); s_vb_capacity = 0; s_stat_vb_fail++; return false; }
		s_vb_capacity = u_cap;
		return true;
	}

	bool bEnsureBumpVB(UINT u_needed)
	{
		if (s_pBumpVB && s_bumpvb_capacity >= u_needed) return true;
		if (s_pBumpVB) { s_pBumpVB->Release(); s_pBumpVB = 0; }
		UINT u_cap = s_bumpvb_capacity ? s_bumpvb_capacity : 4096;
		while (u_cap < u_needed) u_cap *= 2;
		D3D11_BUFFER_DESC bd; ZeroMemory(&bd, sizeof(bd));
		bd.ByteWidth = u_cap * sizeof(RenderD3D11::SBumpVert);
		bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(s_pd3dDevice->CreateBuffer(&bd, 0, &s_pBumpVB))) { Log("TRESPASS_D3D11: CreateBuffer(bump vb) FAILED\n"); s_bumpvb_capacity = 0; return false; }
		s_bumpvb_capacity = u_cap;
		return true;
	}

	void Shutdown_Internal()
	{
		if (s_pd3dContext) s_pd3dContext->ClearState();
		for (std::unordered_map<const void*, ID3D11ShaderResourceView*>::iterator it = s_tex_cache.begin(); it != s_tex_cache.end(); ++it)
			if (it->second) it->second->Release();
		s_tex_cache.clear();
		for (std::unordered_map<const void*, ID3D11ShaderResourceView*>::iterator it = s_norm_cache.begin(); it != s_norm_cache.end(); ++it)
			if (it->second) it->second->Release();
		s_norm_cache.clear();
		for (std::unordered_map<const void*, SDynTex>::iterator it = s_dyn_cache.begin(); it != s_dyn_cache.end(); ++it)
		{
			if (it->second.pSRV) it->second.pSRV->Release();
			if (it->second.pTex) it->second.pTex->Release();
		}
		s_dyn_cache.clear();
		if (s_pSkyImgSRV)     { s_pSkyImgSRV->Release();     s_pSkyImgSRV     = 0; }
		if (s_pSkyImgTex)     { s_pSkyImgTex->Release();     s_pSkyImgTex     = 0; s_sky_img_w = s_sky_img_h = 0; }
		if (s_pSkyDepthState) { s_pSkyDepthState->Release(); s_pSkyDepthState = 0; }
		if (s_pSkyPS)         { s_pSkyPS->Release();         s_pSkyPS         = 0; }
		if (s_pSkyVS)         { s_pSkyVS->Release();         s_pSkyVS         = 0; }
		if (s_pFlatNormSRV)   { s_pFlatNormSRV->Release();   s_pFlatNormSRV   = 0; }
		if (s_pBumpVB)        { s_pBumpVB->Release();        s_pBumpVB        = 0; }
		if (s_pBumpLayout)    { s_pBumpLayout->Release();    s_pBumpLayout    = 0; }
		if (s_pBumpPS)        { s_pBumpPS->Release();        s_pBumpPS        = 0; }
		if (s_pBumpVS)        { s_pBumpVS->Release();        s_pBumpVS        = 0; }
		if (s_pWhiteSRV)      { s_pWhiteSRV->Release();      s_pWhiteSRV      = 0; }
		if (s_pBlend)         { s_pBlend->Release();         s_pBlend         = 0; }
		if (s_pSampClamp)     { s_pSampClamp->Release();     s_pSampClamp     = 0; }
		if (s_pSampWrap)      { s_pSampWrap->Release();      s_pSampWrap      = 0; }
		if (s_pRaster)        { s_pRaster->Release();        s_pRaster        = 0; }
		if (s_pCB)            { s_pCB->Release();            s_pCB            = 0; }
		if (s_pVB)            { s_pVB->Release();            s_pVB            = 0; }
		if (s_pLayout)        { s_pLayout->Release();        s_pLayout        = 0; }
		if (s_pPS)            { s_pPS->Release();            s_pPS            = 0; }
		if (s_pVS)            { s_pVS->Release();            s_pVS            = 0; }
		if (s_pDepthState)    { s_pDepthState->Release();    s_pDepthState    = 0; }
		if (s_pDSV)           { s_pDSV->Release();           s_pDSV           = 0; }
		if (s_pDepthTex)      { s_pDepthTex->Release();      s_pDepthTex      = 0; }
		if (s_pBackBufferRTV) { s_pBackBufferRTV->Release(); s_pBackBufferRTV = 0; }
		if (s_pSwapChain)     { s_pSwapChain->Release();     s_pSwapChain     = 0; }
		if (s_pd3dContext)    { s_pd3dContext->Release();    s_pd3dContext    = 0; }
		if (s_pd3dDevice)     { s_pd3dDevice->Release();     s_pd3dDevice     = 0; }
		s_vb_capacity = 0; s_bumpvb_capacity = 0; s_b_active = false; s_b_pipeline = false; s_frame_open = false;
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

	void SetClearColour(float f_r, float f_g, float f_b)
	{
		s_clear_r = f_r; s_clear_g = f_g; s_clear_b = f_b;
	}

	//
	// b_565: the source is the engine's own 16-bit 565 raster, uploaded verbatim (see
	// UpdateDynamicTexture_).  The sky image is screen-sized, so converting it to BGRA on the
	// CPU cost ~10 ms/frame on its own - a per-pixel price for a blit the GPU can do from 565.
	// i_src_pitch is the source row stride in BYTES.
	//
	void SetSkyImage_(int i_w, int i_h, const void* pv_src, int i_src_pitch, bool b_565)
	{
		if (!s_pd3dDevice || i_w < 1 || i_h < 1) return;

		// (Re)create the dynamic sky texture on first use, or a resolution/format change.
		if (!s_pSkyImgTex || s_sky_img_w != i_w || s_sky_img_h != i_h || s_b_sky_565 != b_565)
		{
			if (s_pSkyImgSRV) { s_pSkyImgSRV->Release(); s_pSkyImgSRV = 0; }
			if (s_pSkyImgTex) { s_pSkyImgTex->Release(); s_pSkyImgTex = 0; }
			D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
			td.Width = i_w; td.Height = i_h; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = b_565 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(s_pd3dDevice->CreateTexture2D(&td, 0, &s_pSkyImgTex)) || !s_pSkyImgTex) { s_pSkyImgTex = 0; return; }
			if (FAILED(s_pd3dDevice->CreateShaderResourceView(s_pSkyImgTex, 0, &s_pSkyImgSRV))) { s_pSkyImgTex->Release(); s_pSkyImgTex = 0; return; }
			s_sky_img_w = i_w; s_sky_img_h = i_h; s_b_sky_565 = b_565;
		}

		// Upload this frame's sky pixels (row by row - the map pitch may exceed the row).
		D3D11_MAPPED_SUBRESOURCE ms;
		if (SUCCEEDED(s_pd3dContext->Map(s_pSkyImgTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
		{
			const unsigned char* p_src = (const unsigned char*)pv_src;
			unsigned char*       p_dst = (unsigned char*)ms.pData;
			size_t               u_row = (size_t)i_w * (b_565 ? 2 : 4);
			for (int y = 0; y < i_h; ++y)
				memcpy(p_dst + (size_t)y * ms.RowPitch, p_src + (size_t)y * i_src_pitch, u_row);
			s_pd3dContext->Unmap(s_pSkyImgTex, 0);
		}
		s_sky_valid = true;
	}

	void SetSkyImage(int i_w, int i_h, const unsigned int* pu4_bgra)
	{
		SetSkyImage_(i_w, i_h, pu4_bgra, i_w * 4, false);
	}

	void SetSkyImage565(int i_w, int i_h, const void* pv_565, int i_src_pitch)
	{
		SetSkyImage_(i_w, i_h, pv_565, i_src_pitch, true);
	}

	void* GetTexture(const void* p_key)
	{
		std::unordered_map<const void*, ID3D11ShaderResourceView*>::iterator it = s_tex_cache.find(p_key);
		return it != s_tex_cache.end() ? (void*)it->second : 0;
	}

	void* CreateTexture(const void* p_key, int i_width, int i_height, const unsigned int* pu4_bgra)
	{
		if (!s_pd3dDevice || i_width < 1 || i_height < 1) { s_tex_cache[p_key] = 0; return 0; }
		ID3D11ShaderResourceView* p_srv = CreateSRVFromBGRA(i_width, i_height, pu4_bgra, true);
		if (!p_srv)
		{
			char sz[160];
			wsprintfA(sz, "TRESPASS_D3D11: CreateTexture FAILED %dx%d\n", i_width, i_height);
			Log(sz);
			FILE* f = fopen("d3d11_texfail.txt", "a");	// dims of the offending texture, for diagnosis
			if (f) { fprintf(f, "CreateTexture FAILED %dx%d key=%p\n", i_width, i_height, p_key); fclose(f); }
		}
		s_tex_cache[p_key] = p_srv;		// cache even null, so we don't retry a bad texture every frame
		return (void*)p_srv;
	}

	bool bTextureKnown(const void* p_key)
	{
		return s_tex_cache.find(p_key) != s_tex_cache.end();
	}

	void PurgeTextures()
	{
		// Every cache here is keyed by a raw engine address (a CRaster or CTexture), and an
		// address only means anything while the object at it is alive.  A level teardown frees
		// all of them at once and the next level's objects can land on the SAME addresses, so
		// entries left behind would hand a new texture the OLD level's image.  The caches are
		// also grow-only, so the old level's GPU memory would never be released either.
		// Dropping the lot at teardown fixes both; textures simply re-upload on first sight,
		// exactly as they did when the level was first entered.
		//
		// Safe to call at any time, including when D3D11 is disabled or was never initialised
		// (the caches are then empty).  Releasing an SRV that is still bound to the context is
		// fine - D3D11 holds its own reference - but the batch lists below hold RAW pointers,
		// so they must be dropped or Present could draw from freed memory.
		s_verts.clear();
		s_batches.clear();
		s_bump_verts.clear();
		s_bump_batches.clear();

		for (std::unordered_map<const void*, ID3D11ShaderResourceView*>::iterator it = s_tex_cache.begin(); it != s_tex_cache.end(); ++it)
			if (it->second) it->second->Release();
		s_tex_cache.clear();

		for (std::unordered_map<const void*, ID3D11ShaderResourceView*>::iterator it = s_norm_cache.begin(); it != s_norm_cache.end(); ++it)
			if (it->second) it->second->Release();
		s_norm_cache.clear();

		for (std::unordered_map<const void*, SDynTex>::iterator it = s_dyn_cache.begin(); it != s_dyn_cache.end(); ++it)
		{
			if (it->second.pSRV) it->second.pSRV->Release();
			if (it->second.pTex) it->second.pTex->Release();
		}
		s_dyn_cache.clear();
	}

	void MarkTextureFailed(const void* p_key)
	{
		s_tex_cache[p_key] = 0;
	}

	bool bHiResEnabled()
	{
		if (s_i_hires < 0)
			s_i_hires = GetEnvironmentVariableA("TRESPASS_HIRES", 0, 0) > 0 ? 1 : 0;
		return s_i_hires != 0;
	}

	void* CreateTextureHiRes(const void* p_key, unsigned int u4_hash)
	{
		if (!s_pd3dDevice || !bHiResEnabled())
			return 0;

		char sz_path[MAX_PATH];
		wsprintfA(sz_path, "hires\\%08lX.bmp", (unsigned long)u4_hash);

		std::vector<unsigned int> bgra;
		int i_w = 0, i_h = 0;
		if (!LoadBmp24ToBGRA(sz_path, bgra, i_w, i_h))
			return 0;						// no asset - caller decodes the stock texture

		ID3D11ShaderResourceView* p_srv = CreateSRVFromBGRA(i_w, i_h, &bgra[0], true);
		if (!p_srv)
			return 0;
		s_tex_cache[p_key] = p_srv;
		return (void*)p_srv;
	}

	void* GetNormalTexture(const void* p_key)
	{
		std::unordered_map<const void*, ID3D11ShaderResourceView*>::iterator it = s_norm_cache.find(p_key);
		return it != s_norm_cache.end() ? (void*)it->second : 0;
	}

	void* CreateNormalTexture(const void* p_key, int i_width, int i_height, const unsigned int* pu4_rgb_normal)
	{
		if (!s_pd3dDevice || i_width < 1 || i_height < 1) return 0;
		ID3D11ShaderResourceView* p_srv = CreateSRVFromBGRA(i_width, i_height, pu4_rgb_normal);
		s_norm_cache[p_key] = p_srv;		// cache even null so we don't retry a bad texture
		return (void*)p_srv;
	}

	void* GetDynamicTexture(const void* p_key)
	{
		// Returns the dynamic SRV only if it was already uploaded THIS frame (so the caller can
		// skip re-decoding a terrain page shared by several polygons); null otherwise.
		std::unordered_map<const void*, SDynTex>::iterator it = s_dyn_cache.find(p_key);
		if (it != s_dyn_cache.end() && it->second.u_frame == s_frame_no && it->second.pSRV)
			return (void*)it->second.pSRV;
		return 0;
	}

	//
	// Shared body for the dynamic-texture upload.  b_565 selects DXGI_FORMAT_B5G6R5_UNORM and
	// a 2-byte texel (the engine's own 16-bit 565 raster, copied verbatim); otherwise BGRA and
	// 4 bytes.  i_src_pitch is the source row stride in BYTES.
	//
	void* UpdateDynamicTexture_(const void* p_key, int i_width, int i_height,
	                            const void* pv_src, int i_src_pitch, bool b_565)
	{
		if (!s_pd3dDevice || i_width < 1 || i_height < 1) return 0;

		SDynTex& dt = s_dyn_cache[p_key];		// inserts a zeroed entry on first sight

		// (Re)create the GPU texture if it doesn't exist, or the size or FORMAT changed.
		if (!dt.pTex || dt.i_w != i_width || dt.i_h != i_height || dt.b_565 != b_565)
		{
			if (dt.pSRV) { dt.pSRV->Release(); dt.pSRV = 0; }
			if (dt.pTex) { dt.pTex->Release(); dt.pTex = 0; }

			D3D11_TEXTURE2D_DESC td; ZeroMemory(&td, sizeof(td));
			td.Width = i_width; td.Height = i_height; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = b_565 ? DXGI_FORMAT_B5G6R5_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DYNAMIC;
			td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(s_pd3dDevice->CreateTexture2D(&td, 0, &dt.pTex)) || !dt.pTex) { dt.pTex = 0; return 0; }
			if (FAILED(s_pd3dDevice->CreateShaderResourceView(dt.pTex, 0, &dt.pSRV))) { dt.pTex->Release(); dt.pTex = 0; return 0; }
			dt.i_w = i_width; dt.i_h = i_height; dt.b_565 = b_565;
		}

		// Upload this dynamic texture at most once per frame - terrain calls this per polygon,
		// so a page shared by many polys was being re-decoded + re-uploaded many times a frame.
		if (dt.u_frame == s_frame_no && dt.pSRV)
			return (void*)dt.pSRV;
		dt.u_frame = s_frame_no;

		int i_texel = b_565 ? 2 : 4;
		s_stat_dyn_bytes += (unsigned int)i_width * (unsigned int)i_height * (unsigned int)i_texel;

		// Upload this frame's pixels (row by row - the map pitch may exceed the row's bytes).
		D3D11_MAPPED_SUBRESOURCE ms;
		if (SUCCEEDED(s_pd3dContext->Map(dt.pTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
		{
			const unsigned char* p_src = (const unsigned char*)pv_src;
			unsigned char*       p_dst = (unsigned char*)ms.pData;
			size_t               u_row = (size_t)i_width * i_texel;
			for (int y = 0; y < i_height; ++y)
				memcpy(p_dst + (size_t)y * ms.RowPitch, p_src + (size_t)y * i_src_pitch, u_row);
			s_pd3dContext->Unmap(dt.pTex, 0);
		}
		return (void*)dt.pSRV;
	}

	void* UpdateDynamicTexture(const void* p_key, int i_width, int i_height, const unsigned int* pu4_bgra)
	{
		return UpdateDynamicTexture_(p_key, i_width, i_height, pu4_bgra, i_width * 4, false);
	}

	void* UpdateDynamicTexture565(const void* p_key, int i_width, int i_height,
	                              const void* pv_565, int i_src_pitch)
	{
		return UpdateDynamicTexture_(p_key, i_width, i_height, pv_565, i_src_pitch, true);
	}

	bool b565DirectSupported()
	{
		// DXGI_FORMAT_B5G6R5_UNORM is a DXGI 1.2 format - effectively universal on feature
		// level 11 hardware, but cheap to verify once rather than assume.
		static int s_i_565 = -1;
		if (s_i_565 < 0 && s_pd3dDevice)
		{
			UINT u_sup = 0;
			s_i_565 = (SUCCEEDED(s_pd3dDevice->CheckFormatSupport(DXGI_FORMAT_B5G6R5_UNORM, &u_sup)) &&
			           (u_sup & D3D11_FORMAT_SUPPORT_TEXTURE2D)) ? 1 : 0;
			Log(s_i_565 ? "TRESPASS_D3D11: B5G6R5 supported - direct 565 upload\n"
			            : "TRESPASS_D3D11: B5G6R5 unsupported - CPU converts to BGRA\n");
		}
		return s_i_565 == 1;
	}

	bool bBeginFrame(int i_width, int i_height)
	{
		if (!bEnabled()) return false;
		if (!s_b_init_tried) TryInit();
		if (!s_b_active)     return false;
		if (!s_b_pipeline)   TryBuildPipeline();
		if (!s_pVS || !s_pPS || !s_pLayout || !s_pCB || !s_pRaster ||
		    !s_pSampWrap || !s_pSampClamp || !s_pBlend || !s_pWhiteSRV ||
		    !s_pDSV || !s_pDepthState)
			return false;

		// The engine calls DrawPolygons (hence bBeginFrame) more than once per frame - a main
		// scene pass and one or more extra passes (e.g. terrain).  Clear + set up state ONCE per
		// frame and accumulate every pass's polygons; the actual draw + swap-chain Present then
		// happens once, at CRasterWin::Flip (the true frame boundary).  Clearing/presenting per
		// pass meant the last (possibly tiny) pass wiped the scene -> the "white-out".
		if (!s_frame_open)
		{
			s_verts.clear();
			s_batches.clear();
			s_bump_verts.clear();
			s_bump_batches.clear();
			s_stat_dyn_bytes = 0;
			++s_frame_no;

			s_pd3dContext->OMSetRenderTargets(1, &s_pBackBufferRTV, s_pDSV);
			s_pd3dContext->OMSetDepthStencilState(s_pDepthState, 0);

			const float af_blend[4] = { 0, 0, 0, 0 };
			s_pd3dContext->OMSetBlendState(s_pBlend, af_blend, 0xFFFFFFFF);

			float f_scale = (float)s_u_width / (float)i_width;
			float f_sh    = (float)s_u_height / (float)i_height;
			if (f_sh < f_scale) f_scale = f_sh;

			D3D11_VIEWPORT vp;
			vp.Width  = i_width * f_scale; vp.Height = i_height * f_scale;
			vp.TopLeftX = (s_u_width - vp.Width) * 0.5f; vp.TopLeftY = (s_u_height - vp.Height) * 0.5f;
			vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
			s_pd3dContext->RSSetViewports(1, &vp);
			s_pd3dContext->RSSetState(s_pRaster);

			const float af_clear[4] = { s_clear_r, s_clear_g, s_clear_b, 1.0f };
			s_pd3dContext->ClearRenderTargetView(s_pBackBufferRTV, af_clear);
			// Depth cleared to 0 (far); closer fragments have larger rhw and win via GEQUAL.
			s_pd3dContext->ClearDepthStencilView(s_pDSV, D3D11_CLEAR_DEPTH, 0.0f, 0);

			D3D11_MAPPED_SUBRESOURCE ms;
			if (SUCCEEDED(s_pd3dContext->Map(s_pCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			{
				static int s_i_bumpdebug = -1;
				if (s_i_bumpdebug < 0)
					s_i_bumpdebug = GetEnvironmentVariableA("TRESPASS_BUMPDEBUG", 0, 0) > 0 ? 1 : 0;
				float* pf = (float*)ms.pData;
				pf[0] = 2.0f / (float)i_width; pf[1] = 2.0f / (float)i_height;
				pf[2] = (float)s_i_bumpdebug; pf[3] = 0.0f;
				s_pd3dContext->Unmap(s_pCB, 0);
			}

			s_frame_open = true;
		}
		return true;
	}

	void SubmitPolygon(const SVert* pav_verts, int i_count, void* p_texture, bool b_clamp)
	{
		if (i_count < 3) return;

		ID3D11ShaderResourceView* p_srv = p_texture ? (ID3D11ShaderResourceView*)p_texture : s_pWhiteSRV;
		UINT u_before = (UINT)s_verts.size();

		for (int k = 2; k < i_count; ++k)
		{
			s_verts.push_back(pav_verts[0]);
			s_verts.push_back(pav_verts[k - 1]);
			s_verts.push_back(pav_verts[k]);
		}
		UINT u_added = (UINT)s_verts.size() - u_before;
		if (!u_added) return;

		// Extend the current batch if it uses the same texture+sampler, else start a new one.
		if (!s_batches.empty() && s_batches.back().pSRV == p_srv && s_batches.back().b_clamp == b_clamp)
			s_batches.back().u_count += u_added;
		else
		{
			SBatch b; b.pSRV = p_srv; b.u_start = u_before; b.u_count = u_added; b.b_clamp = b_clamp;
			s_batches.push_back(b);
		}
	}

	void SubmitBumpPolygon(const SBumpVert* pav_verts, int i_count, void* p_colour, void* p_normal, bool b_clamp)
	{
		if (i_count < 3 || !s_pBumpVS || !s_pBumpLayout) return;

		ID3D11ShaderResourceView* p_col = p_colour ? (ID3D11ShaderResourceView*)p_colour : s_pWhiteSRV;
		ID3D11ShaderResourceView* p_nrm = p_normal ? (ID3D11ShaderResourceView*)p_normal : s_pFlatNormSRV;
		UINT u_before = (UINT)s_bump_verts.size();

		for (int k = 2; k < i_count; ++k)
		{
			s_bump_verts.push_back(pav_verts[0]);
			s_bump_verts.push_back(pav_verts[k - 1]);
			s_bump_verts.push_back(pav_verts[k]);
		}
		UINT u_added = (UINT)s_bump_verts.size() - u_before;
		if (!u_added) return;

		if (!s_bump_batches.empty() && s_bump_batches.back().pCol == p_col &&
		    s_bump_batches.back().pNrm == p_nrm && s_bump_batches.back().b_clamp == b_clamp)
			s_bump_batches.back().u_count += u_added;
		else
		{
			SBumpBatch b; b.pCol = p_col; b.pNrm = p_nrm; b.u_start = u_before; b.u_count = u_added; b.b_clamp = b_clamp;
			s_bump_batches.push_back(b);
		}
	}

	bool bFrameOpen()
	{
		return s_frame_open;
	}

	void Present()
	{
		if (!bActive() || !s_pSwapChain) return;
		// Called once per frame at CRasterWin::Flip.  If no frame was opened (no main-screen
		// pass this frame, e.g. a menu-only frame drawn by the old GDI path), don't present -
		// leave the last frame on screen rather than showing an uncleared/garbage backbuffer.
		if (!s_frame_open) return;

		// Sky pass first, behind everything (no depth test/write): a 1:1 blit of the software
		// sky image captured this frame.  Geometry draws over it.
		if (s_sky_valid && s_pSkyVS && s_pSkyPS && s_pSkyDepthState && s_pSkyImgSRV)
		{
			s_pd3dContext->OMSetDepthStencilState(s_pSkyDepthState, 0);
			s_pd3dContext->IASetInputLayout(0);
			ID3D11Buffer* p_novb = 0; UINT u_z0 = 0, u_z1 = 0;
			s_pd3dContext->IASetVertexBuffers(0, 1, &p_novb, &u_z0, &u_z1);
			s_pd3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			s_pd3dContext->VSSetShader(s_pSkyVS, 0, 0);
			s_pd3dContext->PSSetShader(s_pSkyPS, 0, 0);
			s_pd3dContext->PSSetSamplers(0, 1, &s_pSampClamp);
			s_pd3dContext->PSSetShaderResources(0, 1, &s_pSkyImgSRV);
			s_pd3dContext->Draw(3, 0);
			s_pd3dContext->OMSetDepthStencilState(s_pDepthState, 0);	// restore for geometry
		}

		if (!s_verts.empty() && bEnsureVB((UINT)s_verts.size()))
		{
			D3D11_MAPPED_SUBRESOURCE ms;
			if (SUCCEEDED(s_pd3dContext->Map(s_pVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			{
				memcpy(ms.pData, &s_verts[0], s_verts.size() * sizeof(SVert));
				s_pd3dContext->Unmap(s_pVB, 0);

				UINT u_stride = sizeof(SVert), u_offset = 0;
				s_pd3dContext->IASetInputLayout(s_pLayout);
				s_pd3dContext->IASetVertexBuffers(0, 1, &s_pVB, &u_stride, &u_offset);
				s_pd3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				s_pd3dContext->VSSetShader(s_pVS, 0, 0);
				s_pd3dContext->VSSetConstantBuffers(0, 1, &s_pCB);
				s_pd3dContext->PSSetShader(s_pPS, 0, 0);

				for (size_t i = 0; i < s_batches.size(); ++i)
				{
					ID3D11SamplerState* p_samp = s_batches[i].b_clamp ? s_pSampClamp : s_pSampWrap;
					s_pd3dContext->PSSetSamplers(0, 1, &p_samp);
					s_pd3dContext->PSSetShaderResources(0, 1, &s_batches[i].pSRV);
					s_pd3dContext->Draw(s_batches[i].u_count, s_batches[i].u_start);
				}
			}
		}

		// Bump pass: object-space normal mapping.  Opaque/cutout (the PS clips holes) and the
		// depth buffer resolves ordering, so drawing after the main batches is fine.
		if (!s_bump_verts.empty() && s_pBumpVS && s_pBumpLayout && bEnsureBumpVB((UINT)s_bump_verts.size()))
		{
			D3D11_MAPPED_SUBRESOURCE ms;
			if (SUCCEEDED(s_pd3dContext->Map(s_pBumpVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			{
				memcpy(ms.pData, &s_bump_verts[0], s_bump_verts.size() * sizeof(SBumpVert));
				s_pd3dContext->Unmap(s_pBumpVB, 0);

				UINT u_stride = sizeof(SBumpVert), u_offset = 0;
				s_pd3dContext->IASetInputLayout(s_pBumpLayout);
				s_pd3dContext->IASetVertexBuffers(0, 1, &s_pBumpVB, &u_stride, &u_offset);
				s_pd3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				s_pd3dContext->VSSetShader(s_pBumpVS, 0, 0);
				s_pd3dContext->VSSetConstantBuffers(0, 1, &s_pCB);
				s_pd3dContext->PSSetShader(s_pBumpPS, 0, 0);
				s_pd3dContext->PSSetConstantBuffers(0, 1, &s_pCB);		// for the debug-tint flag

				for (size_t i = 0; i < s_bump_batches.size(); ++i)
				{
					ID3D11SamplerState* p_samp = s_bump_batches[i].b_clamp ? s_pSampClamp : s_pSampWrap;
					s_pd3dContext->PSSetSamplers(0, 1, &p_samp);
					ID3D11ShaderResourceView* ap_srv[2] = { s_bump_batches[i].pCol, s_bump_batches[i].pNrm };
					s_pd3dContext->PSSetShaderResources(0, 2, ap_srv);
					s_pd3dContext->Draw(s_bump_batches[i].u_count, s_bump_batches[i].u_start);
				}
			}
		}

		HRESULT hr_present = s_pSwapChain->Present(0, 0);
		s_frame_open = false;			// frame consumed; next bBeginFrame starts a fresh one
		s_sky_valid  = false;			// require SetSkyImage again next frame (else no sky)

		// ---- Diagnostic: peak vertex/upload counts + device-removed state -------------------
		if ((unsigned int)s_verts.size()      > s_stat_max_verts)     s_stat_max_verts     = (unsigned int)s_verts.size();
		if ((unsigned int)s_bump_verts.size() > s_stat_max_bumpverts) s_stat_max_bumpverts = (unsigned int)s_bump_verts.size();
		if (s_stat_dyn_bytes > s_stat_max_dyn_bytes) s_stat_max_dyn_bytes = s_stat_dyn_bytes;
		if (s_stat_dev_removed == 0)
		{
			HRESULT hr_rem = s_pd3dDevice->GetDeviceRemovedReason();
			if (FAILED(hr_present)) s_stat_dev_removed = (long)hr_present;
			else if (hr_rem != S_OK) s_stat_dev_removed = (long)hr_rem;
		}
		static int s_i_stats = -1;
		if (s_i_stats < 0)
			s_i_stats = GetEnvironmentVariableA("TRESPASS_RENDERSTATS", 0, 0) > 0 ? 1 : 0;
		if (s_i_stats && ++s_stat_frames >= 30)
		{
			s_stat_frames = 0;
			FILE* f = fopen("d3d11_stats.txt", "w");
			if (f)
			{
				fprintf(f, "cur_verts=%u peak_verts=%u  cur_bumpverts=%u peak_bumpverts=%u\n",
				        (unsigned int)s_verts.size(), s_stat_max_verts,
				        (unsigned int)s_bump_verts.size(), s_stat_max_bumpverts);
				fprintf(f, "cur_batches=%u bump_batches=%u  dyn_bytes=%u peak_dyn_bytes=%u\n",
				        (unsigned int)s_batches.size(), (unsigned int)s_bump_batches.size(),
				        s_stat_dyn_bytes, s_stat_max_dyn_bytes);
				fprintf(f, "vb_fail=%d  device_removed_reason=0x%08lX\n", s_stat_vb_fail, s_stat_dev_removed);
				fclose(f);
			}
		}
	}

	void Shutdown() { Shutdown_Internal(); }
}
