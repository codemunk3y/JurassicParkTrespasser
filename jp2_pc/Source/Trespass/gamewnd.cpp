//+--------------------------------------------------------------------------
//
//  Copyright (C) DreamWorks Interactive, 1996.
//
//  File:       mainwnd.cpp
//
//  Contents:
//
//  Classes:
//
//  Functions:
//
//  History:    26-Oct-96   SHernd   Created
//
//---------------------------------------------------------------------------


#include "precomp.h"
#pragma hdrstop

#include "tpassglobals.h"
#include "resource.h"
#include "supportfn.hpp"
#include "uidlgs.h"
#include "ctrls.h"
#include "main.h"
#include "keyremap.h"
#include "..\Lib\Sys\reg.h"
#include "..\lib\sys\reginit.hpp"
#include "..\Lib\EntityDBase\MessageTypes\MsgStep.hpp"
#include "..\Game\AI\AIMain.hpp"
#include "..\Lib\Sys\Profile.hpp"
#include "..\Lib\View\RenderD3D11.hpp"
#include "..\Lib\View\RenderVR.hpp"
#include <stdio.h>
#include <stdlib.h>

BOOL bQuitGame = FALSE;


extern CPlayer *    gpPlayer;
extern HWND         g_hwnd;
extern HINSTANCE    g_hInst;
extern bool         bInvertMouse;

bool g_bDisplayLocation = false;
bool g_bDisplayFPS = false;

float fFrameRate = 0;           // Framerate in frames per second, calculated once per second
int iNumFramesThisSample = 0;   // Number of frames rendered in the current sample period
DWORD dwTimeLastSample = 0;     // Time when the framerate was last calculated
DWORD dwTimeLastFrame = 0;      // The time when the previous frame was rendered
float fTimeElapsed = 0;         // Milliseconds elapsed since the last frame was rendered

#define COUNT_NOTICED_DEAD      20


//+--------------------------------------------------------------------------
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------

enum
{
    CHEAT_BONES,
    CHEAT_INVULNERABLE,
    CHEAT_LOCATION,
    CHEAT_TELEPORT,
    CHEAT_TNEXT,
    CHEAT_GORE,
    CHEAT_INVERTMOUSE,
    CHEAT_WIN,
    CHEAT_SORT,
    CHEAT_ALLAMMO,
    CHEAT_SLOMO,
	CHEAT_DINOS,
    CHEAT_FPS,
};

struct
{
    LPSTR   psz;
    DWORD   dw;
} CHEATS[] =
{
    "BONES", CHEAT_BONES,
    "INVUL", CHEAT_INVULNERABLE,
    "LOC", CHEAT_LOCATION,
    "TELE", CHEAT_TELEPORT,
    "TNEXT", CHEAT_TNEXT,
    "GORE", CHEAT_GORE,
    "IMOUSE", CHEAT_INVERTMOUSE,
    "WIN", CHEAT_WIN,
    "SORT", CHEAT_SORT,
    "WOO", CHEAT_ALLAMMO,
    "BIONICWOMAN", CHEAT_SLOMO,
	"DINOS", CHEAT_DINOS,
    "FPS", CHEAT_FPS,
};

int g_icCheats = sizeof(CHEATS) / sizeof(CHEATS[0]);


bool ExecuteCheat(LPSTR pszCheat)
{
    LPSTR   psz;
    bool    bRet;
    int     i;
    bool    bFound;

    psz = strchr(pszCheat, ' ');

    if (psz)
    {
        *psz = '\0';
        psz++;
    }

    for (i = 0, bFound = false; i < g_icCheats && !bFound; i++)
    {
        if (strcmpi(CHEATS[i].psz, pszCheat) == 0)
        {
            bFound = true;
            break;
        }
    }

    if (!bFound)
    {
        return false;
    }

    switch (CHEATS[i].dw)
    {
        case CHEAT_BONES:
            pphSystem->bShowBones = !pphSystem->bShowBones;
            break;

        case CHEAT_INVULNERABLE:
#if 0
            gpPlayer->bInvulnerable = !gpPlayer->bInvulnerable;
#endif
            break;

        case CHEAT_LOCATION:
            g_bDisplayLocation = !g_bDisplayLocation;
            break;

        case CHEAT_TELEPORT:
#if 0
            {
                float       fX;
                float       fY;
                float       fZ;

                sscanf(psz, "%f, %f, %f", &fX, &fY, &fZ);

			    extern void PlayerTeleportToXYZ(float fX, float fY, float fZ);

                PlayerTeleportToXYZ(fX, fY, fZ);
            }
#endif
            break;

        case CHEAT_TNEXT:
            {
				extern void PlayerTeleportToNextLocation();
				PlayerTeleportToNextLocation();
            }
            break;

        case CHEAT_GORE:
            {
                int         i;

                sscanf(psz, "%i", &i);

                if (i > 2)
                {
                    i = 2;
                }
                else if (i < 0)
                {
                    i = 0;
                }

                SetRegValue(REG_KEY_GORE, i);
                CAnimate::iGoreLevel = i;
            }
            break;

        case CHEAT_INVERTMOUSE:
            {
                int         i;

                sscanf(psz, "%i", &i);

                if (i > 2)
                {
                    i = 2;
                }
                else if (i < 0)
                {
                    i = 0;
                }

                SetRegValue(REG_KEY_INVERTMOUSE, i);
                bInvertMouse = i ? true : false;
            }
            break;

        case CHEAT_WIN:
            wWorld.GameIsOver();
            break;

        case CHEAT_SORT:
            {
                char        sz[50];

                sscanf(psz, "%s", &sz);

                if (strcmpi(sz, "BTF") == 0)
                {
	                prenMain->pSettings->esSortMethod = esPresortBackToFront;
                }
                else if (strcmpi(sz, "FTB") == 0)
                {
                    prenMain->pSettings->esSortMethod = esPresortFrontToBack;
                }
            }
            break;

        case CHEAT_ALLAMMO:
            {
                extern bool g_bUnlimitedAmmo;
                g_bUnlimitedAmmo = !g_bUnlimitedAmmo;
            }
            break;

        case CHEAT_SLOMO:
            {
                if (CMessageStep::sMultiplier == 1.0f)
                {
                    CMessageStep::sMultiplier = 0.30f;
                }
                else
                {
                    CMessageStep::sMultiplier = 1.0f;
                }
            }
            break;
		case CHEAT_DINOS:
			{
				// Toggle the boring dino bit.
				gaiSystem.bBoring = !gaiSystem.bBoring;
			}

        case CHEAT_FPS:
            g_bDisplayFPS = !g_bDisplayFPS;
            break;
    }

    bRet = TRUE;

    return bRet;
}


