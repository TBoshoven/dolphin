// Copyright (C) 2003-2009 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/



// OpenGL Plugin Documentation
/* 

1.1 Display settings


Internal and fullscreen resolution: Since the only internal resolutions allowed are also
fullscreen resolution allowed by the system there is only need for one resolution setting
that applies to both the internal resolution and the fullscreen resolution.

Todo: Make the internal resolution option apply instantly, currently only the native and 2x option
applies instantly. To do this we need to enumerate all avaliable display resolutions before
Renderer:Init().

1.2 Screenshots


The screenshots should be taken from the internal representation of the picture regardless of
what the current window size is. Since AA and wireframe is applied together with the picture resizing
this rule is not currently applied to AA or wireframe pictures, they are instead taken from whatever
the window size is.

Todo: Render AA and wireframe to a separate picture used for the screenshot in addition to the one
for display.

1.3 AA


Make AA apply instantly during gameplay if possible

*/



#include "Globals.h"

#include <cstdarg>

#ifdef _WIN32
#include "OS/Win32.h"
#endif

#if defined(HAVE_WX) && HAVE_WX
#include "GUI/ConfigDlg.h"
GFXConfigDialogOGL *m_ConfigFrame = NULL;
#include "Debugger/Debugger.h"
GFXDebuggerOGL *m_DebuggerFrame = NULL;
#endif

#include "Config.h"
#include "LookUpTables.h"
#include "ImageWrite.h"
#include "Render.h"
#include "GLUtil.h"
#include "Fifo.h"
#include "OpcodeDecoding.h"
#include "TextureMngr.h"
#include "BPStructs.h"
#include "VertexLoader.h"
#include "VertexLoaderManager.h"
#include "VertexManager.h"
#include "PixelShaderCache.h"
#include "PixelShaderManager.h"
#include "VertexShaderCache.h"
#include "VertexShaderManager.h"
#include "XFB.h"
#include "XFBConvert.h"
#include "TextureConverter.h"
#include "PostProcessing.h"
#include "OnScreenDisplay.h"
#include "Setup.h"

#include "VideoState.h"

SVideoInitialize g_VideoInitialize;
PLUGIN_GLOBALS* globals = NULL;

// Logging
int GLScissorX, GLScissorY, GLScissorW, GLScissorH;

static bool s_PluginInitialized = false;

static volatile u32 s_AccessEFBResult = 0, s_EFBx, s_EFBy;
static volatile EFBAccessType s_AccessEFBType;
static Common::Event s_AccessEFBDone;
static Common::CriticalSection s_criticalEFB;


void GetDllInfo (PLUGIN_INFO* _PluginInfo)
{
    _PluginInfo->Version = 0x0100;
    _PluginInfo->Type = PLUGIN_TYPE_VIDEO;
#ifdef DEBUGFAST
    sprintf(_PluginInfo->Name, "Dolphin OpenGL (DebugFast)");
#else
#ifndef _DEBUG
    sprintf(_PluginInfo->Name, "Dolphin OpenGL");
#else
    sprintf(_PluginInfo->Name, "Dolphin OpenGL (Debug)");
#endif
#endif
}

void SetDllGlobals(PLUGIN_GLOBALS* _pPluginGlobals)
{
	globals = _pPluginGlobals;
	LogManager::SetInstance((LogManager *)globals->logManager);
}

// This is used for the functions right below here which use wxwidgets
#if defined(HAVE_WX) && HAVE_WX
#ifdef _WIN32
	WXDLLIMPEXP_BASE void wxSetInstance(HINSTANCE hInst);
	extern HINSTANCE g_hInstance;
#endif

wxWindow* GetParentedWxWindow(HWND Parent)
{
#ifdef _WIN32
	wxSetInstance((HINSTANCE)g_hInstance);
#endif
	wxWindow *win = new wxWindow();
#ifdef _WIN32
	win->SetHWND((WXHWND)Parent);
	win->AdoptAttributesFromHWND();
#endif
	return win;
}
#endif

#if defined(HAVE_WX) && HAVE_WX
void DllDebugger(HWND _hParent, bool Show)
{
	if (!m_DebuggerFrame)
		m_DebuggerFrame = new GFXDebuggerOGL(GetParentedWxWindow(_hParent));

	if (Show)
		m_DebuggerFrame->ShowModal();
	else
		m_DebuggerFrame->Hide();
}
#else
void DllDebugger(HWND _hParent, bool Show) { }
#endif

