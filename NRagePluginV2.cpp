/*	
	N-Rage`s Dinput8 Plugin
    (C) 2002, 2006  Norbert Wladyka

	Author`s Email: norbert.wladyka@chello.at
	Website: http://go.to/nrage


    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "commonIncludes.h"
#include <windows.h>
#include <Commctrl.h>
#include <dinput.h>
#include "NRagePluginV2.h"
#include "Interface.h"
#include "FileAccess.h"
#include "PakIO.h"
#include "DirectInput.h"
#include "International.h"

// ProtoTypes //
bool prepareHeap();
void FillControls(CONTROL Controls[]);
void InitiatePaks( bool bInitialize );
void DoShortcut( int iPlayer, int iShortcut );
DWORD WINAPI MsgThreadFunction( LPVOID lpParam );
DWORD WINAPI DelayedShortcut(LPVOID lpParam);

// Global Variables //
HMODULE g_hDirectInputDLL = NULL;	// Handle to DirectInput8 library
HMODULE g_hResourceDLL = NULL;		// Handle to resource library; used by LoadString for internationalization
HANDLE g_hHeap = NULL;				// Handle to our heap
int g_nDevices = 0;					// number of devices in g_devList
DEVICE g_devList[MAX_DEVICES];		// list of attached input devices, except SysMouse
									// note: we never purge the list of devices during normal operation
DEVICE g_sysMouse;					// we need to treat the sysmouse differently, as we may use "locking"; changed from g_apInputDevice[1] --rabid

EMULATOR_INFO g_strEmuInfo;			// emulator info?  Stores stuff like our hWnd handle and whether the plugin is initialized yet
TCHAR g_aszDefFolders[3][MAX_PATH];	// default folders: DIRECTORY_MEMPAK, DIRECTORY_GBROMS, DIRECTORY_GBSAVES
TCHAR g_aszLastBrowse[6][MAX_PATH];	// last browsed folders: BF_MEMPAK, BF_GBROM, BF_GBSAVE, BF_PROFILE, BF_NOTE, BF_SHORTCUTS

CRITICAL_SECTION g_critical;		// our critical section semaphore
int g_iFirstController = -1;		// The first controller which is plugged in
									// Normally controllers are scanned all at once in sequence, 1-4.  We only want to scan devices once per pass;
									// this is so we get consistent sample rates on our mouse.

bool g_bRunning = false;			// Is the emulator running (i.e. have we opened a ROM)?
bool g_bConfiguring = false;		// Are we currently in a config menu?
bool g_bExclusiveMouse = true;		// Do we have an exclusive mouse lock? defaults to true unless we have no bound mouse buttons/axes
CONTROLLER g_pcControllers[4];		// Our four N64 controllers, connected or otherwise
SHORTCUTS g_scShortcuts;
LPDIRECTINPUTDEVICE8 g_apFFDevice[4] = { NULL, NULL, NULL, NULL };					// added by rabid
LPDIRECTINPUTEFFECT  g_apdiEffect[4] = { NULL, NULL, NULL, NULL };					// array of handles for FF-Effects, one for each controller
TCHAR g_pszThreadMessage[DEFAULT_BUFFER] = _T("");

BOOL APIENTRY DllMain( HINSTANCE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved )
{
	switch ( ul_reason_for_call )
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls( hModule );
		if( !prepareHeap())
			return FALSE;
		DebugWriteA("*** DLL Attach (" VERSIONNUMBER "-Debugbuild | built on " __DATE__ " at " __TIME__")\n");
		ZeroMemory( &g_strEmuInfo, sizeof(g_strEmuInfo) );
		ZeroMemory( g_devList, sizeof(g_devList) );
		ZeroMemory( &g_sysMouse, sizeof(g_sysMouse) );
		ZeroMemory( g_aszDefFolders, sizeof(g_aszDefFolders) );
		ZeroMemory( g_aszLastBrowse, sizeof(g_aszLastBrowse) );
		g_strEmuInfo.hinst = hModule;
		g_strEmuInfo.fDisplayShortPop = true;	// display pak switching message windows by default
#ifdef _UNICODE
		{
			g_strEmuInfo.Language = GetLanguageFromINI();
			if ( g_strEmuInfo.Language == 0 )
			{
				g_strEmuInfo.Language = DetectLanguage();
				DebugWriteA("Autoselect language: %d\n", g_strEmuInfo.Language);
			}
			g_hResourceDLL = LoadLanguageDLL(g_strEmuInfo.Language); // HACK: it's theoretically not safe to call LoadLibraryEx from DllMain... --rabid
			if( g_hResourceDLL == NULL )
			{
				g_strEmuInfo.Language = 0;
				g_hResourceDLL = hModule;
				DebugWriteA("couldn't load language DLL, falling back to defaults\n");
			}
		}
#else
		DebugWriteA("  (compiled in ANSI mode, language detection DISABLED.)\n");
		g_strEmuInfo.Language = 0;
		g_hResourceDLL = hModule;
#endif // #ifndef _UNICODE
		InitializeCriticalSection( &g_critical );
		break;

	case DLL_THREAD_ATTACH:
		break;

	case DLL_THREAD_DETACH:
		break;

	case DLL_PROCESS_DETACH:
		//CloseDLL();
		if (g_hResourceDLL != g_strEmuInfo.hinst)
			FreeLibrary(g_hResourceDLL); // HACK: it's not safe to call FreeLibrary from DllMain... but screw it

		DebugWriteA("*** DLL Detach\n");

		CloseDebugFile(); // Moved here from CloseDll
		DeleteCriticalSection( &g_critical );

		// Moved here from CloseDll... Heap is created from DllMain,
		// and now it's destroyed by DllMain... just safer code --rabid
		if( g_hHeap != NULL )
		{
			HeapDestroy( g_hHeap );
			g_hHeap = NULL;
		}
		break;
	}
    return TRUE;
}


/******************************************************************
  Function: GetDllInfo
  Purpose:  This function allows the emulator to gather information
            about the dll by filling in the PluginInfo structure.
  input:    a pointer to a PLUGIN_INFO stucture that needs to be
            filled by the function. (see def above)
  output:   none
*******************************************************************/ 
EXPORT void CALL GetDllInfo ( PLUGIN_INFO* PluginInfo )
{
	DebugWriteA("CALLED: GetDllInfo\n");
	strncpy(PluginInfo->Name, STRING_PLUGINNAME
#ifndef _UNICODE
		"-ANSI"
#endif
#ifdef _DEBUG
		"-Debugbuild"
#endif
		, sizeof(PluginInfo->Name));
	PluginInfo->Type = PLUGIN_TYPE_CONTROLLER;
	PluginInfo->Version = SPECS_VERSION;
}

