﻿using System;
using System.Collections;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using UnityEngine;
using UnityEngine.XR;
using UnityEngine.UI;

using Nektra.Deviare2;
using System.IO;


public class DrawSBS : MonoBehaviour
{
    static NktSpyMgr _spyMgr;
    static NktProcess _gameProcess;
    string _nativeDLLName;
    static System.Int32 _gameSharedHandle = 0;
    //static Texture2D _tex;
    TextMesh _rate;
    Texture2D _bothEyes;
    RenderTexture _quadTexture;


    [DllImport("UnityNativePlugin64")]
	private static extern void SetTimeFromUnity(float t);
    [DllImport("UnityNativePlugin64")]
    private static extern void SetTextureFromUnity(System.IntPtr texture, int w, int h);
    [DllImport("UnityNativePlugin64")]
    private static extern IntPtr GetRenderEventFunc();
    [DllImport("UnityNativePlugin64")]
    private static extern IntPtr CreateSharedTexture(int sharedHandle);


    void Start()
    {
        _rate = GameObject.Find("rate").GetComponent<TextMesh>();
        
        //_tex = new Texture2D(512, 512, TextureFormat.RGBA32, false);
        //// Set point filtering just so we can see the pixels clearly
        //_tex.filterMode = FilterMode.Point;
        //// Call Apply() so it's actually uploaded to the GPU
        //_tex.Apply();

        //// Set texture onto our material
        //GetComponent<Renderer>().material.mainTexture = _tex;

        //// Pass texture pointer to the native plugin
        //SetTextureFromUnity(_tex.GetNativeTexturePtr(), _tex.width, _tex.height);

        
        //// Sierpinksky triangles for a default view, shows if other updates fail.
        //for (int y = 0; y < _tex.height; y++)
        //{
        //    for (int x = 0; x < _tex.width; x++)
        //    {
        //        Color color = ((x & y) != 0 ? Color.white : Color.grey);
        //        _tex.SetPixel(x, y, color);
        //    }
        //}
        //// Call Apply() so it's actually uploaded to the GPU
        //_tex.Apply();


        int hresult;
        object continueevent;
        
        string drawSBS_directory = Environment.CurrentDirectory;
        print("root directory: " + drawSBS_directory);
        
        _nativeDLLName = Application.dataPath + "/Plugins/DeviarePlugin.dll";

        string game = @"G:\Games\The Ball\Binaries\Win32\theball.exe";
//        string game = @"C:\Users\bo3b\Documents\Visual Studio Projects\DirectXSamples\Textures\Debug\textures.exe";

        _spyMgr = new NktSpyMgr();
        hresult = _spyMgr.Initialize();
        if (hresult != 0)
            throw new Exception("Deviare initialization error.");
#if DEBUG
        _spyMgr.SettingOverride("SpyMgrDebugLevelMask", 0xCF8);
#endif
        print("Successful SpyMgr Init");


        // We must set the game directory specifically, otherwise it winds up being the 
        // C# app directory which can make the game crash.  This must be done before CreateProcess.
        // This also changes the working directory, which will break Deviare's ability to find
        // the NativePlugin, so we'll use full path descriptions for the DLL load.
        // This must be reset back to the Unity game directory, otherwise Unity will
        // crash with a fatal error.

        Directory.SetCurrentDirectory(Path.GetDirectoryName(game));
        {
            // Launch the game, but suspended, so we can hook our first call and be certain to catch it.

            print("Launching: " + game + "...");
            _gameProcess = _spyMgr.CreateProcess(game, true, out continueevent);
            if (_gameProcess == null)
                throw new Exception("Game launch failed.");

//            if (!System.Diagnostics.Debugger.IsAttached)
//System.Diagnostics.Debugger.Break();

            // Load the NativePlugin for the C++ side.  The NativePlugin must be in this app folder.
            // The Agent supports the use of Deviare in the CustomDLL, but does not respond to hooks.

            print("Load NativePlugin... " + _nativeDLLName);
            _spyMgr.LoadAgent(_gameProcess);
            int result = _spyMgr.LoadCustomDll(_gameProcess, _nativeDLLName, true, true);
            if (result != 1)
                throw new Exception("Could not load NativePlugin DLL.");

            // Hook the primary DX9 creation call of Direct3DCreate9, which is a direct export of 
            // the d3d9 DLL.  All DX9 games must call this interface, or the Direct3DCreate9Ex.
            // We set this to flgOnlyPreCall, because we want to always create the IDirect3D9Ex object.

            print("Hook the D3D9.DLL!Direct3DCreate9...");
            NktHook d3dHook = _spyMgr.CreateHook("D3D9.DLL!Direct3DCreate9", (int)eNktHookFlags.flgOnlyPreCall);
            if (d3dHook == null)
                throw new Exception("Failed to hook D3D9.DLL!Direct3DCreate9");

            // Make sure the CustomHandler in the NativePlugin at OnFunctionCall gets called when this 
            // object is created. At that point, the native code will take over.

            d3dHook.AddCustomHandler(_nativeDLLName, 0, "");


            // Finally attach and activate the hook in the still suspended game process.

            d3dHook.Attach(_gameProcess, true);
            d3dHook.Hook(true);


            // Ready to go.  Let the game startup.  When it calls Direct3DCreate9, we'll be
            // called in the NativePlugin::OnFunctionCall

            print("Continue game launch...");
            _spyMgr.ResumeProcess(_gameProcess, continueevent);
        }
        Directory.SetCurrentDirectory(drawSBS_directory);

        print("Restored Working Directory to: " + drawSBS_directory);


        // We've gotten everything launched, hooked, and setup.  Now we need to wait for the
        // game to call through to CreateDevice, so that we can create the shared surface.
        // Let's yield until that happens.

        StartCoroutine("WaitForSharedSurface");
    }


