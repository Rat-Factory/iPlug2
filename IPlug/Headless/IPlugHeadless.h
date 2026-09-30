/*
 ==============================================================================

 This file is part of the iPlug 2 library. Copyright (C) the iPlug 2 developers.

 See LICENSE.txt for  more info.

 ==============================================================================
*/

#ifndef _IPLUGAPI_
#define _IPLUGAPI_

/**
 * @file
 * @copydoc IPlugHeadless
 */

#include <cstddef>
#include <cstdint>

#include "IPlugPlatform.h"
#include "IPlugAPIBase.h"
#include "IPlugProcessor.h"

BEGIN_IPLUG_NAMESPACE

/** MIDI out sink for a headless host. Called on the audio thread with the raw
 * bytes of one message; the host is expected to queue them, not block. */
using HeadlessMidiOutFunc = void (*)(const uint8_t* bytes, size_t n, void* user);

struct InstanceInfo
{
  HeadlessMidiOutFunc midiOut = nullptr;
  void* midiOutUser = nullptr;
  // true: OnIdle runs on the platform Timer (a thread on Linux) every
  // IDLE_TIMER_RATE ms, as in IPlugAPP. false: no timer is created and the
  // host calls HeadlessIdle() itself, which keeps idle work deterministic
  // (an offline render can tick it per block; a live host per main-loop turn).
  bool createIdleTimer = true;
};

/** Headless (no window, no dialogs, no RtAudio / RtMidi) base class for an
 *  IPlug plug-in that some other program drives: an embedded appliance, a
 *  test harness, an offline renderer. Selected with HEADLESS_API, in the
 *  same way APP_API selects IPlugAPP.
 *
 *  Unlike IPlugAPP there is no IPlugAPPHost. The driving program owns the
 *  audio and MIDI I/O and calls the three Headless* methods:
 *
 *    HeadlessReset(sampleRate, blockSize)   once before the first block, and
 *                                          again whenever either changes
 *    HeadlessPushMidiMsg(msg)              from any single producer thread;
 *                                          lock-free, drained on the audio thread
 *    HeadlessProcess(in, out, nFrames)     on the audio thread, nFrames <= blockSize
 *
 *  The idle timer (OnIdle, parameter / MIDI echo to a UI) is the platform
 *  Timer, which on Linux is a thread of its own.
 *
 *   @ingroup APIClasses */
class IPlugHeadless : public IPlugAPIBase
                    , public IPlugProcessor
{
public:
  IPlugHeadless(const InstanceInfo& info, const Config& config);

  //IPlugAPIBase
  void BeginInformHostOfParamChange(int idx) override {};
  void InformHostOfParamChange(int idx, double normalizedValue) override {};
  void EndInformHostOfParamChange(int idx) override {};
  void InformHostOfPresetChange() override {};
  bool EditorResize(int viewWidth, int viewHeight) override;

  //IEditorDelegate
  void SendSysexMsgFromUI(const ISysEx& msg) override;

  //IPlugProcessor
  bool SendMidiMsg(const IMidiMsg& msg) override;
  bool SendSysEx(const ISysEx& msg) override;

  //IPlugHeadless
  /** Sets the sample rate and block size and calls OnReset() / OnActivate(). Not audio-thread safe. */
  void HeadlessReset(double sampleRate, int blockSize);

  /** Queues a MIDI message for the next HeadlessProcess() call. Lock-free
   *  single-producer; returns false if the queue is full. */
  bool HeadlessPushMidiMsg(const IMidiMsg& msg);

  /** Queues SysEx for the next HeadlessProcess() call. Same threading rules as HeadlessPushMidiMsg(). */
  bool HeadlessPushSysEx(const ISysEx& msg);

  /** Renders one block. \p outputs must have MaxNChannels(kOutput) channels of
   *  \p nFrames samples. \p inputs may be nullptr for an instrument. */
  void HeadlessProcess(double** inputs, double** outputs, int nFrames);

  /** Runs one idle tick (parameter / MIDI echo to a UI, then OnIdle()). Call from
   *  the host's main thread when the plug-in was created with createIdleTimer = false. */
  void HeadlessIdle();

  /** \return \c true if the plug-in owns an idle Timer (createIdleTimer was true). */
  bool HeadlessHasIdleTimer() const { return mHasIdleTimer; }

  /** Capacity of the incoming MIDI queue (messages). */
  static constexpr int kMidiQueueSize = 1024;

private:
  // Stands in for the Timer argument of OnTimer() when the host drives idle.
  struct IdleStub final : public Timer { void Stop() override {} };

  IdleStub mIdleStub;
  bool mHasIdleTimer = false;
  HeadlessMidiOutFunc mMidiOut = nullptr;
  void* mMidiOutUser = nullptr;
  IPlugQueue<IMidiMsg> mMidiMsgsFromHost {kMidiQueueSize};
  IPlugQueue<SysExData> mSysExMsgsFromHost {SYSEX_TRANSFER_SIZE};
};

IPlugHeadless* MakePlug(const InstanceInfo& info);

END_IPLUG_NAMESPACE

#endif