/******************************************************************
  Function: DllAbout
  Purpose:  This function is optional function that is provided
            to give further information about the DLL.
  input:    a handle to the window that calls this function
  output:   none
*******************************************************************/ 
EXPORT void CALL DllAbout ( HWND hParent )
{
	DebugWriteA("CALLED: DllAbout\n");
	TCHAR tszTitle[DEFAULT_BUFFER], tszTranslator[DEFAULT_BUFFER];

	LoadString( g_hResourceDLL, IDS_DLG_ABOUT_TITLE, tszTitle, DEFAULT_BUFFER );

	TCHAR szText[DEFAULT_BUFFER * 4] = _T(STRING_PLUGINNAME) _T("\n\n") \
		_T("Forked at: >>https://github.com/libertyernie/nrage-input<<\n\n") \
		_T("V") VERSIONINFO _T(" (") _T(__DATE__) _T(")\n") \
		_T("Original by N-Rage\n") \
		_T("\n") \
		_T(" - - - - -\n") \
		_T("Transferpak emulation done by MadManMarkAu\n") \
		_T("Cleanup, tweaks, and language support by RabidDeity\n") \
		_T("Goomba / Goomba Color SRAM support by libertyernie (GPLv2+)\n");

	LoadString( g_hResourceDLL, IDS_DLG_ABOUT, tszTranslator, DEFAULT_BUFFER );

	_tcscat(szText, tszTranslator);

	MessageBox( hParent, szText, tszTitle, MB_OK | MB_ICONINFORMATION);
	return;
}

/******************************************************************
  Function: DllConfig
  Purpose:  This function is optional function that is provided
            to allow the user to configure the dll
  input:    a handle to the window that calls this function
  output:   none
*******************************************************************/ 
EXPORT void CALL DllConfig ( HWND hParent )
{
	DebugWriteA("CALLED: DllConfig\n");
	static bool bInitCC = false;
	if( !prepareHeap())
		return;

	if( !g_pDIHandle )
	{
		if( InitDirectInput( hParent ))
		{
			EnterCriticalSection ( &g_critical );
			InitMouse();
			g_pDIHandle->EnumDevices( DI8DEVCLASS_ALL, EnumMakeDeviceList, NULL, DIEDFL_ATTACHEDONLY );
			LeaveCriticalSection ( &g_critical );
			DebugWriteA("InitDirectInput run in DllConfig, g_nDevices=%d\n", g_nDevices);
		}
	}

	if( g_pDIHandle && !g_bConfiguring )
	{	
		g_bConfiguring = true;
		if( !bInitCC )
		{
			INITCOMMONCONTROLSEX ccCtrls =	{	sizeof(INITCOMMONCONTROLSEX), 
											ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES };
			InitCommonControlsEx( &ccCtrls ); // needed for TrackBars & Tabs
		}
		
		EnterCriticalSection( &g_critical );
		if( g_sysMouse.didHandle ) { // unlock mouse while configuring
			g_sysMouse.didHandle->SetCooperativeLevel( g_strEmuInfo.hMainWindow, DIB_DEVICE );
			g_sysMouse.didHandle->Acquire();
		}
		LeaveCriticalSection( &g_critical );

		int iOK = DialogBox( g_hResourceDLL, MAKEINTRESOURCE( IDD_MAINCFGDIALOG ), hParent, MainDlgProc );

		// If we go into the dialog box, and the user navigates to the Rumble window, our FF device can get unacquired.
		// So let's reinit them now if we're running, just to be safe --rabid
		if( g_bRunning )
		{
			EnterCriticalSection( &g_critical );
			// PrepareInputDevices resets g_bExclusiveMouse to false if no mouse keys are bound, and the only way to
			// re-enable exclusive mouse is with a shortcut.
			// This is undesirable behavior, but it beats the alternative (and we REALLY need to re-init FF devices here)
			PrepareInputDevices();
			if (iOK)
			{
				InitiatePaks( false );	// only re-init the mempaks and such if the user clicked Save or Use
			}
			
			if( g_sysMouse.didHandle ) {
				if ( g_bExclusiveMouse ) { // if we have exclusive mouse, we need to relock mouse after closing the config
					g_sysMouse.didHandle->SetCooperativeLevel( g_strEmuInfo.hMainWindow, DIB_MOUSE );
					g_sysMouse.didHandle->Acquire();
					if (g_strEmuInfo.fDisplayShortPop)
					{
						LoadString( g_hResourceDLL, IDS_POP_MOUSELOCKED, g_pszThreadMessage, ARRAYSIZE(g_pszThreadMessage) );
						// HWND hMessage = CreateWindowEx( WS_EX_NOPARENTNOTIFY | WS_EX_STATICEDGE | WS_EX_TOPMOST, _T("STATIC"), pszMessage, WS_CHILD | WS_VISIBLE, 10, 10, 200, 30, g_strEmuInfo.hMainWindow, NULL, g_strEmuInfo.hinst, NULL );
						// SetTimer( hMessage, TIMER_MESSAGEWINDOW, 2000, MessageTimer );
						CreateThread(NULL, 0, MsgThreadFunction, g_pszThreadMessage, 0, NULL);
					}
				}
				else {
					g_sysMouse.didHandle->SetCooperativeLevel( g_strEmuInfo.hMainWindow, DIB_KEYBOARD );
					g_sysMouse.didHandle->Acquire();
				}
			}
			LeaveCriticalSection( &g_critical );
		}

		g_bConfiguring = false;
	}
	return;
}

/******************************************************************
  Function: DllTest
  Purpose:  This function is optional function that is provided
            to allow the user to test the dll
  input:    a handle to the window that calls this function
  output:   none
*******************************************************************/ 
EXPORT void CALL DllTest ( HWND hParent )
{
	DebugWriteA("CALLED: DllTest\n");
	return;
}