//+--------------------------------------------------------------------------
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------



CGameWnd::CGameWnd(CUIManager * puimgr) : CUIWnd(puimgr)
{
    g_CTPassGlobals.m_prasBkgnd = NULL;
    g_CTPassGlobals.m_prasMiniBkgnd = NULL;
    m_bPaused = FALSE;
    m_iClear = 0;
    m_pUIMgr->m_bDrawMouse = FALSE;
    m_puictlCheat = NULL;
	g_CTPassGlobals.bInGame = true;
	g_CTPassGlobals.bHardReset = true;
}

CGameWnd::~CGameWnd()
{
    delete g_CTPassGlobals.m_prasBkgnd;
    delete g_CTPassGlobals.m_prasMiniBkgnd;
    g_CTPassGlobals.m_prasBkgnd = NULL;
    g_CTPassGlobals.m_prasMiniBkgnd = NULL;
	g_CTPassGlobals.bInGame = false;
}


BOOL CGameWnd::OnCreate()
{
    int         i;

    CUIWnd::OnCreate();

	// Free cursor movement.
    ClipCursor(0);

	// Create a screen of the right size if required.
	{
		int i_width;
		int i_height;

		bGetDimensions(i_width, i_height);
		if (prasMainScreen->iWidthFront != i_width || prasMainScreen->iHeightFront != i_height)
			SetupGameScreen();
        else
        {
            int iGore = GetRegValue(REG_KEY_GORE, DEFAULT_GORE);
            if (iGore > 2)
            {
                SetRegValue(REG_KEY_GORE, 2);
                iGore = 2;
            }
            else if (iGore < 0)
            {
                SetRegValue(REG_KEY_GORE, 0);
                iGore = 0;
            }

            CAnimate::iGoreLevel = iGore;
            bInvertMouse = GetRegValue(REG_KEY_INVERTMOUSE, DEFAULT_INVERTMOUSE) ? true : false;
        }
	}

    g_CTPassGlobals.SetupBackground();
    RefreshAudioSettings();

    i = GetRegValue(REG_KEY_RENDERING_QUALITY, DEFAULT_RENDERING_QUALITY);
    SetQualitySetting(i);

    CMessageSystem(escSTART_SIM).Dispatch();

    m_puictlCheat = (CUIEditbox *)GetUICtrl(102);

    ResizeScreen(prasMainScreen->iWidthFront, prasMainScreen->iHeightFront);

    g_CTPassGlobals.InitGamma();

    return TRUE;
}

extern BOOL bQuitGame;

void CGameWnd::OnDestroy()
{
    RECT        rc;

    // We are leaving the game and returning to the menu, which renders at the
    // fixed menu resolution.  Clear bInGame first so GetCurrentClientSize()
    // reports the menu size, not the (higher) in-game render size, when the
    // screen below is recreated.  (The destructor also clears it - harmless.)
    g_CTPassGlobals.bInGame = false;

    POINT screenSize = GetCurrentClientSize();

    SetRect(&rc, 0, 0, screenSize.x, screenSize.y);
    ClipCursor(&rc);

	if (!bQuitGame)
		prnshMain->bCreateScreen(screenSize.x, screenSize.y, 16, bGetSystemMem());

    CUIWnd::OnDestroy();
}


