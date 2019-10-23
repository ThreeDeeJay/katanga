// For this part of the DLL, it makes more sense to use Nektra In-Proc
// instead of Nektra Deviare.  The interface for DX11 is not well supported
// by the DB for Deviare, so getting access to the DX11 APIs is simpler
// using In-Proc.  It would be possible to add the DX11 interfaces to 
// the DB files, and rebuild the DB if you wanted to use Deviare.
//
// All In-Proc use is in this file, all Deviare use is in NativePlugin.

// A note in the documentation: https://msdn.microsoft.com/en-us/library/windows/desktop/bb172586(v=vs.85).aspx
// "A cross-process shared-surface should not be locked."
//
// With that use of CreateTexture, the next step was to find that shared surfaces
// cannot be used as the target, necessitating a second copy from the game.  This is
// a second destination of the game bits, which is the final shared resource.  This
// is OK, because we need to swap eyes for Unity, and this is where it's done.

// Overall, seriously hard to get this all working.  Multiple problems with bad
// and misleading and missing documentation. And no sample code.  This complicated
// runtime environment makes it hard to debug as well.  Careful with that axe, Eugene,
// this is all pretty fragile.

// 3-13-19: Changing all DX9 code and comments to use DX11 instead.
//	DX11 SurfaceSharing does not appear to have as many problems as DX9.
//  Using the StereoSnapShot code from 3Dmigoto to get stereo back buffer
//  with NvAPI_Stereo_ReverseStereoBlitControl. 

// --------------------------------------------------------------------------------------------------

#include "DeviarePlugin.h"

#include <thread>


// The surface that we copy the current stereo game frame into. It is shared.
// It starts as a Texture so that it is created stereo, and is shared 
// via GetSharedHandle.

ID3D11Texture2D* gGameTexture = nullptr;

// If we are in 3D Vision Direct Mode, we need to copy the textures from each
// eye, instead of using the ReverseStereoBlit.  This changes the mode of
// copying in Present.

bool gDirectMode = false;

// --------------------------------------------------------------------------------------------------

// Custom routines for this DeviarePlugin.dll, that the master app can call,
// using Deviare access routines.

//HANDLE gSharedThread = nullptr;				// will copy from GameSurface to SharedSurface
//HANDLE gFreshBits = nullptr;				// Synchronization Event object
//static unsigned long triggerCount = 0;


// Shared Event object that is the notification that the VR side
// has called Present.

//HANDLE WINAPI GetEventHandle(int* in)
//{
//	::OutputDebugString(L"GetSharedEvent::\n");
//
//	return gFreshBits;
//}

// Called from C# side after VR app has presented its frame.
// This allows our locked present for the target game to continue.

//static LARGE_INTEGER startTriggeredTicks, resetTriggerTicks;
//static LARGE_INTEGER frequency;
//
//HANDLE WINAPI TriggerEvent(int* in)
//{
//	//	::OutputDebugString(L"TriggerEvent::\n");
//
//
//	if ((int)in == 1)		// Active triggered
//	{
//		BOOL set = SetEvent(gFreshBits);
//		if (!set)
//			::OutputDebugString(L"Bad SetEvent in TriggerEvent.\n");
//
//		// Waste time spinning, while we wait for high resolution timer.
//		// This timer using QueryPerformanceCounter should be accurate
//		// to some 100ns or so, for any system we care about.
//
//		QueryPerformanceFrequency(&frequency);
//		QueryPerformanceCounter(&startTriggeredTicks);
//
//		triggerCount += 1;
//
//		if ((triggerCount % 30) == 0)
//		{
//			LONGLONG startMS = startTriggeredTicks.QuadPart * 1000 / frequency.QuadPart;
//			swprintf_s(info, _countof(info),
//				L"SetEvent - ms: %lld, frequency: %lld, triggerCount: %d\n", startMS, frequency.QuadPart, triggerCount);
//			::OutputDebugString(info);
//		}
//	}
//	else					// Reset untriggered
//	{
//		BOOL reset = ResetEvent(gFreshBits);
//		if (!reset)
//			::OutputDebugString(L"Bad ResetEvent in TriggerEvent.\n");
//
//		QueryPerformanceCounter(&resetTriggerTicks);
//
//		if ((triggerCount % 30) == 0)
//		{
//			LONGLONG frameTicks = resetTriggerTicks.QuadPart - startTriggeredTicks.QuadPart;
//			LONGLONG endMS = frameTicks * 1000 / frequency.QuadPart;
//			swprintf_s(info, _countof(info),
//				L"ResetEvent - ms: %lld\n", endMS);
//			::OutputDebugString(info);
//		}
//	}
//
//	return NULL;
//}