// It's easier to maintain one version of this, as not much really changes
// between versions.  --rabid

#if SPECS_VERSION == 0x0100
#pragma message("Conforming to Zilmar Spec 1.0")
/******************************************************************
  Function: InitiateControllers
  Purpose:  This function initialises how each of the controllers 
            should be handled.
  input:    - The handle to the main window.
            - A controller structure that needs to be filled for 
			  the emulator to know how to handle each controller.
  output:   none
*******************************************************************/  
EXPORT void CALL InitiateControllers( HWND hMainWindow, CONTROL Controls[4])

#elif SPECS_VERSION >= 0x0101
#pragma message("Conforming to Zilmar Spec 1.1")
/******************************************************************
  Function: InitiateControllers
  Purpose:  This function initialises how each of the controllers 
            should be handled.
  input:    - A controller structure that needs to be filled for 
			  the emulator to know how to handle each controller.
  output:   none
*******************************************************************/  
EXPORT void CALL InitiateControllers (CONTROL_INFO ControlInfo)

#endif // SPECS_VERSION
{
	DebugWriteA("CALLED: InitiateControllers\n");
	if( !prepareHeap())
		return;

#if SPECS_VERSION == 0x0100
	g_strEmuInfo.hMainWindow = hMainWindow;
//	g_strEmuInfo.HEADER = NULL;
#elif SPECS_VERSION >= 0x0101
	g_strEmuInfo.hMainWindow = ControlInfo.hMainWindow;
//	g_strEmuInfo.MemoryBswaped = ControlInfo.MemoryBswaped;
//	g_strEmuInfo.HEADER = ControlInfo.HEADER;
	// UNDONE: Instead of just storing the header, figure out what ROM we're running and save that information somewhere
#endif // SPECS_VERSION

	// The emulator expects us to tell what controllers are plugged in and what their paks are at this point.

	if( !g_pDIHandle ) // if we don't have a directinput handle, we need to make one, attach it to the main window (so it will die if our emulator dies), and enumerate devices
	{
		if( InitDirectInput( g_strEmuInfo.hMainWindow ))
		{
			EnterCriticalSection ( &g_critical );
			InitMouse();
			g_pDIHandle->EnumDevices( DI8DEVCLASS_ALL, EnumMakeDeviceList, NULL, DIEDFL_ATTACHEDONLY );
			LeaveCriticalSection ( &g_critical );
			DebugWriteA("InitDirectInput run in InitiateControllers, g_nDevices=%d\n", g_nDevices);
		}
		else
			return;
	}

	int iDevice;

	EnterCriticalSection( &g_critical );

	// ZeroMemory( g_apFFDevice, sizeof(g_apFFDevice) ); // NO, we'll reinit the existing reference if it's already loaded
	// ZeroMemory( g_apdiEffect, sizeof(g_apdiEffect) ); // NO, we'll release it with CloseControllerPak

	for( int i = 3; i >= 0; i-- )
	{
		SaveControllerPak( i );
		CloseControllerPak( i );
		// freePakData( &g_pcControllers[i] ); // already called by CloseControllerPak
		freeModifiers( &g_pcControllers[i] );
		SetControllerDefaults( &g_pcControllers[i] );
	}

	g_pcControllers[0].fPlugged = 1;

	if (! LoadConfigFromINI() )
	{
		DebugWriteA("\tINI load failed, loading defaults from resource\n");
		for ( int i = 0; i < 4; i++ )
			LoadProfileFromResource( i, false );
		LoadShortcutsFromResource(false);
	}

	for( int i = 0; i < 4; i++)	// initiate xinput controller and plug then if connected --tecnicors
	{
		InitiateXInputController( &g_pcControllers[i].xiController, i );
		if( g_pcControllers[i].xiController.bConnected )
		{
			g_pcControllers[i].fPlugged = 1;
			g_pcControllers[i].fGamePad = 1;
		}
	}	// END

	// Init: Find force-feedback devices and init 
	for( int i = 3; i >= 0; i-- )
	{
		DebugWriteA("Controller %d: ", i+1);
		if( g_pcControllers[i].xiController.bConnected && g_pcControllers[i].fXInput)	// if xinput connected, we don't need other config --tecnicors
			continue;

		if( g_pcControllers[i].fPlugged )
		{
			// Search for right Controller
			iDevice = FindDeviceinList( g_pcControllers[i].guidFFDevice );
			if( iDevice != -1 && g_devList[iDevice].bEffType )
			{
				DebugWriteA("rumble device set, ");
			}
			else // we couldn't find the device specified in the INI file, or it was already null
			{
				g_pcControllers[i].guidFFDevice = GUID_NULL;
				DebugWriteA("no rumble device/effect type set, ");
			}

			if( g_pcControllers[i].nModifiers > 0)
				SetModifier( &g_pcControllers[i] );
			g_iFirstController = i;
			DebugWriteA("plugged in, with paktype %d, ", g_pcControllers[i].PakType);
			DebugWriteA("RawMode is %d\n", g_pcControllers[i].fRawData);
		}
		else
		{
			DebugWriteA("unplugged\n");
			freePakData( &g_pcControllers[i] );	// we don't need to do this again, but there's not much overhead so I'll leave it --rabid
			freeModifiers( &g_pcControllers[i] );
		}
	}

	PrepareInputDevices();

	if( g_bExclusiveMouse ) {
		// g_sysMouse.didHandle->Unacquire();
		// g_sysMouse.didHandle->SetCooperativeLevel( g_strEmuInfo.hMainWindow, DIB_MOUSE ); // PrepareInputDevices does this.
		g_sysMouse.didHandle->Acquire();
	}

	InitiatePaks( true );

	g_strEmuInfo.fInitialisedPlugin = true;

	LeaveCriticalSection( &g_critical );

#if SPECS_VERSION == 0x0100
	FillControls(Controls);
#elif SPECS_VERSION >= 0x0101
	FillControls(ControlInfo.Controls);
#endif // SPECS_VERSION

	return;
} // end InitiateControllers