void CGameWnd::UIEditboxKey(CUICtrl * pctrl, UINT vk, int cRepeat, UINT flags)
{
    switch (vk)
    {
        case VK_ESCAPE:
        case VK_F11:
            ClearInputState(true);

            gpInputDeemone->Capture(true);
            m_puictlCheat->SetVisible(FALSE);
            m_iClear = 3;
            break;

        case VK_RETURN:
            {
                char    sz[255];
                LPSTR   psz;

                psz = m_puictlCheat->GetText();
                strcpy(sz, psz);

                if (ExecuteCheat(sz))
                {
                    m_puictlCheat->SetText(NULL);
                }
            }
            break;

    }
}

void CGameWnd::OnChar(TCHAR ch, int cRepeat)
{
    if (m_puictlCheat->GetVisible())
    {
        m_puictlCheat->OnChar(ch, cRepeat);
        return;
    }

    switch (ch)
    {
        case ']':
            g_CTPassGlobals.RenderQualityUp();
            break;

        case '[':
            g_CTPassGlobals.RenderQualityDn();
            break;
    }
}


void CGameWnd::SetupGameStoppage()
{
    CMessageSystem      msg(escSTOP_SIM);

    // Stop the simulation
    msg.Dispatch();

    m_bPaused = TRUE;
    m_pUIMgr->m_bDrawMouse = TRUE;

	// Limit cursor movement to the actual window client area (screen
	// coordinates), not the render resolution.  In borderless fullscreen the
	// render buffer (e.g. 320x240) is upscaled to fill the window, so clipping
	// to the render size would trap the cursor in a tiny top-left box.  The UI
	// loop maps the screen position back into render space.
	{
		RECT  rc;
		POINT ptTL, ptBR;
		GetClientRect(g_hwnd, &rc);
		ptTL.x = rc.left;  ptTL.y = rc.top;
		ptBR.x = rc.right; ptBR.y = rc.bottom;
		ClientToScreen(g_hwnd, &ptTL);
		ClientToScreen(g_hwnd, &ptBR);
		SetRect(&rc, ptTL.x, ptTL.y, ptBR.x, ptBR.y);
		ClipCursor(&rc);

		// Center the cursor on the real client area centre.
		SetCursorPos((ptTL.x + ptBR.x) / 2, (ptTL.y + ptBR.y) / 2);
	}

    // Capture from the back buffer (pddsDraw), not the primary.  In borderless
    // windowed mode the primary is the DWM-composited desktop; reading it back
    // via GDI GetDC is unreliable (and returns the pillar-boxed, upscaled image
    // rather than the clean render), which showed up as a purple pause-menu
    // background.  pddsDraw holds the last rendered frame in native 16-bit 565.
    g_CTPassGlobals.CaptureBackground(true);
	g_CTPassGlobals.bHardReset = false;
}


void CGameWnd::ClearGameStoppage(BOOL bStartSim)
{
    RECT    rc;
    int     i;

    for (i = prasMainScreen->iBuffers; i > 0; i--)
    {
        SetRect(&rc, 0, 0, prasMainScreen->iWidthFront, prasMainScreen->iHeightFront);
        MyFillRect(&rc, 0, prasMainScreen.ptPtrRaw());
        prasMainScreen->Flip();
    }

    if (bStartSim)
    {
        CMessageSystem(escSTART_SIM).Dispatch();
        m_pUIMgr->m_bDrawMouse = FALSE;
    }

    m_bPaused = FALSE;

	// Free cursor movement.
	ClipCursor(0);

	// Center Mouse so we don't throw the user's view out of wack.
	// Copied code out of control.cpp so it will work on Voodoo cards.
	RECT rect;
	POINT point;
	GetClientRect(g_hwnd, &rect);
	point.x = rect.right/2;
	point.y = rect.bottom/2;
	ClientToScreen(g_hwnd, &point);
	SetCursorPos(point.x, point.y);
}


//
// The game's levels in STORY order.  This is NOT the alphabetical order of the scene
// files - they are abbreviations, so sorting by filename puts both Ascent levels before
// the Beach.  (The old Ctrl+Shift+L cycler had its own copy of this list with InGen Lab
// and InGen Town the wrong way round; both cheats now share this one.)
//
//
// Debug cheat: set true while P is held, to walk forward at 10x.  Sampled here in the input
// layer; read by Player.cpp (to walk forward with no key) and by the physics skeleton (which
// applies the 10x, and DEFINES this - see InfoSkeleton.cpp for why it lives there).
//
extern bool g_bSpeedCheat;

static const LPCSTR aszStoryLevels[] =
{
    "be.scn",       // Beach
    "jr.scn",       // Jungle Road
    "ij.scn",       // Industrial Jungle
    "it.scn",       // InGen Town
    "lab.scn",      // InGen Laboratories
    "as.scn",       // Ascent, part 1
    "as2.scn",      // Ascent, part 2
    "sum.scn",      // Summit
};
static const int iStoryLevels = sizeof(aszStoryLevels) / sizeof(aszStoryLevels[0]);