void DllConfig(HWND _hParent)
{
#if defined(HAVE_WX) && HAVE_WX
	if (!m_ConfigFrame)
		m_ConfigFrame = new GFXConfigDialogOGL(GetParentedWxWindow(_hParent));
	else if (!m_ConfigFrame->GetParent()->IsShown())
		m_ConfigFrame->Close(true);

#if defined(_WIN32)

	// Search for avaliable resolutions
	
	DWORD iModeNum = 0;
	DEVMODE dmi;
	ZeroMemory(&dmi, sizeof(dmi));
	dmi.dmSize = sizeof(dmi);
	std::vector<std::string> resos;
	resos.reserve(20);
	int i = 0;

	while (EnumDisplaySettings(NULL, iModeNum++, &dmi) != 0)
	{
		char szBuffer[100];
		sprintf(szBuffer, "%dx%d", dmi.dmPelsWidth, dmi.dmPelsHeight);
		std::string strBuffer(szBuffer);
		// Create a check loop to check every pointer of resolutions to see if the res is added or not
		int b = 0;
		bool resFound = false;
		while (b < i && !resFound)
		{
			// Is the res already added?
			resFound = (resos[b] == strBuffer);
			b++;
		}
		// Add the resolution
		if (!resFound && i < 100)  // don't want to overflow resos array. not likely to happen, but you never know.
		{
			resos.push_back(strBuffer);
			i++;
			m_ConfigFrame->AddFSReso(szBuffer);
			m_ConfigFrame->AddWindowReso(szBuffer);
		}
        ZeroMemory(&dmi, sizeof(dmi));
	}

#elif defined(HAVE_X11) && HAVE_X11 && defined(HAVE_XXF86VM) && HAVE_XXF86VM

    int glxMajorVersion, glxMinorVersion;
    int vidModeMajorVersion, vidModeMinorVersion;
    GLWin.dpy = XOpenDisplay(0);
    glXQueryVersion(GLWin.dpy, &glxMajorVersion, &glxMinorVersion);
    XF86VidModeQueryVersion(GLWin.dpy, &vidModeMajorVersion, &vidModeMinorVersion);
	//Get all full screen resos for the config dialog
	XF86VidModeModeInfo **modes = NULL;
	int modeNum = 0;
	int bestMode = 0;

	//set best mode to current
	bestMode = 0;
	XF86VidModeGetAllModeLines(GLWin.dpy, GLWin.screen, &modeNum, &modes);
	int px = 0, py = 0;
	if (modeNum > 0 && modes != NULL)
	{
		for (int i = 0; i < modeNum; i++)
		{
			if (px != modes[i]->hdisplay && py != modes[i]->vdisplay)
			{
				char temp[32];
				sprintf(temp,"%dx%d", modes[i]->hdisplay, modes[i]->vdisplay);
				m_ConfigFrame->AddFSReso(temp);
				m_ConfigFrame->AddWindowReso(temp);//Add same to Window ones, since they should be nearly all that's needed
				px = modes[i]->hdisplay;//Used to remove repeating from different screen depths
				py = modes[i]->vdisplay;
			}
		}
	}    
	XFree(modes);

#elif defined(HAVE_COCOA) && HAVE_COCOA
	
	CFArrayRef modes;
	CFRange range;
	CFDictionaryRef modesDict;
	CFNumberRef modeValue;

	int modeWidth;
	int modeHeight;
	int modeBpp;
	int modeIndex;
	int px = 0, py = 0;


	modes = CGDisplayAvailableModes(CGMainDisplayID());

	range.location = 0;
	range.length = CFArrayGetCount(modes);

	for (modeIndex=0; modeIndex<range.length; modeIndex++)
	{
		modesDict = (CFDictionaryRef)CFArrayGetValueAtIndex(modes, modeIndex);
		modeValue = (CFNumberRef) CFDictionaryGetValue(modesDict, kCGDisplayWidth);
    		CFNumberGetValue(modeValue, kCFNumberLongType, &modeWidth);
		modeValue = (CFNumberRef) CFDictionaryGetValue(modesDict, kCGDisplayHeight);
    		CFNumberGetValue(modeValue, kCFNumberLongType, &modeHeight);
		modeValue = (CFNumberRef) CFDictionaryGetValue(modesDict, kCGDisplayBitsPerPixel);
    		CFNumberGetValue(modeValue, kCFNumberLongType, &modeBpp);

		if (px != modeWidth && py != modeHeight)
		{
			char temp[32];
			sprintf(temp,"%dx%d", modeWidth, modeHeight);
			m_ConfigFrame->AddFSReso(temp);
			m_ConfigFrame->AddWindowReso(temp);//Add same to Window ones, since they should be nearly all that's needed
			px = modeWidth;
			py = modeHeight;
		}
	}
#endif

	// Check if at least one resolution was found. If we don't and the resolution array is empty
	// CreateGUIControls() will crash because the array is empty.
	if (m_ConfigFrame->arrayStringFor_FullscreenCB.size() == 0)
	{
		m_ConfigFrame->AddFSReso("<No resolutions found>");
		m_ConfigFrame->AddWindowReso("<No resolutions found>");
	}

	// Only allow one open at a time
	if (!m_ConfigFrame->IsShown())
	{
		m_ConfigFrame->CreateGUIControls();
		m_ConfigFrame->ShowModal();
	}
	else
		m_ConfigFrame->Hide();
#endif
}