/******************************************************************
  Function: RomOpen
  Purpose:  This function is called when a rom is open. (from the 
            emulation thread)
  input:    none
  output:   none
*******************************************************************/ 
EXPORT void CALL RomOpen (void)
{
	DebugWriteA("CALLED: RomOpen\n");

	XInputEnable( TRUE );	// enables xinput --tecnicors

	if( !g_strEmuInfo.fInitialisedPlugin )
	{
		ErrorMessage(IDS_ERR_NOINIT, 0, false);
		return;
	}
	
	EnterCriticalSection( &g_critical );
	// re-init our paks and shortcuts
	InitiatePaks( true );
	// LoadShortcuts( &g_scShortcuts ); WHY are we loading shortcuts again?? Should already be loaded!
	LeaveCriticalSection( &g_critical );
	g_bRunning = true;
	return;
}

/******************************************************************
  Function: RomClosed
  Purpose:  This function is called when a rom is closed.
  input:    none
  output:   none
*******************************************************************/ 
EXPORT void CALL RomClosed(void)
{
	int i;

	XInputEnable( FALSE );	// disables xinput --tecnicors

	DebugWriteA("CALLED: RomClosed\n");
	EnterCriticalSection( &g_critical );

	if (g_sysMouse.didHandle)
		g_sysMouse.didHandle->SetCooperativeLevel(g_strEmuInfo.hMainWindow, DIB_KEYBOARD); // unlock the mouse, just in case

	for( i = 0; i < ARRAYSIZE(g_pcControllers); ++i )
	{
		if( g_pcControllers[i].pPakData )
		{
			SaveControllerPak( i );
			CloseControllerPak( i );
		}
		// freePakData( &g_pcControllers[i] ); already done by CloseControllerPak --rabid
		// DON'T free the modifiers!
//		ZeroMemory( &g_pcControllers[i], sizeof(CONTROLLER) );
	}

	for( i = 0; i < ARRAYSIZE( g_apdiEffect ); ++i )
		ReleaseEffect( g_apdiEffect[i] );
	ZeroMemory( g_apdiEffect, sizeof(g_apdiEffect) );

	g_bRunning = false;
	LeaveCriticalSection( &g_critical );
	
	return;
}

/******************************************************************
  Function: GetKeys
  Purpose:  To get the current state of the controllers buttons.
  input:    - Controller Number (0 to 3)
            - A pointer to a BUTTONS structure to be filled with 
			the controller state.
  output:   none
*******************************************************************/  	
EXPORT void CALL GetKeys(int Control, BUTTONS * Keys )
{
#ifdef ENABLE_RAWPAK_DEBUG
	DebugWriteA("CALLED: GetKeys\n");
#endif
	if( g_bConfiguring )
		Keys->Value = 0;
	else
	{
		EnterCriticalSection( &g_critical );
		
		if( g_pcControllers[Control].fPlugged ) {
			if (Control == g_iFirstController )
			{
				GetDeviceDatas();
				CheckShortcuts();
			}
			if( g_pcControllers[Control].xiController.bConnected && g_pcControllers[Control].fXInput )	// reads the xinput controller keys, if connected --tecnicors
				GetXInputControllerKeys( Control, &Keys->Value );
			else
				GetNControllerInput( Control, &Keys->Value );
		}
		LeaveCriticalSection( &g_critical );
	}
	return;
}

/******************************************************************
  Function: ControllerCommand
  Purpose:  To process the raw data that has just been sent to a 
            specific controller.
  input:    - Controller Number (0 to 3) and -1 signalling end of 
              processing the pif ram.
			- Pointer of data to be processed.
  output:   none
  
  note:     This function is only needed if the DLL is allowing raw
            data.

            the data that is being processed looks like this:
            initilize controller: 01 03 00 FF FF FF 
            read controller:      01 04 01 FF FF FF FF
*******************************************************************/
EXPORT void CALL ControllerCommand( int Control, BYTE * Command)
{
	// We don't need to use this because it will be echoed immediately afterwards
	// by a call to ReadController
	return;
}