//+--------------------------------------------------------------------------
//
//  Member:     CGameWnd::JumpLevel
//
//  Synopsis:   Cheat: load the next (iDir +1) or previous (iDir -1) level.
//
//---------------------------------------------------------------------------
void CGameWnd::JumpLevel(int iDir)
{
    // m_szSCN is the scene the current level was loaded from.
    int iCur = -1;
    for (int i = 0; i < iStoryLevels; ++i)
    {
        if (_stricmp(g_CTPassGlobals.m_szSCN, aszStoryLevels[i]) == 0)
        {
            iCur = i;
            break;
        }
    }

    // Not one of the story levels (a test scene, say): there is no "next" from
    // outside the sequence, so enter it at whichever end we are heading towards.
    int iNext = (iCur < 0) ? ((iDir > 0) ? 0 : iStoryLevels - 1) : iCur + iDir;

    if (iNext < 0 || iNext >= iStoryLevels)
        return;         // already at the first/last level - nowhere to jump

    //
    // Hand the load to the world as a DEFERRED one rather than calling LoadLevel
    // here: the game loop picks it up between steps, where it stops the sim, shows
    // the loading message and tears the old level down safely.  Doing that from
    // inside a key handler would destroy the world mid-message-dispatch.
    //
    wWorld.DeferredLoad(aszStoryLevels[iNext]);
}


//+--------------------------------------------------------------------------
//
//  Member:     CGameWnd::UpdateNoClip
//
//  Synopsis:   Cheat: hold Ctrl+C to drift forward through the world, ignoring
//              collisions.  For skipping puzzles when testing.
//
//---------------------------------------------------------------------------
void CGameWnd::UpdateNoClip()
{
    // How far to drift per frame, in world units.
    const float fNOCLIP_SPEED = 0.12f;

    static bool s_bGhosting = false;

    bool b_want = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 &&
                  (GetAsyncKeyState('C')        & 0x8000) != 0;

    if (b_want != s_bGhosting)
    {
        //
        // Physics owns the player's position, so she has to come OUT of the
        // simulation before we can place her by hand - otherwise collision
        // resolution just shoves her back out of whatever she is inside.  This is
        // the same pairing GUIApp's player-physics toggle uses.  bPhysics alone is
        // not enough: it only stops her issuing movement requests.
        //
        gpPlayer->bPhysics = !b_want;

        if (b_want)
            gpPlayer->PhysicsDeactivate();
        else
            gpPlayer->PhysicsActivate();

        s_bGhosting = b_want;
    }

    if (!s_bGhosting)
        return;

    // Drift along the camera's facing; +Y is forward in this engine.
    CCamera* pcam = CWDbQueryActiveCamera().tGet();
    if (!pcam)
        return;

    CVector3<>    v3_fwd = d3YAxis * pcam->pr3Presence().r3Rot;
    CPlacement3<> p3     = gpPlayer->pr3Presence();

    p3.v3Pos += v3_fwd * fNOCLIP_SPEED;
    gpPlayer->Move(p3);
}