// --------------------------------------------------------------------------------------------------
// Shared code for when we need to create the offscreen Texture2D for our stereo
// copy.  This is created when the CreateSwapChain is called, and also whenever
// it ResizeBuffers is called, because we need to change our destination copy
// to always match what the game is drawing.
//
// This will also rewrite the global gGameSharedHandle with a new HANDLE as
// needed, and the Unity side is expected to notice a change and setup a new
// drawing texture as well.  This is thus polling on the Unity side, which is
// not ideal, but it is one call here to fetch the 4 byte HANDLE every 11ms.  
// There does not appear to be a good way for this code to notify the C# code,
// although using a TriggerEvent with some C# interop might work.  We'll only
// do that work if this proves to be a problem.

ID3D11Device* CreateSharedTexture(IDXGISwapChain* pSwapChain)
{
	HRESULT hr;
	ID3D11Device* pDevice;
	ID3D11Texture2D* backBuffer;
	D3D11_TEXTURE2D_DESC desc;
	ID3D11Texture2D* oldGameTexture;

	// Save possible prior usage to be disposed after we recreate.

	oldGameTexture = gGameTexture;

	// It's more reliable to get the pDevice of an actual D3D11Device from
	// the swap chain directly, because bad code like UE4 can pass in a 
	// DXGIDevice, which is not usable here.

	hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice);
	if (FAILED(hr)) FatalExit(L"Failed to GetDevice");

	// Using the D3D11Device we fetched above, we also want to initialize nvidia
	// stereo so that we can fetch the stereo backbuffer during Present.

	NvAPI_Status res = NvAPI_Initialize();
	if (res != NVAPI_OK) FatalExit(L"Failed to NvAPI_Initialize\n");

	// ToDo: need to handle stereo disabled...
	res = NvAPI_Stereo_CreateHandleFromIUnknown(pDevice, &gNVAPI);
	if (res != NVAPI_OK) FatalExit(L"Failed to NvAPI_Stereo_CreateHandleFromIUnknown\n");



	// Now that we have a proper SwapChain from the game, let's also make a 
	// DX11 Texture2D, so that we can snapshot the game output. 
	//
	// Make it exactly match the backbuffer, which ensures that the stereo copy
	// using ReverseStereoBlit will work.

	hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
	if (FAILED(hr)) FatalExit(L"Fail to get backbuffer");

	backBuffer->GetDesc(&desc);
	backBuffer->Release();

	// Some games like TheSurge and Dishonored2 will specify a DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
	// as their backbuffer.  This doesn't work for us because our output is going to the VR HMD,
	// and thus we get a doubled up sRGB/gamma curve, which makes it too dark, and the in-game
	// slider doesn't have enough range to correct.  
	// If we get one of these sRGB formats, we are going to strip that and return the Linear
	// version instead, so that we avoid this problem.  This allows us to use Gamma for the Unity
	// app itself, which matches 90% of the games, and still handle these oddball games automatically.

	if (desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
		desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;

	// This texture needs to use the Shared flag, so that we can share it to 
	// another Device.  Because these are all DX11 objects, the share will work.

	desc.Width *= 2;								// Double width texture for stereo.
	desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;	// Must add bind flag, so SRV can be created in Unity.
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;	// To be shared. maybe D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX is better

	hr = pDevice->CreateTexture2D(&desc, NULL, &gGameTexture);
	if (FAILED(hr)) FatalExit(L"Fail to create shared stereo Texture");

	// Now create the HANDLE which is used to share surfaces.  This follows the model from:
	// https://docs.microsoft.com/en-us/windows/desktop/api/d3d11/nf-d3d11-id3d11device-opensharedresource

	IDXGIResource* pDXGIResource = NULL;

	hr = gGameTexture->QueryInterface(__uuidof(IDXGIResource), (LPVOID*)&pDXGIResource);
	if (FAILED(hr))	FatalExit(L"Fail to QueryInterface on shared surface");

	hr = pDXGIResource->GetSharedHandle(&gGameSharedHandle);
	if (FAILED(hr) || gGameSharedHandle == nullptr)	FatalExit(L"Fail to pDXGIResource->GetSharedHandle");
	
	pDXGIResource->Release();


	// If we already had created one, let the old one go.  We do it after the recreation
	// here fills in the prior globals, to avoid possible dead structure usage in the
	// Unity app.

	if (oldGameTexture)
		oldGameTexture->Release();

	return pDevice;
}

// --------------------------------------------------------------------------------------------------
// Move the image halfway, so that we can see half of each eye on the main view.
// This is just a hack way to be sure we are getting stereo output.

#ifdef _DEBUG
void DrawStereoOnGame(ID3D11DeviceContext* pContext, ID3D11Texture2D* surface, ID3D11Texture2D* back, 
	UINT width, UINT height)
{
	D3D11_BOX srcBox = { width/2, 0, 0, width + width/2, height, 1};
	pContext->CopySubresourceRegion(back, 0, 0, 0, 0, surface, 0, &srcBox);
}
#endif