/******************************************************************
  Function: ReadController
  Purpose:  To process the raw data in the pif ram that is about to
            be read.
  input:    - Controller Number (0 to 3) and -1 signalling end of 
              processing the pif ram.
			- Pointer of data to be processed.
  output:   none  
  note:     This function is only needed if the DLL is allowing raw
            data.
*******************************************************************/
EXPORT void CALL ReadController( int Control, BYTE * Command )
{
#ifdef ENABLE_RAWPAK_DEBUG
	DebugWriteA("CALLED: ReadController\n");
#endif
	if( Control == -1 )
		return;

	EnterCriticalSection( &g_critical );

	if( !g_pcControllers[Control].fPlugged )
	{
		Command[1] |= RD_ERROR;
		LeaveCriticalSection( &g_critical );
		return;
	}

	
	switch( Command[2] )
	{
	case RD_RESETCONTROLLER:
		WriteDatasA( "ResetController-PreProcessing", Control, Command, 0);
	case RD_GETSTATUS:
		// expected: controller gets 1 byte (command), controller sends back 3 bytes
		// should be:	Command[0] == 0x01
		//				Command[1] == 0x03
#ifdef ENABLE_RAWPAK_DEBUG
		WriteDatasA( "GetStatus-PreProcessing", Control, Command, 0);
#endif
		Command[3] = RD_GAMEPAD | RD_ABSOLUTE;
		Command[4] = RD_NOEEPROM;

		if( g_pcControllers[Control].fPakInitialized && g_pcControllers[Control].pPakData )
		{
			if( *(BYTE*)g_pcControllers[Control].pPakData == PAK_ADAPTOID )
			{
				Command[5] = GetAdaptoidStatus( Control );

				if( Command[5] & RD_NOTINITIALIZED )
					((ADAPTOIDPAK*)g_pcControllers[Control].pPakData)->fRumblePak = true;
			}
			else
			{
				Command[5] = ( *(BYTE*)g_pcControllers[Control].pPakData != PAK_NONE ) ? RD_PLUGIN : RD_NOPLUGIN;
				if( g_pcControllers[Control].fPakCRCError )
				{
					Command[5] = Command[5] | RD_ADDRCRCERR;
					g_pcControllers[Control].fPakCRCError = 0;
				}
			}
		}
		else
		{
			if( !g_bConfiguring && InitControllerPak( Control ) && g_pcControllers[Control].pPakData )
			{
				g_pcControllers[Control].fPakInitialized = 1;

				if( *(BYTE*)g_pcControllers[Control].pPakData == PAK_ADAPTOID )
					Command[5] = GetAdaptoidStatus( Control );
				else
				{
					Command[5] = ( *(BYTE*)g_pcControllers[Control].pPakData ) ? RD_PLUGIN : RD_NOPLUGIN;
					Command[5] = Command[5] | ( g_pcControllers[Control].fPakCRCError ? RD_ADDRCRCERR : 0 );
				}
			}
			else
				Command[5] = RD_NOPLUGIN | RD_NOTINITIALIZED;
		}

		if( g_pcControllers[Control].fPakCRCError )
		{
			Command[5] = Command[5] | RD_ADDRCRCERR;
			g_pcControllers[Control].fPakCRCError = 0;
		}

#ifdef ENABLE_RAWPAK_DEBUG
		WriteDatasA( "GetStatus-PostProcessing", Control, Command, 0);
		DebugWriteA( NULL );
#endif
		break;

	case RD_READKEYS:
		// expected: controller gets 1 byte (command), controller sends back 4 bytes
		// should be:	Command[0] == 0x01
		//				Command[1] == 0x04
		if( g_bConfiguring )
			Command[3] = Command[4] = Command[5] = Command[6] = 0;
		else
		{
			if (Control == g_iFirstController )
			{
				GetDeviceDatas();
				CheckShortcuts();
			}
			if( g_pcControllers[Control].xiController.bConnected && g_pcControllers[Control].fXInput )	// reads xinput controller kesy, if connected --tecnicors
				GetXInputControllerKeys( Control, (LPDWORD)&Command[3] );
			else
				GetNControllerInput( Control, (DWORD*)&Command[3] );
		}
		break;
		

	case RD_READPAK:
#ifdef ENABLE_RAWPAK_DEBUG
		WriteDatasA( "ReadPak-PreProcessing", Control, Command, 0);
#endif
		if( g_pcControllers[Control].fPakInitialized )
			//Command[1] = Command[1] | ReadControllerPak( Control, &Command[3] );
			ReadControllerPak( Control, &Command[3] );
		else
		{
			DebugWriteA("Tried to read, but pak wasn't initialized!!\n");
			// InitControllerPak( Control );
			Command[1] |= RD_ERROR;
			//ZeroMemory( &Command[5], 32 );
		}
#ifdef ENABLE_RAWPAK_DEBUG
		WriteDatasA( "ReadPak-PostProcessing", Control, Command, 0);
		DebugWriteA( NULL );
#endif
		break;

	case RD_WRITEPAK:
#ifdef ENABLE_RAWPAK_DEBUG
		WriteDatasA( "WritePak-PreProcessing", Control, Command, 0);
#endif
		if( g_pcControllers[Control].fPakInitialized )
			//Command[1] = Command[1] | WriteControllerPak( Control, &Command[3] );
			WriteControllerPak( Control, &Command[3] );
		else
		{
			DebugWriteA("Tried to write, but pak wasn't initialized! (paktype was %u)\n", g_pcControllers[Control].PakType);
			// InitControllerPak( Control );
			Command[1] |= RD_ERROR;
		}
#ifdef ENABLE_PAK_WRITES_DEBUG
		WriteDatasA( "WritePak-PostProcessing", Control, Command, 0);
		DebugWriteA( NULL );
#endif
		break;

	case RD_READEEPROM:
		// Should be handled by the Emulator
		WriteDatasA( "ReadEeprom-PreProcessing", Control, Command, 0);
		WriteDatasA( "ReadEeprom-PostProcessing", Control, Command, 0);
		DebugWriteA( NULL );
		break;
	case RD_WRITEEPROM:
		// Should be handled by the Emulator
		WriteDatasA( "WriteEeprom-PreProcessing", Control, Command, 0);
		WriteDatasA( "WriteEeprom-PostProcessing", Control, Command, 0);
		DebugWriteA( NULL );
		break;

	default:
		// only accessible if the Emulator has bugs.. or maybe the Rom is flawed
		WriteDatasA( "ReadController: Bad read", Control, Command, 0);
		DebugWriteA( NULL );
		Command[1] = Command[1] | RD_ERROR;
	}

	LeaveCriticalSection( &g_critical );
	return;
}

/******************************************************************
  Function: WM_KeyDown
  Purpose:  To pass the WM_KeyDown message from the emulator to the 
            plugin.
  input:    wParam and lParam of the WM_KEYDOWN message.
  output:   none
*******************************************************************/ 
EXPORT void CALL WM_KeyDown( WPARAM wParam, LPARAM lParam )
{
	return;
}

/******************************************************************
  Function: WM_KeyUp
  Purpose:  To pass the WM_KEYUP message from the emulator to the 
            plugin.
  input:    wParam and lParam of the WM_KEYDOWN message.
  output:   none
*******************************************************************/ 
EXPORT void CALL WM_KeyUp( WPARAM wParam, LPARAM lParam )
{
	return;
}

/******************************************************************
  Function: CloseDLL
  Purpose:  This function is called when the emulator is closing
            down allowing the dll to de-initialise.
  input:    none
  output:   none
*******************************************************************/ 
EXPORT void CALL CloseDLL (void)
{										// HACK: THIS IS BROKEN IN PJ64 1.6 (it calls CloseDLL too often)
	DebugWriteA("CALLED: CloseDLL\n");
	if( g_bRunning )
		RomClosed();
	for( int i = 0; i < 4; i++ )
	{
		freePakData( &g_pcControllers[i] );
		freeModifiers( &g_pcControllers[i] );
	}

	// ZeroMemory( g_pcControllers, sizeof(g_pcControllers) ); // why zero the memory if we're just going to close down?
	
	FreeDirectInput();

	return;
}

// Prepare a global heap.  Use P_malloc and P_free as wrappers to grab/release memory.
bool prepareHeap()
{
	if( g_hHeap == NULL )
		g_hHeap = HeapCreate( 0, 4*1024, 0 );
	return (g_hHeap != NULL);
}