void CGameWnd::OnKey(UINT vk, BOOL fDown, int cRepeat, UINT flags)
{
    if (m_puictlCheat->GetVisible())
    {
        m_puictlCheat->OnKey(vk, fDown, cRepeat, flags);
        return;
    }

    if (!fDown)
    {
        return;
    }

    switch (vk)
    {
        case VK_SPACE:
            if (gpPlayer->bDead() && m_iNoticedDead > COUNT_NOTICED_DEAD)
            {
                SetupGameStoppage();

                CYesNoMsgDlg   dlg(IDS_RESTARTLEVEL, m_pUIMgr);

                dlg.StartUIWnd();
                if (dlg.m_dwExitValue != 0)
                {
                    g_CTPassGlobals.LoadLastScene();
                }

                ClearGameStoppage(TRUE);
            }
            break;

        // Cheat: Ctrl+. jumps forward a level, Ctrl+, jumps back (story order).
        // Without Ctrl these fall through to the game's own handling.
        case VK_OEM_PERIOD:     // . and >
            if (GetAsyncKeyState(VK_CONTROL) < 0)
                JumpLevel(1);
            break;

        case VK_OEM_COMMA:      // , and <
            if (GetAsyncKeyState(VK_CONTROL) < 0)
                JumpLevel(-1);
            break;

		case 0xbb:
			if (GetAsyncKeyState(VK_CONTROL) < 0)
			{
                ChangeViewportSize(-1, 0);
				break;
			}
			if (GetAsyncKeyState(VK_SHIFT) < 0)
			{
                ChangeViewportSize(0, -1);
				break;
			}
            ChangeViewportSize(-1, -1);
			break;

		case 0xbd:		// - and _
			if (GetAsyncKeyState(VK_CONTROL) < 0)
			{
                ChangeViewportSize(1, 0);
				break;
			}
			if (GetAsyncKeyState(VK_SHIFT) < 0)
			{
                ChangeViewportSize(0, 1);
				break;
			}
            ChangeViewportSize(1, 1);
			break;

        case VK_F1:
            {
                CHintWnd        dlg(m_pUIMgr);

                SetupGameStoppage();
                g_CTPassGlobals.CreateMenuAudioDatabase();
                g_CTPassGlobals.SetupButtonAudio();

                dlg.StartUIWnd();

                g_CTPassGlobals.FreeMenuAudio();
                ClearGameStoppage(TRUE);
            }
            break;

        case VK_F11:
			if (GetAsyncKeyState(VK_CONTROL) < 0)
			{
                if (m_puictlCheat->GetVisible())
                {
                    ClearInputState(true);

                    gpInputDeemone->Capture(true);

                    m_puictlCheat->SetVisible(FALSE);
                }
                else
                {
                    ClearInputState(false);

                    gpInputDeemone->Capture(false);
                    m_puictlCheat->SetVisible(TRUE);
                    m_puictlCheat->SetText(NULL);
                }

                m_iClear = 3;
            }
            break;

        case VK_F12:
            {
                CControlsWnd    dlg(m_pUIMgr);

                SetupGameStoppage();
                g_CTPassGlobals.CreateMenuAudioDatabase();
                g_CTPassGlobals.SetupButtonAudio();

                dlg.StartUIWnd();

                g_CTPassGlobals.FreeMenuAudio();
                ClearGameStoppage(TRUE);
            }
            break;

        case VK_ESCAPE:
            {
                CInGameOptionsWnd   dlg(m_pUIMgr);
                BOOL                bContinue = TRUE;

                SetupGameStoppage();

                // Popup the dialog
                dlg.StartUIWnd();

                if (dlg.m_dwExitValue == (DWORD)-1)
                {
                    CQuitWnd        dlgQuit(m_pUIMgr);

                    dlgQuit.StartUIWnd();
                
                    switch (dlgQuit.m_dwExitValue)
                    {
                        // Quit Game
                        case 1:
                            m_pUIMgr->m_bDrawMouse = TRUE;
                            bContinue = FALSE;
							bQuitGame = TRUE;
                            EndUIWnd(-1);
                            break;

                        // Quit Menu
                        case 2:
                            EndUIWnd(1);
                            m_pUIMgr->m_bDrawMouse = TRUE;
                            bContinue = FALSE;
                            break;
                    }
                }
				else if (dlg.m_dwExitValue == 3)
				{
					// restart the game with the Render dialog
					EndUIWnd(3);
				}

				if (dlg.m_dwExitValue == 3)
				{
					// If we are going to the render dialog then do not send a start
					// message otherwise audio will be started for a fraction of a
					// second and it sounds bad.
					ClearGameStoppage(false);
				}
				else
				{
					ClearGameStoppage(bContinue);
				}
            }
            break;

#if BUILDVER_MODE != MODE_FINAL
		case 'T':
		{
			if ((GetAsyncKeyState(VK_CONTROL) & (SHORT)0xFFFE) != 0)
			{
				extern void PlayerTeleportToNextLocation();
				PlayerTeleportToNextLocation();
			}
		}
		break;
#endif

		// DIAGNOSTIC: Ctrl+Shift+L cycles to the next game level, so all 8
		// levels' textures can be streamed in and dumped in one session.  Unlike
		// Ctrl+. this WRAPS, so a single session can walk the whole set.
		case 'L':
			if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
			    (GetAsyncKeyState(VK_SHIFT)   & 0x8000))
			{
				static int s_i_level = 0;
				wWorld.DeferredLoad(aszStoryLevels[s_i_level % iStoryLevels]);
				s_i_level++;
			}
			break;
    }
}


void CGameWnd::InnerLoopCall()
{
    if (m_bPaused)
    {
        return;
    }

    // Cheat: Ctrl+C walks through the world (see UpdateNoClip).
    UpdateNoClip();

    // Cheat: hold P to walk forward at 10x (applied in Player.cpp's move handling).
    g_bSpeedCheat = (GetAsyncKeyState('P') & 0x8000) != 0;

    if (gpPlayer->bDead())
    {
        m_iNoticedDead++;
    }
    else
    {
        m_iNoticedDead = 0;
    }


    if (wWorld.bIsGameOver())
    {
		// Stop simulation.
		CMessageSystem(escSTOP_SIM).Dispatch();

        EndUIWnd((DWORD)-2);
    }

	// Check for pending save.
	if (wWorld.bIsSavePending())
	{
		const std::string &strName = wWorld.strGetPendingSave();

		// Stop simulation.
		CMessageSystem(escSTOP_SIM).Dispatch();

		// Save the level.
		g_CTPassGlobals.SaveGame(strName.c_str());

		// Start simulation.
		CMessageSystem(escSTART_SIM).Dispatch();
	}

	// Check for pending level load.
	if (wWorld.bIsLoadPending())
	{
		const std::string &strName = wWorld.strGetPendingLoad();

		// Stop simulation.
		CMessageSystem(escSTOP_SIM).Dispatch();

        MiddleMessage(IDS_LOADING_LEVEL);

		// Load a new level.
		g_CTPassGlobals.LoadLevel(strName.c_str());

		// Start simulation.
		CMessageSystem(escSTART_SIM).Dispatch();

		// Don't step just now.
		return;
	}

	gmlGameLoop.Step();
	InvalidateRect(NULL);
}


