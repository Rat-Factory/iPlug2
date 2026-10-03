/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

// IGraphicsKMS — DRM/KMS + GBM + EGL (GLES2) + evdev. See IGraphicsKMS.h.

#include "IGraphicsKMS.h"
#include "IPlugPaths.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include "stb_image_write.h"

using namespace iplug;
using namespace igraphics;

namespace
{
double NowSecs()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

class KMSFont : public PlatformFont
{
public:
  KMSFont(const void* pData, int dataSize, int faceIdx)
  : PlatformFont(false), mFontData(new IFontData(pData, dataSize, faceIdx)) {}

  IFontDataPtr GetFontData() override
  {
    if (mFontData && mFontData->IsValid())
      return IFontDataPtr(new IFontData(mFontData->Get(), mFontData->GetSize(), mFontData->GetFaceIdx()));
    return IFontDataPtr(new IFontData());
  }

private:
  IFontDataPtr mFontData;
};

void DestroyFBCallback(gbm_bo* bo, void* data)
{
  const uint32_t fb = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
  gbm_device* gbm = gbm_bo_get_device(bo);
  if (fb)
    drmModeRmFB(gbm_device_get_fd(gbm), fb);
}

void PageFlipHandler(int, unsigned int, unsigned int, unsigned int, void* user)
{
  *static_cast<bool*>(user) = false;
}

bool TestBit(const unsigned long* bits, int bit)
{
  const int n = 8 * sizeof(unsigned long);
  return (bits[bit / n] >> (bit % n)) & 1UL;
}
} // namespace

IGraphicsKMS::Config& IGraphicsKMS::Settings()
{
  static Config sConfig;
  return sConfig;
}

IGraphicsKMS::IGraphicsKMS(IGEditorDelegate& dlg, int w, int h, int fps, float scale)
: IGRAPHICS_DRAW_CLASS(dlg, w, h, fps, scale)
{
}

IGraphicsKMS::~IGraphicsKMS()
{
  CloseWindow();
}

void* IGraphicsKMS::OpenWindow(void* pWindow)
{
  if (mWindowOpen)
    return GetWindow();

  const Config& cfg = Settings();
  mOffscreen = cfg.offscreen || cfg.surfaceless;
  mSurfaceless = cfg.surfaceless;

  if (!(mSurfaceless ? InitSurfaceless() : mOffscreen ? InitOffscreen() : InitKMS()))
  {
    ShutdownDisplay();
    return nullptr;
  }

  mWindowOpen = true;
  mVisible = !cfg.startHidden;
  mSuppressTouch = false;
  mScissorHW = cfg.hwScissor;
  OnViewInitialized(nullptr);   // nvgCreateGLES2 on the current context
  SetScreenScale(1.f);

  // F69: IGraphics letterboxes the panel in the surface (FitToContainer: uniform scale, centred, the bands drawn
  // by the letterbox draw function in the panel's frame buffer, which then covers the whole surface); the panel's
  // own layout is untouched. The matte stays the default band colour, and fills the surface around the panel
  // when fit is off (1:1, top left).
  SetLetterboxColor(cfg.matte);
  if (cfg.fit)
    FitToContainer(mSurfaceW, mSurfaceH, false);
  else
    Resize(Width(), Height(), 1.f, false);
  mFitScale = GetDrawScale(); // after Clip(min, max scale)
  mOffX = GetLetterboxOffsetX();
  mOffY = GetLetterboxOffsetY();
  const IRECT panel = GetLetterboxPanelBounds();
  const float panelW = std::round(panel.W() * mFitScale), panelH = std::round(panel.H() * mFitScale);
  SetPresentTarget(mSurfaceW, mSurfaceH, 0.f, 0.f, cfg.matte); // the frame buffer is the whole surface when letterboxed

  fprintf(stderr, "IGraphicsKMS: %s %dx%d, panel %dx%d at scale %.4f -> %.0fx%.0f at (%.0f, %.0f), %s%s\n",
          mSurfaceless ? "surfaceless" : mOffscreen ? "offscreen" : "kms", mSurfaceW, mSurfaceH, Width(), Height(), mFitScale,
          panelW, panelH, mOffX, mOffY, cfg.rgb565 ? "RGB565" : "XRGB8888", mVisible ? "" : ", hidden (the screen is left as it is)");
  fprintf(stderr, "IGraphicsKMS: GL_RENDERER %s | GL_VERSION %s\n",
          (const char*) glGetString(GL_RENDERER), (const char*) glGetString(GL_VERSION));

  GetDelegate()->LayoutUI(this);
  GetDelegate()->OnUIOpen();

  if (!cfg.touchDevice.empty())
    OpenTouch(cfg.touchDevice);

  return GetWindow();
}

void IGraphicsKMS::CloseWindow()
{
  if (!mWindowOpen)
    return;

  mWindowOpen = false;
  OnViewDestroyed();
  GetDelegate()->OnUIClose();

  if (mTouchFD >= 0)
  {
    close(mTouchFD);
    mTouchFD = -1;
  }
  ShutdownDisplay();
}

EMsgBoxResult IGraphicsKMS::ShowMessageBox(const char* str, const char* title, EMsgBoxType type, IMsgBoxCompletionHandlerFunc completionHandler)
{
  fprintf(stderr, "IGraphicsKMS message box: %s: %s\n", title ? title : "", str ? str : "");
  if (completionHandler)
    completionHandler(EMsgBoxResult::kOK);
  return EMsgBoxResult::kOK;
}

