/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#pragma once

/**
 * @file
 * @brief JSON helpers for IGraphics structs (IRECT, IColor, IVStyle), built on IPlugJSON.h
 */

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

#include "IPlugJSON.h"
#include "IGraphicsStructs.h"
#include "IGraphicsConstants.h"

BEGIN_IPLUG_NAMESPACE
BEGIN_IGRAPHICS_NAMESPACE

/** Append an IRECT as {"l":..,"t":..,"r":..,"b":..}.
 * @param round If true the values are rounded to integers (the live edit event format) */
static inline void AppendJsonRect(std::string& out, const IRECT& r, bool round = false)
{
  auto put = [&](float v) {
    if (round)
      AppendJsonInt(out, static_cast<long long>(std::lround(v)));
    else
      AppendJsonNumber(out, v);
  };

  out += "{\"l\":"; put(r.L);
  out += ",\"t\":"; put(r.T);
  out += ",\"r\":"; put(r.R);
  out += ",\"b\":"; put(r.B);
  out += "}";
}

/** Append an IColor as a "#rrggbbaa" string literal */
static inline void AppendJsonColor(std::string& out, const IColor& c)
{
  char buf[16];
  std::snprintf(buf, sizeof(buf), "#%02x%02x%02x%02x", c.R, c.G, c.B, c.A);
  AppendJsonString(out, buf);
}

/** Strictly parse "#RRGGBB" or "#RRGGBBAA" (case insensitive) into an IColor.
 * @return \c true on success. Unlike IColor::FromColorCodeStr this never asserts and preserves alpha. */
static inline bool ParseHexColor(const char* str, IColor& out)
{
  if (!str || str[0] != '#')
    return false;

  const size_t len = std::strlen(str + 1);

  if (len != 6 && len != 8)
    return false;

  for (size_t i = 1; i <= len; i++)
  {
    if (!std::isxdigit(static_cast<unsigned char>(str[i])))
      return false;
  }

  auto byteAt = [&](size_t pos) {
    char pair[3] = { str[pos], str[pos + 1], 0 };
    return static_cast<int>(std::strtol(pair, nullptr, 16));
  };

  out.R = byteAt(1);
  out.G = byteAt(3);
  out.B = byteAt(5);
  out.A = len == 8 ? byteAt(7) : 255;
  return true;
}

/** Canonical short names for EVColor, usable as JSON keys (no trailing spaces, unlike kVColorStrs) */
static inline const char* VColorName(EVColor color)
{
  switch (color)
  {
    case kBG: return "bg";
    case kFG: return "fg";
    case kPR: return "pr";
    case kFR: return "fr";
    case kHL: return "hl";
    case kSH: return "sh";
    case kX1: return "x1";
    case kX2: return "x2";
    case kX3: return "x3";
    default: return "";
  }
}

/** Parse a canonical or descriptive EVColor name ("fg", "off", "foreground", "extra1", ...)
 * @return \c true if recognised */
static inline bool VColorFromName(const char* name, EVColor& out)
{
  if (!name)
    return false;

  struct Entry { const char* name; EVColor color; };
  static const Entry entries[] = {
    { "bg", kBG }, { "background", kBG },
    { "fg", kFG }, { "off", kFG }, { "foreground", kFG },
    { "pr", kPR }, { "on", kPR }, { "pressed", kPR },
    { "fr", kFR }, { "frame", kFR },
    { "hl", kHL }, { "highlight", kHL },
    { "sh", kSH }, { "shadow", kSH },
    { "x1", kX1 }, { "extra1", kX1 },
    { "x2", kX2 }, { "extra2", kX2 },
    { "x3", kX3 }, { "extra3", kX3 },
  };

  for (const auto& e : entries)
  {
    if (std::strcmp(e.name, name) == 0)
    {
      out = e.color;
      return true;
    }
  }

  return false;
}

/** Append an IVStyle as a JSON object (colors as "#rrggbbaa" keyed by canonical EVColor names) */
static inline void AppendJsonVStyle(std::string& out, const IVStyle& s)
{
  bool first = true;
  out += "{";
  AppendJsonKey(out, "showLabel", first); AppendJsonBool(out, s.showLabel);
  AppendJsonKey(out, "showValue", first); AppendJsonBool(out, s.showValue);
  AppendJsonKey(out, "drawFrame", first); AppendJsonBool(out, s.drawFrame);
  AppendJsonKey(out, "drawShadows", first); AppendJsonBool(out, s.drawShadows);
  AppendJsonKey(out, "emboss", first); AppendJsonBool(out, s.emboss);
  AppendJsonKey(out, "hideCursor", first); AppendJsonBool(out, s.hideCursor);
  AppendJsonKey(out, "roundness", first); AppendJsonNumber(out, s.roundness);
  AppendJsonKey(out, "frameThickness", first); AppendJsonNumber(out, s.frameThickness);
  AppendJsonKey(out, "shadowOffset", first); AppendJsonNumber(out, s.shadowOffset);
  AppendJsonKey(out, "widgetFrac", first); AppendJsonNumber(out, s.widgetFrac);
  AppendJsonKey(out, "angle", first); AppendJsonNumber(out, s.angle);
  AppendJsonKey(out, "colors", first);
  out += "{";
  for (int i = 0; i < kNumVColors; i++)
  {
    if (i) out += ",";
    AppendJsonString(out, VColorName(static_cast<EVColor>(i)));
    out += ":";
    AppendJsonColor(out, s.colorSpec.GetColor(static_cast<EVColor>(i)));
  }
  out += "}}";
}

END_IGRAPHICS_NAMESPACE
END_IPLUG_NAMESPACE