//-----------------------------------------------------------
// Interface to implement the hook for IDXGISwapChain->Present

// This declaration serves a dual purpose of defining the interface routine as required by
// DX11, and also is the storage for the original call, returned by nktInProc.Hook

HRESULT (__stdcall *pOrigPresent)(IDXGISwapChain * This,
	/* [in] */ UINT SyncInterval,
	/* [in] */ UINT Flags
	) = nullptr;

// TODO:  It should be possible to add Temporal AntiAliasing here.  
//  Since we get each frame as a SBS cross eyed buffer, we can save last frame
//  and use it for TAA.  Should really be valuable in VR.


// This is it. The one we are after.  This is the hook for the DX11 Present call
// which the game will call for every frame.  At each call, we will make a copy
// of whatever the game drew, and that will be passed along via the shared surface
// to the VR layer.
//
// If we are in DirectMode, we use SetActiveEye to fetch each eye's bits, and 
// update the double-wide stereo surface gGameTexture.  In Automatic mode, we
// just copy the double wide stereo backbuffer directly.

HRESULT __stdcall Hooked_Present(IDXGISwapChain * This,
	/* [in] */ UINT SyncInterval,
	/* [in] */ UINT Flags)
{
	HRESULT hr;
	ID3D11Texture2D* backBuffer = nullptr;
	D3D11_TEXTURE2D_DESC pDesc;
	ID3D11Device* pDevice = nullptr;
	ID3D11DeviceContext* pContext = nullptr;

	if (gGameTexture == nullptr)
		CreateSharedTexture(This);
	
	hr = This->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer);
	if (SUCCEEDED(hr) && gGameTexture != nullptr)
	{
		backBuffer->GetDesc(&pDesc);
		backBuffer->GetDevice(&pDevice);
		pDevice->GetImmediateContext(&pContext);

		if (gDirectMode)
		{
			hr = NvAPI_Stereo_SetActiveEye(gNVAPI, NVAPI_STEREO_EYE_RIGHT);
			pContext->CopySubresourceRegion(gGameTexture, 0, 0, 0, 0, backBuffer, 0, nullptr);

			hr = NvAPI_Stereo_SetActiveEye(gNVAPI, NVAPI_STEREO_EYE_LEFT);
			pContext->CopySubresourceRegion(gGameTexture, 0, pDesc.Width, 0, 0, backBuffer, 0, nullptr);
		}
		else
		{
			hr = NvAPI_Stereo_ReverseStereoBlitControl(gNVAPI, true);

			pContext->CopySubresourceRegion(gGameTexture, 0, 0, 0, 0, backBuffer, 0, nullptr);

			hr = NvAPI_Stereo_ReverseStereoBlitControl(gNVAPI, false);
		}

#ifdef _DEBUG
		DrawStereoOnGame(pContext, gGameTexture, backBuffer, pDesc.Width, pDesc.Height);
#endif
		pContext->Release();
		pDevice->Release();
	}
	backBuffer->Release();

	HRESULT hrp = pOrigPresent(This, SyncInterval, Flags);

	return hrp;
}


// This thread routine will be Resumed whenever the Present call has fresh data.
// It will be Suspended mostly.  This is a separate thread, even with the complexity
// that adds, because we otherwise have two back to back StretchRect on the main
// game thread.  The first one is inescapable, we must copy the stereo bits from the 
// backbuffer into the gGameSurface.  The second is to copy to the gSharedTarget
// RenderTarget, and this second copy would stall waiting for the first to finish.
// The GPU driver enforces the stall.  Rather than stall there, which would affect
// the game, it's better to stall here in a separate thread where it causes no harm.
//
// WaitForSingleObject returns 0=WAIT_OBJECT_0 when signaled, anything else can be
// considered and error and will exit the thread.  This includes timeouts, because
// we should never have a timeout. The thread is started suspended, and so we wait
// until the first Present call to act, which avoids the largest startup delay.
// After that, anything taking 5 seconds has to be a fatal error, we are expecting
// this to fire every frame.

//DWORD __stdcall CopyGameToShared(LPVOID lpDevice)
//{
//	IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(lpDevice);
//	HRESULT hr;
//	DWORD object;
//	BOOL reset;
//
//	while (true)
//	{
//		object = WaitForSingleObject(gFreshBits, 60 * 1000);
//		if (object != WAIT_OBJECT_0)
//			break;
//
//		hr = device->StretchRect(gGameSurface, nullptr, gSharedTarget, nullptr, D3DTEXF_NONE);
//		// Not supposed to use CLR.
//		//if (FAILED(hr))
//		//	::OutputDebugString(L"Bad StretchRect to RenderTarget in CopyGameToShared thread.\n");
//
//		reset = ResetEvent(gFreshBits);
//		if (!reset)
//			break;
//	}
//
//	return E_FAIL;
//}


