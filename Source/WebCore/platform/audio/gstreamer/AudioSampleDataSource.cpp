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

#include "config.h"
#include "AudioSampleDataSource.h"

#include "GStreamerAudioData.h"
#include "Logging.h"

#if USE(WHISPER)
#include <whisper.h>
#endif

#if USE(GSTREAMER)

namespace WebCore {

Ref<AudioSampleDataSource> AudioSampleDataSource::create(size_t maximumSampleCount, LoggerHelper& loggerHelper, size_t waitToStartForPushCount)
{
    return adoptRef(*new AudioSampleDataSource(maximumSampleCount, loggerHelper, waitToStartForPushCount));
}

AudioSampleDataSource::AudioSampleDataSource(size_t maximumSampleCount, LoggerHelper& loggerHelper, size_t waitToStartForPushCount)
    : m_maximumSampleCount(maximumSampleCount)
    , m_waitToStartForPushCount(waitToStartForPushCount)
#if !RELEASE_LOG_DISABLED
    , m_logger(loggerHelper.logger())
    , m_logIdentifier(loggerHelper.logIdentifier())
#endif
{
#if RELEASE_LOG_DISABLED
    UNUSED_PARAM(loggerHelper);
#endif
}

AudioSampleDataSource::~AudioSampleDataSource()
{
}

bool AudioSampleDataSource::setupConverter()
{
    ASSERT(m_inputDescription && m_outputDescription);

    auto inputAudioInfo = m_inputDescription->getInfo();
    auto outputAudioInfo = m_outputDescription->getInfo();
    m_converter.reset(gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_IN_WRITABLE, &inputAudioInfo, &outputAudioInfo, nullptr));

    return !!m_converter;
}

bool AudioSampleDataSource::setInputFormat(const GStreamerAudioStreamDescription& format)
{
    ASSERT(format.sampleRate() >= 0);

    m_inputDescription = GStreamerAudioStreamDescription { format };
    return !(m_outputDescription ? setupConverter() : true);
}

bool AudioSampleDataSource::setOutputFormat(const GStreamerAudioStreamDescription& format)
{
    ASSERT(m_inputDescription);
    ASSERT(format.sampleRate() >= 0);

    GstAudioInfo whisperAudioInfo;
#if USE(WHISPER)
    gst_audio_info_set_format(&whisperAudioInfo, GST_AUDIO_FORMAT_F32LE, WHISPER_SAMPLE_RATE, format.getInfo().channels, nullptr);
#else
    UNUSED_PARAM(format);
#endif

    if (m_outputDescription && gst_audio_info_is_equal(&m_outputDescription->getInfo(), &whisperAudioInfo))
        return false;

    m_outputDescription = GStreamerAudioStreamDescription(WTFMove(whisperAudioInfo));
    return !setupConverter();
}

Vector<float> AudioSampleDataSource::convertToWhisperAudioData(const GStreamerAudioData& audioData, size_t inFrameCount)
{
    GstMappedBuffer mappedBuffer(gst_sample_get_buffer(audioData.getSample().get()), GST_MAP_READ);

    size_t outFrameCount = gst_audio_converter_get_out_frames(m_converter.get(), inFrameCount);

    Vector<float> convertedSamples(outFrameCount);
    gpointer in[1] = { mappedBuffer.data() };
    gpointer out[1] = { convertedSamples.data() };
    if (!gst_audio_converter_samples(m_converter.get(), GST_AUDIO_CONVERTER_FLAG_NONE, in, inFrameCount, out, outFrameCount))
        return { };
    return convertedSamples;
}

void AudioSampleDataSource::pushSamples(const MediaTime& sampleTime, const PlatformAudioData& audioData, size_t sampleCount)
{
    UNUSED_PARAM(sampleTime);

    ASSERT(is<GStreamerAudioData>(audioData));

    auto newSamples = convertToWhisperAudioData(downcast<GStreamerAudioData>(audioData), sampleCount);
    if (newSamples.isEmpty())
        return;

    // Whisper.cpp takes four bytes of float values as audio samples. We chop
    // float sample data and wrap them in GstBuffer which holds data in a
    // one-byte array.
    std::span<const uint8_t> newSamplesSpan { reinterpret_cast<const uint8_t*>(newSamples.data()), newSamples.sizeInBytes() };
    m_whisperAudioSamples.append(newSamplesSpan);
}

GRefPtr<GstSample> AudioSampleDataSource::pullSamples(size_t sampleCount, uint64_t timeStamp, double hostTime, PullMode mode)
{
    UNUSED_PARAM(sampleCount);
    UNUSED_PARAM(hostTime);
    UNUSED_PARAM(mode);

    if (m_whisperAudioSamples.isEmpty())
        return nullptr;

    auto buffer = wrapSpanData(m_whisperAudioSamples.span());
    GST_BUFFER_PTS(buffer.get()) = timeStamp;

    m_whisperAudioSamples.clear();

    auto caps = adoptGRef(gst_audio_info_to_caps(&m_outputDescription->getInfo()));
    return adoptGRef(gst_sample_new(buffer.get(), caps.get(), nullptr, nullptr));;
}

#if !RELEASE_LOG_DISABLED
WTFLogChannel& AudioSampleDataSource::logChannel() const
{
    return LogWebRTC;
}
#endif

} // namespace WebCore

#endif // USE(GSTREAMER)