#pragma mark - display

bool IGraphicsKMS::InitKMS()
{
  const Config& cfg = Settings();
  mDrmFD = open(cfg.drmDevice.c_str(), O_RDWR | O_CLOEXEC);
  if (mDrmFD < 0)
  {
    fprintf(stderr, "IGraphicsKMS: open %s: %s\n", cfg.drmDevice.c_str(), strerror(errno));
    return false;
  }

  drmModeRes* res = drmModeGetResources(mDrmFD);
  if (!res)
  {
    fprintf(stderr, "IGraphicsKMS: drmModeGetResources failed (not a KMS device?)\n");
    return false;
  }

  drmModeConnector* conn = nullptr;
  for (int i = 0; i < res->count_connectors && !conn; i++)
  {
    drmModeConnector* c = drmModeGetConnector(mDrmFD, res->connectors[i]);
    if (c && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0)
      conn = c;
    else if (c)
      drmModeFreeConnector(c);
  }
  if (!conn)
  {
    fprintf(stderr, "IGraphicsKMS: no connected connector\n");
    drmModeFreeResources(res);
    return false;
  }

  int modeIdx = -1;
  for (int i = 0; i < conn->count_modes && modeIdx < 0; i++)
  {
    const drmModeModeInfo& m = conn->modes[i];
    if (cfg.modeW > 0 ? (m.hdisplay == cfg.modeW && m.vdisplay == cfg.modeH) : (m.type & DRM_MODE_TYPE_PREFERRED))
      modeIdx = i;
  }
  if (modeIdx < 0)
    modeIdx = 0;

  auto* mode = new drmModeModeInfo(conn->modes[modeIdx]);
  mMode = mode;
  mConnectorID = conn->connector_id;
  mSurfaceW = mode->hdisplay;
  mSurfaceH = mode->vdisplay;

  // CRTC: the encoder's current one, else the first the connector's encoders can drive.
  if (conn->encoder_id)
  {
    if (drmModeEncoder* enc = drmModeGetEncoder(mDrmFD, conn->encoder_id))
    {
      mCrtcID = enc->crtc_id;
      drmModeFreeEncoder(enc);
    }
  }
  for (int e = 0; e < conn->count_encoders && !mCrtcID; e++)
  {
    drmModeEncoder* enc = drmModeGetEncoder(mDrmFD, conn->encoders[e]);
    if (!enc) continue;
    for (int c = 0; c < res->count_crtcs; c++)
      if (enc->possible_crtcs & (1u << c)) { mCrtcID = res->crtcs[c]; break; }
    drmModeFreeEncoder(enc);
  }

  fprintf(stderr, "IGraphicsKMS: connector %u, mode %s %dx%d@%u, crtc %u\n", mConnectorID, mode->name,
          mode->hdisplay, mode->vdisplay, mode->vrefresh, mCrtcID);

  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  if (!mCrtcID)
  {
    fprintf(stderr, "IGraphicsKMS: no CRTC for the connector\n");
    return false;
  }

  mSavedCrtc = drmModeGetCrtc(mDrmFD, mCrtcID);

  const uint32_t fmt = cfg.rgb565 ? GBM_FORMAT_RGB565 : GBM_FORMAT_XRGB8888;
  mGBM = gbm_create_device(mDrmFD);
  if (!mGBM)
  {
    fprintf(stderr, "IGraphicsKMS: gbm_create_device failed\n");
    return false;
  }
  mGBMSurface = gbm_surface_create(mGBM, mSurfaceW, mSurfaceH, fmt, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!mGBMSurface)
  {
    fprintf(stderr, "IGraphicsKMS: gbm_surface_create failed\n");
    return false;
  }
  return InitEGL(mGBM, fmt);
}

bool IGraphicsKMS::InitOffscreen()
{
  const Config& cfg = Settings();
  mDrmFD = open(cfg.renderDevice.c_str(), O_RDWR | O_CLOEXEC);
  if (mDrmFD < 0)
  {
    fprintf(stderr, "IGraphicsKMS: open %s: %s\n", cfg.renderDevice.c_str(), strerror(errno));
    return false;
  }
  mSurfaceW = cfg.offscreenW;
  mSurfaceH = cfg.offscreenH;
  const uint32_t fmt = cfg.rgb565 ? GBM_FORMAT_RGB565 : GBM_FORMAT_XRGB8888;
  mGBM = gbm_create_device(mDrmFD);
  if (!mGBM)
    return false;
  mGBMSurface = gbm_surface_create(mGBM, mSurfaceW, mSurfaceH, fmt, GBM_BO_USE_RENDERING);
  if (!mGBMSurface)
  {
    fprintf(stderr, "IGraphicsKMS: gbm_surface_create (offscreen) failed\n");
    return false;
  }
  return InitEGL(mGBM, fmt);
}