// --------------------------------------------------------------------------------------------------
// Interface to implement the hook for IDXGISwapChain->ResizeBuffers

// The original declaration of the call, which also doubles as the storage
// for the original D3D11 call, so that we can call through.
// We need this ResizeBuffers call, because a game will likely have a setting
// for resolution, and we want to change with the game.  UE4 does this on launch.

HRESULT(__stdcall *pOrigResizeBuffers)(IDXGISwapChain* This,
	/* [in] */ UINT BufferCount,
	/* [in] */ UINT Width,
	/* [in] */ UINT Height,
	/* [in] */ DXGI_FORMAT NewFormat,
	/* [in] */ UINT SwapChainFlags) = nullptr;

// The actual Hooked routine for ResizeBuffers, called whenever the game
// makes a IDXGISwapChain->ResizeBuffers call.

HRESULT __stdcall Hooked_ResizeBuffers(IDXGISwapChain* This,
	/* [in] */ UINT BufferCount,
	/* [in] */ UINT Width,
	/* [in] */ UINT Height,
	/* [in] */ DXGI_FORMAT NewFormat,
	/* [in] */ UINT SwapChainFlags)
{
	HRESULT hr;

	// As soon as we know we are setting up a new resolution, we want to set the
	// gGameSharedHandle to null, to notify the VR side that this is going away.
	// Given the async and multi-threaded nature of these pieces in different
	// processes, it's not clear if this will work in every case. 
	//
	// ToDo: We might need to keep VR and game side in sync to avoid dead texture use.
	//
	// No good way to properly dispose of this shared handle, we cannot CloseHandle
	// because it's not a real handle.  Microsoft.  Geez.

	gGameSharedHandle = nullptr;

#ifdef _DEBUG
	wchar_t info[512];

	::OutputDebugString(L"NativePlugin::Hooked_ResizeBuffers called\n");

	swprintf_s(info, _countof(info), L"  Width: %d, Height: %d, Format: %d\n"
		, Width, Height, NewFormat);
	::OutputDebugString(info);
#endif

	// Run original call game is expecting.

	hr = pOrigResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	if (FAILED(hr)) FatalExit(L"Failed to IDXGISwapChain->ResizeBuffers");

	// Refresh our shared texture and shared HANDLE to match, otherwise we draw only
	// a partial image on the big screen.

	CreateSharedTexture(This);

	return hr;
}

// --------------------------------------------------------------------------------------------------
// Interface for Win10 or Win7+evilUpdate variant IDXGIFactory2->CreateSwapChainForHwnd
// Known to be used by BatmanTelltale and recent Unity games.

HRESULT(__stdcall *pOrigCreateSwapChainForHwnd)(IDXGIFactory2 * This,
	_In_  IUnknown *pDevice,
	_In_  HWND hWnd,
	_In_  const DXGI_SWAP_CHAIN_DESC1 *pDesc,
	_In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
	_In_opt_  IDXGIOutput *pRestrictToOutput,
	_COM_Outptr_  IDXGISwapChain1 **ppSwapChain) = nullptr;

HRESULT __stdcall Hooked_CreateSwapChainForHwnd(IDXGIFactory2 * This,
	_In_  IUnknown *pDevice,
	_In_  HWND hWnd,
	_In_  const DXGI_SWAP_CHAIN_DESC1 *pDesc,
	_In_opt_  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *pFullscreenDesc,
	_In_opt_  IDXGIOutput *pRestrictToOutput,
	_COM_Outptr_  IDXGISwapChain1 **ppSwapChain)
{
	HRESULT hr;

#ifdef _DEBUG
	wchar_t info[512];

	::OutputDebugString(L"NativePlugin::Hooked_CreateSwapChainForHwnd called\n");

	if (pDesc)
	{
		swprintf_s(info, _countof(info), L"  Width: %d, Height: %d, Format: %d\n"
			, pDesc->Width, pDesc->Height, pDesc->Format);
		::OutputDebugString(info);
	}
#endif

	// Run original call game is expecting.

	hr = pOrigCreateSwapChainForHwnd(This, pDevice, hWnd, pDesc, pFullscreenDesc, pRestrictToOutput, ppSwapChain);
	if (FAILED(hr)) FatalExit(L"Failed to create CreateSwapChainForHwnd");


	// Using that fresh IDXGISwapChain, we can now hook the Present call, which 
	// is what we are really after.

	HookPresent(*ppSwapChain);

	return hr;
}


// --------------------------------------------------------------------------------------------------
// Interface to implement the hook for IDXGIFactory1->CreateSwapChain

// This declaration serves a dual purpose of defining the interface routine as required by
// DX11, and also is the storage for the original call, returned by nktInProc.Hook

HRESULT(__stdcall *pOrigCreateSwapChain)(IDXGIFactory1 * This,
	_In_  IUnknown *pDevice,
	_In_  DXGI_SWAP_CHAIN_DESC *pDesc,
	_COM_Outptr_  IDXGISwapChain **ppSwapChain
	) = nullptr;

