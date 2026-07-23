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

#include "supportfn.hpp"
#include "resource.h"
#include "main.h"
#include "tpassglobals.h"
// bActive() - VR is in play, so keep rendering through focus loss.  Deliberately NOT
// bSessionRunning(): the runtime takes foreground BEFORE the session can exist (the session
// needs the D3D11 device, which is only created once the game starts painting), so a
// session-based guard is still false at the exact moment the focus is stolen.
#include "..\Lib\View\RenderVR.hpp"
#include "..\Lib\Sys\reg.h"
#include "..\lib\sys\reginit.hpp"
#include "uiwnd.h"
#include "uidlgs.h"
#include "video.h"


extern HINSTANCE        g_hInst;
extern HWND			    g_hwnd;
extern CMainWnd *       g_pMainWnd;
extern UINT             g_uiRegMsg;
extern bool             g_bSystemMem;
extern CRenderShell *   prnshMain;
extern bool				g_bRestartWithRenderDlg;


CMainWnd::CMainWnd()
{
    m_pUIMgr = NULL;
    m_hwnd = NULL;
    m_bRelaunch = false;
}


CMainWnd::~CMainWnd()
{
    delete m_pUIMgr;
}



BOOL CMainWnd::InitSurface()
{
    char szSource[_MAX_PATH];
    RECT rc;

	#if defined(__MWERKS__)
		SetupForSelfModifyingCode(g_hInst);
	#endif

    GetRegString("Data Drive", szSource, sizeof(szSource), "");
    strcat(szSource, "data\\");

    CAudioDaemon::SetDataPath(szSource);

	// Create the main palette if there isn't one.
    if (!pcdbMain.ppalMain)
	{
		pcdbMain.ppalMain = ppalGetPaletteFromResource(g_hInst, IDB_DEF_PAL);
	}

	// Create the main clut if there isn't one.
    pcdbMain.CreateMainClut();

	// Create the world database.
	if (!pwWorld)
		pwWorld = new CWorld();

	// Open the performance monitoring system
	// set all performance timers to read clock ticks
	iPSInit();

    POINT screenSize = GetCurrentClientSize();

	if (!prnshMain)
	{
		prnshMain = new CRenderShell(m_hwnd, g_hInst, false);
		if (!prnshMain)
		{
			return FALSE;
		}

		if (!prnshMain->bCreateScreen(screenSize.x, screenSize.y, 16, bGetSystemMem()))
		{
			return FALSE;
		}
	}

    // Clip the cursor to the actual window client area (screen coordinates),
    // not the render resolution.  In borderless fullscreen the render buffer is
    // upscaled, so the window (full screen) is larger than screenSize; the
    // cursor position is mapped back to render space in the UI loop.
    {
        RECT  rcClient;
        POINT ptTL, ptBR;
        GetClientRect(m_hwnd, &rcClient);
        ptTL.x = rcClient.left;  ptTL.y = rcClient.top;
        ptBR.x = rcClient.right; ptBR.y = rcClient.bottom;
        ClientToScreen(m_hwnd, &ptTL);
        ClientToScreen(m_hwnd, &ptBR);
        SetRect(&rc, ptTL.x, ptTL.y, ptBR.x, ptBR.y);
    }
    ClipCursor(&rc);

	if (!m_pUIMgr)
	{
		m_pUIMgr = new CUIManager();
		m_pUIMgr->m_hwnd = g_hwnd;
	}

    if (prasMainScreen->pxf.cposG.u1WidthDiff != 3)
    {
        m_pUIMgr->m_wTransColor = MAKE565(0, 255, 0);
    }
    else
    {
        m_pUIMgr->m_wTransColor = MAKE555(0, 255, 0);
    }

    return TRUE;
}


