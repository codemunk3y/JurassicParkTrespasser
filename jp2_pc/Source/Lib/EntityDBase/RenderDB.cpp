/***********************************************************************************************
 *
 * Copyright (c) DreamWorks Interactive. 1997
 *
 * Implementation of RenderDB.hpp.
 *
 ***********************************************************************************************
 *
 * $Log:: /JP2_PC/Source/Lib/EntityDBase/RenderDB.cpp                                          $
 * 
 * 86    10/02/98 2:59a Pkeet
 * Added different schedule times for Direct3D.
 * 
 * 85    10/01/98 4:05p Mlange
 * Improved move message stat reporting.
 * 
 * 84    10/01/98 1:56a Pkeet
 * Moved call to disable parent caches from the world move function to the render database move
 * message.
 * 
 * 83    10/01/98 12:24a Pkeet
 * Moved the cache lru to the cache module.
 * 
 * 82    9/28/98 10:07p Pkeet
 * Added automatic allocations for the VM loader.
 * 
 * 81    9/25/98 7:23p Rwyatt
 * RenderDBase sets no VM paging on scene and groff file loads
 * 
 **********************************************************************************************/

#include "GblInc/Common.hpp"
#include "Lib/W95/WinInclude.hpp"
#include "Lib/GeomDBase/PartitionPriv.hpp"
#include "RenderDB.hpp"
#include "Query/QRenderer.hpp"
#include "Query/QTerrain.hpp"
#include "MessageTypes/MsgPaint.hpp"
#include "MessageTypes/MsgMove.hpp"
#include "MessageTypes/MsgSystem.hpp"
#include "Lib/Renderer/PipeLine.hpp"
#include "Lib/Renderer/Camera.hpp"
#include "Lib/Renderer/RenderCacheInterface.hpp"
#include "Lib/Renderer/RenderCache.hpp"
#include "Lib/Renderer/Light.hpp"
#include "Lib/Sys/FixedHeap.hpp"
#include "Lib/Renderer/Occlude.hpp"
#include "Lib/Sys/VirtualMem.hpp"
#include "Lib/Loader/TextureManager.hpp"
#include "Lib/Sys/Profile.hpp"
#include "Lib/Std/LocalArray.hpp"
#include "Lib/Loader/SaveFile.hpp"
#include "Lib/Renderer/DepthSort.hpp"
#include "Lib/Renderer/Fog.hpp"
#include "Lib/Renderer/Sky.hpp"
#include "Lib/Sys/LRU.hpp"
#include "Lib/View/AGPTextureMemManager.hpp"
#include "Lib/View/RenderD3D11.hpp"
#include "Lib/View/RenderVR.hpp"
#include "Lib/EntityDBase/QualitySettings.hpp"
#include "Lib/View/Direct3DRenderState.hpp"
#include "Lib/Renderer/ScreenRenderAuxD3D.hpp"
#include "Lib/Sys/W95/Render.hpp"

// The following mess is for the perspective settings.
#include "Lib/Types/FixedP.hpp"
#include "Lib/Renderer/Primitives/DrawTriangle.hpp"
#include "Lib/Renderer/Primitives/Walk.hpp"
#include "Lib/Renderer/Primitives/IndexT.hpp"
#include "Lib/Renderer/Primitives/IndexPerspectiveT.hpp"

#include <memory.h>

//
// Module specific variables.
//

// Switch for turning terrain off.
bool bRenderTerrain = true;

// Stats for renderering.
static CProfileStat psOcclusionGetList("Get list", &proProfile.psOcclusion);

// Render save/load version number.
const int iRenderVersion = 3;

static rptr<CLightAmbient>	pamb;
//
// Class implementations.
//

