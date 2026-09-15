/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#pragma once

/**
 * @file
 * @brief Platform-independent introspection and programmatic manipulation of
 * an IGraphics control stack, with JSON in/out. This is the core behind the
 * WebMCP agent bridge (IGraphics/Platforms/IGraphicsWebMCP.cpp) and is written
 * so that a native transport can reuse it later. Controls are addressed by
 * their index in the control stack; stable identity should use the control
 * tag which every descriptor reports.
 */

#include <cstdlib>
#include <cstring>
#include <string>
#include <typeinfo>

#include "IGraphics.h"
#include "IControl.h"

#if defined(IPLUG_LIVE_EDIT_CLASS_NAME) || (defined(IPLUG_WEBMCP) && !defined(IPLUG_WEBMCP_NO_CLASS_NAME))
  #define IPLUG_INTROSPECT_CLASS_NAME 1
#endif

#if defined(IPLUG_INTROSPECT_CLASS_NAME) && defined(__GNUG__) && !defined(_WIN32)
  #include <cxxabi.h>
#endif

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

/** @return the demangled class name of a control (minus the iplug::igraphics:: prefix),
 * or an empty string when class names are compiled out (IPLUG_INTROSPECT_CLASS_NAME undefined).
 * Header-only so that IGraphicsLiveEdit.h can use it in native debug builds that do not compile IGraphicsIntrospect.cpp. */
static inline std::string GetControlClassName(IControl* pControl)
{
#if defined(IPLUG_INTROSPECT_CLASS_NAME)
  if (!pControl)
    return "";

  const char* mangled = typeid(*pControl).name();
#if defined(__GNUG__) && !defined(_WIN32)
  int status = 0;
  char* demangled = abi::__cxa_demangle(mangled, nullptr, nullptr, &status);
  const char* name = (status == 0 && demangled) ? demangled : (mangled ? mangled : "");
#else
  const char* name = mangled ? mangled : "";
#endif
  static const char prefix[] = "iplug::igraphics::";
  const std::size_t prefixLen = sizeof(prefix) - 1;
  const char* compact = std::strncmp(name, prefix, prefixLen) == 0 ? name + prefixLen : name;
  std::string result(compact);

#if defined(__GNUG__) && !defined(_WIN32)
  if (demangled)
    std::free(demangled);
#endif

  return result;
#else
  (void) pControl;
  return "";
#endif
}

/** Append a JSON descriptor for one control (idx, tag, className, group, parentIdx, tooltip, hidden,
 * disabled, bounds, targetBounds, vals[], plus text/label/style/pattern for the control types that have them) */
void DescribeControl(IGraphics& g, int idx, std::string& out);

/** Append a JSON description of the whole UI: dimensions, scales, live edit/text entry state,
 * background colour (solid IPanelControl only, else null) and every control (see DescribeControl) */
void DescribeUI(IGraphics& g, std::string& out);

/** @return \c true if the live edit overlay is compiled into this build */
bool LiveEditCompiledIn();

/** Set a control value through the user-input path (SetValueFromUserInput), so action functions,
 * peers and the delegate all see it as a user gesture.
 * @param gesture If true, and the value is bound to a parameter, wrap the change in
 * Begin/EndInformHostOfParamChangeFromUI (relevant for native hosts; a no-op in WASM) */
bool SetControlValue(IGraphics& g, int idx, int valIdx, double normalized, bool gesture);

/** Reset a control value (or all of its values when valIdx is kNoValIdx) to the parameter default */
bool SetControlValueToDefault(IGraphics& g, int idx, int valIdx);

bool SetControlHidden(IGraphics& g, int idx, bool hide);
bool SetControlDisabled(IGraphics& g, int idx, bool disable);

/** Set the string of an ITextControl, or the label of an IVectorBase control. @return \c false if neither */
bool SetControlText(IGraphics& g, int idx, const char* str);

/** Set a named property. Keys: label, text, hidden, disabled, showLabel, showValue, drawFrame, drawShadows,
 * emboss, roundness, frameThickness, shadowOffset, widgetFrac, angle, color.<bg|fg|pr|fr|hl|sh|x1|x2|x3>, pattern.
 * Values are strings and parsed per key (bools accept true/false/1/0, colours are #RRGGBB[AA]).
 * @param err Receives a reason on failure */
bool SetControlProperty(IGraphics& g, int idx, const char* key, const char* value, std::string& err);

/** Move/resize a control. Draw and target rects are both set to r (running the control's OnResize) unless
 * pTarget is supplied, in which case the target (hit-test) rect is overridden afterwards. */
bool SetControlBounds(IGraphics& g, int idx, const IRECT& r, const IRECT* pTarget);

/** Set a solid colour on the background control (index 0), which must be a solid IPanelControl */
bool SetBackgroundColor(IGraphics& g, const IColor& color);

/** Dispatch a key press/release to the UI at the given logical position */
bool DispatchKey(IGraphics& g, float x, float y, const IKeyPress& key, bool down);

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