void CMainWnd::StartGame()
{
    LONG    lExStyle;
    LONG    lStyle;

    lExStyle = GetWindowLong(m_hwnd, GWL_EXSTYLE);
    lStyle = GetWindowLong(m_hwnd, GWL_STYLE);

    SetCapture(m_hwnd);
    SetCursor(LoadCursor(NULL, IDC_ARROW));
    ForceShowCursor(FALSE);

    m_pUIMgr->m_bActive = TRUE;

    GameLoop();

    ClipCursor(NULL);
    ReleaseCapture();
	ForceShowCursor(TRUE);

    SetWindowLong(m_hwnd, GWL_EXSTYLE, lExStyle);
    SetWindowLong(m_hwnd, GWL_STYLE, lStyle);

	// Purge the world database to ensure that all the instances are deleted 
    // before the static variables are deleted.
	wWorld.Purge();

	// And then delete the world database itself.
	delete pwWorld;
    pwWorld = NULL;
}



void CMainWnd::OnCommand(HWND hwnd, int id, HWND hwndCtl, UINT codeNotify)
{
    CUIWnd *        puiwnd;

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnCommand(id, hwndCtl, codeNotify);
    }
}



void CMainWnd::OnKey(HWND hwnd, UINT vk, BOOL fDown, int cRepeat, UINT flags)
{
    CUIWnd *    puiwnd;

    if (vk == VK_SNAPSHOT)
    {
        ScreenCapture();
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnKey(vk, fDown, cRepeat, flags);
    }
}



void CMainWnd::OnChar(HWND hwnd, TCHAR ch, int cRepeat)
{
    CUIWnd *    puiwnd;

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnChar(ch, cRepeat);
    }
}



BOOL CMainWnd::OnCreate(HWND hwnd, LPCREATESTRUCT lpCreateStruct)
{
    m_hwnd = hwnd;
    g_hwnd = hwnd;

    return TRUE;
}



void CMainWnd::OnDestroy(HWND hwnd)
{
    m_hwnd = NULL;
}

void CMainWnd::OnClose(HWND hwnd)
{
}



void CMainWnd::OnMouseMove(HWND hwnd, int x, int y, UINT keyFlags)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnMouseMove(x, y, keyFlags);
    }
}


void CMainWnd::OnRButtonDown(HWND hwnd,
                             BOOL fDoubleClick,
                             int x,
                             int y,
                             UINT keyFlags)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnRButtonDown(fDoubleClick, x, y, keyFlags);
    }
}


void CMainWnd::OnLButtonDown(HWND hwnd,
                             BOOL fDoubleClick,
                             int x,
                             int y,
                             UINT keyFlags)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnLButtonDown(fDoubleClick, x, y, keyFlags);
    }
}


void CMainWnd::OnLButtonUp(HWND hwnd, int x, int y, UINT keyFlags)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnLButtonUp(x, y, keyFlags);
    }
}


void CMainWnd::OnMButtonDown(HWND hwnd, BOOL fDoubleClick, int x, int y, UINT keyFlags)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnMButtonDown(fDoubleClick, x, y, keyFlags);
    }
}


void CMainWnd::OnSysCommand(HWND hwnd, UINT cmd, int x, int y)
{
    switch (cmd)
    {
        default:
            FORWARD_WM_SYSCOMMAND(hwnd, cmd, x, y, DefWindowProc);

        case SC_KEYMENU:
        case SC_SCREENSAVE:
            break;
    }
}



void CMainWnd::SendActivate(BOOL fActivate, DWORD dwThreadId)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnActivateApp(fActivate, dwThreadId);
    }
}


void CMainWnd::OnActivate(HWND hwnd,
                          UINT state,
                          HWND hwndActDeact,
                          BOOL fMinimized)
{
	// Purge Direct3D textures.
	//d3dDriver.PurgePrimary();
}


