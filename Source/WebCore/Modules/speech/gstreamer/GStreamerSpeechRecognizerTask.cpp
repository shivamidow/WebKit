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
#include "GStreamerSpeechRecognizerTask.h"

#if USE(GLIB_EVENT_LOOP)
#include <wtf/glib/RunLoopSourcePriority.h>
#endif

#if USE(WHISPER)
#include "whisper.h"
namespace WTF {
WTF_DEFINE_GPTR_DELETER(struct whisper_context, whisper_free)
}
#endif

namespace WebCore {

class GStreamerSpeechRecognizerTaskImpl : public ThreadSafeRefCounted<GStreamerSpeechRecognizerTaskImpl, WTF::DestructionThread::MainRunLoop> {
    WTF_MAKE_FAST_ALLOCATED;
public:
    using DelegateCallback = Function<void(const SpeechRecognitionUpdate&)>;
    static Ref<GStreamerSpeechRecognizerTaskImpl> create(SpeechRecognitionConnectionClientIdentifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives, DelegateCallback&&);

    void audioSamplesAvailable(Vector<float>&& audioSamples);
    void abort();
    void stop();

    void processAudioSamples();

    void sendSpeechStartIfNeeded();
    void sendSpeechEndIfNeeded();

private:
    explicit GStreamerSpeechRecognizerTaskImpl(SpeechRecognitionConnectionClientIdentifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives, DelegateCallback&&);
#if USE(WHISPER)
    void initializeWhisper(const String& localeIdentifier);
#endif
    void sendEndIfNeeded();

    Ref<RunLoop> m_runLoop;
    SpeechRecognitionConnectionClientIdentifier m_identifier;

    bool m_doMultipleRecognitions { false };
    bool m_shouldReportPartialResults { false };
    bool m_hasSentSpeechStart { false };
    bool m_hasSentSpeechEnd { false };
    bool m_hasSentEnd { false };

    Atomic<bool> m_didStop { false };

    uint64_t m_maxAlternatives;
    DelegateCallback m_delegateCallback;