// Frees pakdata memory (called more often than you'd think).
void freePakData( CONTROLLER *pcController )
{
	if( pcController && pcController->pPakData )
	{
		P_free( pcController->pPakData );
		pcController->pPakData = NULL;
	}
}

// Frees modifier memory
void freeModifiers( CONTROLLER *pcController )
{
	if( pcController && pcController->pModifiers )
	{
		pcController->nModifiers = 0;
		P_free( pcController->pModifiers );
		pcController->pModifiers = NULL;
	}
}

// After enumerating DirectInput devices into g_devList, find a device by product name and counter
int FindDeviceinList( const TCHAR *pszProductName, const BYTE bProductCounter, bool fFindSimilar )
{
	if( !(*pszProductName) )
		return -1;

	int i = 0, iSimilar = -1, iExact = -1;
	while(( i < ARRAYSIZE(g_devList) ) && ( iExact == -1 ))
	{
		if( !lstrcmp( g_devList[i].szProductName, pszProductName ))
		{
			if(( bProductCounter > 0 ) || ( iSimilar == -1 ))
				iSimilar = i;
			if( g_devList[i].bProductCounter == bProductCounter )
				iExact = i;
		}
		i++;
	}

	if( fFindSimilar && ( iExact == -1 ))
		iExact = iSimilar;

	return iExact;
}

// After enumerating DirectInput devices into g_devList, find a device by GUID.  Finding similar devices is impossible.
int FindDeviceinList( REFGUID rGUID )
{
	if (rGUID == GUID_NULL )
		return -1;
	int i = 0;
	while( i < ARRAYSIZE(g_devList) )
	{
		if ( IsEqualGUID(g_devList[i].guidInstance, rGUID) )
			return i;
		i++;
	}
	return -1;
}

// Let's initialize all plugged in controllers' paks.
// Input: false means run InitControlPak on each plugged controller.  Input true means just clear each pak's CRC error status?
// When we call this from RomOpen, it's true, otherwise (in DllConfig) it's false.
// Rather counterintuitive.
void InitiatePaks( bool bInitialize )
{
	for( int i = 0; i < 4; i++ )
	{
		if( g_pcControllers[i].fPlugged)
		{
			g_pcControllers[i].fPakCRCError = 0;
			
			if( g_pcControllers[i].fRawData )
			{
				if( !bInitialize )
					g_pcControllers[i].fPakInitialized = InitControllerPak( i );
			}
			else
			{	// we only support "RAW mode" paks so this won't do much
				;//if( g_pcControllers[i].PakType == PAK_RUMBLE )
				//	CreateEffectHandle( i, g_pcControllers[i].bRumbleTyp, g_pcControllers[i].bRumbleStrength );
			}
		}
	}
}

// This used to be "NotifyEmulator" which was supposed to tell the emulator if we changed the way it sees controllers.
//		Unfortunately the spec doesn't work that way.  Fixed the func and changed the func name to something that makes more sense.
// FillControls takes a Controls array from InitiateControllers and fills it with what we know about whether
//		a controller is plugged in, accepting raw data, and what type of pak is plugged in.
void FillControls(CONTROL Controls[4])
{
	for( int i = 4-1; i >= 0; i-- )
	{
		if( g_pcControllers[i].fPlugged )
		{
			Controls[i].Present	= g_pcControllers[i].fPlugged;
			Controls[i].RawData	= g_pcControllers[i].fRawData;

			switch( g_pcControllers[i].PakType )
			{
			case PAK_MEM:
				Controls[i].Plugin = PLUGIN_MEMPAK;
				break;
			case PAK_RUMBLE:
				Controls[i].Plugin = PLUGIN_RUMBLE_PAK;
				break;
			case PAK_TRANSFER:
				Controls[i].Plugin = PLUGIN_TRANSFER_PAK;
				break;
			case PAK_VOICE:
				Controls[i].Plugin = g_pcControllers[i].fRawData ? PLUGIN_RAW : PLUGIN_NONE;
				break;
			case PAK_ADAPTOID:
				Controls[i].Plugin = g_pcControllers[i].fRawData ? PLUGIN_RAW : PLUGIN_NONE;
				break;

			case PAK_NONE:
			default:
				Controls[i].Plugin = PLUGIN_NONE;
			}	
		}
		else
		{
			Controls[i].Plugin	= PLUGIN_NONE;
			Controls[i].Present	= false;
			Controls[i].RawData	= true;
		}
	}
}

// called after a poll to execute any shortcuts
void CheckShortcuts()
{
	static bool bWasPressed[ sizeof(SHORTCUTSPL)/sizeof(BUTTON) ][4];
	static bool bMLWasPressed;	// mouselock
	bool bMatching = false;

	if ( g_bConfiguring || !g_bRunning )
		return; // we don't process shortcuts if we're in a config menu or are not running emulation

	// just process if key wasnt pressed before
	for ( int i = 0; i < 4; i++ ) // controllers
	{
		for( int j = 0; j < SC_TOTAL; j++ ) 
		{
			bMatching = IsBtnPressed( g_scShortcuts.Player[i].aButtons[j] );

			if( bMatching && !bWasPressed[j][i] )
				DoShortcut(i, j);

			bWasPressed[j][i] = bMatching;
		}
	}

	bMatching = IsBtnPressed( g_scShortcuts.bMouseLock );

	if( bMatching && !bMLWasPressed )
		DoShortcut(-1, -1); // controller -1 means do mouselock shortcut

	bMLWasPressed = bMatching;
}

