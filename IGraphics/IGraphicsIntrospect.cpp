/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#include "IGraphicsIntrospect.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "IGraphicsJSON.h"

using namespace iplug;
using namespace igraphics;

bool igraphics::LiveEditCompiledIn()
{
#if !defined(NDEBUG) || defined(IPLUG_LIVE_EDIT)
  return true;
#else
  return false;
#endif
}

static IControl* ControlAt(IGraphics& g, int idx)
{
  if (idx < 0 || idx >= g.NControls())
    return nullptr;

  return g.GetControl(idx);
}

static IPanelControl* SolidBackground(IGraphics& g)
{
  if (!g.NControls())
    return nullptr;

  auto* pPanel = dynamic_cast<IPanelControl*>(g.GetBackgroundControl());

  if (!pPanel || pPanel->GetPattern().mType != EPatternType::Solid)
    return nullptr;

  return pPanel;
}

void igraphics::DescribeControl(IGraphics& g, int idx, std::string& out)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl)
  {
    out += "null";
    return;
  }

  bool first = true;
  out += "{";
  AppendJsonKey(out, "idx", first); AppendJsonInt(out, idx);
  AppendJsonKey(out, "tag", first); AppendJsonInt(out, g.GetControlTag(pControl));
#if defined(IPLUG_INTROSPECT_CLASS_NAME)
  AppendJsonKey(out, "className", first); AppendJsonString(out, GetControlClassName(pControl).c_str());
#endif
  AppendJsonKey(out, "group", first); AppendJsonString(out, pControl->GetGroup());
  AppendJsonKey(out, "parentIdx", first); AppendJsonInt(out, pControl->GetParent() ? g.GetControlIdx(pControl->GetParent()) : -1);
  AppendJsonKey(out, "tooltip", first); AppendJsonString(out, pControl->GetTooltip());
  AppendJsonKey(out, "hidden", first); AppendJsonBool(out, pControl->IsHidden());
  AppendJsonKey(out, "disabled", first); AppendJsonBool(out, pControl->IsDisabled());
  AppendJsonKey(out, "bounds", first); AppendJsonRect(out, pControl->GetRECT());
  AppendJsonKey(out, "targetBounds", first); AppendJsonRect(out, pControl->GetTargetRECT());

  AppendJsonKey(out, "vals", first);
  out += "[";
  for (int v = 0; v < pControl->NVals(); v++)
  {
    if (v) out += ",";
    const int paramIdx = pControl->GetParamIdx(v);
    const double value = pControl->GetValue(v);
    bool vfirst = true;
    out += "{";
    AppendJsonKey(out, "valIdx", vfirst); AppendJsonInt(out, v);
    AppendJsonKey(out, "paramIdx", vfirst); AppendJsonInt(out, paramIdx);
    AppendJsonKey(out, "value", vfirst); AppendJsonNumber(out, value);

    if (const IParam* pParam = pControl->GetParam(v))
    {
      WDL_String display;
      pParam->GetDisplay(value, true, display);
      const char* label = pParam->GetLabel();

      if (label && *label)
      {
        display.Append(" ");
        display.Append(label);
      }

      AppendJsonKey(out, "paramName", vfirst); AppendJsonString(out, pParam->GetName());
      AppendJsonKey(out, "display", vfirst); AppendJsonString(out, display.Get());
    }
    out += "}";
  }
  out += "]";

  if (auto* pText = dynamic_cast<ITextControl*>(pControl))
  {
    AppendJsonKey(out, "text", first); AppendJsonString(out, pText->GetStr());
  }

  if (auto* pVector = dynamic_cast<IVectorBase*>(pControl))
  {
    AppendJsonKey(out, "label", first); AppendJsonString(out, pVector->GetLabelStr());
    AppendJsonKey(out, "style", first); AppendJsonVStyle(out, pVector->GetStyle());
  }

  if (auto* pPanel = dynamic_cast<IPanelControl*>(pControl))
  {
    AppendJsonKey(out, "pattern", first);
    const IPattern pattern = pPanel->GetPattern();

    if (pattern.mType == EPatternType::Solid && pattern.NStops() > 0)
      AppendJsonColor(out, pattern.GetStop(0).mColor);
    else
      out += "null";
  }

  out += "}";
}

