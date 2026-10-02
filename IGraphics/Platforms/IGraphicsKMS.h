/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#pragma once

/**
 * @file IGraphicsKMS.h
 * @brief IGraphics platform class for Linux without a window system: DRM/KMS + GBM + EGL
 * (GLES2, NanoVG) for drawing, evdev for touch. Rat Factory L9 spike: one full-screen
 * surface, one connector, one touch device. Not a desktop backend (that is IGraphicsLinux,
 * X11 / GLX, upstream PR #1336); this is the appliance's console-less case.
 *
 * Two surface modes:
 *  - kms:       the first connected connector's mode, scanned out with page flips (vsync).
 *               Needs DRM master (no other process may own the display).
 *  - offscreen: a GBM surface on a render node, no scanout, no vsync. Exercises the same
 *               GPU path while another process owns the display (and measures headroom).
 *
 * The panel keeps its own layout and is drawn at a uniform scale that fits the surface,
 * centred, with a matte around it (ROADMAP F69), through IGraphicsNanoVG::SetPresentTarget().
 *
 * There is no event loop: the program that owns the process calls PollInput() and
 * RenderFrame() from its main loop (the appliance's main loop, or a test driver).
 *
 * Frame pacing (kms): by default a frame's page flip is queued and RenderFrame() returns
 * without waiting for it, so the CPU work of the next frame overlaps the GPU work and scan-out
 * of this one; the flip is waited for just before the next one is queued. Config::maxFps caps
 * the frame rate: RenderFrame() returns 0 before the next frame is due and leaves what is dirty
 * for the call that is (SecondsToNextFrame() tells the owner's loop how long it may sleep).
 */

#include "IGraphics_select.h"

#include <cstdint>
#include <string>
#include <vector>

struct gbm_device;
struct gbm_surface;
struct gbm_bo;

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

/** IGraphics platform class for DRM/KMS + EGL + evdev (no X11, no Wayland)
 *  @ingroup PlatformClasses */
class IGraphicsKMS final : public IGRAPHICS_DRAW_CLASS
{
public:
  /** Settings read when the window opens. Fill IGraphicsKMS::Settings() before OpenWindow(). */
  struct Config
  {
    bool offscreen = false;                       // true: render node + GBM surface, no scanout
    bool surfaceless = false;                     // offscreen without a DRM device: EGL_MESA_platform_surfaceless + pbuffer
    std::string drmDevice = "/dev/dri/card0";     // kms mode
    std::string renderDevice = "/dev/dri/renderD128"; // offscreen mode
    int offscreenW = 1024, offscreenH = 600;      // offscreen surface size
    int modeW = 0, modeH = 0;                     // kms: wanted mode (0 = the connector's preferred)
    bool rgb565 = false;                          // scanout / surface format: RGB565 instead of XRGB8888
    bool fit = true;                              // uniform scale to fit the surface (F69); false: 1:1, top left
    IColor matte = IColor(255, 12, 12, 14);       // dead space around the panel
    std::string touchDevice;                      // evdev node ("" = none, "auto" = first INPUT_PROP_DIRECT device)
    bool touchSwapXY = false, touchInvertX = false, touchInvertY = false;
    bool logInput = false;                        // one stderr line per dispatched touch event
    double maxFps = 0.;                           // > 0: frames start at most this often (dropped, not queued)
    bool overlap = true;                          // kms: queue the flip and return (CPU / GPU overlap)
    bool gpuTiming = false;                       // glFinish after the panel and after the present:
                                                  // per-stage GPU times (serialises CPU and GPU; diagnostic)
    bool hwScissor = true;                        // NanoVG's draw calls also clipped by the GL scissor
                                                  // (false: shader clipping only, as upstream; diagnostic)
    bool partialPresent = false;                  // present only what changed (EGL_EXT_buffer_age), the rest
                                                  // of the surface kept: ~55 % less GPU time per small frame on
                                                  // the vc4, but the audio threads beside it ran with higher
                                                  // per-period peaks than with a full present (L9 round 2), so
                                                  // it is opt-in
    bool startHidden = false;                     // kms: open without taking the screen (the CRTC keeps what it
                                                  // scans out, e.g. the host's own panel on fbdev); SetVisible(true)
                                                  // takes it. The owner can open the UI early and show it later
                                                  // without paying EGL, the layout and the first frame again
  };
  static Config& Settings();

