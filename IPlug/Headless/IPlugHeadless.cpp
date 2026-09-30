/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#include "IPlugHeadless.h"

using namespace iplug;

IPlugHeadless::IPlugHeadless(const InstanceInfo& info, const Config& config)
: IPlugAPIBase(config, kAPIAPP)
, IPlugProcessor(config, kAPIAPP)
, mMidiOut(info.midiOut)
, mMidiOutUser(info.midiOutUser)
{
  Trace(TRACELOC, "%s%s", config.pluginName, config.channelIOStr);

  SetChannelConnections(ERoute::kInput, 0, MaxNChannels(ERoute::kInput), !IsInstrument());
  SetChannelConnections(ERoute::kOutput, 0, MaxNChannels(ERoute::kOutput), true);

  SetBlockSize(DEFAULT_BLOCK_SIZE);

  if (info.createIdleTimer)
  {
    CreateTimer();
    mHasIdleTimer = true;
  }
}

void IPlugHeadless::HeadlessIdle()
{
  OnTimer(mIdleStub);
}

bool IPlugHeadless::EditorResize(int viewWidth, int viewHeight)
{
  if (viewWidth != GetEditorWidth() || viewHeight != GetEditorHeight())
    SetEditorSize(viewWidth, viewHeight);

  return false;
}

bool IPlugHeadless::SendMidiMsg(const IMidiMsg& msg)
{
  if (DoesMIDIOut() && mMidiOut)
  {
    const uint8_t bytes[3] = { msg.mStatus, msg.mData1, msg.mData2 };
    mMidiOut(bytes, 3, mMidiOutUser);
    return true;
  }

  return false;
}

bool IPlugHeadless::SendSysEx(const ISysEx& msg)
{
  if (DoesMIDIOut() && mMidiOut && msg.mData && msg.mSize > 0)
  {
    mMidiOut(reinterpret_cast<const uint8_t*>(msg.mData), static_cast<size_t>(msg.mSize), mMidiOutUser);
    return true;
  }

  return false;
}

void IPlugHeadless::SendSysexMsgFromUI(const ISysEx& msg)
{
  SendSysEx(msg);
}

void IPlugHeadless::HeadlessReset(double sampleRate, int blockSize)
{
  SetBlockSize(blockSize);
  SetSampleRate(sampleRate);
  OnReset();
  OnActivate(true);
}

bool IPlugHeadless::HeadlessPushMidiMsg(const IMidiMsg& msg)
{
  return mMidiMsgsFromHost.Push(msg);
}

bool IPlugHeadless::HeadlessPushSysEx(const ISysEx& msg)
{
  SysExData data(msg.mOffset, msg.mSize, msg.mData);
  return mSysExMsgsFromHost.Push(data);
}

void IPlugHeadless::HeadlessProcess(double** inputs, double** outputs, int nFrames)
{
  if (inputs)
    AttachBuffers(ERoute::kInput, 0, NChannelsConnected(ERoute::kInput), inputs, nFrames);

  AttachBuffers(ERoute::kOutput, 0, NChannelsConnected(ERoute::kOutput), outputs, nFrames);

  if (mMidiMsgsFromHost.ElementsAvailable())
  {
    IMidiMsg msg;

    while (mMidiMsgsFromHost.Pop(msg))
    {
      ProcessMidiMsg(msg);
      mMidiMsgsFromProcessor.Push(msg); // queue incoming MIDI for UI
    }
  }

  if (mSysExMsgsFromHost.ElementsAvailable())
  {
    SysExData data;

    while (mSysExMsgsFromHost.Pop(data))
    {
      ISysEx msg { data.mOffset, data.mData, data.mSize };
      ProcessSysEx(msg);
      mSysExDataFromProcessor.Push(data); // queue incoming Sysex for UI
    }
  }

  if (mMidiMsgsFromEditor.ElementsAvailable())
  {
    IMidiMsg msg;

    while (mMidiMsgsFromEditor.Pop(msg))
    {
      ProcessMidiMsg(msg);
    }
  }

  //Do not handle Sysex messages here - SendSysexMsgFromUI overridden

  ENTER_PARAMS_MUTEX
  ProcessBuffers(0.0, nFrames);
  LEAVE_PARAMS_MUTEX
}