bool IGraphicsKMS::InitSurfaceless()
{
  const Config& cfg = Settings();
  mSurfaceW = cfg.offscreenW;
  mSurfaceH = cfg.offscreenH;
  auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
  if (!getPlatformDisplay)
    return false;
  EGLDisplay dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
  {
    fprintf(stderr, "IGraphicsKMS: surfaceless EGL init failed (0x%x)\n", eglGetError());
    return false;
  }
  mEGLDisplay = dpy;
  fprintf(stderr, "IGraphicsKMS: EGL %d.%d surfaceless, %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint attribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_STENCIL_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };
  EGLConfig cfgEGL = nullptr;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, attribs, &cfgEGL, 1, &n) || n < 1)
  {
    fprintf(stderr, "IGraphicsKMS: no surfaceless pbuffer config\n");
    return false;
  }
  const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  mEGLContext = eglCreateContext(dpy, cfgEGL, EGL_NO_CONTEXT, ctxAttribs);
  const EGLint pbAttribs[] = { EGL_WIDTH, mSurfaceW, EGL_HEIGHT, mSurfaceH, EGL_NONE };
  mEGLSurface = eglCreatePbufferSurface(dpy, cfgEGL, pbAttribs);
  if (mEGLContext == EGL_NO_CONTEXT || mEGLSurface == EGL_NO_SURFACE || !eglMakeCurrent(dpy, mEGLSurface, mEGLSurface, mEGLContext))
  {
    fprintf(stderr, "IGraphicsKMS: surfaceless context / pbuffer failed (0x%x)\n", eglGetError());
    return false;
  }
  return true;
}

bool IGraphicsKMS::InitEGL(void* nativeDisplay, uint32_t gbmFormat)
{
  auto getPlatformDisplay = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
  EGLDisplay dpy = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_GBM_KHR, nativeDisplay, nullptr)
                                      : eglGetDisplay((EGLNativeDisplayType) nativeDisplay);
  EGLint major = 0, minor = 0;
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor))
  {
    fprintf(stderr, "IGraphicsKMS: EGL init failed (0x%x)\n", eglGetError());
    return false;
  }
  mEGLDisplay = dpy;
  fprintf(stderr, "IGraphicsKMS: EGL %d.%d, %s\n", major, minor, eglQueryString(dpy, EGL_VENDOR));
  eglBindAPI(EGL_OPENGL_ES_API);

  const bool is565 = gbmFormat == GBM_FORMAT_RGB565;
  const EGLint attribs[] = {
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RED_SIZE, is565 ? 5 : 8, EGL_GREEN_SIZE, is565 ? 6 : 8, EGL_BLUE_SIZE, is565 ? 5 : 8,
    EGL_ALPHA_SIZE, 0,
    EGL_STENCIL_SIZE, 8,   // NanoVG fills (stencil-then-cover)
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_NONE
  };
  EGLint n = 0;
  eglChooseConfig(dpy, attribs, nullptr, 0, &n);
  std::vector<EGLConfig> configs(std::max(n, 1));
  eglChooseConfig(dpy, attribs, configs.data(), n, &n);
  EGLConfig cfg = nullptr;
  for (int i = 0; i < n; i++)
  {
    EGLint id = 0;
    eglGetConfigAttrib(dpy, configs[i], EGL_NATIVE_VISUAL_ID, &id);
    if (static_cast<uint32_t>(id) == gbmFormat) { cfg = configs[i]; break; }
  }
  if (!cfg)
  {
    fprintf(stderr, "IGraphicsKMS: no EGL config for the GBM format (%d candidates)\n", n);
    return false;
  }

  const EGLint ctxAttribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttribs);
  if (ctx == EGL_NO_CONTEXT)
  {
    fprintf(stderr, "IGraphicsKMS: eglCreateContext failed (0x%x)\n", eglGetError());
    return false;
  }
  mEGLContext = ctx;
  EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType) mGBMSurface, nullptr);
  if (surf == EGL_NO_SURFACE)
  {
    fprintf(stderr, "IGraphicsKMS: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
    return false;
  }
  mEGLSurface = surf;
  if (!eglMakeCurrent(dpy, surf, surf, ctx))
  {
    fprintf(stderr, "IGraphicsKMS: eglMakeCurrent failed (0x%x)\n", eglGetError());
    return false;
  }
  const char* ext = eglQueryString(dpy, EGL_EXTENSIONS);
  mHasBufferAge = ext && strstr(ext, "EGL_EXT_buffer_age");
  fprintf(stderr, "IGraphicsKMS: EGL_EXT_buffer_age %s\n", mHasBufferAge ? "yes (partial present)" : "no (full present)");
  return true;
}