void CGameWnd::GetWndFile(LPSTR psz, int ic)
{
    strcpy(psz, "gamewnd.ddf");
}


BOOL CGameWnd::OnEraseBkgnd(HWND hwnd, HDC hdc)
{
    HDC     hdcSrc;

    if (!g_CTPassGlobals.m_prasBkgnd)
    {
        return FALSE;
    }

    hdcSrc = g_CTPassGlobals.m_prasBkgnd->hdcGet();

    BitBlt(hdc, 
           0, 
           0, 
           g_CTPassGlobals.m_prasBkgnd->iWidth, 
           g_CTPassGlobals.m_prasBkgnd->iHeight, 
           hdcSrc,
           0, 
           0, 
           SRCCOPY);

    g_CTPassGlobals.m_prasBkgnd->ReleaseDC(hdcSrc);

    return TRUE;
}


void CGameWnd::ResizeScreen(int iWidth, int iHeight)
{
    CUICtrl *   pctrl;
    RECT        rc;

    pctrl = GetUICtrl(100);
    if (pctrl)
    {
        pctrl->GetRect(&rc);
        rc.bottom = rc.bottom - rc.top;
        rc.top = prasMainScreen->iHeightFront - rc.bottom;
        rc.bottom += rc.top;
        pctrl->SetRect(&rc);
    }

    pctrl = GetUICtrl(101);
    if (pctrl)
    {
        pctrl->GetRect(&rc);
        rc.bottom = rc.bottom - rc.top;
        rc.right = rc.right - rc.left;
        rc.left = prasMainScreen->iWidthFront - rc.right;
        rc.right += rc.left;
        rc.bottom += rc.top;
        pctrl->SetRect(&rc);
    }

    pctrl = GetUICtrl(102);
    if (pctrl)
    {
        pctrl->GetRect(&rc);
        rc.bottom = rc.bottom - rc.top;
        rc.top = prasMainScreen->iHeightFront - rc.bottom;
        rc.bottom += rc.top;
        pctrl->SetRect(&rc);
    }
}


//+--------------------------------------------------------------------------
//
//  Function:   ReclaimForeground
//
//  Synopsis:   Take the foreground window back, for when something else stole
//              it - specifically the OpenXR runtime, which puts its own window
//              up as it starts and leaves Trespasser running without focus.
//
//              THE SYMPTOM THIS FIXES IS DELIBERATELY MISLEADING: the game
//              carries on responding to movement and mouse-look, because the
//              live input path reads GetAsyncKeyState/GetCursorPos, which are
//              global and do not care about focus (Control.cpp).  Only the
//              MESSAGE-driven keys die - Esc, F12, the cheat keys - so the game
//              feels focused while its menu key does nothing, which reads like a
//              broken menu rather than a focus problem.
//
//              SetForegroundWindow alone usually fails here: Windows only
//              grants it to a process that is already foreground or received
//              the last input event, and by definition neither is true.  The
//              documented way through is to attach to the foreground thread's
//              input queue first, which makes the two threads share focus state
//              for the duration of the call.
//
//  History:    23-Jul-26   Created
//
//---------------------------------------------------------------------------
static void ReclaimForeground(HWND hwnd)
{
    HWND    hwndFore = GetForegroundWindow();

    if (!hwnd || hwndFore == hwnd)
    {
        return;
    }

    if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd)
    {
        return;
    }

    DWORD   dwFore = GetWindowThreadProcessId(hwndFore, NULL);
    DWORD   dwSelf = GetCurrentThreadId();

    if (dwFore && dwFore != dwSelf && AttachThreadInput(dwSelf, dwFore, TRUE))
    {
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(dwSelf, dwFore, FALSE);
    }
}