// The actual Hooked routine for CreateSwapChain, called whenever the game
// makes a IDXGIFactory1->CreateSwapChain call.

HRESULT __stdcall Hooked_CreateSwapChain(IDXGIFactory1 * This,
	_In_  IUnknown *pDevice,
	_In_  DXGI_SWAP_CHAIN_DESC *pDesc,
	_COM_Outptr_  IDXGISwapChain **ppSwapChain)
{
	HRESULT hr;

#ifdef _DEBUG
	wchar_t info[512];

	::OutputDebugString(L"NativePlugin::Hooked_CreateSwapChain called\n");

	if (pDesc)
	{
		swprintf_s(info, _countof(info), L"  Width: %d, Height: %d, Format: %d\n"
			, pDesc->BufferDesc.Width, pDesc->BufferDesc.Height, pDesc->BufferDesc.Format);
		::OutputDebugString(info);
	}
#endif

	// Run original call game is expecting.

	hr = pOrigCreateSwapChain(This, pDevice, pDesc, ppSwapChain);
	if (FAILED(hr)) FatalExit(L"Failed to create CreateSwapChain");


	// Using that fresh IDXGISwapChain, we can now hook the Present call, which 
	// is what we are really after.

	HookPresent(*ppSwapChain);

	return hr;
}


// --------------------------------------------------------------------------------------------------

// Here we want to start the daisy chain of hooking DX11 interfaces, to
// ultimately get access to IDXGISwapChain::Present
//
// The sequence a game will use is:
//   IDXGIFactory1* CreateDXGIFactory1();
//   IDXGIFactory1::CreateSwapChain(return pIDXGISwapChain);
//   pIDXGISwapChain->Present
//
// This hook call is called from the Deviare side, to continue the 
// daisy-chain to IDXGISwapChain::Present.

void HookCreateSwapChain(IDXGIFactory* dDXGIFactory)
{
	// This can be called multiple times by a game, so let's be sure to
	// only hook once.
	if (pOrigCreateSwapChain == nullptr && dDXGIFactory != nullptr)
	{
#ifdef _DEBUG 
		nktInProc.SetEnableDebugOutput(TRUE);
#endif

		// If we are here, we want to now hook the IDXGIFactory::CreateSwapChain
		// routine, as that will be the next thing the game does, and we
		// need access to the IDXGISwapChain.
		// This can't be done directly, because this is a vtable based API
		// call, not an export from a DLL, so we need to directly hook the 
		// address of the CreateSwapChain function. This is also why we are 
		// using In-Proc here.  Since we are using the CINTERFACE, we can 
		// just directly access the address.

		SIZE_T hook_id;
		DWORD dwOsErr = nktInProc.Hook(&hook_id, (void**)&pOrigCreateSwapChain,
			lpvtbl_CreateSwapChain(dDXGIFactory), Hooked_CreateSwapChain, 0);

		if (dwOsErr != S_OK) FatalExit(L"Failed to hook IDXGIFactory1::CreateSwapChain");
	}
}


// Win10 recommended variant.

void HookCreateSwapChainForHwnd(IDXGIFactory2* dDXGIFactory)
{
	// This can be called multiple times by a game, so let's be sure to
	// only hook once.
	if (pOrigCreateSwapChainForHwnd == nullptr && dDXGIFactory != nullptr)
	{
#ifdef _DEBUG 
		nktInProc.SetEnableDebugOutput(TRUE);
#endif

		// If we are here, we want to now hook the IDXGIFactory::CreateSwapChainForHwnd
		// routine, as that will be the next thing the game does, and we
		// need access to the IDXGISwapChain.
		// This can't be done directly, because this is a vtable based API
		// call, not an export from a DLL, so we need to directly hook the 
		// address of the CreateSwapChainForHwnd function. This is also why we are 
		// using In-Proc here.  Since we are using the CINTERFACE, we can 
		// just directly access the address.

		SIZE_T hook_id;
		DWORD dwOsErr = nktInProc.Hook(&hook_id, (void**)&pOrigCreateSwapChainForHwnd,
			lpvtbl_CreateSwapChainForHwnd(dDXGIFactory), Hooked_CreateSwapChainForHwnd, 0);

		if (dwOsErr != S_OK) FatalExit(L"Failed to hook IDXGIFactory1::CreateSwapChainForHwnd");
	}
}


// Here we want to hook IDXGISwapChain::Present
//
// SwapChain can be created from D3D11 with CreateDeviceAndSwapChain, hence this 
// might be called implicitly from there.
//
// It is common code for both that path, and the direct path from CreateSwapChain
// or CreateSwapChainForHwnd.