//**********************************************************************************************
//
// CRenderDB implementation.
//
	//*****************************************************************************************
	CRenderDB::CRenderDB(const CWorld& w)
		: wDBase(w)
	{
		SetInstanceName("Render DBase");

		// No defaults yet.
		pc_defaults = 0;
		u4NoPageFrames = 0;

		// Register this entity with the message types it needs to receive.
		  CMessageMove::RegisterRecipient(this);
		 CMessagePaint::RegisterRecipient(this);
		CMessageSystem::RegisterRecipient(this);
	}

	//*****************************************************************************************
	CRenderDB::~CRenderDB()
	{
		CMessageSystem::UnregisterRecipient(this);
		 CMessagePaint::UnregisterRecipient(this);
		  CMessageMove::UnregisterRecipient(this);

		delete pc_defaults;
	}

	//******************************************************************************************
	void CRenderDB::Process(const CMessagePaint& msgpaint)
	{
		// Make sure Direct3D is enabled.
		d3dDriver.Restore();

		// Report stats if enabled.
		//d3dDriver.ReportPerFrameStats();

		// Prepare AGP texturing if active.
		agpAGPTextureMemManager.BeginFrame();

		// Process moved objects.
		EndCacheUpdate();

		// Increment the LRU count of image caches, allowing them to use the LRU container.
		renclRenderCacheList.IncrementCount();

		// Set up values for use by the partitioning system.
		SPartitionSettings::SetGlobalPartitionData();

		// Set up values for use by the render cache system.
		CRenderCache::SetParameters();
				
		TOccludeList oclist;		// List of occluding polygons.
		CPresence3<> pr3_inv_cam;	// Inverse presence of the camera.

		// Update the frame key for render caching.
		++CRenderCache::iFrameKeyGlobal;

		// Get the current active camera.
		CWDbQueryActiveCamera wqcam(wDBase);

		// Perform camera prediction.
		wqcam.tGet()->UpdatePrediction();

		//
		// Render the backdrop.
		//
		{
			// In stereo, the backdrop is rendered ONCE and replayed into both eyes rather than
			// projected per eye.  Its far clip is 20000 units against an interpupillary distance
			// of six centimetres, so the parallax between the eyes is far below one pixel - a
			// second pass would cost a full extra scene render to produce an identical image.
			// (Submitting it under a real eye index instead would put the whole distance in one
			// eye and leave the other's empty.)
			RenderD3D11::SetEye(RenderD3D11::i_EYE_ALL, RenderVR::iEyeCount());

			// Create a camera with a much farther clipping plane.
			CCamera::SProperties camprop = wqcam.tGet()->campropGetProperties();
			camprop.rFarClipPlaneDist = 20000.0f;
			CPresence3<> pr3_backdrop = wqcam.tGet()->pr3VPresence();

			// HEAD TRACKING.  Sharing one backdrop pass between the eyes is an argument about the
			// six centimetres BETWEEN them - it says nothing about the head, which can turn ninety
			// degrees away from the body.  Drawing this from the body presence leaves the sky
			// pinned to the screen, so it appears glued to the headset and slides with every head
			// movement.  The pass stays shared; it just has to be aimed where the head is looking.
			//
			// Orientation only: at a 20000-unit far clip the room-scale head offset moves the
			// backdrop by far less than a pixel, which is the same reasoning that makes one shared
			// pass correct in the first place.
			{
				RenderVR::SEyeView eyev0, eyev1;
				bool b_0 = RenderVR::bEyeView(0, eyev0);
				bool b_1 = RenderVR::bEyeView(1, eyev1);

				if (b_0 || b_1)
				{
					float f_w, af_v[3];

					if (b_0 && b_1)
					{
						// Average the two eye orientations rather than taking eye 0's: on a canted
						// headset (Index, Vive Pro) the panels are physically angled outward, so
						// either eye alone points several degrees off the head's own direction and
						// the sky would sit visibly askew.
						//
						// Align the signs first - q and -q are the same rotation, so summing them
						// blind can cancel instead of blend.
						float f_dot = eyev0.f_rot_w     * eyev1.f_rot_w
						            + eyev0.af_rot_v[0] * eyev1.af_rot_v[0]
						            + eyev0.af_rot_v[1] * eyev1.af_rot_v[1]
						            + eyev0.af_rot_v[2] * eyev1.af_rot_v[2];
						float f_sign = (f_dot < 0.0f) ? -1.0f : 1.0f;

						f_w     = eyev0.f_rot_w     + f_sign * eyev1.f_rot_w;
						af_v[0] = eyev0.af_rot_v[0] + f_sign * eyev1.af_rot_v[0];
						af_v[1] = eyev0.af_rot_v[1] + f_sign * eyev1.af_rot_v[1];
						af_v[2] = eyev0.af_rot_v[2] + f_sign * eyev1.af_rot_v[2];
					}
					else
					{
						// Only one eye has a tracked pose this frame - a few degrees of cant beats
						// no head rotation at all.
						const RenderVR::SEyeView& eyev = b_0 ? eyev0 : eyev1;
						f_w     = eyev.f_rot_w;
						af_v[0] = eyev.af_rot_v[0];
						af_v[1] = eyev.af_rot_v[1];
						af_v[2] = eyev.af_rot_v[2];
					}

					// CRotate3 normalises on construction, so the unnormalised sum above is fine.
					CRotate3<> r3_head(f_w, CVector3<>(af_v[0], af_v[1], af_v[2]));

					// Head first, then body - the same convention as the eye loop (Rotate.hpp:
					// a*b means "apply a, then b").
					pr3_backdrop.r3Rot = r3_head * pr3_backdrop.r3Rot;
				}
			}

			CCamera cam_backdrop(pr3_backdrop, camprop);

			// The SKY is a separate pass from the backdrop geometry above, and it does not use
			// the camera it is handed - CSkyRender::SetTransformedCameraCorners asks the world
			// database for the ACTIVE camera, which is the body's.  This override is how you
			// reach it at all.
			//
			// ⚠ THIS DOES NOT FIX THE SKY YET (2026-07-25).  With this composition it moved
			// OPPOSITE to yaw and pitch; with the rotation inverted it moved WITH the head; roll
			// did nothing either way.  Neither is world-stable, so the error is not a simple sign
			// flip and the model of this pipeline is still incomplete.  Re-tested after the Ctrl+R
			// recentre lined the view up with the body - the sky still followed the headset, so
			// the 90-degree head/body offset was not the cause either.
			//
			// RECOMMENDED FIX: stop patching the capture-and-blit path and draw the sky as real
			// GEOMETRY - a plane or dome at CSkyRender::fSkyHeight, projected per eye like
			// everything else.  Under D3D11 the sky is currently software-rasterised into the main
			// raster and then blitted FULL-SCREEN with a shader that has no camera in it, so it is
			// a flat photograph pasted over the view: no depth, no stereo, and no way to represent
			// roll.  The 1998 reason for doing it that way (no GPU) no longer applies.
			CSkyRender::SetCameraOverride(&cam_backdrop);

			// Get pointer to the settings.
			ptr<CRenderer::SSettings>     prenset   = msgpaint.renContext.pSettings;

			// Store the render settings.
			CRenderer::SSettings     renset   = *prenset;

			//
			// Set the rendering properties to turn off shading, texturing, and fogging.
			// We still perform lighting, using a full ambient light, so that textures are
			// maximally lit.
			//
			prenset->seterfState   = Set(erfLIGHT);

			// Do not execute the scheduler for the backdrop.
			prenset->bExecuteScheduler = false;

			if (!pamb)
				pamb =  rptr_new CLightAmbient(1.0f);

			// Construct a special ambient light for the backdrop, for full lighting.
			//CInstance insFullAmbient(rptr_cast(CRenderType, rptr_new CLightAmbient(1.0f)));
			CInstance insFullAmbient(rptr_cast(CRenderType, pamb));

			// Start the 3D renderer.
			if (d3dDriver.bUseD3D())
			{
				srd3dRenderer.SetOutputFlag(true);
				srd3dRenderer.bBeginScene();
				d3dstState.SetAllowZBuffer(false);
			}
			else
			{
				srd3dRenderer.SetOutputFlag(false);
			}

			// Lock and render.
			wDBase.Lock();
			CLArray(COcclude*, papoc, 0);	// Use a dummy occlusion list.
			msgpaint.renContext.RenderScene
			(
				cam_backdrop,					// Special backdrop camera.
				std::list<CInstance*>(1, &insFullAmbient),	// Special backdrop light list.
				wDBase.ppartBackdropsList(),	// Special backdrop partition.
				papoc,							// Use no occlusion objects.
				esfINTERSECT,					// Assume intersection.
				0								// Us no terrain.
			);
			wDBase.Unlock();

			// Reenable Z buffering.
			if (d3dDriver.bUseD3D())
			{
				d3dstState.SetAllowZBuffer(true);
			}

			// Reset the render settings.
			*prenset = renset;

			// Stop overriding the sky's camera - cam_backdrop is about to go out of scope, and
			// every later pass (and every flat frame) must use the normal active-camera lookup.
			CSkyRender::SetCameraOverride(0);

			// Unlock the main buffer.
			prasMainScreen->Unlock();
		}


		// If the terrain exists, update it.
		if (CWDbQueryTerrain().tGet() != 0)
			CWDbQueryTerrain().tGet()->FrameBegin(wqcam.tGet());

		// Get the terrain's mesh.
		CWDbQueryTerrainMesh wqtmsh(wDBase);

		// Start the timer.
		CCycleTimer	ctmr;

		// Build an occlusion list.
		if (COcclude::bBuildOcclusionList())
		{
			// This is a good time to reset the heap used for occlusion data.
			COcclude::Reset();

			// Get the inverse presence of the camera.
			pr3_inv_cam = ~wqcam.tGet()->pr3Presence();

			// Get a list from the world database.
			GetOccludePolygons
			(
				wDBase.ppartPartitionList(),	// Root partition node.
				pr3_inv_cam,					// Inverse camera presence.
				wqcam.tGet()->pbvBoundingVol(),	// Camera bounding volume.
				oclist							// Linked list of visible occluding objects.
			);
		}
		
		// Create an array of occluding polygon objects.
		CLArray(COcclude*, papoc, oclist.size());

		// Copy the list to the array.
		CopyOccludePolygons(papoc, *wqcam.tGet(), oclist);

		// Set stats for occlusion.
		TCycles cy_get_list = ctmr();
		psOcclusionGetList.Add(cy_get_list, oclist.size());
		proProfile.psOcclusion.Add(cy_get_list, 1);

		// ⚠ KNOWN VR BUG, CONFIRMED 2026-07-25 - THE OCCLUDERS ABOVE ARE BUILT FROM THE BODY.
		// In VR, geometry vanishes when the head looks the SAME way as the body and renders
		// correctly when it looks away; distant terrain and small objects (crates) worst, near
		// terrain and trees fine.  Proved by handing the eye loop an EMPTY occluder array: the
		// vanishing stopped completely.
		//
		// Cause: the list is built from the body camera and transformed into BODY camera space
		// (CopyOccludePolygons above), then applied to an eye camera that differs by the head
		// rotation, half an IPD, and the seated room-scale offset.  Look away from the body's
		// facing and the occluders cull nothing; look along it and they cull from the wrong place.
		//
		// FIX (not yet written): build this list from the HEAD-composed camera.  It can still be
		// built ONCE and shared by both eyes - six centimetres of IPD really is negligible here;
		// it was only ever the head ROTATION that was wrong.  Do NOT simply disable occlusion:
		// measured with it off, the heaviest frames went 8.3 ms -> 13.9 ms.

		// Render the scene.
		wDBase.Lock();

		{
			// STEREO (see RenderVR.hpp).  The renderer's GPU path is handed geometry that this
			// CPU pipeline has ALREADY projected to 2D screen space, so a second eye cannot be
			// produced downstream - by the time the polygons reach the GPU the depth information
			// they were projected from is gone.  The only way to get one is to project the world
			// again from the other eye, which is what this loop does: the whole main-scene render
			// runs once per eye, with the camera shifted along its own X (right) axis by half the
			// interpupillary distance.  Trespasser's world unit is the metre, so that shift is a
			// true physical IPD with no scale conversion.
			//
			// Only the render repeats.  The occlusion list and terrain update above are built
			// once, from the head-centre camera, and shared by both eyes: rebuilding them per eye
			// would double the most expensive part of the frame to account for a few centimetres
			// of parallax.  Culling from the centre is very slightly conservative at the extreme
			// left and right edges, which is the standard trade and is invisible in practice.
			const int i_eyes = RenderVR::iEyeCount();

			for (int i_eye = 0; i_eye < i_eyes; ++i_eye)
			{
				// Tell the backend which eye's geometry is about to arrive, so it can tag the
				// batches and later replay each eye through its own viewport/target.
				RenderVR::SetEye(i_eye);
				RenderD3D11::SetEye(i_eye, i_eyes);

				CCamera::SProperties camprop = wqcam.tGet()->campropGetProperties();
				CPresence3<> pr3_eye = wqcam.tGet()->pr3VPresence();

				// HEAD TRACKING (M4).  When the runtime has a tracked pose for this eye, it
				// supersedes the fixed IPD offset below: the pose IS that eye's, half an IPD
				// included, so applying both would separate the eyes twice.
				RenderVR::SEyeView eyev;
				if (RenderVR::bEyeView(i_eye, eyev))
				{
					// The game camera stays in charge of where the player is and which way the
					// body faces (mouse look, physics, cutscenes); the head pose is applied
					// RELATIVE to it.  Composed head-then-body, because the pose is expressed in
					// the player's own frame - and in this engine a*b means "a first, then b"
					// (Rotate.hpp: operator* defers to b.r3Rotate(a)).
					CRotate3<> r3_head
					(
						eyev.f_rot_w,
						CVector3<>(eyev.af_rot_v[0], eyev.af_rot_v[1], eyev.af_rot_v[2])
					);

					// Room-scale movement: the LOCAL reference space puts its origin where the
					// player's head was when the session began, so this is a small offset from
					// the game camera rather than a position in the level.  It is rotated into
					// world space by the BODY rotation, not the composed one - leaning left is
					// leftward relative to the body, and must not itself be turned by the head.
					CVector3<> v3_head(eyev.af_pos[0], eyev.af_pos[1], eyev.af_pos[2]);

					pr3_eye.v3Pos += v3_head * pr3_eye.r3Rot;
					pr3_eye.r3Rot  = r3_head * pr3_eye.r3Rot;

					// PROJECTION.  The headset's frustum is asymmetric and this camera can only
					// be symmetric, so render the smallest symmetric frustum that CONTAINS it -
					// take the larger half-angle on each axis.  That draws everything the runtime
					// wants plus a margin it will crop, which costs some fill and is correct;
					// undersizing would leave the headset with nothing to show at the edges.
					float f_tan_h = Max(Abs(eyev.f_tan_left), Abs(eyev.f_tan_right));
					float f_tan_v = Max(Abs(eyev.f_tan_up),   Abs(eyev.f_tan_down));

					// NB the frustum aspect here does NOT match the eye texture's (measured on an
					// Index: 1.0896 against 2468x2740 = 0.9007).  Expanding the short axis to make
					// them agree was tried on 2026-07-25 and REVERTED: the numbers came out exactly
					// right and the reported distortion was unchanged, so the mismatch - though real
					// - is not what the user is seeing.  The symptom is a SHEAR ("square pixels look
					// like diamonds", correct at ~45 degrees of roll, wrong at 0), and a shear cannot
					// be produced by an anisotropic scale.  Suspect the projection's CShear3<>('y')
					// and CViewport::vcOriginX/Y (the centre-of-projection offset) instead.  The
					// expansion also grew the vertical field 21%, beyond what the occlusion list was
					// built for, which may have been causing missing geometry.
					if (f_tan_h > 0.0f && f_tan_v > 0.0f)
					{
						// rViewWidth is the tangent of the half view angle, but the engine
						// divides it by the zoom factor when it builds the projection, so fold
						// the zoom back in to get the angle actually asked for.
						camprop.rViewWidth   = f_tan_h * camprop.fZoomFactor;

						// Width-to-height of the view volume.  This also cancels the engine's
						// hardcoded 4:3, which would otherwise stretch the image on its way into
						// a square-ish eye texture.
						camprop.fAspectRatio = f_tan_h / f_tan_v;

						// Tell the VR layer what it is about to be given, so the projection layer
						// describes this frustum rather than the runtime's.
						RenderVR::SetRenderedFov(i_eye, -f_tan_h, f_tan_h, f_tan_v, -f_tan_v);

						// DIAGNOSTIC (skew on pitch/roll).  The frustum we render is stretched to
						// fill the whole eye texture, so its aspect must match that texture's pixel
						// aspect or the image carries a fixed anisotropic stretch.  Head-level that
						// is nearly invisible; roll the head and it shears, because the stretch axis
						// stops lining up with the world's vertical.  Log both once per eye and
						// compare - if these two numbers differ, that is the bug.
						{
							static bool ab_logged[2] = { false, false };
							if (i_eye >= 0 && i_eye < 2 && !ab_logged[i_eye])
							{
								ab_logged[i_eye] = true;

								int i_tex_w = RenderVR::iRecommendedEyeWidth();
								int i_tex_h = RenderVR::iRecommendedEyeHeight();

								char buf[256];
								sprintf(buf,
									"TRESPASS_VR: eye %d frustum tan h=%.4f v=%.4f aspect=%.4f | "
									"texture %dx%d aspect=%.4f\n",
									i_eye, f_tan_h, f_tan_v, f_tan_h / f_tan_v,
									i_tex_w, i_tex_h,
									(i_tex_h > 0) ? ((float)i_tex_w / (float)i_tex_h) : 0.0f);
								RenderVR::LogLine(buf);
							}
						}
					}
				}
				else
				{
					// No tracked pose - the desktop side-by-side stereo path (TRESPASS_VR_STEREO
					// with no runtime), or a frame where the runtime has lost tracking.  Offset
					// along the camera's LOCAL right axis, so the eyes separate across the view
					// however the head is turned or pitched.  Camera space here is X = right,
					// Y = forward, Z = up (see the view-normalising transform in CCamera).
					float f_offset = RenderVR::fEyeOffsetX(i_eye);
					if (f_offset != 0.0f)
						pr3_eye.v3Pos += CVector3<>(f_offset, 0, 0) * pr3_eye.r3Rot;
				}

				CCamera cam(pr3_eye, camprop);

				// Toggle the clear off.
				CScreenRender::SSettings screnset = *msgpaint.renContext.pScreenRender->pSettings;
				msgpaint.renContext.pScreenRender->pSettings->bClearBackground = false;
				msgpaint.renContext.pScreenRender->pSettings->bDrawSky         = false;

				// Get the lights whose influence intersects the camera's bounding volume.
				CWDbQueryLights wqlt(&cam, wDBase);

				// Render the main scene.
				msgpaint.renContext.RenderScene
				(
					cam,
					wqlt,
					wDBase.ppartPartitionList(),
					papoc,
					esfINTERSECT,
					(bRenderTerrain) ? (wqtmsh.tGet()) : (0)
				);

				// Reset the clear.
				*msgpaint.renContext.pScreenRender->pSettings = screnset;
			}

			// Leave the backend back in its mono state, so anything drawn outside the eye loop
			// (2D overlays, the next frame's early passes) is not tagged as the last eye's.
			RenderVR::SetEye(0);
			RenderD3D11::SetEye(0, 1);
		}
	
		wDBase.Unlock();

		// Finish hardware renderering.
		srd3dRenderer.EndScene();

		// Execute terrain updates scheduled for this frame.
		msgpaint.renContext.ExecuteScheduleForTerrain();

		if (CWDbQueryTerrain().tGet() != 0)
			CWDbQueryTerrain().tGet()->FrameEnd();

		// Clean up the fixed heap used for render caches and terrain textures.
		fxhHeap.bConglomerate();

		// Update the count for the next frame.
		BeginCacheUpdate();

		if (u4NoPageFrames>0)
		{
			u4NoPageFrames--;
			if (u4NoPageFrames == 0)
			{
				// Let the texture manager VM system dynamically page textures
				gtxmTexMan.pvmeTextures->SetAlwaysLoad(false);
			}
		}

		// Clean up AGP texturing if active.
		agpAGPTextureMemManager.EndFrame();

		// Decommit the pipeline heap if we are not running.
		if (!CMessageSystem::bSimulationGoing())
			CPipelineHeap::Decommit();
	}

	extern CProfileStat psMoveMsgRenderDB;
	
	//******************************************************************************************
	void CRenderDB::Process(const CMessageMove& msgmv)
	{
		CTimeBlock tmb(&psMoveMsgRenderDB);

		// Help render caching by tracking all currently woken objects.
		if (msgmv.pinsMover->pshGetShape())
		{
			// A visible object.
			switch (msgmv.etType)
			{
				case CMessageMove::etAWOKE:
					// Invalidate parent caches.
					PurgeParentCaches(msgmv.pinsMover);
					break;

				case CMessageMove::etMOVED :
				case CMessageMove::etACTIVE :
					AddToMovingList(msgmv.pinsMover);
					break;

				case CMessageMove::etSLEPT :
					AddToStoppedList(msgmv.pinsMover);
					break;
			}
		}
		else if (msgmv.etType == CMessageMove::etMOVED && ptCast<CCamera>(msgmv.pinsMover))
		{
			// Detect significant camera movement, and inform render cache.
			//if (msgmv.bSignificant())
				//CRenderCache::bCameraMoved = true;
		}
	}

	//******************************************************************************************
	void CRenderDB::Process(const CMessageSystem& msg_system)
	{
		if (msg_system.escCode == escQUALITY_CHANGE)
		{
			// Perspective settings (all hacked up).
			extern float fPerspectivePixelError;
			extern float fAltPerspectivePixelError;
			extern CPerspectiveSettings persetSettings;

			// Update the far clipping distance.
			ptr<CCamera> pcam = CWDbQueryActiveCamera(wWorld).tGet();
			if (pcam)
			{
				CCamera::SProperties camprop = pcam->campropGetProperties();

				// Update actual far clip distance based on desired & quality.
				camprop.SetFarClipFromDesired();

				pcam->SetProperties(camprop);

				// Adjust the fog for the changed far clipping distance.
				fogFog.SetQualityAdjustment(camprop.rFarClipPlaneDist, camprop.rDesiredFarClipPlaneDist);
				fogTerrainFog.SetQualityAdjustment(camprop.rFarClipPlaneDist, camprop.rDesiredFarClipPlaneDist);
			}

			// Set the object culling priority.
			SetPrioritySetting(iGetQualitySetting());

			//
			// Set discrete quality values.
			//
			int i_quality = iGetQualitySetting();

			// Sky (none).
			if (gpskyRender)
			{
				gpskyRender->SetDrawMode
				(
					(CSkyRender::SkyDrawMode)(qdQualitySettings[i_quality].iSkyDrawMode)
				);
			}

			// Perspective.
			fPerspectivePixelError = qdQualitySettings[i_quality].fPersectiveError;
			persetSettings.iMinSubdivision = qdQualitySettings[i_quality].iMinSubdivison;
			fAltPerspectivePixelError = qdQualitySettings[i_quality].fAltPerspectiveError;
			persetSettings.iAltMinSubdivision = qdQualitySettings[i_quality].iAltMinSubdivision;
			persetSettings.iAdaptiveMinSubdivision = qdQualitySettings[i_quality].iAdaptiveMinSubdivision;

			// Bi-linear filter (off).
			if (prenMain && prenMain->pSettings &&  prenMain->pScreenRender->seterfModify()[erfFILTER])
			{
				prenMain->pSettings->seterfState[erfFILTER] = qdQualitySettings[i_quality].bBilinearFilter;
			}

			// Disable largest mip-maps.
			if (qdQualitySettings[i_quality].bDisableLargestMip)
				CTexture::emuMipUse = emuNO_LARGEST;
			else
				CTexture::emuMipUse = emuNORMAL;

			// Set the maximum amount of physical memory used by the texture VM.
			gtxmTexMan.pvmeTextures->AutoSetMemory();

			// Don't delay paging for the next N frames.
			SetNoPageFrames(2);

			// Default maximum number of polys to sort.
			ptsTolerances.iMaxToDepthsort = qdQualitySettings[i_quality].iMaxToDepthsort;

			// Redraw image caches and the terrain.
			if (CWDbQueryTerrain().tGet())
				CWDbQueryTerrain().tGet()->Rebuild(true);

			// Set the scheduler time slices.
			//shcScheduler.uMSSlice = qdQualitySettings[i_quality].uCacheMs;
			//shcSchedulerTerrainTextures.uMSSlice = qdQualitySettings[i_quality].uTerrainMs;
		}

		//
		// If we load a scene or a groff then set no VM paging for a couple of frames
		//
		if ((msg_system.escCode == escGROFF_LOADED) || (msg_system.escCode == escSCENE_FILE_LOADED))
		{
			SetNoPageFrames(2);
		}
	}

	//*****************************************************************************************
	char *CRenderDB::pcSave
	(
		char* pc
	) const
	{
		// Version number.
		pc = pcSaveT(pc, iRenderVersion);

		// Depthsort Settings.
		pc = ptsTolerances.pcSave(pc);

		// Perspective settings (all hacked up).
		extern float fPerspectivePixelError;
		extern float fMinZPerspective;
		extern float fAltPerspectivePixelError;
		extern CPerspectiveSettings persetSettings;

		pc = pcSaveT(pc, 1);
		pc = pcSaveT(pc, fPerspectivePixelError);
		pc = pcSaveT(pc, fAltPerspectivePixelError);
		pc = pcSaveT(pc, persetSettings.iMinSubdivision);
		pc = pcSaveT(pc, persetSettings.iAltMinSubdivision);
		pc = pcSaveT(pc, fMinZPerspective);

		// Image Cache settings.
		pc = rcsRenderCacheSettings.pcSave(pc);

		// Fog settings.
		CFog::SProperties fogprop = fogFog.fogpropGetProperties();
		pc = pcSaveT(pc, fogprop.rHalfFogY);
		pc = pcSaveT(pc, fogprop.rPower);

		// Culling settings.
		float f_cull_dist = SPartitionSettings::fGetCullMaxAtDistance();
		pc = pcSaveT(pc, f_cull_dist);

		float f_cull_radius = SPartitionSettings::fGetMaxRadius();
		pc = pcSaveT(pc, f_cull_radius);

		float f_cull_dist_shadow = SPartitionSettings::fGetCullMaxAtDistanceShadow();
		pc = pcSaveT(pc, f_cull_dist_shadow);

		float f_cull_radius_shadow = SPartitionSettings::fGetMaxRadiusShadow();
		pc = pcSaveT(pc, f_cull_radius_shadow);

		// Mip-Map settings.
		pc = pcSaveT(pc, CTexture::fMipmapThreshold);

		// Sky settings.
		pc = gpskyRender->pcSave(pc);

		// Camera.
		ptr<CCamera> pcam = CWDbQueryActiveCamera(wWorld).tGet();

		pc = pcSaveT(pc, pcam->campropGetProperties().fZoomFactor);
		pc = pcSaveT(pc, pcam->campropGetProperties().angGetAngleOfView());
		pc = pcSaveT(pc, pcam->campropGetProperties().rNearClipPlaneDist);
		pc = pcSaveT(pc, pcam->campropGetProperties().rDesiredFarClipPlaneDist);

		// Filtering render flag.
		int i_filter = 0;				// Value indicating this is not supported.

		if (prenMain && prenMain->pSettings)
		{
			if (prenMain->pScreenRender->seterfModify()[erfFILTER])
			{
				if (prenMain->pSettings->seterfState[erfFILTER])
					i_filter = 5;	// On.
				else
					i_filter = 4;	// Off.
			}
		}

		pc = pcSaveT(pc, i_filter);

		// Save animating mesh data.
		LPMA::const_iterator i = lpmaAnimatedMeshes.begin();
		for ( ; i != lpmaAnimatedMeshes.end(); ++i)
		{
			pc = (*i)->pcSave(pc);
		}

		return pc;
	}

	//*****************************************************************************************
	const char *CRenderDB::pcLoad
	(
		const char* pc
	)
	{
		int iVersion;

		pc = pcLoadT(pc, &iVersion);

		if (iVersion >= 1)
		{
			// Depthsort Settings.
			pc = ptsTolerances.pcLoad(pc);

			// Perspective settings (all hacked up).
			extern float fPerspectivePixelError;
			extern float fMinZPerspective;
			extern float fAltPerspectivePixelError;
			extern CPerspectiveSettings persetSettings;

			int iPerspSetVersion;
			pc = pcLoadT(pc, &iPerspSetVersion);

			float f_pixel_error, f_alt_pixel_error, f_min_z_perspective;
			int i_min_subdivision, i_alt_min_subdivision;

			pc = pcLoadT(pc, &f_pixel_error);
			pc = pcLoadT(pc, &f_alt_pixel_error);
			pc = pcLoadT(pc, &i_min_subdivision);
			pc = pcLoadT(pc, &i_alt_min_subdivision);
			pc = pcLoadT(pc, &f_min_z_perspective);

			/* 
			 * Ignore these values, they are set via the global quality value.
			 *
			fPerspectivePixelError = f_pixel_error;
			fAltPerspectivePixelError = f_alt_pixel_error;
			persetSettings.iMinSubdivision = i_min_subdivision;
			persetSettings.iAltMinSubdivision = i_alt_min_subdivision;
			fMinZPerspective = f_min_z_perspective;
			*/

			// Image Cache settings.
			pc = rcsRenderCacheSettings.pcLoad(pc);

			// Fog settings.
			CFog::SProperties fogprop = fogFog.fogpropGetProperties();

			pc = pcLoadT(pc, &fogprop.rHalfFogY);
			pc = pcLoadT(pc, &fogprop.rPower);

			fogFog.SetProperties(fogprop);
			fogTerrainFog.SetProperties(fogprop);

			// Culling settings.
			if (iVersion >= 2)
			{
				float f_cull_dist, f_cull_dist_shadow;
				float f_cull_radius, f_cull_radius_shadow;

				pc = pcLoadT(pc, &f_cull_dist);
				SPartitionSettings::SetCullMaxAtDistance(f_cull_dist);

				pc = pcLoadT(pc, &f_cull_radius);
				SPartitionSettings::SetMaxRadius(f_cull_radius);

				pc = pcLoadT(pc, &f_cull_dist_shadow);
				SPartitionSettings::SetCullMaxAtDistanceShadow(f_cull_dist_shadow);

				pc = pcLoadT(pc, &f_cull_radius_shadow);
				SPartitionSettings::SetMaxRadiusShadow(f_cull_radius_shadow);
			}
			else
			{
				float f_cull_dist;
				pc = pcLoadT(pc, &f_cull_dist);
				// Hack for now to ensure compatibility with future scene files.
				if (f_cull_dist < 250.0f)
					f_cull_dist = 250.0f;
				SPartitionSettings::SetCullMaxAtDistance(f_cull_dist);
			}

			// Mip-Map settings.
			pc = pcLoadT(pc, &CTexture::fMipmapThreshold);

			// Sky settings.
			pc = gpskyRender->pcLoad(pc);

			// Camera.
			ptr<CCamera> pcam = CWDbQueryActiveCamera(wWorld).tGet();
			if (pcam)
			{
				CCamera::SProperties camprop = pcam->campropGetProperties();

				pc = pcLoadT(pc, &camprop.fZoomFactor);

				CAngle angTemp;

				pc = pcLoadT(pc, &angTemp);
				camprop.SetAngleOfView(angTemp);

				pc = pcLoadT(pc, &camprop.rNearClipPlaneDist);
				pc = pcLoadT(pc, &camprop.rDesiredFarClipPlaneDist);
				camprop.SetFarClipFromDesired();

				pcam->SetProperties(camprop);

				// Adjust the fog.
				fogFog.SetQualityAdjustment(camprop.rFarClipPlaneDist, camprop.rDesiredFarClipPlaneDist);
				fogTerrainFog.SetQualityAdjustment(camprop.rFarClipPlaneDist, camprop.rDesiredFarClipPlaneDist);
			}

			// Filtering render flag.
			int i_filter;
			pc = pcLoadT(pc, &i_filter);

			/* 
			 * Ignore this value, it is set via the global quality value.
			 *
			// Only set the flag if it is allowed for this processor.
			if (prenMain->pScreenRender->seterfModify()[erfFILTER] && (i_filter >= 4))
				prenMain->pSettings->seterfState[erfFILTER] = (i_filter - 4);
			*/


			if (iVersion >= 3)
			{
				// Load animating mesh data.
				std::list<CMeshAnimating*>::iterator i = lpmaAnimatedMeshes.begin();
				for ( ; i != lpmaAnimatedMeshes.end(); ++i)
				{
					pc = (*i)->pcLoad(pc);
				}
			}
		}
		else
		{
			AlwaysAssert("Unknown version of render settings");
		}

		return pc;
	}

	//*****************************************************************************************
	//
	void CRenderDB::SaveDefaults()
	{
		char ac_buffer[4096];
		char *pc_end;

		pc_end = pcSave(ac_buffer);

		if (pc_defaults)
			delete pc_defaults; 

		int i_len = pc_end - ac_buffer;
		pc_defaults = new char[i_len];

		if (pc_defaults)
			memcpy(pc_defaults, ac_buffer, i_len);
	}

	//*****************************************************************************************
	//
	void CRenderDB::RestoreDefaults()
	{
		if (pc_defaults)
			pcLoad(pc_defaults);
	}

	//*****************************************************************************************
	void CRenderDB::SetNoPageFrames(uint32 u4_frames)
	{
		// Tell the VM to stop paging and load textures as requested.
		gtxmTexMan.pvmeTextures->SetAlwaysLoad(true);
		u4NoPageFrames = u4_frames;
	}


//
// Global variables.
//
uint32 CRenderDB::u4NoPageFrames = 0;