void CMainWnd::OnActivateApp(HWND hwnd, BOOL fActivate, DWORD dwThreadId)
{
    bool        bState;

    if (!m_pUIMgr)
    {
        return;
    }

    bState = m_pUIMgr->m_bActive;

    if (!fActivate)
    {
        // Drop the always-on-top style whenever we lose focus so the taskbar,
        // Alt+Tab and Task Manager can come forward over the borderless
        // fullscreen window.  Without this a hung game stays pinned above
        // everything, which can leave Ctrl+Alt+Del showing only a black
        // screen.  Also release the cursor clip so the mouse isn't trapped.
        if (bGetFullScreen())
        {
            SetWindowPos(g_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ClipCursor(NULL);
        }

        // IN VR, DO NOT TEAR THE RENDERER DOWN ON FOCUS LOSS.
        //
        // Releasing the display when a fullscreen game is alt-tabbed away from is right for
        // 1998 - but in VR the headset is the primary display and the desktop window is only a
        // mirror, so losing foreground is routine (the runtime's own compositor or control
        // panel taking focus does it).  Destroying prasMainScreen there stops the game
        // rendering entirely: the window goes black, the cursor reappears, and the session
        // keeps running with nothing to submit until the player clicks the window back.
        //
        // The session is what matters, not the desktop focus, so while one is live this whole
        // branch is skipped and the game keeps rendering both eyes.
        if (m_pUIMgr->m_bActive && !RenderVR::bActive())
        {
            m_pUIMgr->m_bPause = TRUE;
            ForceShowCursor(TRUE);
            ClipCursor(NULL);

            // BUGBUG:  We may have to pause/halt audio and what not

			// Delete renderer stuff if required.
			if (g_CTPassGlobals.bInGame)
				std::destroy_at(&prasMainScreen);
        }

        if (!RenderVR::bActive())
            m_pUIMgr->m_bActive = FALSE;
    }
    else
    {
        if (GetForegroundWindow() != hwnd)
        {
            return;
        }

        ShowWindow(m_hwnd, SW_RESTORE);

        // Restore always-on-top now that we have focus again (fullscreen only).
        if (bGetFullScreen())
            SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

        ClearInputState();
        ForceShowCursor(FALSE);

        if (m_bInit)
        {
		    // Create renderer stuff.
		    if (!prasMainScreen)
		    {
			    InitSurface();
			    SetupGameScreen();
		    }

            if (!m_pUIMgr->m_bActive)
            {
                RECT    rc;

                m_pUIMgr->m_bPause = FALSE;

                if (prasMainScreen)
                {
                    SetRect(&rc, 
                            0, 
                            0, 
                            prasMainScreen->iWidthFront, 
                            prasMainScreen->iHeightFront);

                    SetWindowPos(g_hwnd, NULL, -1, -1, rc.right, rc.bottom,  
                                 SWP_NOMOVE | SWP_NOREDRAW | SWP_NOZORDER);

                    ClipCursor(&rc);
                }
            }
        }

        m_pUIMgr->m_bActive = TRUE;
    }

    InvalidateRect(hwnd, NULL, TRUE);
}


void CMainWnd::OnTimer(HWND hwnd, UINT id)
{
    for (auto i = m_pUIMgr->m_vUIWnd.rbegin(); i != m_pUIMgr->m_vUIWnd.rend(); i++)
    {
        if (*i && !(*i)->m_bExitWnd)
        {
            (*i)->OnTimer(id);
        }
    }
}


BOOL CMainWnd::OnEraseBkgnd(HWND hwnd, HDC hdc)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return FALSE;
    }

    if (!m_pUIMgr->m_bActive || IsIconic(hwnd))
    {
        return FORWARD_WM_ERASEBKGND(hwnd, hdc, DefWindowProc);
    }

    if (IsIconic(hwnd) && m_pUIMgr->m_bActive)
    {
        return FALSE;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        return puiwnd->OnEraseBkgnd(hwnd, hdc);
    }

    return FALSE;
}


void CMainWnd::OnPaint(HWND hwnd)
{
    CUIWnd *    puiwnd;

    if (!m_pUIMgr)
    {
        return;
    }

    if ((!m_pUIMgr->m_bActive || IsIconic(hwnd)) ||
        (m_pUIMgr->m_bActive && !m_bInit))
    {
        FORWARD_WM_PAINT(hwnd, DefWindowProc);
        return;
    }

    if (IsIconic(hwnd) && m_pUIMgr->m_bActive)
    {
        return;
    }

    puiwnd = m_pUIMgr->GetActiveUIWnd();
    if (puiwnd && !puiwnd->m_bExitWnd)
    {
        puiwnd->OnPaint(hwnd);
    }

    ValidateRect(hwnd, NULL);
}