void HookPresent(IDXGISwapChain* pSwapChain)
{
	// This can be called multiple times by a game, so let's be sure to
	// only hook once.
	if (pOrigPresent == nullptr && pSwapChain != nullptr)
	{
#ifdef _DEBUG 
		nktInProc.SetEnableDebugOutput(TRUE);
#endif

		SIZE_T hook_id;
		DWORD dwOsErr;

		dwOsErr = nktInProc.Hook(&hook_id, (void**)&pOrigPresent,
			lpvtbl_Present_DX11(pSwapChain), Hooked_Present, NKTHOOKLIB_DisallowReentrancy);
		if (FAILED(dwOsErr))
			::OutputDebugStringA("Failed to hook IDXGISwapChain::Present\n");

		dwOsErr = nktInProc.Hook(&hook_id, (void**)&pOrigResizeBuffers,
			lpvtbl_ResizeBuffers(pSwapChain), Hooked_ResizeBuffers, NKTHOOKLIB_DisallowReentrancy);
		if (FAILED(dwOsErr))
			::OutputDebugStringA("Failed to hook IDXGISwapChain::ResizeBuffers\n");

		// Create Texture2D and HANDLE we'll use to share the stereo game bits across
		// the process boundary.

//		pDevice = CreateSharedTexture(pSwapChain);


		// Since we are doing setup here, also create a thread that will be used to copy
		// from the stereo game surface into the shared surface.  This way the game will
		// not stall while waiting for that copy.
		//
		// And the thread synchronization Event object. Signaled when we get fresh bits.
		// Starts in off state, thread active, so it should pause at launch.
		//ToDo: Not presently active
		//gFreshBits = CreateEvent(
		//	NULL,               // default security attributes
		//	TRUE,               // manual, not auto-reset event
		//	FALSE,              // initial state is nonsignaled
		//	nullptr);			// object name
		//if (gFreshBits == nullptr) FatalExit(L"Fail to CreateEvent for gFreshBits");

		//gSharedThread = CreateThread(
		//	NULL,                   // default security attributes
		//	0,                      // use default stack size  
		//	CopyGameToShared,       // thread function name
		//	pDevice9,		        // device, as argument to thread function 
		//	0,				        // runs immediately, to a pause state. 
		//	nullptr);			    // returns the thread identifier 
		//if (gSharedThread == nullptr) FatalExit(L"Fail to CreateThread for GameToShared");

		// We are certain to be being called from the game's primary thread here,
		// as this is CreateSwapChain.  Save the reference.
		// ToDo: Can't use Suspend/Resume, because the task switching time is too
		// high, like >16ms, which is must larger than we can use.
		//HANDLE thread = GetCurrentThread();
		//DuplicateHandle(GetCurrentProcess(), thread, GetCurrentProcess(), &gameThread, 0, TRUE, DUPLICATE_SAME_ACCESS);
	}
}


// --------------------------------------------------------------------------------------------------

// The SetDriverMode override, where we can check the input mode.
// This will be called directly after a game calls NvAPI_Stereo_SetDriverMode
// as we've hooked the function directly.  It's worth noting that this function 
// requires __cdecl, not __stdcall.
//
// We don't need any serious processing, just need to set the gDirectMode
// global to let us know to draw differently in Present.

typedef NvAPI_Status(__cdecl *tNvAPI_Stereo_SetDriverMode)(NV_STEREO_DRIVER_MODE mode);
tNvAPI_Stereo_SetDriverMode pOrigNvAPI_Stereo_SetDriverMode = nullptr;

NvAPI_Status __cdecl Hooked_NvAPI_Stereo_SetDriverMode(NV_STEREO_DRIVER_MODE mode)
{
	if (mode == NVAPI_STEREO_DRIVER_MODE_DIRECT)
		gDirectMode = true;

	NvAPI_Status ret = pOrigNvAPI_Stereo_SetDriverMode(mode);

#ifdef _DEBUG
	wchar_t info[512];
	swprintf_s(info, _countof(info),
		L"NativePlugin::Hooked_NvAPI_Stereo_SetDriverMode - mode: %d  ret: %d\n", mode, ret);
	::OutputDebugString(info);
#endif

	return ret;
}


// Hook the nvapi.  This is required to support Direct Mode in the driver, for 
// games like Tomb Raider and Deus Ex that have no SBS.
// There is only one call in the nvidia dll, nvapi_QueryInterface.  That will
// be fetched, and then the _NvAPI_Stereo_SetDriverMode call will be hooked
// so that we can see when a game sets Direct Mode and change behavior in Present.
// This is also done in DeviarePlugin at OnLoad.
//
// We are not hooking nvapi_QueryInterface, because In-Proc has a bug that
// will crash if it's x64.  Does not update an IP relative address, so we just
// call through nvapi_QueryInterface to fetch the SetDriverMode address.

typedef void* (__cdecl *t_nvapi_QueryInterface)(UINT32 offset);
t_nvapi_QueryInterface pOrig_nvapi_QueryInterface = nullptr;

