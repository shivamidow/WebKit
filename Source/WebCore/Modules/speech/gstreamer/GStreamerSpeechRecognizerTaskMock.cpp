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
#include "GStreamerSpeechRecognizerTaskMock.h"

namespace WebCore {

GStreamerSpeechRecognizerTaskMock::GStreamerSpeechRecognizerTaskMock(SpeechRecognitionConnectionClientIdentifier identifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives, DelegateCallback&& delegateCallback)
    : m_identifier(identifier)
    , m_doMultipleRecognitions(continuous)
    , m_delegateCallback(WTFMove(delegateCallback))
{
    UNUSED_PARAM(localeIdentifier);
    UNUSED_PARAM(interimResults);
    UNUSED_PARAM(alternatives);
}

GStreamerSpeechRecognizerTaskMock::~GStreamerSpeechRecognizerTaskMock() = default;

void GStreamerSpeechRecognizerTaskMock::audioSamplesAvailable(Vector<float>&& audioSamples)
{
    UNUSED_PARAM(audioSamples);

    if (!m_hasSentSpeechStart) {
        m_hasSentSpeechStart = true;
        m_delegateCallback(WebCore::SpeechRecognitionUpdate::create(m_identifier, WebCore::SpeechRecognitionUpdateType::SpeechStart));
    }

    WebCore::SpeechRecognitionAlternativeData alternative { "Test"_s, 1.0 };
    m_delegateCallback(WebCore::SpeechRecognitionUpdate::createResult(m_identifier, { WebCore::SpeechRecognitionResultData { { WTFMove(alternative) }, true } }));

    if (!m_doMultipleRecognitions)
        abort();
}

void GStreamerSpeechRecognizerTaskMock::abort()
{
    if (m_completed)
        return;
    m_completed = true;

    if (!m_hasSentSpeechEnd && m_hasSentSpeechStart) {
        m_hasSentSpeechEnd = true;
        m_delegateCallback(WebCore::SpeechRecognitionUpdate::create(m_identifier, WebCore::SpeechRecognitionUpdateType::SpeechEnd));
    }

    m_delegateCallback(WebCore::SpeechRecognitionUpdate::create(m_identifier, WebCore::SpeechRecognitionUpdateType::End));
}

void GStreamerSpeechRecognizerTaskMock::stop()
{
    abort();
}

}
