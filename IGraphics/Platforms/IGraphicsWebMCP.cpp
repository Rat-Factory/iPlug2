/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

/**
 * @file
 * @brief Emscripten exports for the WebMCP agent bridge. Thin extern "C"
 * wrappers over IGraphicsIntrospect, addressed by the IGraphics pointer that
 * the web component finds on canvas._iplugGraphics. Compiled only when
 * IPLUG_WEBMCP is defined (the default for Wasm builds, see WASMUI.cmake).
 *
 * String-returning functions hand back a pointer into one static buffer:
 * JavaScript must copy it (UTF8ToString) before the next bridge call.
 */

#if defined(IPLUG_WEBMCP)

#include <algorithm>
#include <string>
#include <vector>

#include <emscripten.h>

#include "IGraphicsWeb.h"
#include "IGraphicsIntrospect.h"
#include "IGraphicsJSON.h"

using namespace iplug;
using namespace igraphics;

extern std::vector<IGraphicsWeb*> gGraphicsInstances;

// Canvas event entry points defined in IGraphicsWeb.cpp (CSS pixel coordinates)
extern "C" void iGraphicsMouseCallback(void* pGraphics, int eventType, double x, double y, double dx, double dy, int buttons, int button, int shift, int ctrl, int alt);
extern "C" void iGraphicsWheelCallback(void* pGraphics, double x, double y, double deltaY, int shift, int ctrl, int alt);

static std::string sWebMCPJson;
static std::string sWebMCPError;

static IGraphicsWeb* ResolveGraphics(void* pGraphics)
{
  if (!pGraphics)
  {
    sWebMCPError = "Null IGraphics pointer.";
    return nullptr;
  }

  auto* pG = static_cast<IGraphicsWeb*>(pGraphics);

  if (std::find(gGraphicsInstances.begin(), gGraphicsInstances.end(), pG) == gGraphicsInstances.end())
  {
    sWebMCPError = "IGraphics instance is no longer registered.";
    return nullptr;
  }

  return pG;
}

static int Result(bool ok, const char* err)
{
  if (!ok)
    sWebMCPError = err;

  return ok ? 1 : 0;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_version()
{
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_num_graphics()
{
  return static_cast<int>(gGraphicsInstances.size());
}

EMSCRIPTEN_KEEPALIVE
void* iplug_webmcp_get_graphics(int i)
{
  if (i < 0 || i >= static_cast<int>(gGraphicsInstances.size()))
    return nullptr;

  return gGraphicsInstances[i];
}

EMSCRIPTEN_KEEPALIVE
const char* iplug_webmcp_last_error()
{
  return sWebMCPError.c_str();
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_live_edit_available()
{
  return LiveEditCompiledIn() ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
const char* iplug_webmcp_get_ui_tree(void* pGraphics)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  sWebMCPJson.clear();

  if (!pG)
    return "null";

  DescribeUI(*pG, sWebMCPJson);
  return sWebMCPJson.c_str();
}

EMSCRIPTEN_KEEPALIVE
const char* iplug_webmcp_get_control(void* pGraphics, int idx)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  sWebMCPJson.clear();

  if (!pG)
    return "null";

  DescribeControl(*pG, idx, sWebMCPJson);
  return sWebMCPJson.c_str();
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_value(void* pGraphics, int idx, int valIdx, double normalized, int gesture)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  return pG && Result(SetControlValue(*pG, idx, valIdx, normalized, gesture != 0), "Could not set control value.");
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_default(void* pGraphics, int idx, int valIdx)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  return pG && Result(SetControlValueToDefault(*pG, idx, valIdx), "Could not reset control value.");
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_hidden(void* pGraphics, int idx, int hide)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  return pG && Result(SetControlHidden(*pG, idx, hide != 0), "Unknown control index.");
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_disabled(void* pGraphics, int idx, int disable)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  return pG && Result(SetControlDisabled(*pG, idx, disable != 0), "Unknown control index.");
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_text(void* pGraphics, int idx, const char* str)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  return pG && Result(SetControlText(*pG, idx, str), "Control has no text or label.");
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_prop(void* pGraphics, int idx, const char* key, const char* value)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);

  if (!pG)
    return 0;

  std::string err;

  if (!SetControlProperty(*pG, idx, key, value, err))
  {
    sWebMCPError = err;
    return 0;
  }

  return 1;
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_control_bounds(void* pGraphics, int idx, float l, float t, float r, float b,
                                    float tl, float tt, float tr, float tb, int overrideTarget)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);

  if (!pG)
    return 0;

  const IRECT bounds(l, t, r, b);
  const IRECT target(tl, tt, tr, tb);
  return Result(SetControlBounds(*pG, idx, bounds, overrideTarget ? &target : nullptr), "Bounds must be finite and non-empty, and the control index valid.");
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_set_background_color(void* pGraphics, const char* hex)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);
  IColor color;

  if (!pG)
    return 0;

  if (!ParseHexColor(hex, color))
    return Result(false, "Expected #RRGGBB or #RRGGBBAA.");

  return Result(SetBackgroundColor(*pG, color), "Background is not a solid IPanelControl.");
}

/** Pointer input in LOGICAL IGraphics units (the canvas listeners use CSS pixels).
 * eventType: 0 down, 1 up, 2 move, 3 enter, 4 leave (as iGraphicsMouseCallback) */
EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_mouse(void* pGraphics, int eventType, double lx, double ly, double ldx, double ldy,
                       int buttons, int button, int shift, int ctrl, int alt)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);

  if (!pG || eventType < 0 || eventType > 4)
    return Result(false, "Invalid graphics pointer or event type.");

  const double scale = pG->GetDrawScale();
  iGraphicsMouseCallback(pG, eventType, lx * scale, ly * scale, ldx, ldy, buttons, button, shift, ctrl, alt);
  return 1;
}

EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_wheel(void* pGraphics, double lx, double ly, double deltaY, int shift, int ctrl, int alt)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);

  if (!pG)
    return 0;

  const double scale = pG->GetDrawScale();
  iGraphicsWheelCallback(pG, lx * scale, ly * scale, deltaY, shift, ctrl, alt);
  return 1;
}

/** Key input, dispatched at the last known pointer position. vk is an iPlug kVK_ virtual key code. */
EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_key(void* pGraphics, int vk, const char* utf8, int shift, int ctrl, int alt, int down)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);

  if (!pG)
    return 0;

  char safe[5] = { 0 };

  if (utf8)
  {
    for (int i = 0; i < 4 && utf8[i]; i++)
      safe[i] = utf8[i];
  }

  const IKeyPress key(safe, vk, shift != 0, ctrl != 0, alt != 0);
  return DispatchKey(*pG, static_cast<float>(pG->mPrevX), static_cast<float>(pG->mPrevY), key, down != 0) ? 1 : 0;
}

/** Synchronously draw any dirty controls (as the main loop timer would), so a screenshot taken
 * straight after a mutation reflects it. @return 1 if anything was drawn */
EMSCRIPTEN_KEEPALIVE
int iplug_webmcp_flush_draw(void* pGraphics)
{
  IGraphicsWeb* pG = ResolveGraphics(pGraphics);

  if (!pG)
    return 0;

  IRECTList rects;

  if (pG->IsDirty(rects))
  {
    pG->SetAllControlsClean();
    pG->Draw(rects);
    return 1;
  }

  return 0;
}

} // extern "C"

#endif // IPLUG_WEBMCP