void IGraphicsKMS::ShutdownDisplay()
{
  if (mFlipPending && mDrmFD >= 0)
  {
    drmEventContext ev = {};
    ev.version = 2;
    ev.page_flip_handler = PageFlipHandler;
    pollfd pfd = { mDrmFD, POLLIN, 0 };
    if (poll(&pfd, 1, 100) > 0)
      drmHandleEvent(mDrmFD, &ev);
  }
  if (mPendingBO && mGBMSurface)
    gbm_surface_release_buffer(mGBMSurface, mPendingBO);
  mPendingBO = nullptr;

  if (mModeSet)
    RestoreSavedCrtc();
  if (mSavedCrtc)
  {
    drmModeFreeCrtc(static_cast<drmModeCrtc*>(mSavedCrtc));
    mSavedCrtc = nullptr;
  }
  mModeSet = false;

  if (mEGLDisplay)
  {
    eglMakeCurrent(mEGLDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (mEGLSurface) eglDestroySurface(mEGLDisplay, mEGLSurface);
    if (mEGLContext) eglDestroyContext(mEGLDisplay, mEGLContext);
    eglTerminate(mEGLDisplay);
  }
  mEGLDisplay = mEGLContext = mEGLSurface = nullptr;

  if (mFrontBO && mGBMSurface)
    gbm_surface_release_buffer(mGBMSurface, mFrontBO);
  mFrontBO = nullptr;
  if (mGBMSurface) gbm_surface_destroy(mGBMSurface);
  mGBMSurface = nullptr;
  if (mGBM) gbm_device_destroy(mGBM);
  mGBM = nullptr;
  delete static_cast<drmModeModeInfo*>(mMode);
  mMode = nullptr;
  if (mDrmFD >= 0) close(mDrmFD);
  mDrmFD = -1;
}

uint32_t IGraphicsKMS::FramebufferForBO(gbm_bo* bo)
{
  if (void* data = gbm_bo_get_user_data(bo))
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));

  uint32_t handles[4] = { gbm_bo_get_handle(bo).u32 };
  uint32_t pitches[4] = { gbm_bo_get_stride(bo) };
  uint32_t offsets[4] = { 0 };
  uint32_t fb = 0;
  if (drmModeAddFB2(mDrmFD, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo), handles, pitches, offsets, &fb, 0))
  {
    fprintf(stderr, "IGraphicsKMS: drmModeAddFB2 failed: %s\n", strerror(errno));
    return 0;
  }
  gbm_bo_set_user_data(bo, reinterpret_cast<void*>(static_cast<uintptr_t>(fb)), DestroyFBCallback);
  return fb;
}

bool IGraphicsKMS::RestoreSavedCrtc()
{
  auto* c = static_cast<drmModeCrtc*>(mSavedCrtc);
  if (!c || mDrmFD < 0)
    return false;
  if (!c->buffer_id)
  {
    fprintf(stderr, "IGraphicsKMS: the CRTC scanned out no frame buffer when the window opened; nothing to give it back to\n");
    return false;
  }
  // a legacy SetCrtc is a blocking commit: when it returns, the previous frame buffer is on screen
  // again (from the next vblank: no tearing) and ours is no longer scanned out
  if (drmModeSetCrtc(mDrmFD, c->crtc_id, c->buffer_id, c->x, c->y, &mConnectorID, 1, &c->mode))
  {
    fprintf(stderr, "IGraphicsKMS: giving the CRTC back failed: %s\n", strerror(errno));
    return false;
  }
  return true;
}

bool IGraphicsKMS::SetVisible(bool visible)
{
  if (!mWindowOpen || visible == mVisible)
    return true;
  bool ok = true;
  if (!visible)
  {
    ReleaseTouchInProgress();
    mVisible = false;
    if (!mOffscreen && mModeSet)
    {
      WaitForFlip();
      ok = RestoreSavedCrtc();
      mModeSet = false;
      // nothing scans our buffers out any more: all of them go back to the surface
      if (mFrontBO && mGBMSurface)
        gbm_surface_release_buffer(mGBMSurface, mFrontBO);
      mFrontBO = nullptr;
    }
    return ok;
  }
  mVisible = true;
  ReleaseMouseCapture(); // nothing from before the panel was hidden still holds the pointer
  DrainTouch();
  // the next frame redraws the whole panel (the controls kept their values while hidden) and, kms, takes
  // the CRTC with drmModeSetCrtc
  mNextFrameAt = 0.;
  mPresentHistoryN = 0;
  SetAllControlsDirty();
  return ok;
}

void IGraphicsKMS::Prewarm()
{
  if (!mWindowOpen)
    return;
  SetAllControlsDirty();
  IRECTList rects;
  IsDirty(rects);
  SetAllControlsClean();
  SetPresentRegion(IRECT());
  Draw(rects);
  glFinish();
  // what was drawn went to the window surface's back buffer, never presented; the next shown frame is a
  // full one either way
  SetAllControlsDirty();
}

void IGraphicsKMS::DrainTouch()
{
  if (mTouchFD < 0)
    return;
  input_event ev[64];
  while (read(mTouchFD, ev, sizeof(ev)) > 0) {}
  // where the finger is now: a touch that began while hidden is not a press on this panel
  unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
  const bool down = ioctl(mTouchFD, EVIOCGKEY(sizeof(keys)), keys) >= 0 && (TestBit(keys, BTN_TOUCH) || TestBit(keys, BTN_LEFT));
  input_absinfo ax = {}, ay = {};
  if (ioctl(mTouchFD, EVIOCGABS(mTouchMT ? ABS_MT_POSITION_X : ABS_X), &ax) >= 0) mRawX = ax.value;
  if (ioctl(mTouchFD, EVIOCGABS(mTouchMT ? ABS_MT_POSITION_Y : ABS_Y), &ay) >= 0) mRawY = ay.value;
  mTouchDown = mTouchWasDown = down;
  mTouchMoved = false;
  mSuppressTouch = down;
}