void CMainWnd::CopyrightScreen()
{
#if 0
    CRaster *       prasBmp;
    RECT            rc;

    prasBmp = ReadAndConvertBMP("menu\\confid.tga");

    SetRect(&rc, 0, 0, prasBmp->iWidth, prasBmp->iHeight);

    RasterBlt(prasBmp, &rc, prasMainScreen.ptPtrRaw(), 0, 0, FALSE, 0);

    prasMainScreen->Flip();

    if (GetRegValue("NoCopyright", FALSE) == FALSE)
    {
        SleepEx(10000, FALSE);
    }

    delete prasBmp;
#endif
}


void CMainWnd::DWIPresents()
{
#if 0
    CRaster *       prasBmp;
    RECT            rc;

    prasBmp = ReadAndConvertBMP("menu\\dwipresents.tga");

    SetRect(&rc, 0, 0, prasBmp->iWidth, prasBmp->iHeight);

    RasterBlt(prasBmp, &rc, prasMainScreen.ptPtrRaw(), 0, 0, FALSE, 0);

    prasMainScreen->Flip();

    if (GetRegValue("NoCopyright", FALSE) == FALSE)
    {
        SleepEx(3000, FALSE);
    }

    delete prasBmp;
#endif
}


void CMainWnd::DWIIntroAVI()
{
#if 0 
    CVideoWnd   video(m_pUIMgr);

    video.Play("menu\\dwiintro");
#endif
}


void CMainWnd::IntroAVI()
{
    CVideoWnd   video(m_pUIMgr);

    video.Play("menu\\tpassintro");
}


void CMainWnd::LoadSplash()
{
    CRaster *       prasBmp;
    RECT            rc;

    prasBmp = ReadAndConvertBMP("menu\\splash.tga");

    SetRect(&rc, 0, 0, prasBmp->iWidth, prasBmp->iHeight);

    RasterBlt(prasBmp, &rc, prasMainScreen.ptPtrRaw(), 0, 0, FALSE, 0);

    prasMainScreen->Flip();

    delete prasBmp;
}


void CMainWnd::GameLoop()
{
    int             i = 1;
    BOOL            bQuit;
    CUIWnd *        puiwnd;

	// Check auto loading.
	BOOL b_autoload = bGetAutoLoad();
	SetAutoLoad(FALSE);

	if (!b_autoload && !m_bRelaunch)
	{
		CopyrightScreen();
        DWIPresents();
		DWIIntroAVI();
		IntroAVI();
	}

#if 0
    LoadSplash();
#endif

    if (m_pUIMgr->m_prasMouse[0])
    {
        delete m_pUIMgr->m_prasMouse[0];
    }
    m_pUIMgr->m_prasMouse[0] = ReadAndConvertBMP("menu\\mouse.bmp", true);

	if (b_autoload)
	{
		char  szDir[_MAX_PATH];

		GetFileLoc(FA_INSTALLDIR, szDir, sizeof(szDir));
		strcat(szDir, strTEMP_GAME);

		g_CTPassGlobals.LoadScene(szDir, NULL);
		i = 2;
	}

    bQuit = FALSE;

    do
    {
        switch(i)
        {
            case 1:
                puiwnd = new CMainScreenWnd(m_pUIMgr);
                break;

            case 2:
                puiwnd = new CGameWnd(m_pUIMgr);
                break;

			case 3:
				g_bRestartWithRenderDlg = TRUE;

            case (DWORD)-1:
                puiwnd = NULL;
                bQuit = TRUE;
                break;

            case (DWORD)-2:
                {
                    // Play Win Video
                    puiwnd = NULL;
                    i = 1;

					{
						CVideoWnd   video(m_pUIMgr);
						video.Play("menu\\win");
					}
					{
						CVideoWnd	videoWin(m_pUIMgr);
						videoWin.Play("menu\\credits");
					}
                }
                break;
        }

        if (puiwnd)
        {
            if (!puiwnd->StartUIWnd())
            {
                TraceError(("CMainWnd::GameLoop() -- StartUIWnd failed"));
                bQuit = TRUE;
            }

            i = (int)puiwnd->m_dwExitValue;
            delete puiwnd;
        }
    }
    while (!bQuit);
}



