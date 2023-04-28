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
#include "ThreadedSpeechRecognitionServer.h"

#include "WebProcessProxyMessages.h"
#include <WebCore/SpeechRecognitionUpdate.h>

namespace WebKit {

static WebCore::CaptureSourceOrError createRealtimeMediaSourceForSpeechRecognition(SpeechRecognitionServerIdentifier identifier)
{
    auto captureDevice = SpeechRecognitionCaptureSource::findCaptureDevice();
    if (!captureDevice)
        return CaptureSourceOrError { { "No device is available for capture"_s, WebCore::MediaAccessDenialReason::PermissionDenied } };

    return SpeechRecognitionCaptureSource::createRealtimeMediaSource(*captureDevice, identifier);
}

Ref<ThreadedSpeechRecognitionServer> ThreadedSpeechRecognitionServer::create(Client& client, SpeechRecognitionServerIdentifier identifier, SpeechRecognitionPermissionChecker&& permissionChecker, SpeechRecognitionCheckIfMockSpeechRecognitionEnabled&& checkIfEnabled)
{
#if ENABLE(MEDIA_STREAM)
    auto createRealtimeMediaSource = [identifier]() {
        return createRealtimeMediaSourceForSpeechRecognition(identifier);
    };
    return adoptRef(*new ThreadedSpeechRecognitionServer(client, identifier, WTFMove(permissionChecker), WTFMove(checkIfEnabled), WTFMove(createRealtimeMediaSource)));
#else
    return adoptRef(*new ThreadedSpeechRecognitionServer(client, identifier, WTFMove(permissionChecker), WTFMove(checkIfEnabled)));
#endif
}

#if ENABLE(MEDIA_STREAM)
ThreadedSpeechRecognitionServer::ThreadedSpeechRecognitionServer(Client& client, SpeechRecognitionServerIdentifier identifier, SpeechRecognitionPermissionChecker&& permissionChecker, SpeechRecognitionCheckIfMockSpeechRecognitionEnabled&& checkIfEnabled, RealtimeMediaSourceCreateFunction&& function)
#else
ThreadedSpeechRecognitionServer::ThreadedSpeechRecognitionServer(Client& client, SpeechRecognitionServerIdentifier identifier, SpeechRecognitionPermissionChecker&& permissionChecker, SpeechRecognitionCheckIfMockSpeechRecognitionEnabled&& checkIfEnabled)
#endif
    : m_client(client)
    , m_identifier(identifier)
    , m_permissionChecker(WTFMove(permissionChecker))
    , m_checkIfMockSpeechRecognitionEnabled(WTFMove(checkIfEnabled))
    , m_runLoop(RunLoop::create("org.webkit.ThreadedSpeechRecognitionServer"_s, ThreadType::Audio))
#if ENABLE(MEDIA_STREAM)
    , m_realtimeMediaSourceCreateFunction(WTFMove(function))
#endif
{
}

ThreadedSpeechRecognitionServer::~ThreadedSpeechRecognitionServer() = default;

void ThreadedSpeechRecognitionServer::performTask(Function<void()>&& function)
{
    ASSERT(RunLoop::isMain());
    m_runLoop->dispatch(WTFMove(function));
}

void ThreadedSpeechRecognitionServer::start(WebCore::SpeechRecognitionConnectionClientIdentifier clientIdentifier, const String& lang, bool continuous, bool interimResults, uint64_t maxAlternatives, WebCore::ClientOrigin&& origin, WebCore::FrameIdentifier frameIdentifier)
{
    ASSERT(!m_requests.contains(clientIdentifier));
    performTask([this, protectedThis = Ref { *this }, clientIdentifier, lang, continuous, interimResults, maxAlternatives, origin, frameIdentifier] {
        auto requestInfo = WebCore::SpeechRecognitionRequestInfo { clientIdentifier, WTFMove(const_cast<String&>(lang)), continuous, interimResults, maxAlternatives, const_cast<WebCore::ClientOrigin&&>(origin), frameIdentifier };
        auto& newRequest = m_requests.add(clientIdentifier, makeUnique<WebCore::SpeechRecognitionRequest>(WTFMove(requestInfo))).iterator->value;

        requestPermissionForRequest(*newRequest);
    });
}

void ThreadedSpeechRecognitionServer::requestPermissionForRequest(WebCore::SpeechRecognitionRequest& request)
{
    m_permissionChecker(request, [this, protectedThis = Ref { *this }, weakRequest = WeakPtr { request }](auto error) mutable {
        if (!weakRequest)
            return;

        auto identifier = weakRequest->clientIdentifier();
        auto request = m_requests.take(identifier);
        if (error) {
            sendUpdate(identifier, WebCore::SpeechRecognitionUpdateType::Error, const_cast<std::optional<WebCore::SpeechRecognitionError>&&>(error));
            return;
        }

        ASSERT(request);
        handleRequest(makeUniqueRefFromNonNullUniquePtr(WTFMove(request)));
    });
}

void ThreadedSpeechRecognitionServer::handleRequest(UniqueRef<WebCore::SpeechRecognitionRequest>&& request)
{
    if (m_recognizer) {
        m_recognizer->abort(WebCore::SpeechRecognitionError { WebCore::SpeechRecognitionErrorType::Aborted, "Another request is started"_s });
        m_recognizer->prepareForDestruction();
    }

    auto clientIdentifier = request->clientIdentifier();
    m_recognizer = makeUnique<WebCore::SpeechRecognizer>([this, protectedThis = Ref { *this }](auto& update) {
        performTask([this, protectedThis = Ref { *this }, update] {
            sendUpdate(update);

            if (update.type() == WebCore::SpeechRecognitionUpdateType::Error)
                m_recognizer->abort();
            else if (update.type() == WebCore::SpeechRecognitionUpdateType::End)
                m_recognizer->setInactive();
        });
    }, WTFMove(request));

#if ENABLE(MEDIA_STREAM)
    auto sourceOrError = m_realtimeMediaSourceCreateFunction();
    if (!sourceOrError) {
        sendUpdate(WebCore::SpeechRecognitionUpdate::createError(clientIdentifier, WebCore::SpeechRecognitionError { WebCore::SpeechRecognitionErrorType::AudioCapture, sourceOrError.error.errorMessage }));
        return;
    }

    WebProcess::singleton().parentProcessConnection()->send(Messages::WebProcessProxy::RequestToMuteCaptureInPagesExcept(m_identifier), 0);

    bool mockDeviceCapturesEnabled = m_checkIfMockSpeechRecognitionEnabled();
    m_recognizer->start(sourceOrError.source(), mockDeviceCapturesEnabled);
#else
    sendUpdate(clientIdentifier, WebCore::SpeechRecognitionUpdateType::Error, WebCore::SpeechRecognitionError { WebCore::SpeechRecognitionErrorType::AudioCapture, "Audio capture is not implemented"_s });
#endif
}

void ThreadedSpeechRecognitionServer::stop(WebCore::SpeechRecognitionConnectionClientIdentifier clientIdentifier)
{
    performTask([this, protectedThis = Ref { *this }, clientIdentifier] {
        if (m_requests.remove(clientIdentifier)) {
            sendUpdate(clientIdentifier, WebCore::SpeechRecognitionUpdateType::End);
            return;
        }

        if (m_recognizer && m_recognizer->clientIdentifier() == clientIdentifier)
            m_recognizer->stop();
    });
}

void ThreadedSpeechRecognitionServer::abort(WebCore::SpeechRecognitionConnectionClientIdentifier clientIdentifier)
{
    performTask([this, protectedThis = Ref { *this }, clientIdentifier] {
        if (m_requests.remove(clientIdentifier)) {
            sendUpdate(clientIdentifier, WebCore::SpeechRecognitionUpdateType::End);
            return;
        }

        if (m_recognizer && m_recognizer->clientIdentifier() == clientIdentifier)
            m_recognizer->abort();
    });
}

void ThreadedSpeechRecognitionServer::invalidate(WebCore::SpeechRecognitionConnectionClientIdentifier clientIdentifier)
{
    performTask([this, protectedThis = Ref { *this }, clientIdentifier] {
        if (m_recognizer && m_recognizer->clientIdentifier() == clientIdentifier)
            m_recognizer->abort();
    });
}

void ThreadedSpeechRecognitionServer::sendUpdate(WebCore::SpeechRecognitionConnectionClientIdentifier clientIdentifier, WebCore::SpeechRecognitionUpdateType type, std::optional<WebCore::SpeechRecognitionError> error, std::optional<Vector<WebCore::SpeechRecognitionResultData>> result)
{
    auto update = WebCore::SpeechRecognitionUpdate::create(clientIdentifier, type);
    if (type == WebCore::SpeechRecognitionUpdateType::Error)
        update = WebCore::SpeechRecognitionUpdate::createError(clientIdentifier, *error);
    if (type == WebCore::SpeechRecognitionUpdateType::Result)
        update = WebCore::SpeechRecognitionUpdate::createResult(clientIdentifier, *result);
    sendUpdate(update);
}

void ThreadedSpeechRecognitionServer::sendUpdate(const WebCore::SpeechRecognitionUpdate& update)
{
    callOnMainRunLoop([this, protectedThis = Ref { *this }, update] {
        m_client.didReceiveUpdate(WTFMove(const_cast<SpeechRecognitionUpdate&>(update)));
    });
}

} // namespace WebKit