void IGraphicsKMS::ReleaseTouchInProgress()
{
  // A control still captured has had its press and not its release: the finger is down (a knob being
  // turned when the owner hid the panel) or its lift will come while hidden and be drained. The usual
  // case is the button whose press hid the panel (it acts on mouse down): without a release here it stays
  // captured, and IGraphics::GetMouseControl() hands every later touch to it, so after one round trip
  // every tap on the panel pressed that button again. The release goes to the control as a lift at the
  // last position (a drag ends, the host gets its end-of-gesture); then nothing is captured.
  if (ControlIsCaptured())
  {
    IMouseInfo info;
    if (mTouchWasDown)
    {
      info.x = mLastX;
      info.y = mLastY;
    }
    else
    {
      GetMouseDownPoint(info.x, info.y); // UI coordinates
      UIToView(info.x, info.y);
    }
    info.ms = IMouseMod(false);
    OnMouseUp({info});
    if (Settings().logInput)
      fprintf(stderr, "touch up   (released on hide) -> view (%.1f, %.1f)\n", info.x, info.y);
  }
  ReleaseMouseCapture();
  mTouchWasDown = mTouchDown = false;
  mTouchMoved = false;
}

void IGraphicsKMS::OnPanelFlushed()
{
  if (!Settings().gpuTiming)
    return;
  const double t = NowSecs();
  glFinish();
  const double dt = NowSecs() - t;
  mStats.panelGpuSecs += dt;
  mFrameGpuWait += dt;
}

void IGraphicsKMS::WaitForFlip()
{
  if (!mFlipPending || mDrmFD < 0)
    return;
  const double tw = NowSecs();
  drmEventContext ev = {};
  ev.version = 2;
  ev.page_flip_handler = PageFlipHandler;
  while (mFlipPending)
  {
    pollfd pfd = { mDrmFD, POLLIN, 0 };
    if (poll(&pfd, 1, 1000) <= 0)
    {
      fprintf(stderr, "IGraphicsKMS: no page-flip event in 1 s\n");
      mFlipPending = false;
      break;
    }
    drmHandleEvent(mDrmFD, &ev);
  }
  mStats.flipWaitSecs += NowSecs() - tw;
  // the queued buffer is on screen now: the one it replaced goes back to the surface
  if (mPendingBO)
  {
    if (mFrontBO)
      gbm_surface_release_buffer(mGBMSurface, mFrontBO);
    mFrontBO = mPendingBO;
    mPendingBO = nullptr;
  }
}

double IGraphicsKMS::SecondsToNextFrame() const
{
  if (Settings().maxFps <= 0.)
    return 0.;
  return std::max(0., mNextFrameAt - NowSecs());
}

bool IGraphicsKMS::Present()
{
  double t0 = NowSecs();
  if (mSurfaceless)
  {
    glFinish();
    mStats.swapSecs += NowSecs() - t0;
    return true;
  }
  if (Settings().gpuTiming)
  {
    glFinish(); // the present's GPU work (matte + composite) done before the swap
    const double dt = NowSecs() - t0;
    mStats.presentGpuSecs += dt;
    t0 += dt;
  }
  eglSwapBuffers(mEGLDisplay, mEGLSurface);
  if (mOffscreen)
    glFinish(); // no scanout to pace on: count the GPU's time in the frame
  gbm_bo* bo = gbm_surface_lock_front_buffer(mGBMSurface);
  mStats.swapSecs += NowSecs() - t0;
  if (!bo)
  {
    fprintf(stderr, "IGraphicsKMS: gbm_surface_lock_front_buffer failed\n");
    return false;
  }

  if (mOffscreen)
  {
    if (mFrontBO) gbm_surface_release_buffer(mGBMSurface, mFrontBO);
    mFrontBO = bo;
    return true;
  }

  const uint32_t fb = FramebufferForBO(bo);
  if (!fb)
    return false;

  if (!mModeSet)
  {
    if (drmModeSetCrtc(mDrmFD, mCrtcID, fb, 0, 0, &mConnectorID, 1, static_cast<drmModeModeInfo*>(mMode)))
    {
      fprintf(stderr, "IGraphicsKMS: drmModeSetCrtc failed: %s (is another process DRM master?)\n", strerror(errno));
      gbm_surface_release_buffer(mGBMSurface, bo);
      return false;
    }
    mModeSet = true;
  }
  else
  {
    // One flip may be queued at a time: wait for the previous frame's (with overlap this is
    // the only wait, and the CPU work of this frame has already run beside it).
    WaitForFlip();
    mFlipPending = true;
    if (drmModePageFlip(mDrmFD, mCrtcID, fb, DRM_MODE_PAGE_FLIP_EVENT, &mFlipPending))
    {
      fprintf(stderr, "IGraphicsKMS: drmModePageFlip failed: %s\n", strerror(errno));
      mFlipPending = false;
      gbm_surface_release_buffer(mGBMSurface, bo);
      return false;
    }
    // The kernel flips once the GPU has finished the buffer (implicit fence) and the next
    // vblank comes; the buffer goes to mFrontBO in WaitForFlip().
    mPendingBO = bo;
    if (!Settings().overlap)
      WaitForFlip();
    return true;
  }

  if (mFrontBO)
    gbm_surface_release_buffer(mGBMSurface, mFrontBO);
  mFrontBO = bo;
  return true;
}