UINT32 SetDriverMode = 0x5E8F0BEC;

void HookNvapiSetDriverMode()
{
#if (_WIN64)
#define REAL_NVAPI_DLL L"nvapi64.dll"
#else
#define REAL_NVAPI_DLL L"nvapi.dll"
#endif

	HMODULE hNvapi = LoadLibrary(REAL_NVAPI_DLL);
	if (hNvapi == NULL) FatalExit(L"Failed to LoadLibrary for nvapi.dll");

	FARPROC pQueryInterface = GetProcAddress(hNvapi, "nvapi_QueryInterface");
	if (pQueryInterface == NULL) FatalExit(L"Failed to GetProcAddress for nvapi_QueryInterface");

	// This could be called multiple times by a game, so let's be sure to
	// only hook once.
	if (pOrigNvAPI_Stereo_SetDriverMode == nullptr && pQueryInterface != nullptr)
	{
#ifdef _DEBUG 
		nktInProc.SetEnableDebugOutput(TRUE);
#endif

		// Use original nvapi_QueryInterface to fetch the address of the 
		// _NvAPI_Stereo_SetDriverMode function, so we can hook that directly.

		pOrig_nvapi_QueryInterface = reinterpret_cast<t_nvapi_QueryInterface>(pQueryInterface);
		void* pSetDriverMode = pOrig_nvapi_QueryInterface(SetDriverMode);

		SIZE_T hook_id;
		DWORD dwOsErr = nktInProc.Hook(&hook_id, (void**)&pOrigNvAPI_Stereo_SetDriverMode,
			pSetDriverMode, Hooked_NvAPI_Stereo_SetDriverMode, 0);

		if (FAILED(dwOsErr)) FatalExit(L"Failed to hook NVAPI.DLL NvAPI_Stereo_SetDriverMode");
	}
}


// From: https://github.com/Rebzzel/kiero/blob/master/kiero.cpp
//
// Create a window, then a DX11 device and swapchain so we can then fetch the Present
// call Address.  It will be the same address for both this fake window and the game.
// 
// This is only for the DX11 games.  Should have no impact on them to hook the DX11
// Present call.