void Initialize(void *init)
{
    frameCount = 0;
    SVideoInitialize *_pVideoInitialize = (SVideoInitialize*)init;
    g_VideoInitialize = *(_pVideoInitialize); // Create a shortcut to _pVideoInitialize that can also update it
    InitLUTs();
	InitXFBConvTables();
    g_Config.Load();
    
	g_Config.GameIniLoad();
	g_Config.UpdateProjectionHack();

    if (!OpenGL_Create(g_VideoInitialize, 640, 480)) // 640x480 will be the default if all else fails
	{
        g_VideoInitialize.pLog("Renderer::Create failed\n", TRUE);
        return;
    }

	_pVideoInitialize->pPeekMessages = g_VideoInitialize.pPeekMessages;
    _pVideoInitialize->pUpdateFPSDisplay = g_VideoInitialize.pUpdateFPSDisplay;

	// Now the window handle is written
    _pVideoInitialize->pWindowHandle = g_VideoInitialize.pWindowHandle;

	OSD::AddMessage("Dolphin OpenGL Video Plugin" ,5000);
}

void DoState(unsigned char **ptr, int mode) {
#ifndef _WIN32
	// WHY is this here??
	OpenGL_MakeCurrent();
#endif
    // Clear all caches that touch RAM
    TextureMngr::Invalidate(false);
    VertexLoaderManager::MarkAllDirty();
    
    PointerWrap p(ptr, mode);
    VideoCommon_DoState(p);
    
    // Refresh state.
	if (mode == PointerWrap::MODE_READ)
	{
        BPReload();
		RecomputeCachedArraybases();
	}
}

// This is called after Video_Initialize() from the Core
void Video_Prepare(void)
{
    OpenGL_MakeCurrent();
    if (!Renderer::Init()) {
        g_VideoInitialize.pLog("Renderer::Create failed\n", TRUE);
        PanicAlert("Can't create opengl renderer. You might be missing some required opengl extensions, check the logs for more info");
        exit(1);
    }

    TextureMngr::Init();

    BPInit();
    VertexManager::Init();
    Fifo_Init(); // must be done before OpcodeDecoder_Init()
    OpcodeDecoder_Init();
    VertexShaderCache::Init();
    VertexShaderManager::Init();
    PixelShaderCache::Init();
    PixelShaderManager::Init();
	PostProcessing::Init();
    GL_REPORT_ERRORD();
    VertexLoaderManager::Init();
    TextureConverter::Init();

	s_PluginInitialized = true;
	INFO_LOG(VIDEO, "Video plugin initialized.");
}

void Shutdown(void)
{
	s_PluginInitialized = false;

	Fifo_Shutdown();
	PostProcessing::Shutdown();
	TextureConverter::Shutdown();
	VertexLoaderManager::Shutdown();
	VertexShaderCache::Shutdown();
	VertexShaderManager::Shutdown();
	PixelShaderManager::Shutdown();
	PixelShaderCache::Shutdown();
	VertexManager::Shutdown();
	TextureMngr::Shutdown();
	OpcodeDecoder_Shutdown();
	Renderer::Shutdown();
	OpenGL_Shutdown();
}



// Enter and exit the video loop
void Video_EnterLoop()
{
	Fifo_EnterLoop(g_VideoInitialize);
}

void Video_ExitLoop()
{
	Fifo_ExitLoop();
}




// Screenshot and screen message