int IGraphicsKMS::RenderFrame(bool forceAll)
{
  if (!mWindowOpen)
    return -1;
  if (!mVisible)
    return 0; // hidden: nothing drawn, what is dirty stays dirty for the frame that shows the panel again

  const double t0 = NowSecs();
  const Config& cfg = Settings();
  if (cfg.maxFps > 0. && t0 < mNextFrameAt)
  {
    // not due yet: what is dirty stays dirty for the frame that is (frames are dropped, never queued)
    mStats.framesDeferred++;
    return 0;
  }
  if (forceAll)
    SetAllControlsDirty();

  IRECTList rects;
  if (!IsDirty(rects))
  {
    mStats.framesSkipped++;
    return 0;
  }
  if (cfg.maxFps > 0.)
    mNextFrameAt = t0 + 0.95 / cfg.maxFps; // 5 % early: the flip lands on the vblank either way
  {
    IRECTList merged; // what Draw() will draw: aligned and merged
    for (int i = 0; i < rects.Size(); i++)
      merged.Add(rects.Get(i));
    merged.PixelAlign(GetBackingPixelScale());
    merged.Optimize();
    mStats.dirtyRects += merged.Size();
    for (int i = 0; i < merged.Size(); i++)
      mStats.dirtyArea += merged.Get(i).W() * merged.Get(i).H();
  }
  SetAllControlsClean();
  mFrameGpuWait = 0.;
  const IRECT region = PresentRegionFor(rects);
  SetPresentRegion(region);
  mStats.presentArea += region.W() > 0.f ? region.W() * region.H() : static_cast<double>(mSurfaceW) * mSurfaceH;
  Draw(rects);
  mStats.drawSecs += NowSecs() - t0 - mFrameGpuWait;

  const bool ok = Present();
  const double dt = NowSecs() - t0;
  mStats.maxFrameSecs = std::max(mStats.maxFrameSecs, dt);
  if (!ok)
    return -1;
  mStats.framesPresented++;
  return 1;
}

IRECT IGraphicsKMS::PresentRegionFor(const IRECTList& panelRects)
{
  // this frame's dirty rects, panel units -> surface pixels, grown to whole pixels plus one
  IRECT r;
  const float s = GetDrawScale() * GetScreenScale();
  for (int i = 0; i < panelRects.Size(); i++)
  {
    const IRECT& p = panelRects.Get(i);
    const IRECT q(std::floor(mOffX + p.L * s) - 1.f, std::floor(mOffY + p.T * s) - 1.f, std::ceil(mOffX + p.R * s) + 1.f, std::ceil(mOffY + p.B * s) + 1.f);
    r = r.Empty() ? q : r.Union(q);
  }
  r = r.Intersect(IRECT(0.f, 0.f, static_cast<float>(mSurfaceW), static_cast<float>(mSurfaceH)));

  // what the back buffer lacks: the regions of the frames presented since it was last on screen
  int age = 0;
  if (!Settings().partialPresent)
    age = 0;
  else if (mSurfaceless)
    age = 1; // a pbuffer is never swapped: it always holds the last frame
  else if (mHasBufferAge)
  {
    EGLint a = 0;
    if (eglQuerySurface(mEGLDisplay, mEGLSurface, EGL_BUFFER_AGE_EXT, &a))
      age = a;
  }
  mStats.bufferAge[std::min(std::max(age, 0), 3)]++;
  IRECT region = r;
  bool full = age <= 0 || age - 1 > mPresentHistoryN || r.Empty();
  for (int i = 0; i < age - 1 && !full; i++)
  {
    if (mPresentHistory[i].Empty())
      full = true;
    else
      region = region.Union(mPresentHistory[i]);
  }
  // a large region costs more than a full present: a full clear lets the GPU skip loading the
  // old contents (vc4: no tile loads), a scissored one does not
  if (!full && region.W() * region.H() > 0.5f * mSurfaceW * mSurfaceH)
    full = true;
  if (full)
    region = IRECT();

  // remember what this frame changes, for the buffers that come back later (a full present
  // changes no more than that: the rest of the surface was already the current picture)
  for (int i = kPresentHistory - 1; i > 0; i--)
    mPresentHistory[i] = mPresentHistory[i - 1];
  mPresentHistory[0] = r;
  mPresentHistoryN = std::min(mPresentHistoryN + 1, kPresentHistory);
  return region;
}

bool IGraphicsKMS::SaveScreenshot(const char* path)
{
  // The back buffer is undefined after a swap: draw everything again and read it before presenting.
  SetAllControlsDirty();
  IRECTList rects;
  IsDirty(rects);
  SetAllControlsClean();
  SetPresentRegion(IRECT());
  for (int i = kPresentHistory - 1; i > 0; i--)
    mPresentHistory[i] = mPresentHistory[i - 1];
  mPresentHistory[0] = IRECT(0.f, 0.f, static_cast<float>(mSurfaceW), static_cast<float>(mSurfaceH));
  mPresentHistoryN = std::min(mPresentHistoryN + 1, kPresentHistory);
  Draw(rects);

  std::vector<uint8_t> px(static_cast<size_t>(mSurfaceW) * mSurfaceH * 4);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, mSurfaceW, mSurfaceH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  const size_t row = static_cast<size_t>(mSurfaceW) * 4;
  std::vector<uint8_t> flipped(px.size());
  for (int y = 0; y < mSurfaceH; y++)
    memcpy(&flipped[y * row], &px[(mSurfaceH - 1 - y) * row], row);
  for (size_t i = 3; i < flipped.size(); i += 4)
    flipped[i] = 255;
  const bool ok = stbi_write_png(path, mSurfaceW, mSurfaceH, 4, flipped.data(), static_cast<int>(row)) != 0;
  if (mVisible)
    Present(); // hidden: read back only, the screen stays its previous owner's
  return ok;
}