    Lock m_lock;
    Vector<float> m_audioSampleBuffer WTF_GUARDED_BY_LOCK(m_lock);
#if USE(WHISPER)
    GUniquePtr<struct whisper_context> m_whisperContext;
    struct whisper_full_params m_whisperParams;
#endif
};

Ref<GStreamerSpeechRecognizerTaskImpl> GStreamerSpeechRecognizerTaskImpl::create(SpeechRecognitionConnectionClientIdentifier identifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives, DelegateCallback&& delegateCallback)
{
    return adoptRef(*new GStreamerSpeechRecognizerTaskImpl(identifier, localeIdentifier, continuous, interimResults, alternatives, WTFMove(delegateCallback)));
}

GStreamerSpeechRecognizerTaskImpl::GStreamerSpeechRecognizerTaskImpl(SpeechRecognitionConnectionClientIdentifier identifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives, DelegateCallback&& delegateCallback)
    : m_runLoop(RunLoop::create("org.webkit.GStreamerSpeechRecognizerTask"_s, ThreadType::Audio))
    , m_identifier(identifier)
    , m_doMultipleRecognitions(continuous)
    , m_shouldReportPartialResults(interimResults)
    , m_maxAlternatives(alternatives ? alternatives : 1)
    , m_delegateCallback(WTFMove(delegateCallback))
{
#if USE(WHISPER)
    initializeWhisper(localeIdentifier);
#else
    UNUSED_PARAM(localeIdentifier);
#endif
}

#if USE(WHISPER)
void GStreamerSpeechRecognizerTaskImpl::initializeWhisper(const String& localeIdentifier)
{
    // FIXME: The following API suppresses logs of Whisper, but it is unavailable in v1.4.2.
    // whisper_set_log_callback(nullptr);

    auto path = String::fromUTF8(g_getenv("WEBKIT_WHISPER_MODEL_PATH"));
    if (path.isEmpty()) {
        auto error = SpeechRecognitionError { SpeechRecognitionErrorType::Aborted, "WEBKIT_WHISPER_MODEL_PATH is not specified."_s };
        m_delegateCallback(SpeechRecognitionUpdate::createError(m_identifier, WTFMove(error)));

        WTFLogAlways("WEBKIT_WHISPER_MODEL_PATH is not specified.");
        return;
    }

    const auto whisperLocale = localeIdentifier.left(localeIdentifier.find('-'));
    const auto modelFileName = String("ggml-base."_s + whisperLocale + ".bin"_s);
    const auto whisperModelFilePath = String::fromUTF8(g_build_filename(path.utf8().data(), modelFileName.utf8().data(), nullptr));
    m_whisperContext.reset(whisper_init_from_file(whisperModelFilePath.utf8().data()));
    if (!m_whisperContext) {
        auto error = SpeechRecognitionError { SpeechRecognitionErrorType::Aborted, "Failed to initialize the whisper context."_s };
        m_delegateCallback(SpeechRecognitionUpdate::createError(m_identifier, WTFMove(error)));

        WTFLogAlways("Failed to initialize the whisper context with %s.", whisperModelFilePath.utf8().data());
        return;
    }

    m_whisperParams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    m_whisperParams.print_progress = false;
    m_whisperParams.print_special = false;
    m_whisperParams.print_realtime = false;
    m_whisperParams.print_timestamps = false;

    // FIXME: The following parameters suppress blank and non-speech tokens,
    // but they are insufficient. They cause arbitrary speech instead.
    // m_whisperParams.suppress_blank = true;
    // m_whisperParams.suppress_non_speech_tokens = true;

    m_whisperParams.translate = false;
    m_whisperParams.single_segment = true;
    m_whisperParams.max_tokens = 0;
    m_whisperParams.language = whisperLocale.utf8().data();

    // FIXME: We may provide a API to control the number of threads.
    m_whisperParams.n_threads = 4;
    m_whisperParams.audio_ctx = 0;
    m_whisperParams.speed_up = false;

    m_whisperParams.temperature_inc = 0.4;
    m_whisperParams.prompt_tokens = nullptr;
    m_whisperParams.prompt_n_tokens = 0;

    m_whisperParams.progress_callback_user_data = this;
    m_whisperParams.progress_callback = [](struct whisper_context*, struct whisper_state*, int progress, void* userData) {
        if (!userData)
            return;

        auto* task = static_cast<GStreamerSpeechRecognizerTaskImpl*>(userData);
        if (!progress) {
            task->sendSpeechStartIfNeeded();
            return;
        }
        task->sendSpeechEndIfNeeded();
    };
}
#endif

void GStreamerSpeechRecognizerTaskImpl::audioSamplesAvailable(Vector<float>&& audioSamples)
{
    {
        Locker locker { m_lock };
        m_audioSampleBuffer.appendVector(WTFMove(audioSamples));
    }
    m_runLoop->dispatch([this, protectedThis = Ref { *this }] {
        processAudioSamples();
    });
}

void GStreamerSpeechRecognizerTaskImpl::abort()
{
    stop();
}

void GStreamerSpeechRecognizerTaskImpl::stop()
{
    if (!m_didStop.load())
        m_didStop.store(true);

    // FIXME: Process the remaining samples in the buffer before stop.
    sendSpeechEndIfNeeded();
    sendEndIfNeeded();
}

void GStreamerSpeechRecognizerTaskImpl::processAudioSamples()
{
    ASSERT(!isMainThread());

#if USE(WHISPER)
    static constexpr size_t retainedSampleCount = WHISPER_SAMPLE_RATE * 0.2; // 0.2s
    static constexpr size_t minSampleCount = WHISPER_SAMPLE_RATE * 3 + retainedSampleCount;

    Vector<float> audioSamples;
    {
        Locker locker { m_lock };
        if (m_audioSampleBuffer.size() < minSampleCount)
            return;
        audioSamples.swap(m_audioSampleBuffer);
        // Keep the last part of the audio samples for next iteration to mitigate word boundary issues.
        m_audioSampleBuffer = Vector<float>(audioSamples.end() - retainedSampleCount, retainedSampleCount);
    }

    if (whisper_full(m_whisperContext.get(), m_whisperParams, audioSamples.data(), audioSamples.size())) {
        WTFLogAlways("Failed to process audio");
        return;
    }

    const int segmentCount = whisper_full_n_segments(m_whisperContext.get());
    if (segmentCount < 1) {
        WTFLogAlways("No segments to process: %lu.", audioSamples.size());
        return;
    }

    Vector<WebCore::SpeechRecognitionAlternativeData> alternatives;
    alternatives.reserveInitialCapacity(m_maxAlternatives);
    for (int i = 0; i < segmentCount; ++i) {
        float maxConfidence = 0.0;
        const int tokenCount = whisper_full_n_tokens(m_whisperContext.get(), i);
        for (int j = 0; j < tokenCount; ++j) {
            // FIXME: Is concatinating tokens better than using whisper_full_get_segment_text?
            // const char* token = whisper_full_get_token_text(m_whisperContext.get(), i, j);
            const float confidence = whisper_full_get_token_p(m_whisperContext.get(), i, j);
            maxConfidence = std::max(maxConfidence, confidence);
        }

        const char* text = whisper_full_get_segment_text(m_whisperContext.get(), i);
        WTFLogAlways(">>> %s, confidence: %f\n", text, maxConfidence);

        alternatives.append(WebCore::SpeechRecognitionAlternativeData { String::fromUTF8(text), maxConfidence });
        if (alternatives.size() >= m_maxAlternatives)
            break;
    }
    if (m_didStop.load())
        return;

    m_delegateCallback(WebCore::SpeechRecognitionUpdate::createResult(m_identifier, { WebCore::SpeechRecognitionResultData { alternatives, true } }));
#endif
}

void GStreamerSpeechRecognizerTaskImpl::sendSpeechStartIfNeeded()
{
    if (m_hasSentSpeechStart)
        return;

    m_hasSentSpeechStart = true;
    m_delegateCallback(WebCore::SpeechRecognitionUpdate::create(m_identifier, WebCore::SpeechRecognitionUpdateType::SpeechStart));
}

void GStreamerSpeechRecognizerTaskImpl::sendSpeechEndIfNeeded()
{
    if (!m_hasSentSpeechStart || m_hasSentSpeechEnd)
        return;

    m_hasSentSpeechEnd = true;
    m_delegateCallback(WebCore::SpeechRecognitionUpdate::create(m_identifier, WebCore::SpeechRecognitionUpdateType::SpeechEnd));
}

void GStreamerSpeechRecognizerTaskImpl::sendEndIfNeeded()
{
    if (m_hasSentEnd)
        return;

    m_hasSentEnd = true;
    m_delegateCallback(WebCore::SpeechRecognitionUpdate::create(m_identifier, WebCore::SpeechRecognitionUpdateType::End));
}

GStreamerSpeechRecognizerTask::GStreamerSpeechRecognizerTask(SpeechRecognitionConnectionClientIdentifier identifier, const String& localeIdentifier, bool continuous, bool interimResults, uint64_t alternatives, DelegateCallback&& delegateCallback)
    : m_impl(GStreamerSpeechRecognizerTaskImpl::create(identifier, localeIdentifier, continuous, interimResults, alternatives, WTFMove(delegateCallback)))
{
}

GStreamerSpeechRecognizerTask::GStreamerSpeechRecognizerTask() = default;
GStreamerSpeechRecognizerTask::~GStreamerSpeechRecognizerTask() = default;

void GStreamerSpeechRecognizerTask::audioSamplesAvailable(Vector<float>&& audioSamples)
{
    m_impl->audioSamplesAvailable(WTFMove(audioSamples));
}

void GStreamerSpeechRecognizerTask::abort()
{
    m_impl->stop();
}

void GStreamerSpeechRecognizerTask::stop()
{
    m_impl->stop();
}

} // namespace WebCore