// Executes the shortcut iShortcut on controller iController
// Special case: if iPlayer is -1, run the mouselock shortcut
void DoShortcut( int iControl, int iShortcut )
{
	DebugWriteA("Shortcut: %d %d\n", iControl, iShortcut);
	TCHAR pszMessage[DEFAULT_BUFFER / 2] = TEXT("");
	bool bEjectFirst = false;

	if (iControl == -1)
	{
		EnterCriticalSection( &g_critical );
		if( g_sysMouse.didHandle )
		{
			g_sysMouse.didHandle->Unacquire();
			if( g_bExclusiveMouse )
			{
				g_sysMouse.didHandle->Unacquire();
				g_sysMouse.didHandle->SetCooperativeLevel( g_strEmuInfo.hMainWindow, DIB_KEYBOARD );
				g_sysMouse.didHandle->Acquire();
				LoadString( g_hResourceDLL, IDS_POP_MOUSEUNLOCKED, pszMessage, ARRAYSIZE(pszMessage) );
			}
			else
			{
				g_sysMouse.didHandle->Unacquire();
				g_sysMouse.didHandle->SetCooperativeLevel( g_strEmuInfo.hMainWindow, DIB_MOUSE );
				g_sysMouse.didHandle->Acquire();
				LoadString( g_hResourceDLL, IDS_POP_MOUSELOCKED, pszMessage, ARRAYSIZE(pszMessage) );
			}
			g_sysMouse.didHandle->Acquire();
			g_bExclusiveMouse = !g_bExclusiveMouse;
		}
		LeaveCriticalSection( &g_critical );
	}
	else if( g_pcControllers[iControl].fPlugged )
	{
		if( g_pcControllers[iControl].pPakData )
		{
			SaveControllerPak( iControl );
			CloseControllerPak( iControl );
		}

		switch (iShortcut)
		{
		case SC_NOPAK:
			EnterCriticalSection( &g_critical );
			g_pcControllers[iControl].PakType = PAK_NONE;
			g_pcControllers[iControl].fPakInitialized = 0;
			LoadString( g_hResourceDLL, IDS_P_NONE, pszMessage, ARRAYSIZE(pszMessage) );
			LeaveCriticalSection( &g_critical );
			break;

		case SC_MEMPAK:
			if (PAK_NONE == g_pcControllers[iControl].PakType)
			{
				EnterCriticalSection( &g_critical );
				g_pcControllers[iControl].PakType = PAK_MEM;
				g_pcControllers[iControl].fPakInitialized = 0;
				LoadString( g_hResourceDLL, IDS_P_MEMPAK, pszMessage, ARRAYSIZE(pszMessage) );
				LeaveCriticalSection( &g_critical );
			}
			else
			{
				bEjectFirst = true;
			}
			break;
		
		case SC_RUMBPAK:
			if (PAK_NONE == g_pcControllers[iControl].PakType)
			{
				EnterCriticalSection( &g_critical );
				g_pcControllers[iControl].PakType = PAK_RUMBLE;
				g_pcControllers[iControl].fPakInitialized = 0;

				if( g_pcControllers[iControl].fRawData )
					if (CreateEffectHandle( iControl, g_pcControllers[iControl].bRumbleTyp, g_pcControllers[iControl].bRumbleStrength ) )
					{
						DebugWriteA("CreateEffectHandle for shortcut switch: OK\n");
					}
					else
					{
						DebugWriteA("Couldn't CreateEffectHandle for shortcut switch.\n");
					}

				LoadString( g_hResourceDLL, IDS_P_RUMBLEPAK, pszMessage, ARRAYSIZE(pszMessage) );
				LeaveCriticalSection( &g_critical );
			}
			else
			{
				bEjectFirst = true;
			}
			break;
		
		case SC_TRANSPAK:
			if (PAK_NONE == g_pcControllers[iControl].PakType)
			{
				EnterCriticalSection( &g_critical );
				g_pcControllers[iControl].PakType = PAK_TRANSFER;
				g_pcControllers[iControl].fPakInitialized = 0;

				LoadString( g_hResourceDLL, IDS_P_TRANSFERPAK, pszMessage, ARRAYSIZE(pszMessage) );
				LeaveCriticalSection( &g_critical );
			}
			else
			{
				bEjectFirst = true;
			}
			break;
		
		case SC_VOICEPAK:
			if (PAK_NONE == g_pcControllers[iControl].PakType)
			{
				EnterCriticalSection( &g_critical );
				g_pcControllers[iControl].PakType = PAK_VOICE;
				g_pcControllers[iControl].fPakInitialized = 0;

				LoadString( g_hResourceDLL, IDS_P_VOICEPAK, pszMessage, ARRAYSIZE(pszMessage) );
				LeaveCriticalSection( &g_critical );
			}
			else
			{
				bEjectFirst = true;
			}
			break;

		case SC_ADAPTPAK:
			if (PAK_NONE == g_pcControllers[iControl].PakType)
			{
				EnterCriticalSection( &g_critical );
				g_pcControllers[iControl].PakType = PAK_ADAPTOID;
				g_pcControllers[iControl].fPakInitialized = 0;

				LoadString( g_hResourceDLL, IDS_P_ADAPTOIDPAK, pszMessage, ARRAYSIZE(pszMessage) );
				LeaveCriticalSection( &g_critical );
			}
			else
			{
				bEjectFirst = true;
			}
			break;

		case SC_SWMEMRUMB:
			bEjectFirst = true;
			if( g_pcControllers[iControl].PakType == PAK_MEM )
			{
				iShortcut = PAK_RUMBLE;
			}
			else
			{
				iShortcut = PAK_MEM;
			}
			break;

		case SC_SWMEMADAPT:
			bEjectFirst = true;
			if( g_pcControllers[iControl].PakType == PAK_MEM )
			{
				iShortcut = PAK_ADAPTOID;
			}
			else
			{
				iShortcut = PAK_MEM;
			}
			break;

		default:
			DebugWriteA("Invalid iShortcut passed to DoShortcut\n");
			EnterCriticalSection( &g_critical );
			g_pcControllers[iControl].fPakInitialized = 0;
			LeaveCriticalSection( &g_critical );
			return;
		} // switch (iShortcut)
	} // else if

	// let the game code re-init the pak.

	if (bEjectFirst)	// we need to eject the current pack first; then set a DoShortcut to try again in 1 second
	{
		EnterCriticalSection( &g_critical );
		g_pcControllers[iControl].PakType = PAK_NONE;
		g_pcControllers[iControl].fPakInitialized = 0;
		LoadString( g_hResourceDLL, IDS_P_SWITCHING, pszMessage, ARRAYSIZE(pszMessage) );
		LeaveCriticalSection( &g_critical );

		LPMSHORTCUT lpmNextShortcut = (LPMSHORTCUT)P_malloc(sizeof(MSHORTCUT));
		if (!lpmNextShortcut)
			return;
		lpmNextShortcut->iControl = iControl;
		lpmNextShortcut->iShortcut = iShortcut;
		CreateThread(NULL, 0, DelayedShortcut, lpmNextShortcut, 0, NULL);
		iControl = -2;	// this is just a hack to get around the check that appends "Changing Pak X to ..."
	}
	
	if( g_strEmuInfo.fDisplayShortPop && _tcslen(pszMessage) > 0 )
	{
		if( iControl >= 0 )
		{
			TCHAR tszNotify[DEFAULT_BUFFER / 2];

			LoadString( g_hResourceDLL, IDS_POP_CHANGEPAK, tszNotify, ARRAYSIZE(tszNotify));
			wsprintf( g_pszThreadMessage, tszNotify, iControl+1, pszMessage );
		}
		else
			lstrcpyn( g_pszThreadMessage, pszMessage, ARRAYSIZE(g_pszThreadMessage) );

		CreateThread(NULL, 0, MsgThreadFunction, g_pszThreadMessage, 0, NULL);
	}
}