bool IGraphicsKMS::ReadSurface(std::vector<uint8_t>& rgba, int& w, int& h)
{
  w = mSurfaceW;
  h = mSurfaceH;
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  GLint prev = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindFramebuffer(GL_FRAMEBUFFER, prev);
  const size_t row = static_cast<size_t>(w) * 4;
  rgba.resize(px.size());
  for (int y = 0; y < h; y++)
    memcpy(&rgba[y * row], &px[(h - 1 - y) * row], row);
  return true;
}

bool IGraphicsKMS::ReadPanel(std::vector<uint8_t>& rgba, int& w, int& h)
{
  NVGframebuffer* pFB = GetMainFrameBuffer();
  if (!pFB)
    return false;
  w = static_cast<int>(WindowWidth() * GetScreenScale());
  h = static_cast<int>(WindowHeight() * GetScreenScale());
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  GLint prev = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev);
  glBindFramebuffer(GL_FRAMEBUFFER, pFB->fbo);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  glBindFramebuffer(GL_FRAMEBUFFER, prev);
  // GL row 0 is the panel's bottom row
  const size_t row = static_cast<size_t>(w) * 4;
  rgba.resize(px.size());
  for (int y = 0; y < h; y++)
    memcpy(&rgba[y * row], &px[(h - 1 - y) * row], row);
  return true;
}

#pragma mark - touch

bool IGraphicsKMS::OpenTouch(const std::string& wanted)
{
  std::vector<std::string> candidates;
  if (wanted == "auto")
  {
    if (DIR* d = opendir("/dev/input"))
    {
      while (dirent* e = readdir(d))
        if (!strncmp(e->d_name, "event", 5))
          candidates.push_back(std::string("/dev/input/") + e->d_name);
      closedir(d);
    }
    std::sort(candidates.begin(), candidates.end());
  }
  else
    candidates.push_back(wanted);

  for (const auto& path : candidates)
  {
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
      if (wanted != "auto") fprintf(stderr, "IGraphicsKMS: open %s: %s\n", path.c_str(), strerror(errno));
      continue;
    }
    unsigned long props[1] = {0}, absBits[(ABS_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {0};
    ioctl(fd, EVIOCGPROP(sizeof(props)), props);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);
    const bool direct = TestBit(props, INPUT_PROP_DIRECT);
    const bool hasXY = TestBit(absBits, ABS_X) && TestBit(absBits, ABS_Y);
    const bool hasMT = TestBit(absBits, ABS_MT_POSITION_X) && TestBit(absBits, ABS_MT_POSITION_Y);
    if (!(hasXY || hasMT) || (wanted == "auto" && !direct))
    {
      close(fd);
      continue;
    }
    char name[256] = "?";
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    mTouchMT = !hasXY;
    input_absinfo ax = {}, ay = {};
    ioctl(fd, EVIOCGABS(mTouchMT ? ABS_MT_POSITION_X : ABS_X), &ax);
    ioctl(fd, EVIOCGABS(mTouchMT ? ABS_MT_POSITION_Y : ABS_Y), &ay);
    mAbsMinX = ax.minimum; mAbsMaxX = ax.maximum; mAbsMinY = ay.minimum; mAbsMaxY = ay.maximum;
    mRawX = ax.value; mRawY = ay.value; // the kernel drops a repeated value, so start from the current one
    mTouchName = name;
    mTouchFD = fd;
    input_absinfo mtx = {}, mty = {}, slot = {};
    if (hasMT)
    {
      ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &mtx);
      ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &mty);
      ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot);
    }
    fprintf(stderr, "IGraphicsKMS: touch %s \"%s\" direct=%d, %s X %d..%d Y %d..%d (res %d/%d)%s",
            path.c_str(), name, direct, mTouchMT ? "ABS_MT_POSITION" : "ABS", mAbsMinX, mAbsMaxX, mAbsMinY, mAbsMaxY,
            ax.resolution, ay.resolution, hasMT ? "" : "\n");
    if (hasMT)
      fprintf(stderr, "; MT X %d..%d Y %d..%d, slots %d\n", mtx.minimum, mtx.maximum, mty.minimum, mty.maximum, slot.maximum + 1);
    return true;
  }
  fprintf(stderr, "IGraphicsKMS: no touch device (%s)\n", wanted.c_str());
  return false;
}