LRESULT CALLBACK WndProc(HWND hwnd, UINT uiMsg, WPARAM wParam, LPARAM lParam)
{
    if (g_pMainWnd == NULL)
    {
        return DefWindowProc(hwnd, uiMsg, wParam, lParam);
    }

    if (uiMsg == g_uiRegMsg)
    {
        if (IsMinimized(hwnd))
        {
            ShowWindow(hwnd, SW_RESTORE);
        }
        else
        {
            ShowWindow(hwnd, SW_SHOW);
        }
        return 0;
    }

    switch (uiMsg)
    {
		case WM_SYSKEYDOWN:
		case WM_SYSKEYUP:
			if (wParam==VK_MENU || wParam==VK_SPACE) return 0;
			break;

		case WM_WINDOWPOSCHANGING:
			// In borderless fullscreen, keep the window covering the whole
			// screen.  Several code paths (SetupGameScreen, the GDI loading
			// dialogs) shrink/move the main window to the render resolution,
			// which would break fullscreen; override them here in one place.
			if (bGetFullScreen())
			{
				WINDOWPOS* pwp = (WINDOWPOS*)lParam;
				pwp->x  = 0;
				pwp->y  = 0;
				pwp->cx = GetSystemMetrics(SM_CXSCREEN);
				pwp->cy = GetSystemMetrics(SM_CYSCREEN);
				pwp->flags &= ~(SWP_NOSIZE | SWP_NOMOVE);
				return 0;
			}
			break;

        HANDLE_MSG(hwnd, WM_ACTIVATE,       g_pMainWnd->OnActivate);
        HANDLE_MSG(hwnd, WM_ACTIVATEAPP,    g_pMainWnd->OnActivateApp);
        HANDLE_MSG(hwnd, WM_CLOSE,          g_pMainWnd->OnClose);
        HANDLE_MSG(hwnd, WM_COMMAND,        g_pMainWnd->OnCommand);
        HANDLE_MSG(hwnd, WM_DESTROY,        g_pMainWnd->OnDestroy);
        HANDLE_MSG(hwnd, WM_CREATE,         g_pMainWnd->OnCreate);
        HANDLE_MSG(hwnd, WM_KEYDOWN,        g_pMainWnd->OnKey);
        HANDLE_MSG(hwnd, WM_KEYUP,          g_pMainWnd->OnKey);
        HANDLE_MSG(hwnd, WM_CHAR,           g_pMainWnd->OnChar);
        HANDLE_MSG(hwnd, WM_MOUSEMOVE,      g_pMainWnd->OnMouseMove);
        HANDLE_MSG(hwnd, WM_LBUTTONDOWN,    g_pMainWnd->OnLButtonDown);
        HANDLE_MSG(hwnd, WM_LBUTTONDBLCLK,  g_pMainWnd->OnLButtonDown);
        HANDLE_MSG(hwnd, WM_LBUTTONUP,      g_pMainWnd->OnLButtonUp);
        HANDLE_MSG(hwnd, WM_PAINT,          g_pMainWnd->OnPaint);
        HANDLE_MSG(hwnd, WM_ERASEBKGND,     g_pMainWnd->OnEraseBkgnd);
        HANDLE_MSG(hwnd, WM_RBUTTONDOWN,    g_pMainWnd->OnRButtonDown);
        HANDLE_MSG(hwnd, WM_RBUTTONDBLCLK,  g_pMainWnd->OnRButtonDown);
        HANDLE_MSG(hwnd, WM_TIMER,          g_pMainWnd->OnTimer);
        HANDLE_MSG(hwnd, WM_SYSCOMMAND,     g_pMainWnd->OnSysCommand);
    }

    return DefWindowProc(hwnd, uiMsg, wParam, lParam);
}



// Porting code.
void *hwndGetMainHwnd()
{
	return g_hwnd;
}

// Porting code.
HINSTANCE hinstGetMainHInstance()
{
	return g_hInst;
}

// Porting code.
void ResetAppData()
{
	// Clears all data that needs clearing on a world dbase reset.
}