Status::Enum FindAndHookPresent()
{
	WNDCLASSEX windowClass;
	windowClass.cbSize = sizeof(WNDCLASSEX);
	windowClass.style = CS_HREDRAW | CS_VREDRAW;
	windowClass.lpfnWndProc = DefWindowProc;
	windowClass.cbClsExtra = 0;
	windowClass.cbWndExtra = 0;
	windowClass.hInstance = GetModuleHandle(NULL);
	windowClass.hIcon = NULL;
	windowClass.hCursor = NULL;
	windowClass.hbrBackground = NULL;
	windowClass.lpszMenuName = NULL;
	windowClass.lpszClassName = KIERO_TEXT("Kiero");
	windowClass.hIconSm = NULL;

	::RegisterClassEx(&windowClass);

	HWND window = ::CreateWindow(windowClass.lpszClassName, KIERO_TEXT("Kiero DirectX Window"), WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, windowClass.hInstance, NULL);

	// 3Dmigoto goes to great lengths to prevent other software from getting to the System32 
	// directory.  We need that directory here though, so that we can hook the System32\d3d11.dll
	// version of Present.  We need that address because we want to be able to hook Present
	// right *after* 3Dmigoto, so that the software cursor code will have run.  If we hook
	// normally, then 3Dmigoto comes in first, and we only get the 3Dmigoto address for Present.
	// But, there is a feature in 3Dmigoto that allows for getting the system directory, which
	// is the suppress.  This makes the next LoadLibrary work normally with no bypass.

	LoadLibraryEx(L"SUPPRESS_3DMIGOTO_REDIRECT", NULL, 0);

	wchar_t sysLib[MAX_PATH];
	UINT ret = GetSystemDirectoryW(sysLib, ARRAYSIZE(sysLib));
	if (ret == 0) FatalExit(L"Failed to FindAndHookPresent->GetSystemDirectoryW");
	wcscat_s(sysLib, MAX_PATH, L"\\d3d11.dll");
	
	// With suppress active, and us specifying the full System32 path, we can get the address of the normal d3d11.

	HMODULE libD3D11;
	if ((libD3D11 = ::LoadLibraryExW(sysLib, NULL, 0)) == NULL)
	{
		::OutputDebugStringA("Failed to load System32 d3d11.dll");
		::DestroyWindow(window);
		::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
		return Status::ModuleNotFoundError;
	}

#ifdef _DEBUG
	HMODULE migotoD3D11 = ::GetModuleHandle(KIERO_TEXT("d3d11.dll"));

	wchar_t info[512];

	::OutputDebugString(L"NativePlugin::FindAndHookPresent called. \n");

	swprintf_s(info, _countof(info), L"Looking for %s.  libD3D11: %p, migotoD3D11: %p \n",
		sysLib, libD3D11, migotoD3D11);
	::OutputDebugString(info);
#endif

	void* fnD3D11CreateDevice;
	if ((fnD3D11CreateDevice = ::GetProcAddress(libD3D11, "D3D11CreateDevice")) == NULL)
	{
		::OutputDebugStringA("Failed to GetProcAddress on D3D11CreateDevice");
		::DestroyWindow(window);
		::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
		return Status::UnknownError;
	}

	ID3D11Device* sysDevice;

	HRESULT hr = ((long(__stdcall*)(
		IDXGIAdapter*,
		D3D_DRIVER_TYPE,
		HMODULE,
		UINT,
		const D3D_FEATURE_LEVEL*,
		UINT,
		UINT,
		ID3D11Device**,
		D3D_FEATURE_LEVEL*,
		ID3D11DeviceContext**
		))(fnD3D11CreateDevice))(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &sysDevice, NULL, NULL);
	if (FAILED(hr))
	{
		::OutputDebugStringA("Failed call to D3D11CreateDevice");
		::DestroyWindow(window);
		::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
		return Status::UnknownError;
	}

	// First, convert our ID3D11Device1 into an IDXGIDevice1
	IDXGIDevice1* dxgiDevice;
	if (FAILED(sysDevice->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgiDevice)))
	{
		::OutputDebugStringA("Failed call to QueryInterface(dxgidevice)");
		::DestroyWindow(window);
		::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
		return Status::UnknownError;
	}

	// Second, use the IDXGIDevice1 interface to get access to the adapter
	IDXGIAdapter* dxgiAdapter;
	dxgiDevice->GetAdapter(&dxgiAdapter);

	// Third, use the IDXGIAdapter interface to get access to the factory
	IDXGIFactory2* dxgiFactory;
	dxgiAdapter->GetParent(__uuidof(IDXGIFactory2), (void**)&dxgiFactory);

	// At this point, dxgiFactory object.  But we need to create Swap chain using it.

	DXGI_RATIONAL refreshRate = { 0 };
	refreshRate.Numerator = 60;
	refreshRate.Denominator = 1;

	DXGI_MODE_DESC bufferDesc = { 0 };
	bufferDesc.Width = 100;
	bufferDesc.Height = 100;
	bufferDesc.RefreshRate = refreshRate;
	bufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	bufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	bufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

	DXGI_SAMPLE_DESC sampleDesc = { 0 };
	sampleDesc.Count = 1;
	sampleDesc.Quality = 0;

	DXGI_SWAP_CHAIN_DESC swapChainDesc = { 0 };
	swapChainDesc.BufferDesc = bufferDesc;
	swapChainDesc.SampleDesc = sampleDesc;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 1;
	swapChainDesc.OutputWindow = window;
	swapChainDesc.Windowed = 1;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	IDXGISwapChain* swapChain;
	ID3D11Device* device;
	ID3D11DeviceContext* context;

	hr = dxgiFactory->CreateSwapChain(sysDevice, &swapChainDesc, &swapChain);
	if (FAILED(hr))
	{
		::OutputDebugStringA("Failed call to CreateSwapChain");
		::DestroyWindow(window);
		::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
		return Status::UnknownError;
	}

	// This can fetch the 3Dmigoto dll
	// Which can lead to errors if the game d3dx.ini specifies returning errors.
	// Also seems to introduce a bug if we request only the swapchain.

	//HRESULT hr = ((long(__stdcall*)(
	//	IDXGIAdapter*,
	//	D3D_DRIVER_TYPE,
	//	HMODULE,
	//	UINT,
	//	const D3D_FEATURE_LEVEL*,
	//	UINT,
	//	UINT,
	//	const DXGI_SWAP_CHAIN_DESC*,
	//	IDXGISwapChain**,
	//	ID3D11Device**,
	//	D3D_FEATURE_LEVEL*,
	//	ID3D11DeviceContext**))(fnD3D11CreateDevice))(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, NULL, D3D11_SDK_VERSION, &swapChainDesc, &swapChain, NULL, NULL, NULL);
	//if (FAILED(hr))
	//{
	//	::OutputDebugStringA("Failed call to D3D11CreateDevice");
	//	::DestroyWindow(window);
	//	::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
	//	return Status::UnknownError;
	//}

	// At this point, it's created the Device and SwapChain, so now we can fetch the address of Present
	// from the SwapChain's vtable.

	HookPresent(swapChain);

	// Now release all the created objects, as they were just used to get us to the vtable.

	swapChain->Release();
	swapChain = NULL;

	//device->Release();
	//device = NULL;

	//context->Release();
	//context = NULL;

	::DestroyWindow(window);
	::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);

	::OutputDebugStringA("Successfully hooked DXGI::Present");

	return Status::Success;
}