void CGameWnd::DrawWndInfo(CRaster * pRaster, RECT * prc)
{
    if (m_bPaused)
    {
        RasterBlt(g_CTPassGlobals.m_prasBkgnd, prc, pRaster, 0, 0, FALSE, 0);
    }
    else
    {
	    // EXPERIMENTAL D3D11 PRESENT (env TRESPASS_D3D11): hand the backend the game
	    // window so it can lazily create its device/swap chain on it.  No-op unless
	    // enabled; idempotent.
	    RenderD3D11::SetWindow(g_hwnd);

	    // EXPERIMENTAL VR BRING-UP (env TRESPASS_VR): attempt OpenXR instance/system
	    // creation once (idempotent; self-guards after the first try).  No-op unless
	    // enabled or no OpenXR runtime is present - it just logs and returns.  M1 only
	    // brings the runtime up and logs what it finds; it renders nothing yet.
	    RenderVR::bInit();

	    // Start this frame on the VR runtime's clock: creates the session once the renderer
	    // has a device, pumps the OpenXR event queue, and picks up the frame the timing
	    // thread has waited for.  Never blocks - see RenderVR.hpp.  No-op without VR.
	    RenderVR::FrameBegin();

	    // Take the foreground back once the VR session is up.  The runtime grabbed it while
	    // starting, which costs the player every message-driven key (Esc above all) while
	    // leaving movement working, so the game looks fine and has no menu.
	    //
	    // ONCE, not every frame: if the player deliberately switches to the runtime's own
	    // window later, snatching focus back would be a fight they cannot win.  This only
	    // undoes the theft that happens during start-up, which is the case that has no other
	    // remedy - by the time the session exists the player has no way to ask for focus but
	    // to alt-tab twice, and no reason to know that is what is wrong.
	    {
	        static bool s_b_reclaimed = false;
	        if (!s_b_reclaimed && RenderVR::bSessionRunning())
	        {
	            s_b_reclaimed = true;
	            ReclaimForeground(g_hwnd);
	            RenderVR::LogLine(GetForegroundWindow() == g_hwnd
	                ? "TRESPASS_VR: took the foreground back from the runtime (Esc/menu keys live)\n"
	                : "TRESPASS_VR: could NOT take the foreground back - Esc will not reach the game\n"
	                  "             until its window is clicked or alt-tabbed to\n");
	        }
	    }

	    gmlGameLoop.Paint();

	    // PROFILER DUMP (DIAGNOSTIC, env TRESPASS_PROFILE): the render pipeline
	    // already feeds per-stage timers (Transform/Clip/DepthSort/DrawPolygon)
	    // into proProfile via CProfileStat::Add.  When TRESPASS_PROFILE is set,
	    // accumulate those over a window of frames and write the averaged tree
	    // to %TEMP%\trespass_profile.txt, then reset for the next window.  In
	    // the normalised output psRender's per-count value is ms/frame; leaf
	    // stages show ms and % of parent - i.e. how much of a frame the CPU
	    // geometry pass costs (the number that gates VR feasibility).  Requires
	    // a Release or Debug build (VER_TIMING_STATS is FALSE in MODE_FINAL).
#if VER_TIMING_STATS
	    {
	        static int s_i_profile = -1;
	        static int s_i_window  = 300;
	        if (s_i_profile < 0)
	        {
	            char ach_env[32];
	            int  i_len = GetEnvironmentVariableA("TRESPASS_PROFILE", ach_env, sizeof(ach_env));
	            s_i_profile = i_len > 0 ? 1 : 0;
	            if (s_i_profile)
	            {
	                int i_val = atoi(ach_env);
	                if (i_val >= 10)
	                    s_i_window = i_val;
	            }
	        }
	        if (s_i_profile)
	        {
	            static int s_i_frames = 0;
	            if (++s_i_frames >= s_i_window)
	            {
	                CStrBuffer strbuf(8000);
	                CProfileStat::WriteHeader(strbuf);
	                proProfile.psMain.WriteToBuffer(strbuf, 0, true);

	                char ach_path[MAX_PATH];
	                int  i_dir = GetTempPathA(sizeof(ach_path), ach_path);
	                strcpy(ach_path + i_dir, "trespass_profile.txt");

	                // Explicit ms/frame for the stages that matter.  The tree below
	                // normalises to ms-per-COUNT (per-poly/per-vertex), which rounds
	                // the big sub-stages to 0.0; fSeconds()/frames gives ms/FRAME.
	                float f_fr = (float)s_i_frames;
	                #define MSPF(ps) ((ps).fSeconds() * 1000.0f / f_fr)

	                // APPEND, numbering each window.  Overwriting meant only the last window
	                // survived, so a reading depended entirely on where the player happened to
	                // be standing - and successive windows could not be compared at all.  With
	                // every window kept, standing still gives repeated windows of the same
	                // scene: if those climb, the per-window Reset() is not working; if they are
	                // steady, differences between runs are the scene, not a measurement bug.
	                static int s_i_dump = 0;
	                ++s_i_dump;

	                FILE* pf = fopen(ach_path, "a");
	                if (pf)
	                {
	                    fprintf(pf, "\n================ window %d (%d frames) ================\n",
	                            s_i_dump, s_i_frames);
	                    fprintf(pf, "KEY STAGES (ms/frame, single CPU thread)\n");
	                    fprintf(pf, "  Frame TOTAL   : %6.2f\n", MSPF(proProfile.psFrame));
	                    fprintf(pf, "   Step (sim)   : %6.2f   [AI %5.2f  Physics %5.2f]\n",
	                            MSPF(proProfile.psStep), MSPF(proProfile.psAI), MSPF(proProfile.psPhysics));
	                    fprintf(pf, "   Render TOTAL : %6.2f\n", MSPF(proProfile.psRender));
	                    fprintf(pf, "    Occlusion   : %6.2f\n", MSPF(proProfile.psOcclusion));
	                    fprintf(pf, "    RenderShape : %6.2f   (geometry: transform/clip/light/project)\n",
	                            MSPF(proProfile.psRenderShape));
	                    fprintf(pf, "    Presort     : %6.2f   (DEPTH SORT + quicksort + splits)\n",
	                            MSPF(proProfile.psPresort));
	                    // psDrawPolygon brackets the whole VIRTUAL DrawPolygons call
	                    // (PipeLine.cpp), so under TRESPASS_D3D11 it times the GPU MIRROR
	                    // (texture resolve + vertex build + submit) and NOT the software
	                    // fill, which the mirror skips.  Labelling it "software FILL" sent
	                    // us looking for a rasteriser that was already gone.
	                    fprintf(pf, "    DrawPolygon : %6.2f   (%s)\n",
	                            MSPF(proProfile.psDrawPolygon),
	                            RenderD3D11::bEnabled() ? "D3D11: GPU mirror; software fill skipped"
	                                                    : "software FILL - scales with pixels/res");
	                    fprintf(pf, "    TerrainUpd  : %6.2f\n", MSPF(proProfile.psTerrainUpdate));
	                    // Brackets ClearMemSurfaces, which also draws the sky - and under
	                    // D3D11 captures it full-screen for the GPU blit.
	                    fprintf(pf, "    ClearScreen : %6.2f   (%s)\n",
	                            MSPF(proProfile.psClearScreen),
	                            RenderD3D11::bEnabled() ? "clear + sky draw + sky capture for the GPU"
	                                                    : "clear + sky draw");
	                    fprintf(pf, "    BeginFrame  : %6.2f\n", MSPF(proProfile.psBeginFrame));
	                    fprintf(pf, "    EndFrame    : %6.2f   [Flip %5.2f]\n",
	                            MSPF(proProfile.psEndFrame), MSPF(proProfile.psFlip));
	                    // psDrawPolygon carries no COUNT - PipeLine adds only cycles to it, so
	                    // this always printed 0 and was useless.  psPresort counts the polygons
	                    // it sorts, which is the same list the rasteriser/mirror then walks.
	                    // Reporting that, and the per-polygon cost, lets readings taken at
	                    // different viewpoints be compared at all - ms/frame alone cannot
	                    // distinguish "my change got slower" from "there is more on screen".
	                    float f_polys = proProfile.psPresort.iGetCount() / f_fr;
	                    fprintf(pf, "  Render polys/frame : %.0f\n", f_polys);
	                    fprintf(pf, "  DrawPolygon us/poly: %.2f   <- compare THIS across runs\n",
	                            f_polys > 0.0f ? MSPF(proProfile.psDrawPolygon) * 1000.0f / f_polys : 0.0f);
	                    fprintf(pf, "\n---- full normalised tree (ms per COUNT) ----\n");
	                    fputs((const char*)strbuf, pf);
	                    fclose(pf);
	                    OutputDebugStringA("TRESPASS_PROFILE: wrote ");
	                    OutputDebugStringA(ach_path);
	                    OutputDebugStringA("\n");
	                }

	                #undef MSPF
	                proProfile.psMain.Reset();
	                s_i_frames = 0;
	            }
	        }
	    }
#endif // VER_TIMING_STATS

        if (m_puictlCheat->GetVisible() || m_iClear)
        {
            prasMainScreen->ClearBorder(true);
        }

        if (m_iClear > 0)
        {
            m_iClear--;
        }

	    prenMain->pScreenRender->EndFrame();

        if (g_bDisplayLocation)
        {
            char            sz[50];
            HDC             hdc;
			CCamera* pcam = CWDbQueryActiveCamera().tGet();
            CPlacement3<>   place = pcam->pr3Presence();
            CVector3<>      unit(0.0, 1.0, 0.0);

            sprintf(sz, 
                    "Location:  %.1f, %.1f, %.1f",
                    place.v3Pos.tX,
                    place.v3Pos.tY,
                    place.v3Pos.tZ);

            hdc = prasMainScreen->hdcGet();
            TextOut(hdc, 0, 15, sz, strlen(sz));
            prasMainScreen->ReleaseDC(hdc);

            unit = unit * place.r3Rot;
            sprintf(sz, 
                    "Facing:    %.1f, %.1f, %.1f",
                    unit.tX,
                    unit.tY,
                    unit.tZ);

            hdc = prasMainScreen->hdcGet();
            TextOut(hdc, 0, 30, sz, strlen(sz));
            prasMainScreen->ReleaseDC(hdc);

        }

        if (g_bDisplayFPS)
        {
            char sz[50];
            HDC hdc;

            if (dwTimeLastSample != 0)
            {
                DWORD dwTimeNow = timeGetTime();
                DWORD dwTimeElapsed = dwTimeNow - dwTimeLastSample;
                if ((dwTimeElapsed > 1000) && (iNumFramesThisSample > 0))
                {
                    float fTimeElapsed = (float)dwTimeElapsed / 1000.0f;
                    fFrameRate = iNumFramesThisSample / fTimeElapsed;
                    iNumFramesThisSample = 0;
                    dwTimeLastSample = dwTimeNow;
                }
            }
            else
            {
                dwTimeLastSample = timeGetTime();
            }
            iNumFramesThisSample++;

            sprintf(sz, 
                    "FPS: %i (%.2f ms)", 
                    (int)fFrameRate, 
                    1000.0f / fFrameRate);

            hdc = prasMainScreen->hdcGet();
            TextOut(hdc, 0, 0, sz, strlen(sz));
            prasMainScreen->ReleaseDC(hdc);
        }
    }
}