  /** Frame statistics since the last ResetStats(). */
  struct Stats
  {
    uint64_t framesPresented = 0;   // RenderFrame() calls that drew and presented
    uint64_t framesSkipped = 0;     // RenderFrame() calls with nothing dirty
    uint64_t framesDeferred = 0;    // RenderFrame() calls before the next frame was due (maxFps)
    double drawSecs = 0.;           // CPU-side time in IGraphics::Draw (tessellation + GL calls)
    double swapSecs = 0.;           // eglSwapBuffers (incl. GPU wait when the driver blocks)
    double flipWaitSecs = 0.;       // waiting for the page-flip event (vsync), kms only
    double maxFrameSecs = 0.;       // longest single RenderFrame()
    double panelGpuSecs = 0.;       // gpuTiming: the panel's frame buffer, flushed to done
    double presentGpuSecs = 0.;     // gpuTiming: the present (matte + panel composited), to done
    double dirtyArea = 0.;          // sum of the dirty rects' areas as drawn (merged), panel pixels
    uint64_t dirtyRects = 0;        // dirty rects as drawn (merged)
    double presentArea = 0.;        // surface pixels presented (matte + composite)
    uint64_t bufferAge[4] = {};     // frames by back-buffer age: 0 (unknown: full present), 1, 2, 3+
  };

  IGraphicsKMS(IGEditorDelegate& dlg, int w, int h, int fps, float scale);
  ~IGraphicsKMS();

  void* OpenWindow(void* pWindow) override;
  void CloseWindow() override;
  bool WindowIsOpen() override { return mWindowOpen; }
  void* GetWindow() override { return mWindowOpen ? (void*) this : nullptr; }
  void PlatformResize(bool parentHasResized) override {}

  void HideMouseCursor(bool hide, bool lock) override {}
  void MoveMouseCursor(float x, float y) override {}
  ECursor SetMouseCursor(ECursor cursorType) override { return ECursor::ARROW; }
  void GetMouseLocation(float& x, float& y) const override { x = mLastX; y = mLastY; }

  EMsgBoxResult ShowMessageBox(const char* str, const char* title, EMsgBoxType type, IMsgBoxCompletionHandlerFunc completionHandler) override;
  void ForceEndUserEdit() override {}
  const char* GetPlatformAPIStr() override { return mSurfaceless ? "KMS (surfaceless pbuffer)" : mOffscreen ? "KMS (offscreen GBM)" : "KMS"; }
  void UpdateTooltips() override {}
  bool RevealPathInExplorerOrFinder(WDL_String& path, bool select) override { return false; }
  void PromptForFile(WDL_String& fileName, WDL_String& path, EFileAction action, const char* ext, IFileDialogCompletionHandlerFunc completionHandler) override {}
  void PromptForDirectory(WDL_String& dir, IFileDialogCompletionHandlerFunc completionHandler) override {}
  bool PromptForColor(IColor& color, const char* str, IColorPickerHandlerFunc func) override { return false; }
  bool OpenURL(const char* url, const char* msgWindowTitle, const char* confirmMsg, const char* errMsgOnFailure) override { return false; }
  bool GetTextFromClipboard(WDL_String& str) override { return false; }
  bool SetTextInClipboard(const char* str) override { return false; }

  // --- the main-loop interface (no window system drives this platform) ---

  /** Reads every pending touch event and dispatches it to IGraphics. Non-blocking. */
  void PollInput();

  /** Draws what is dirty and presents it. \p forceAll marks every control dirty first.
   * kms: blocks until the page flip of this frame completes (vsync), so the loop is paced at
   * the panel's refresh rate. \return 1 if a frame was presented, 0 if nothing was dirty, -1 on error. */
  int RenderFrame(bool forceAll = false);

  /** Show or hide the panel (kms: hand the screen back and forth with whatever scanned out before).
   * Hiding waits for a queued page flip, puts the CRTC back on the frame buffer it scanned out when the
   * window opened (the console's fbdev buffer, or the host's own panel drawn there) and stops drawing:
   * RenderFrame() draws nothing and returns 0, the controls keep their state and stay dirty, nothing touches
   * the GPU. Showing drops the touch events queued meanwhile (and a finger that is still down, until it is
   * lifted), marks every control dirty, and the next RenderFrame() takes the CRTC with a full frame.
   * offscreen / surfaceless: only the drawing stops. \return false when the screen could not be handed back. */
  bool SetVisible(bool visible);
  bool IsVisible() const { return mVisible; }

  /** Draws every control once into the panel's frame buffer without presenting anything and waits for the
   * GPU: the shaders, the glyph atlas and the SVGs are ready before the first frame that is shown. For an
   * owner that opens the UI hidden. */
  void Prewarm();

  /** Seconds until the next frame is due under Config::maxFps (0 when it is due now). */
  double SecondsToNextFrame() const;

  /** Waits for a queued page flip, if any (kms, overlap). */
  void WaitForFlip();

  /** Writes the last presented surface (whole screen incl. matte) to a PNG. Call right after RenderFrame(). */
  bool SaveScreenshot(const char* path);

  /** Reads the panel's own frame buffer (Width x Height at the draw scale, RGBA, top row first) as
   * it is now, without drawing: what partial redraws have built up. Tests compare it with a full redraw. */
  bool ReadPanel(std::vector<uint8_t>& rgba, int& w, int& h);

