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
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "SpeechRecognizer.h"

#if USE(GSTREAMER) && ENABLE(MEDIA_STREAM)

#include "GStreamerAudioData.h"
#include "GStreamerAudioStreamDescription.h"
#include "GStreamerCommon.h"
#include "GStreamerSpeechRecognizerTask.h"
#include "SpeechRecognitionRequest.h"
#include "SpeechRecognitionUpdate.h"

namespace WebCore {

void SpeechRecognizer::dataCaptured(const MediaTime& time, const PlatformAudioData& audioData, const AudioStreamDescription& description, size_t sampleCount)
{
    UNUSED_PARAM(time);

#if USE(WHISPER)
    const auto& gstAudioData = static_cast<const GStreamerAudioData&>(audioData);
    GstMappedBuffer mappedBuffer(gst_sample_get_buffer(gstAudioData.getSample().get()), GST_MAP_READ);

    RELEASE_ASSERT(description.isFloat());
    Vector<float> newSamples(reinterpret_cast<float*>(mappedBuffer.data()), sampleCount);
    m_task->audioSamplesAvailable(WTFMove(newSamples));
#else
    UNUSED_PARAM(audioData);
    UNUSED_PARAM(description);
    UNUSED_PARAM(sampleCount);
#endif
}

bool SpeechRecognizer::startRecognition(bool mockSpeechRecognitionEnabled, SpeechRecognitionConnectionClientIdentifier identifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives)
{
#if USE(WHISPER)
    WTFLogAlways("Loading model for locale %s", localeIdentifier.ascii().data());
    WTFLogAlways("mockSpeechRecognitionEnabled: %d, continuous: %d, interimResults: %d, alternatives: %lu", mockSpeechRecognitionEnabled, continuous, interimResults, alternatives);

    auto delegateCallback = [weakThis = WeakPtr { *this }](const SpeechRecognitionUpdate& update) {
        if (weakThis)
            weakThis->m_delegateCallback(update);
    };
    if (mockSpeechRecognitionEnabled)
        m_task = std::make_unique<GStreamerSpeechRecognizerTaskMock>(identifier, localeIdentifier, continuous, interimResults, alternatives, WTFMove(delegateCallback));
    else
        m_task = std::make_unique<GStreamerSpeechRecognizerTask>(identifier, localeIdentifier, continuous, interimResults, alternatives, WTFMove(delegateCallback));

    return !!m_task;
#else
    UNUSED_PARAM(mockSpeechRecognitionEnabled);
    UNUSED_PARAM(identifier);
    UNUSED_PARAM(localeIdentifier);
    UNUSED_PARAM(continuous);
    UNUSED_PARAM(interimResults);
    UNUSED_PARAM(alternatives);
    return false;
#endif
}

void SpeechRecognizer::abortRecognition()
{
#if USE(WHISPER)
    ASSERT(m_task);
    m_task->abort();
#endif
}

void SpeechRecognizer::stopRecognition()
{
#if USE(WHISPER)
    ASSERT(m_task);
    m_task->stop();
#endif
}

} // namespace WebCore

#endif // USE(GSTREAMER) && ENABLE(MEDIA_STREAM)