// Use string table refs to generate and throw an error message with title IDS_ERR_TITLE (see string table in resources)
// Also if compiled with DEBUG, will log the message with DebugWrite
// uID			the string table ref to display
// dwError		if nonzero, will display a Windows error message using FormatMessage (for use when an API function fails)
// fUserChoose	if true, display buttons Retry and Cancel.  if false, display single button OK.
//		for fUserChoose==true; ErrorMessage returns true if user selects Retry, and false if user selects Cancel.
//		for fUserChoose==false; ErrorMessage always returns false.
bool ErrorMessage( UINT uID, DWORD dwError, bool fUserChoose )
{
	TCHAR pszFirstLine[DEFAULT_BUFFER];

	bool fReturn = false;
	int iBytes;
	TCHAR szError[512];
	TCHAR tszErrorTitle[DEFAULT_BUFFER];

	LoadString( g_hResourceDLL, uID, pszFirstLine, DEFAULT_BUFFER );
	LoadString( g_hResourceDLL, IDS_ERR_TITLE, tszErrorTitle, DEFAULT_BUFFER );

	if( dwError )
	{
		iBytes = wsprintf( szError, _T("%s\n\n Error description: "), pszFirstLine );
		FormatMessage( FORMAT_MESSAGE_FROM_SYSTEM, NULL, dwError , 0, &szError[iBytes], sizeof(szError) - iBytes, NULL );
	}
	else
		lstrcpyn( szError, pszFirstLine, 512 );
	
	DebugWrite(_T("ErrorMessage! ID:%d "), uID);
	DebugFlush();

	if( fUserChoose )
		fReturn = MessageBox( g_strEmuInfo.hMainWindow, szError, tszErrorTitle, MB_RETRYCANCEL | MB_ICONERROR ) == IDRETRY;
	else
		MessageBox( g_strEmuInfo.hMainWindow, szError, tszErrorTitle, MB_OK | MB_ICONERROR );

	DebugWriteA(fReturn ? "(user: retry)\n" : "(user: acknowledge)\n");
	return fReturn;
}

// Post a message box, using string resource uTextID and MessageBox style uType
int WarningMessage( UINT uTextID, UINT uType )
{
	DebugWriteA("WarningMessage: ID:%d Type:%d\n", uTextID, uType);
	DebugFlush();

	TCHAR tszTitle[DEFAULT_BUFFER], tszText[DEFAULT_BUFFER];

	LoadString( g_hResourceDLL, uTextID, tszText, DEFAULT_BUFFER );
	LoadString( g_hResourceDLL, IDS_DLG_WARN_TITLE, tszTitle, DEFAULT_BUFFER );

	return MessageBox( g_strEmuInfo.hMainWindow, tszText, tszTitle, uType );
}


/* H.Morii <koolsmoky(at)users.sourceforge.net>
   MsgThreadFunction is used because the SetTimer function relies 
   on the WM_TIMER message which is low priority and will not be 
   executed if other windows messages are frequently dispatched. */

DWORD WINAPI MsgThreadFunction( LPVOID lpParam ) 
{
	HWND hMessage = CreateWindowEx( WS_EX_NOPARENTNOTIFY | WS_EX_STATICEDGE | WS_EX_TOPMOST, _T("STATIC"), NULL, WS_CHILD | WS_VISIBLE, 10, 10, 200, 40, g_strEmuInfo.hMainWindow, NULL, g_strEmuInfo.hinst, NULL );

	/* prepare the screen to bitblt */
	RECT rt;
	GetClientRect(hMessage, &rt);
	HDC hdc = GetDC(hMessage);
	HDC memdc = CreateCompatibleDC(hdc);
	HBITMAP hbitmap = CreateCompatibleBitmap(hdc, rt.right - rt.left, rt.bottom - rt.top);
	SelectObject(memdc, hbitmap);

	/* draw some nice stuff. 
		choose fonts, paint the back ground, etc here. */
	FillRect(memdc, &rt, (HBRUSH)(COLOR_WINDOW+1));
	DrawText(memdc, (LPCTSTR)lpParam, -1, &rt, DT_WORDBREAK);

	/* bitblt to kingdom come */
	for (int i = 0; i < 60; i++)
	{
		Sleep(16);	// 1/60 second
		BitBlt(hdc, rt.left, rt.top, rt.right - rt.left, rt.bottom - rt.top, memdc, 0, 0, SRCCOPY);
	}

	/* cleanup */
	DeleteObject(hbitmap);
	DeleteDC(memdc);
	ReleaseDC(hMessage, hdc);

	/* Can only destroy windows created from the owner thread */
	DestroyWindow(hMessage);

	return 0;
}

// This function is called as a thread by DoShortcut in order to tell the plugin to insert a pak shortly
// (usually after we've just removed whatever pak was there)
DWORD WINAPI DelayedShortcut(LPVOID lpParam)
{
	LPMSHORTCUT sc = (LPMSHORTCUT)lpParam;
	if (sc && sc->iShortcut != SC_SWMEMRUMB && sc->iShortcut != SC_SWMEMADAPT) // don't allow recursion into self, it would cause a deadlock
	{
		Sleep(1000);	// sleep a little bit before calling DoShortcut again
		DoShortcut(sc->iControl, sc->iShortcut);
	}
	P_free(lpParam);
	return 0;
}