  /** Reads the whole surface (matte included) as it is now, without drawing. Meaningful in the
   * surfaceless mode (a pbuffer, never swapped): what partial presents have built up. */
  bool ReadSurface(std::vector<uint8_t>& rgba, int& w, int& h);

  /** File descriptors for the owner's poll(): the touch device (-1 if none). */
  int TouchFD() const { return mTouchFD; }

  /** Inverse of the touch mapping: the raw device coordinates that land on UI point (x, y). */
  bool UIToRawTouch(float x, float y, int& rawX, int& rawY) const;
  /** The raw device coordinates for a surface pixel. */
  void SurfaceToRawTouch(float sx, float sy, int& rawX, int& rawY) const;

  int SurfaceWidth() const { return mSurfaceW; }
  int SurfaceHeight() const { return mSurfaceH; }
  float FitScale() const { return mFitScale; }
  float OffsetX() const { return mOffX; }
  float OffsetY() const { return mOffY; }
  const char* TouchName() const { return mTouchName.c_str(); }
  void TouchRange(int& minX, int& maxX, int& minY, int& maxY) const { minX = mAbsMinX; maxX = mAbsMaxX; minY = mAbsMinY; maxY = mAbsMaxY; }

  const Stats& GetStats() const { return mStats; }
  void ResetStats() { mStats = Stats(); }

protected:
  IPopupMenu* CreatePlatformPopupMenu(IPopupMenu& menu, const IRECT bounds, bool& isAsync) override { isAsync = false; return nullptr; }
  void CreatePlatformTextEntry(int paramIdx, const IText& text, const IRECT& bounds, int length, const char* str) override {}

  void OnPanelFlushed() override;

private:
  PlatformFontPtr LoadPlatformFont(const char* fontID, const char* fileNameOrResID) override;
  PlatformFontPtr LoadPlatformFont(const char* fontID, const char* fontName, ETextStyle style) override;
  PlatformFontPtr LoadPlatformFont(const char* fontID, void* pData, int dataSize) override;
  void CachePlatformFont(const char* fontID, const PlatformFontPtr& font) override {}

  bool InitKMS();
  bool InitOffscreen();
  bool InitEGL(void* nativeDisplay, uint32_t gbmFormat);
  bool InitSurfaceless();
  void ShutdownDisplay();
  bool Present();
  uint32_t FramebufferForBO(gbm_bo* bo);
  bool OpenTouch(const std::string& path);
  void DispatchTouch();
  void DrainTouch();
  bool RestoreSavedCrtc();

  bool mWindowOpen = false;
  bool mVisible = true;          // SetVisible(); false: nothing drawn, the CRTC left to its previous owner
  bool mSuppressTouch = false;   // after SetVisible(true): ignore a touch that began while hidden, until it lifts
  bool mOffscreen = false;
  bool mSurfaceless = false;

  int mDrmFD = -1;
  uint32_t mConnectorID = 0, mCrtcID = 0;
  void* mMode = nullptr;        // drmModeModeInfo*
  void* mSavedCrtc = nullptr;   // drmModeCrtc*
  bool mModeSet = false;
  gbm_device* mGBM = nullptr;
  gbm_surface* mGBMSurface = nullptr;
  gbm_bo* mFrontBO = nullptr;   // on screen
  gbm_bo* mPendingBO = nullptr; // flip queued, not yet on screen
  double mNextFrameAt = 0.;     // maxFps: the earliest start of the next frame
  double mFrameGpuWait = 0.;    // gpuTiming: glFinish time inside this frame's Draw()
  bool mHasBufferAge = false;   // EGL_EXT_buffer_age
  static constexpr int kPresentHistory = 4;
  IRECT mPresentHistory[kPresentHistory]; // what the last frames changed, surface pixels, newest first
  int mPresentHistoryN = 0;

  /** The surface region this frame must present: its dirty rects in surface pixels, plus what the
   * frames since this back buffer was last on screen changed (buffer age). Empty: everything. */
  IRECT PresentRegionFor(const IRECTList& panelRects);
  void* mEGLDisplay = nullptr;
  void* mEGLContext = nullptr;
  void* mEGLSurface = nullptr;
  int mSurfaceW = 0, mSurfaceH = 0;
  float mFitScale = 1.f, mOffX = 0.f, mOffY = 0.f;
  bool mFlipPending = false;

  int mTouchFD = -1;
  std::string mTouchName;
  int mAbsMinX = 0, mAbsMaxX = 0, mAbsMinY = 0, mAbsMaxY = 0;
  bool mTouchMT = false;
  int mRawX = 0, mRawY = 0;
  bool mTouchDown = false, mTouchWasDown = false, mTouchMoved = false;
  float mLastX = 0.f, mLastY = 0.f;

  Stats mStats;
};

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