    // This will just wait until the CreateDevice has been called in DeviarePlugin,
    // and thus we have created a shared surface for copying game bits into.
    // This is asynchronous because it's in the game world, and we don't know when
    // it will happen.
    //
    // Once the GetSharedSurface returns with non-null, we are ready to continue
    // with the VR side of showing those bits.

    private IEnumerator WaitForSharedSurface()
    {
        while (_gameSharedHandle == 0)
        {
            // Check-in every 200ms.
            yield return new WaitForSecondsRealtime(0.2f);

            print("... WaitForSharedSurface");

            // ToDo: To work, we need to pass in a parameter? 
            // This will call to DeviarePlugin native DLL routine to fetch current gGameSurfaceShare HANDLE.
            System.Int32 native = 0; // (int)_tex.GetNativeTexturePtr();
            object parm = native;
            _gameSharedHandle = _spyMgr.CallCustomApi(_gameProcess, _nativeDLLName, "GetSharedHandle", ref parm, true);
        }

        print("-> Got shared handle: " + _gameSharedHandle.ToString("x"));

        // We finally have a valid gGameSurfaceShare as a DX11 HANDLE.  
        // We can thus finish up the init.

        // Call into the UnityNativePlugin for DX11 access to create a ID3D11ShaderResourceView.
        // You'd expect this to be a IDX11Texture2D, but that's not what Unity wants.
        IntPtr shared = CreateSharedTexture(_gameSharedHandle);

        // This is the Unity Texture2D, double width texture, with right eye on the left half.
        // It will always be up to date with latest game image.
        _bothEyes = Texture2D.CreateExternalTexture(3200, 900, TextureFormat.BGRA32, false, true, shared);
        print("..eyes width: " + _bothEyes.width + " height: " + _bothEyes.height + " format: " + _bothEyes.format);

        Material leftMat = GameObject.Find("left").GetComponent<Renderer>().material;
        leftMat.mainTexture = _bothEyes;
        Material rightMat = GameObject.Find("right").GetComponent<Renderer>().material;
        rightMat.mainTexture = _bothEyes;

        leftMat.mainTextureScale = new Vector2(0.5f, 1.0f);
        leftMat.mainTextureOffset = new Vector2(0.5f, 0);
        rightMat.mainTextureScale = new Vector2(0.5f, 1.0f);
        rightMat.mainTextureOffset = new Vector2(0.0f, 0);


        // The texture for the Quad, that will be a RenderTexture, so we can blit into it.
        // Needs to be double width, and vrUsage set, for Blit to know.
        //RenderTextureDescriptor vrDesc = UnityEngine.XR.XRSettings.eyeTextureDesc;
        //vrDesc.width = 1600;
        //vrDesc.height = 900;
        //vrDesc.colorFormat = RenderTextureFormat.BGRA32;
        //vrDesc.vrUsage = VRTextureUsage.TwoEyes;
        //_quadTexture = new RenderTexture(vrDesc);
        //_quadTexture.Create();

        GetComponent<Renderer>().material.mainTexture = _bothEyes;


        StartCoroutine("UpdateFPS");

        yield return null;

        // And allow the final update loop to start.
        //StartCoroutine("CallPluginAtEndOfFrames");
    }


    // Infinite loop of fetching the bits from the shared surface, and drawing them into
    // this VR apps texture, so that Unity will display them.

    private IEnumerator CallPluginAtEndOfFrames()
    {
        while (true)
        {
            // Wait until all frame rendering is done
            yield return new WaitForEndOfFrame();

            // Set time for the plugin
            SetTimeFromUnity(Time.timeSinceLevelLoad);

            // Issue a plugin event with arbitrary integer identifier.
            // The plugin can distinguish between different
            // things it needs to do based on this ID.
            // For our simple plugin, it does not matter which ID we pass here.
            GL.IssuePluginEvent(GetRenderEventFunc(), 1);
        }
    }

    private IEnumerator UpdateFPS()
    {
        while (true)
        {
            yield return new WaitForSecondsRealtime(0.2f);

            float gpuTime;
            if (XRStats.TryGetGPUTimeLastFrame(out gpuTime))
            {
                // At 90 fps, we want to know the % of a single VR frame we are using.
            //    gpuTime = gpuTime / ((1f / 90f) * 1000f) * 100f;
                _rate.text = System.String.Format("{0:F1} ms", gpuTime);
            }
        }
    }



    
    // Update is called once per frame
    // Update is much slower than coroutines.  Unless it's required for VR, skip it.
    void Update()
    {
        //Graphics.Blit(_bothEyes, _quadTexture);
        if (_quadTexture != null)
        {
//            Graphics.CopyTexture(_bothEyes, 0, 0, 0, 0, 3200, 900, _quadTexture, 0, 0, 0, 0);
        }
        //SetTimeFromUnity(Time.timeSinceLevelLoad);
        //GL.IssuePluginEvent(GetRenderEventFunc(), 1);

        //   ModifyTexturePixels();
        //System.Int32 pGameScreen;
        //System.Int32 native = (int)_noiseTex.GetNativeTexturePtr();
        //object parm = native;
//        pGameScreen = _spyMgr.CallCustomApi(_gameProcess, _nativeDLLName, "GetGameSurface", ref parm, true);

        //if (pGameScreen != 0)
        //{
        //    _noiseTex.UpdateExternalTexture((IntPtr)pGameScreen);
        //}
        if (Input.GetKey("escape"))
            Application.Quit();
    }

}