void igraphics::DescribeUI(IGraphics& g, std::string& out)
{
  bool first = true;
  out += "{";
  AppendJsonKey(out, "width", first); AppendJsonInt(out, g.Width());
  AppendJsonKey(out, "height", first); AppendJsonInt(out, g.Height());
  AppendJsonKey(out, "drawScale", first); AppendJsonNumber(out, g.GetDrawScale());
  AppendJsonKey(out, "screenScale", first); AppendJsonNumber(out, g.GetScreenScale());
  AppendJsonKey(out, "windowWidth", first); AppendJsonInt(out, g.WindowWidth());
  AppendJsonKey(out, "windowHeight", first); AppendJsonInt(out, g.WindowHeight());
  AppendJsonKey(out, "liveEditAvailable", first); AppendJsonBool(out, LiveEditCompiledIn());
  AppendJsonKey(out, "liveEditEnabled", first); AppendJsonBool(out, g.LiveEditEnabled());
  AppendJsonKey(out, "inTextEntry", first); AppendJsonBool(out, g.GetControlInTextEntry() != nullptr);
  AppendJsonKey(out, "classNames", first);
#if defined(IPLUG_INTROSPECT_CLASS_NAME)
  AppendJsonBool(out, true);
#else
  AppendJsonBool(out, false);
#endif

  AppendJsonKey(out, "backgroundColor", first);
  if (auto* pPanel = SolidBackground(g))
    AppendJsonColor(out, pPanel->GetPattern().GetStop(0).mColor);
  else
    out += "null";

  AppendJsonKey(out, "nControls", first); AppendJsonInt(out, g.NControls());
  AppendJsonKey(out, "controls", first);
  out += "[";
  for (int i = 0; i < g.NControls(); i++)
  {
    if (i) out += ",";
    DescribeControl(g, i, out);
  }
  out += "]}";
}

bool igraphics::SetControlValue(IGraphics& g, int idx, int valIdx, double normalized, bool gesture)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl || !std::isfinite(normalized))
    return false;

  if (valIdx < 0 || valIdx >= pControl->NVals())
    return false;

  normalized = Clip(normalized, 0., 1.);
  const int paramIdx = pControl->GetParamIdx(valIdx);
  IGEditorDelegate* pDelegate = g.GetDelegate();
  const bool inform = gesture && paramIdx > kNoParameter && pDelegate;

  if (inform)
    pDelegate->BeginInformHostOfParamChangeFromUI(paramIdx);

  pControl->SetValueFromUserInput(normalized, valIdx);

  if (inform)
    pDelegate->EndInformHostOfParamChangeFromUI(paramIdx);

  return true;
}

bool igraphics::SetControlValueToDefault(IGraphics& g, int idx, int valIdx)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl)
    return false;

  if (valIdx != kNoValIdx && (valIdx < 0 || valIdx >= pControl->NVals()))
    return false;

  pControl->SetValueToDefault(valIdx);
  return true;
}

bool igraphics::SetControlHidden(IGraphics& g, int idx, bool hide)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl)
    return false;

  pControl->Hide(hide);
  g.SetAllControlsDirty();
  return true;
}

bool igraphics::SetControlDisabled(IGraphics& g, int idx, bool disable)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl)
    return false;

  pControl->SetDisabled(disable);
  return true;
}

bool igraphics::SetControlText(IGraphics& g, int idx, const char* str)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl || !str)
    return false;

  if (auto* pText = dynamic_cast<ITextControl*>(pControl))
  {
    pText->SetStr(str);
    pText->SetDirty(false);
    return true;
  }

  if (auto* pVector = dynamic_cast<IVectorBase*>(pControl))
  {
    pVector->SetLabelStr(str);
    return true;
  }

  return false;
}

static bool ParseBool(const char* value, bool& out)
{
  if (!value)
    return false;

  if (!std::strcmp(value, "true") || !std::strcmp(value, "1"))
  {
    out = true;
    return true;
  }

  if (!std::strcmp(value, "false") || !std::strcmp(value, "0"))
  {
    out = false;
    return true;
  }

  return false;
}

static bool ParseFloat(const char* value, float& out)
{
  if (!value || !*value)
    return false;

  char* end = nullptr;
  const double v = std::strtod(value, &end);

  if (end == value || *end || !std::isfinite(v))
    return false;

  out = static_cast<float>(v);
  return true;
}

