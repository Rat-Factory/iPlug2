/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#pragma once

/**
 * @file
 * @brief Minimal hand-rolled JSON writer helpers, shared by the IGraphics live
 * edit event stream and the WebMCP agent bridge. Deliberately WDL-style: no
 * parser, no allocations beyond the std::string being appended to.
 */

#include <cmath>
#include <cstdio>
#include <string>

#include "IPlugPlatform.h"

BEGIN_IPLUG_NAMESPACE

/** Append a JSON string literal (with quotes) to out, escaping as required. A null pointer is written as "". */
static inline void AppendJsonString(std::string& out, const char* str)
{
  out.push_back('"');

  if (str)
  {
    for (const char* p = str; *p; ++p)
    {
      unsigned char c = static_cast<unsigned char>(*p);

      switch (c)
      {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
        {
          if (c < 0x20)
          {
            char esc[8];
            std::snprintf(esc, sizeof(esc), "\\u%04x", c);
            out += esc;
          }
          else
          {
            out.push_back(static_cast<char>(c));
          }
          break;
        }
      }
    }
  }

  out.push_back('"');
}

/** Append a JSON number. Uses %.9g (locale independent, round-trips floats); NaN/Inf become null. */
static inline void AppendJsonNumber(std::string& out, double v)
{
  if (!std::isfinite(v))
  {
    out += "null";
    return;
  }

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  out += buf;
}

/** Append a JSON integer. */
static inline void AppendJsonInt(std::string& out, long long v)
{
  out += std::to_string(v);
}

/** Append a JSON boolean. */
static inline void AppendJsonBool(std::string& out, bool b)
{
  out += b ? "true" : "false";
}

/** Append an object key (with a leading comma unless it is the first key). Caller appends the value. */
static inline void AppendJsonKey(std::string& out, const char* key, bool& first)
{
  if (!first)
    out.push_back(',');

  first = false;
  AppendJsonString(out, key);
  out.push_back(':');
}

END_IPLUG_NAMESPACE