void Video_Screenshot(const char *_szFilename)
{
	Renderer::SetScreenshot(_szFilename);
}

void Video_AddMessage(const char* pstr, u32 milliseconds)
{
	OSD::AddMessage(pstr, milliseconds);
}


// TODO: Protect this structure with a mutex.
volatile struct 
{ 
	u32 xfbAddr;
	u32 width;
	u32 height;
	s32 yOffset;
} tUpdateXFBArgs;

// Run from the CPU thread (from VideoInterface.cpp) for certain homebrew games only
void Video_UpdateXFB(u32 _dwXFBAddr, u32 _dwWidth, u32 _dwHeight, s32 _dwYOffset, bool scheduling)
{
	if (s_PluginInitialized)
	{
		if (scheduling) // From CPU in DC mode
		{
			tUpdateXFBArgs.xfbAddr = _dwXFBAddr;
			tUpdateXFBArgs.width = _dwWidth;
			tUpdateXFBArgs.height = _dwHeight;
			tUpdateXFBArgs.yOffset = _dwYOffset;

			g_XFBUpdateRequested = TRUE;
		}
		else // From CPU in SC mode or graphics thread in DC mode
		{
			g_XFBUpdateRequested = FALSE;

			if (!_dwXFBAddr)
			{
				// From graphics thread in DC mode
				_dwXFBAddr = tUpdateXFBArgs.xfbAddr;
				_dwWidth = tUpdateXFBArgs.width;
				_dwHeight = tUpdateXFBArgs.height;
				_dwYOffset = tUpdateXFBArgs.yOffset;
			}

			// TODO: Use real XFB source parameters based on VI settings
			Renderer::Swap(_dwXFBAddr, _dwWidth, _dwHeight, g_Config.bUseXFB ? _dwYOffset : 0);

			g_VideoInitialize.pCopiedToXFB();
		}
	}
}

void Video_OnThreadAccessEFB()
{
	s_criticalEFB.Enter();
	s_AccessEFBResult = 0;

	switch (s_AccessEFBType)
	{
	case PEEK_Z:
		{
			u32 z = 0;
			float xScale = Renderer::GetTargetScaleX();
			float yScale = Renderer::GetTargetScaleY();

			if (g_Config.iMultisampleMode != MULTISAMPLE_OFF)
			{
				// Find the proper dimensions
				TRectangle source, scaledTargetSource;
				ComputeBackbufferRectangle(&source);
				source.Scale(xScale, yScale, &scaledTargetSource);
				// This will resolve and bind to the depth buffer
				glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, Renderer::ResolveAndGetDepthTarget(scaledTargetSource));
			}

			// Read the z value! Also adjust the pixel to read to the upscaled EFB resolution
			// Plus we need to flip the y value as the OGL image is upside down
			glReadPixels(s_EFBx*xScale, Renderer::GetTargetHeight() - s_EFBy*yScale, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &z);
			GL_REPORT_ERRORD();

			// Clamp the 32bits value returned by glReadPixels to a 24bits value (GC uses a 24bits Z-Buffer)
			s_AccessEFBResult = z / 0x100;

			// We should probably re-bind the old fbo here.
			if (g_Config.iMultisampleMode != MULTISAMPLE_OFF) {
				Renderer::SetFramebuffer(0);
			}
		}
		break;

	case POKE_Z:
		break;

	case PEEK_COLOR:
		break;

	case POKE_COLOR:
		//WARN_LOG(VIDEOINTERFACE, "This is probably some kind of software rendering");
		break;

	default:
		break;
	}

	g_EFBAccessRequested = false;
	s_AccessEFBDone.Set();

	s_criticalEFB.Leave();
}

u32 Video_AccessEFB(EFBAccessType type, u32 x, u32 y)
{
	u32 result;

	s_criticalEFB.Enter();

	s_AccessEFBType = type;
	s_EFBx = x;
	s_EFBy = y;

	if (g_VideoInitialize.bUseDualCore)
	{
		s_AccessEFBDone.Init();
		g_EFBAccessRequested = true;
	}

	s_criticalEFB.Leave();

	if (g_VideoInitialize.bUseDualCore)
		s_AccessEFBDone.Wait();
	else
		Video_OnThreadAccessEFB();

	s_criticalEFB.Enter();

	if (g_VideoInitialize.bUseDualCore)
		s_AccessEFBDone.Shutdown();

	result = s_AccessEFBResult;

	s_criticalEFB.Leave();

	return result;
}