void IGraphicsKMS::PollInput()
{
  if (mTouchFD < 0)
    return;
  if (!mVisible)
  {
    DrainTouch(); // the panel is not on screen: its touches are someone else's
    return;
  }
  input_event ev[64];
  for (;;)
  {
    const ssize_t n = read(mTouchFD, ev, sizeof(ev));
    if (n <= 0)
      break;
    for (size_t i = 0; i < n / sizeof(input_event); i++)
    {
      const input_event& e = ev[i];
      if (e.type == EV_ABS)
      {
        if (e.code == (mTouchMT ? ABS_MT_POSITION_X : ABS_X)) { mRawX = e.value; mTouchMoved = true; }
        else if (e.code == (mTouchMT ? ABS_MT_POSITION_Y : ABS_Y)) { mRawY = e.value; mTouchMoved = true; }
        else if (mTouchMT && e.code == ABS_MT_TRACKING_ID) mTouchDown = e.value >= 0;
      }
      else if (e.type == EV_KEY && (e.code == BTN_TOUCH || e.code == BTN_LEFT))
        mTouchDown = e.value != 0;
      else if (e.type == EV_SYN && e.code == SYN_REPORT)
        DispatchTouch();
    }
  }
}

void IGraphicsKMS::DispatchTouch()
{
  const Config& cfg = Settings();
  float nx = (mRawX - mAbsMinX) / static_cast<float>(mAbsMaxX - mAbsMinX + 1);
  float ny = (mRawY - mAbsMinY) / static_cast<float>(mAbsMaxY - mAbsMinY + 1);
  if (cfg.touchSwapXY) std::swap(nx, ny);
  if (cfg.touchInvertX) nx = 1.f - nx;
  if (cfg.touchInvertY) ny = 1.f - ny;
  const float sx = nx * mSurfaceW, sy = ny * mSurfaceH;
  // view units (surface pixels / draw scale): IGraphics takes the letterbox offset off (ViewToUI)
  const float x = sx / mFitScale, y = sy / mFitScale;

  if (mSuppressTouch)
  {
    // a finger that was already down when the panel was shown (it pressed something on the screen that
    // was there before): nothing until it lifts
    if (!mTouchDown)
      mSuppressTouch = false;
    mTouchWasDown = mTouchDown;
    mTouchMoved = false;
    mLastX = x;
    mLastY = y;
    return;
  }

  IMouseInfo info;
  info.x = x;
  info.y = y;
  info.ms = IMouseMod(true);
  const char* what = nullptr;

  if (mTouchDown && !mTouchWasDown)
  {
    what = "down";
    OnMouseDown({info});
  }
  else if (mTouchDown && mTouchWasDown && mTouchMoved)
  {
    what = "drag";
    info.dX = x - mLastX;
    info.dY = y - mLastY;
    OnMouseDrag({info});
  }
  else if (!mTouchDown && mTouchWasDown)
  {
    what = "up";
    info.ms.L = false;
    OnMouseUp({info});
  }

  if (what && cfg.logInput)
  {
    float ux = x, uy = y;
    ViewToUI(ux, uy);
    fprintf(stderr, "touch %-4s raw (%d, %d) -> surface (%.1f, %.1f) -> ui (%.1f, %.1f)%s\n", what, mRawX, mRawY, sx, sy, ux, uy,
            (ux < 0 || uy < 0 || ux > Width() || uy > Height()) ? " [matte]" : "");
  }

  mLastX = x;
  mLastY = y;
  mTouchWasDown = mTouchDown;
  mTouchMoved = false;
}

void IGraphicsKMS::SurfaceToRawTouch(float sx, float sy, int& rawX, int& rawY) const
{
  const Config& cfg = Settings();
  float nx = (sx + 0.5f) / mSurfaceW, ny = (sy + 0.5f) / mSurfaceH;
  if (cfg.touchInvertX) nx = 1.f - nx;
  if (cfg.touchInvertY) ny = 1.f - ny;
  if (cfg.touchSwapXY) std::swap(nx, ny);
  rawX = mAbsMinX + static_cast<int>(nx * (mAbsMaxX - mAbsMinX + 1));
  rawY = mAbsMinY + static_cast<int>(ny * (mAbsMaxY - mAbsMinY + 1));
}

bool IGraphicsKMS::UIToRawTouch(float x, float y, int& rawX, int& rawY) const
{
  if (mTouchFD < 0)
    return false;
  SurfaceToRawTouch(mOffX + x * mFitScale, mOffY + y * mFitScale, rawX, rawY);
  return true;
}

#pragma mark - fonts

PlatformFontPtr IGraphicsKMS::LoadPlatformFont(const char* fontID, const char* fileNameOrResID)
{
  WDL_String fullPath;
  if (LocateResource(fileNameOrResID, "ttf", fullPath, GetBundleID(), GetWinModuleHandle(), GetSharedResourcesSubPath()) == EResourceLocation::kNotFound)
  {
    fprintf(stderr, "IGraphicsKMS: font %s not found\n", fileNameOrResID);
    return nullptr;
  }
  FILE* f = fopen(fullPath.Get(), "rb");
  if (!f)
    return nullptr;
  fseek(f, 0, SEEK_END);
  const long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(size);
  const bool ok = fread(data.data(), 1, size, f) == static_cast<size_t>(size);
  fclose(f);
  return ok ? LoadPlatformFont(fontID, data.data(), static_cast<int>(size)) : nullptr;
}

PlatformFontPtr IGraphicsKMS::LoadPlatformFont(const char* fontID, const char* fontName, ETextStyle style)
{
  return nullptr; // no fontconfig on the appliance: fonts ship as files
}

PlatformFontPtr IGraphicsKMS::LoadPlatformFont(const char* fontID, void* pData, int dataSize)
{
  return PlatformFontPtr(new KMSFont(pData, dataSize, 0));
}
