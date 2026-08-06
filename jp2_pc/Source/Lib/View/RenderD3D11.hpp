/***********************************************************************************************
 *
 * Copyright (c) 2026.  Experimental modernization.
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
	// Set the backbuffer clear colour (0..1).  The engine's sky is drawn into the software
	// raster, not the polygon stream the GPU path mirrors, so the caller passes the sky's
	// horizon/fog colour here each frame to stand in for the (not-yet-GPU) sky.  If never
	// called, a neutral default is used.
	//
	void SetClearColour(float f_r, float f_g, float f_b);

	//******************************************************************************************
	//
	// Supply this frame's sky as a finished screen-space image (BGRA, one 0xAARRGGBB texel per
	// pixel, top-down, i_w x i_h).  The engine's software renderer already draws a correct cloud
	// sky into the main raster each frame before any geometry; the caller captures that and hands
	// it here, and the backend blits it full-screen behind the geometry (a 1:1 copy - no
	// reprojection, so no streaking).  Call each frame before Present; a frame that omits it
	// draws no sky (the clear colour shows).
	//
	void SetSkyImage(int i_width, int i_height, const unsigned int* pu4_bgra);

	//******************************************************************************************
	//
	// Texture cache.  Kept engine-agnostic: the caller converts the engine texture to a plain
	// BGRA image (one 0xAARRGGBB texel each) and supplies a stable key pointer (the CTexture
	// address).  GetTexture returns the cached opaque handle for p_key, or null if not created
	// yet.  CreateTexture uploads the image, caches it under p_key, and returns the handle
	// (pass it as SubmitPolygon's p_texture).  A null handle there means "untextured" (the
	// module binds a 1x1 white texture, so the vertex colour shows through).
	//
	void* GetTexture(const void* p_key);
	void* CreateTexture(const void* p_key, int i_width, int i_height, const unsigned int* pu4_bgra);

	// True once a key has been through CreateTexture (even if the upload FAILED and the cached
	// handle is null).  The caller uses this to avoid re-decoding + re-uploading a failed
	// texture every frame - a large failing texture that way causes a white object plus a
	// continuous per-frame stutter.  MarkTextureFailed records a known-failed key without an
	// upload attempt (e.g. for a texture too large to be safe on the GPU).
	bool bTextureKnown(const void* p_key);
	void MarkTextureFailed(const void* p_key);

	// Release every cached texture and drop all cache entries.  MUST be called when the engine
	// tears a level down (CWorld::Purge): the caches are keyed by raw CRaster/CTexture
	// addresses, which the next level can reuse for different textures - stale entries would
	// then draw the previous level's images - and, being grow-only, would otherwise never
	// release the old level's GPU memory.  Safe to call when D3D11 is disabled or was never
	// initialised.  Textures re-upload on first sight afterwards.
	void PurgeTextures();

	//******************************************************************************************
	//
	// HI-RES TEXTURE SIDE-LOAD (env TRESPASS_HIRES).  The GPU samples normalised [0,1] UVs,
	// so an upscaled image can be displayed for a texture with no change to geometry or UVs,
	// and - unlike the CPU rasteriser - regardless of the 256 tiling cap (the wrap sampler
	// handles any resolution).  bHiResEnabled() reports whether the feature is on (cached).
	// CreateTextureHiRes looks for hires\<u4_hash>.bmp (a 24-bit BMP whose name is the
	// exporter's FNV-1a content hash of the source texture); if present it uploads that image
	// at its native resolution, caches it under p_key, and returns the handle.  Returns null
	// (and caches nothing) when the feature is off or no hi-res asset exists, so the caller
	// falls back to the normal texture decode.
	//
	bool  bHiResEnabled();
	void* CreateTextureHiRes(const void* p_key, unsigned int u4_hash);

	// For DYNAMIC textures (terrain atlas pages) whose contents change every frame and
	// whose CTexture objects are recycled: re-uploads the pixels into a persistent dynamic
	// GPU texture (created/resized as needed) and returns its handle.  Call every frame.
	void* UpdateDynamicTexture(const void* p_key, int i_width, int i_height, const unsigned int* pu4_bgra);

	//******************************************************************************************
	//
	// DIRECT 565 UPLOAD.  The engine's dynamic rasters (terrain pages, water) and its sky image
	// are 16-bit 565 in memory, which is exactly DXGI_FORMAT_B5G6R5_UNORM - so their rows can be
	// memcpy'd to the GPU with no conversion at all.  Converting them to BGRA per texel on the
	// CPU instead was costing ~25 ms/frame (10.4 MB of dynamic upload EVERY frame, measured with
	// TRESPASS_RENDERSTATS), and doubled the bytes moved for nothing.  i_src_pitch is the source
	// row stride in BYTES (a raster's row may be padded - use CRaster::iLineBytes()).
	//
	// Only call these when b565DirectSupported() AND the source really is 565: the caller must
	// check the raster's pixel format, not assume it.
	//
	//******************************************************************************************
	//
	// Read the last presented frame back into a 16-bit 565 buffer (i_dst_pitch = destination
	// row stride in BYTES).  Returns false if nothing has been presented yet, or the read-back
	// failed - the caller should leave its buffer alone in that case.
	//
	// For the pause menu, whose background is a darkened snapshot of the live frame: it used to
	// BitBlt that out of the software back buffer, which no longer holds the scene now that the
	// software rasteriser is skipped.  Only the scene's pillar-boxed viewport is returned (not
	// the black bars), rescaled if the destination is a different size.
	//
	// Costs a GPU->CPU read-back, so it is for one-off use (opening a menu), never per frame.
	//
	bool  bCaptureBackBuffer565(int i_w, int i_h, void* pv_dst_565, int i_dst_pitch);

	bool  b565DirectSupported();
	void* UpdateDynamicTexture565(const void* p_key, int i_width, int i_height,
	                              const void* pv_565, int i_src_pitch);
	void  SetSkyImage565(int i_w, int i_h, const void* pv_565, int i_src_pitch);

	// Returns a dynamic texture's handle only if it was already uploaded this frame, else null.
	// Lets the caller skip re-decoding a terrain page that several polygons share.
	void* GetDynamicTexture(const void* p_key);

	//******************************************************************************************
	//
	// BUMP / NORMAL MAPPING.  Bump surfaces carry, per texel, a base colour and a decoded
	// object-space surface normal; the engine hands each polygon its light already in the
	// surface's object/texture space (CRenderPolygon::Bump.d3Light), so a plain dot(N,L)
	// reproduces the engine's bump lighting with no per-vertex tangent frame.  This is a
	// separate pipeline (own vertex format + shaders + normal-map SRV) so the main textured
	// path is untouched.
	//
	struct SBumpVert
	{
		float        fSX, fSY, fSZ, fInvW;	// Screen pos + rhw (as SVert).
		unsigned int u4Color;				// Unused for bump (kept for layout parity).
		float        fU, fV;				// Texture coordinates, [0,1].
		float        fLx, fLy, fLz;			// Object/texture-space light direction (unit).
		float        fLStrength;			// Directional light strength.
		float        fLAmbient;				// Ambient light term.
		float        fSpecular;				// Material specular intensity (rvSpecular; 0 = matte).

		// The engine's specular falloff is a linear ramp between two cone COSINES, not a
		// Phong lobe: full strength at cos >= fAngwSize (the light's angular size), zero
		// below fAngwSpecular * fAngwSize, linear between (CMaterial::fSpecular ->
		// fAngularStrength, Material.hpp).  Both are cosines, so 1 = a zero-width angle.
		float        fAngwSize;				// Cosine of the light's angular size.
		float        fAngwSpecular;			// Material's specular sharpness (1 = mirror).
	};

	// Normal-map SRV cache, parallel to the colour texture cache and keyed the same way
	// (the CTexture address).  The normal map is decoded once (object-space normals encoded
	// into RGB) and reused; only the per-polygon light varies (carried per SBumpVert).
	void* GetNormalTexture(const void* p_key);
	void* CreateNormalTexture(const void* p_key, int i_width, int i_height, const unsigned int* pu4_rgb_normal);

	// Submit a bump polygon (triangle fan) with its colour SRV (t0) and normal SRV (t1).
	void SubmitBumpPolygon(const SBumpVert* pav_verts, int i_count, void* p_colour, void* p_normal, bool b_clamp);

	//******************************************************************************************
	//
	// Submit one screen-space polygon (triangle fan of i_count vertices) with its texture.
	//
	// SLICE 2 (not yet implemented): append to a dynamic vertex buffer and issue the draw
	// through the passthrough vertex shader + textured pixel shader.  Stubbed no-op for now
	// so the device/swap-chain/present infrastructure can be brought up and verified first.
	//
	// b_clamp selects clamp (vs wrap) texture addressing - set it for non-tileable
	// textures so their UVs don't repeat.  b_terrain marks landscape polygons, so the hex-tiling
	// (detile) blend can be limited to the terrain and not applied to other tiling surfaces such as
	// fences.
	void SubmitPolygon(const SVert* pav_verts, int i_count, void* p_texture, bool b_clamp, bool b_terrain = false);

	//******************************************************************************************
	//
	// STEREO (see RenderVR.hpp).  The engine renders the scene once per eye - the CPU pipeline
	// bakes the projection in, so each eye is a separate pass - and all those passes accumulate
	// into the SAME frame here, which is presented once.  SetEye tells the backend which eye's
	// geometry is arriving so each submitted batch can be tagged; Present then replays each
	// eye's batches through its own viewport.
	//
	// i_eye_count 1 (the default) is the ordinary mono path and behaves exactly as before.
	// With 2, the window is split side by side, left eye in the left half - which is what makes
	// the stereo pipeline verifiable on a plain monitor, with no headset and no OpenXR runtime.
	// Call once per eye, before that eye's geometry is submitted.
	//
	// Eye index meaning "this geometry belongs to EVERY eye".  Anything far enough away to have
	// no measurable stereo parallax - the backdrop, which the engine renders with a 20000-unit
	// far clip - can be projected once and replayed into both eyes instead of being rendered
	// twice.  Submitting it under a real eye index instead would draw it into that eye only,
	// leaving the other eye's distance empty.
	const int i_EYE_ALL = -1;

	void SetEye(int i_eye, int i_eye_count);

	//******************************************************************************************
	//
	// VR: ADAPTER SELECTION.  An OpenXR runtime dictates which GPU the session must run on (it
	// reports the adapter LUID the headset is attached to), and xrCreateSession REJECTS a D3D11
	// device created on any other adapter.  This module otherwise creates its device on the
	// default adapter, which is the wrong one on a multi-GPU machine - the common laptop case of
	// an integrated display GPU plus a discrete one driving the headset.
	//
	// Call BEFORE the device is created - i.e. before the first bBeginFrame - or it has no
	// effect (the device is not torn down and rebuilt underneath a running renderer).  Passing
	// a LUID no adapter matches falls back to the default adapter and logs, rather than failing
	// to bring up a renderer at all; VR then won't start but the game still runs.
	//
	void SetRequiredAdapterLuid(unsigned int u_low, int i_high);

	//******************************************************************************************
	//
	// The live D3D11 device and immediate context, as void* so this header needs no <d3d11.h>.
	// Null until the device has been created (lazily, on the first bBeginFrame).  Exposed so the
	// VR layer can build its XrSession graphics binding on the SAME device the renderer uses -
	// sharing the device is what lets the eye images be rendered and handed to the runtime with
	// no cross-device copy.
	//
	void* pGetDevice();
	void* pGetContext();

	//******************************************************************************************
	//
	// VR: render one eye's accumulated geometry into a caller-supplied D3D11 texture - in
	// practice an OpenXR swapchain image, which the runtime owns and hands over per frame.
	//
	// This is the same draw the desktop path does, aimed at a different target: the geometry for
	// the frame is uploaded once and replayed per eye, so calling this for each eye and then
	// presenting the window costs one upload, not three.  A render-target view (and a matching
	// depth buffer) is created on first sight of each texture and cached, since a swapchain
	// cycles through a small fixed set of images.
	//
	// Must be called while the frame is still open - i.e. before Present, which consumes it.
	// Returns false if VR is inactive, the frame is empty, or the target could not be bound.
	//
	bool bRenderEyeToTexture(int i_eye, void* p_texture, int i_width, int i_height);

	//******************************************************************************************
	//
	// Release every cached eye render target.  Call when the VR swapchains are destroyed: the
	// cache is keyed by texture pointer and the runtime may reuse those addresses for different
	// images afterwards.
	//
	void PurgeEyeTargets();

	//******************************************************************************************
	//
	// True between bBeginFrame and Present, i.e. a 3D frame's polygons have been accumulated
	// and are waiting to be presented.  False on a UI-only frame (e.g. the paused in-game
	// menu, drawn into the software raster), which lets CRasterWin::Flip fall back to the GDI
	// present so that 2D content is actually shown in D3D11 mode.
	//
	bool bFrameOpen();

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
