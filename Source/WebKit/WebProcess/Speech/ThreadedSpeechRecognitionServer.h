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

#pragma once

#include "SpeechRecognitionPermissionRequest.h"
#include <WebCore/PageIdentifier.h>
#include <WebCore/SpeechRecognitionResultData.h>
#include <WebCore/SpeechRecognizer.h>
#include <wtf/Lock.h>
#include <wtf/RunLoop.h>
#include <wtf/UniqueRef.h>

namespace WebKit {

using SpeechRecognitionServerIdentifier = WebCore::PageIdentifier;
using SpeechRecognitionPermissionChecker = Function<void(WebCore::SpeechRecognitionRequest&, SpeechRecognitionPermissionRequestCallback&&)>;
using SpeechRecognitionCheckIfMockSpeechRecognitionEnabled = Function<bool()>;

class ThreadedSpeechRecognitionServer : public ThreadSafeRefCounted<ThreadedSpeechRecognitionServer, WTF::DestructionThread::MainRunLoop> {
    WTF_MAKE_NONCOPYABLE(ThreadedSpeechRecognitionServer);
    WTF_MAKE_FAST_ALLOCATED;
public:
    class Client {
    public:
        virtual void didReceiveUpdate(WebCore::SpeechRecognitionUpdate&&) = 0;
    };

    static Ref<ThreadedSpeechRecognitionServer> create(Client&, SpeechRecognitionServerIdentifier, SpeechRecognitionPermissionChecker&&, SpeechRecognitionCheckIfMockSpeechRecognitionEnabled&&);
    virtual ~ThreadedSpeechRecognitionServer();

    void start(WebCore::SpeechRecognitionConnectionClientIdentifier, const String& lang, bool continuous, bool interimResults, uint64_t maxAlternatives, WebCore::ClientOrigin&&, WebCore::FrameIdentifier);
    void stop(WebCore::SpeechRecognitionConnectionClientIdentifier);
    void abort(WebCore::SpeechRecognitionConnectionClientIdentifier);
    void invalidate(WebCore::SpeechRecognitionConnectionClientIdentifier);

    void performTask(Function<void()>&&);
private:
#if ENABLE(MEDIA_STREAM)
    using RealtimeMediaSourceCreateFunction = Function<WebCore::CaptureSourceOrError()>;
    ThreadedSpeechRecognitionServer(Client&, SpeechRecognitionServerIdentifier, SpeechRecognitionPermissionChecker&&, SpeechRecognitionCheckIfMockSpeechRecognitionEnabled&&, RealtimeMediaSourceCreateFunction&&);
#else
    ThreadedSpeechRecognitionServer(Client&, SpeechRecognitionServerIdentifier, SpeechRecognitionPermissionChecker&&, SpeechRecognitionCheckIfMockSpeechRecognitionEnabled&&);
#endif

    void requestPermissionForRequest(WebCore::SpeechRecognitionRequest&);
    void handleRequest(UniqueRef<WebCore::SpeechRecognitionRequest>&&);
    void sendUpdate(WebCore::SpeechRecognitionConnectionClientIdentifier, WebCore::SpeechRecognitionUpdateType, std::optional<WebCore::SpeechRecognitionError> = std::nullopt, std::optional<Vector<WebCore::SpeechRecognitionResultData>> = std::nullopt);
    void sendUpdate(const WebCore::SpeechRecognitionUpdate&);

    Client& m_client;
    SpeechRecognitionServerIdentifier m_identifier;
    HashMap<WebCore::SpeechRecognitionConnectionClientIdentifier, std::unique_ptr<WebCore::SpeechRecognitionRequest>> m_requests;
    std::unique_ptr<WebCore::SpeechRecognizer> m_recognizer;
    SpeechRecognitionPermissionChecker m_permissionChecker;
    SpeechRecognitionCheckIfMockSpeechRecognitionEnabled m_checkIfMockSpeechRecognitionEnabled;

    Lock m_lock;
    Ref<RunLoop> m_runLoop;

#if ENABLE(MEDIA_STREAM)
    RealtimeMediaSourceCreateFunction m_realtimeMediaSourceCreateFunction;
#endif
};

} // namespace WebKit
