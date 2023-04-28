/*
 * Copyright (C) 2023 ChangSeok Oh <changseok@webkit.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(GSTREAMER)

#include "GStreamerAudioStreamDescription.h"
#include "GUniquePtrGStreamer.h"
#include <optional>
#include <wtf/LoggerHelper.h>
#include <wtf/MediaTime.h>
#include <wtf/ThreadSafeRefCounted.h>

namespace WebCore {

class GStreamerAudioData;
class PlatformAudioData;

class AudioSampleDataSource : public ThreadSafeRefCounted<AudioSampleDataSource, WTF::DestructionThread::MainRunLoop>
#if !RELEASE_LOG_DISABLED
    , private LoggerHelper
#endif
    {
public:
    static Ref<AudioSampleDataSource> create(size_t, LoggerHelper&, size_t waitToStartForPushCount = 2);

    ~AudioSampleDataSource();

    // The following two methods return an inverse result to align with Cocoa
    // platforms. i.e., True means something wrong happened with a non-zero
    // error code and False means everything went fine with no error code.
    bool setInputFormat(const GStreamerAudioStreamDescription&);
    bool setOutputFormat(const GStreamerAudioStreamDescription&);

    void pushSamples(const MediaTime&, const PlatformAudioData&, size_t);

    enum PullMode { Copy, Mix };
    GRefPtr<GstSample> pullSamples(size_t, uint64_t, double, PullMode);

    const GStreamerAudioStreamDescription* inputDescription() const { return m_inputDescription ? &m_inputDescription.value() : nullptr; }
    const GStreamerAudioStreamDescription* outputDescription() const { return m_outputDescription ? &m_outputDescription.value() : nullptr; }

#if !RELEASE_LOG_DISABLED
    const Logger& logger() const final { return m_logger; }
    const void* logIdentifier() const final { return m_logIdentifier; }
#endif

private:
    AudioSampleDataSource(size_t, LoggerHelper&, size_t waitToStartForPushCount);
    bool setupConverter();
    Vector<float> convertToWhisperAudioData(const GStreamerAudioData&, size_t inFrameCount);

    size_t m_maximumSampleCount { 0 };
    size_t m_waitToStartForPushCount { 2 };

    GUniquePtr<GstAudioConverter> m_converter;

    std::optional<GStreamerAudioStreamDescription> m_inputDescription;
    std::optional<GStreamerAudioStreamDescription> m_outputDescription;

    Vector<uint8_t> m_whisperAudioSamples;

#if !RELEASE_LOG_DISABLED
    const char* logClassName() const final { return "AudioSampleDataSource"; }
    WTFLogChannel& logChannel() const final;

    Ref<const Logger> m_logger;
    const void* m_logIdentifier;
#endif
};

} // namespace WebCore

#endif // USE(GSTREAMER)