bool igraphics::SetControlProperty(IGraphics& g, int idx, const char* key, const char* value, std::string& err)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl)
  {
    err = "Unknown control index.";
    return false;
  }

  if (!key || !value)
  {
    err = "Missing key or value.";
    return false;
  }

  if (!std::strcmp(key, "text") || !std::strcmp(key, "label"))
  {
    if (!SetControlText(g, idx, value))
    {
      err = "Control has no text or label.";
      return false;
    }
    return true;
  }

  if (!std::strcmp(key, "hidden") || !std::strcmp(key, "disabled"))
  {
    bool flag = false;

    if (!ParseBool(value, flag))
    {
      err = "Expected a boolean.";
      return false;
    }

    return key[0] == 'h' ? SetControlHidden(g, idx, flag) : SetControlDisabled(g, idx, flag);
  }

  if (!std::strncmp(key, "color.", 6))
  {
    auto* pVector = dynamic_cast<IVectorBase*>(pControl);
    EVColor colorIdx;
    IColor color;

    if (!pVector)
    {
      err = "Control is not an IVectorBase control.";
      return false;
    }

    if (!VColorFromName(key + 6, colorIdx))
    {
      err = "Unknown colour name.";
      return false;
    }

    if (!ParseHexColor(value, color))
    {
      err = "Expected #RRGGBB or #RRGGBBAA.";
      return false;
    }

    pVector->SetColor(colorIdx, color);
    return true;
  }

  if (!std::strcmp(key, "pattern"))
  {
    auto* pPanel = dynamic_cast<IPanelControl*>(pControl);
    IColor color;

    if (!pPanel)
    {
      err = "Control is not an IPanelControl.";
      return false;
    }

    if (!ParseHexColor(value, color))
    {
      err = "Expected #RRGGBB or #RRGGBBAA.";
      return false;
    }

    pPanel->SetPattern(IColor(color));
    g.SetAllControlsDirty();
    return true;
  }

  auto* pVector = dynamic_cast<IVectorBase*>(pControl);

  if (!pVector)
  {
    err = "Control is not an IVectorBase control.";
    return false;
  }

  struct BoolProp { const char* key; void (IVectorBase::*setter)(bool); };
  static const BoolProp boolProps[] = {
    { "showLabel", &IVectorBase::SetShowLabel },
    { "showValue", &IVectorBase::SetShowValue },
    { "drawFrame", &IVectorBase::SetDrawFrame },
    { "drawShadows", &IVectorBase::SetDrawShadows },
    { "emboss", &IVectorBase::SetEmboss },
  };

  for (const auto& prop : boolProps)
  {
    if (!std::strcmp(key, prop.key))
    {
      bool flag = false;

      if (!ParseBool(value, flag))
      {
        err = "Expected a boolean.";
        return false;
      }

      (pVector->*prop.setter)(flag);
      return true;
    }
  }

  struct FloatProp { const char* key; void (IVectorBase::*setter)(float); };
  static const FloatProp floatProps[] = {
    { "roundness", &IVectorBase::SetRoundness },
    { "frameThickness", &IVectorBase::SetFrameThickness },
    { "shadowOffset", &IVectorBase::SetShadowOffset },
    { "widgetFrac", &IVectorBase::SetWidgetFrac },
    { "angle", &IVectorBase::SetAngle },
  };

  for (const auto& prop : floatProps)
  {
    if (!std::strcmp(key, prop.key))
    {
      float number = 0.f;

      if (!ParseFloat(value, number))
      {
        err = "Expected a number.";
        return false;
      }

      (pVector->*prop.setter)(number);
      return true;
    }
  }

  err = "Unknown property.";
  return false;
}

static bool ValidRect(const IRECT& r)
{
  return std::isfinite(r.L) && std::isfinite(r.T) && std::isfinite(r.R) && std::isfinite(r.B) && r.R > r.L && r.B > r.T;
}

bool igraphics::SetControlBounds(IGraphics& g, int idx, const IRECT& r, const IRECT* pTarget)
{
  IControl* pControl = ControlAt(g, idx);

  if (!pControl || !ValidRect(r) || (pTarget && !ValidRect(*pTarget)))
    return false;

  pControl->SetTargetAndDrawRECTs(r);

  if (pTarget)
    pControl->SetTargetRECT(*pTarget);

  g.SetAllControlsDirty();
  return true;
}

bool igraphics::SetBackgroundColor(IGraphics& g, const IColor& color)
{
  IPanelControl* pPanel = SolidBackground(g);

  if (!pPanel)
    return false;

  pPanel->SetPattern(IColor(color));
  g.SetAllControlsDirty();
  return true;
}

bool igraphics::DispatchKey(IGraphics& g, float x, float y, const IKeyPress& key, bool down)
{
  return down ? g.OnKeyDown(x, y, key) : g.OnKeyUp(x, y, key);
}
