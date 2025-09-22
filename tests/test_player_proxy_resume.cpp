/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <cassert>
#include <iostream>

#include "Common/config.h"
#include "Common/macros.h"
#include "Common/MediaSource.h"
#include "Player/PlayerBase.h"

#define private public
#define protected public
#include "Player/PlayerProxy.h"
#undef private
#undef protected

using namespace std;
using namespace mediakit;

namespace {

class DummyDelegate : public PlayerBase {
public:
    using Ptr = std::shared_ptr<DummyDelegate>;

    void play(const std::string &) override {}
    void pause(bool) override {}
    void speed(float) override {}
    void teardown() override {}
    void seekTo(float) override {}
    void seekTo(uint32_t) override {}
    void setMediaSource(const MediaSource::Ptr &) override {}
    void setOnShutdown(const Event &) override {}
    void setOnPlayResult(const Event &) override {}
    void setOnResume(const std::function<void()> &) override {}
    uint32_t getProgressPos() const override { return _progress; }

    void setProgress(uint32_t progress) { _progress = progress; }

private:
    uint32_t _progress = 0;
};

MediaTuple makeTuple() {
    return MediaTuple{DEFAULT_VHOST, "app", "stream", ""};
}

ProtocolOption makeOption() {
    return ProtocolOption();
}

void testDisabledWhenConfigOff() {
    mINI::Instance()[General::kKeepReplayProgress] = 0;
    PlayerProxy proxy(makeTuple(), makeOption());
    string url = "rtsp://example.com/live/stream";
    proxy.initPlaybackResume(url);
    assert(!proxy._playback_resume.enabled);
    assert(proxy._playback_resume.last_url == url);
}

void testParseAndAssembleKeepsOriginalOrder() {
    mINI::Instance()[General::kKeepReplayProgress] = 1;
    PlayerProxy proxy(makeTuple(), makeOption());
    string url =
        "rtsp://admin:password01!@172.24.9.2:554/Streaming/tracks/201?starttime="
        "20250825T080124Z&endtime=20250825T082408Z&foo=bar";
    proxy.initPlaybackResume(url);
    assert(proxy._playback_resume.enabled);
    auto assembled = proxy.assemblePlaybackUrl();
    assert(assembled == url);
}

void testBuildPlaybackUrlAdvancesStartTime() {
    mINI::Instance()[General::kKeepReplayProgress] = 1;
    PlayerProxy proxy(makeTuple(), makeOption());
    string url =
        "rtsp://admin:password01!@172.24.9.2:554/Streaming/tracks/201?starttime="
        "20250825T080124Z&endtime=20250825T082408Z";
    proxy.initPlaybackResume(url);
    assert(proxy._playback_resume.enabled);

    auto delegate = std::make_shared<DummyDelegate>();
    delegate->setProgress(30);
    proxy._delegate = delegate;

    auto new_url = proxy.buildPlaybackUrl(url);
    assert(new_url.find("starttime=20250825T080154Z") != std::string::npos);
    assert(proxy._playback_resume.last_url == new_url);
}

void testBuildPlaybackUrlClampedToEndTime() {
    mINI::Instance()[General::kKeepReplayProgress] = 1;
    PlayerProxy proxy(makeTuple(), makeOption());
    string url =
        "rtsp://admin:password01!@172.24.9.2:554/Streaming/tracks/201?starttime="
        "20250825T080124Z&endtime=20250825T082408Z";
    proxy.initPlaybackResume(url);
    assert(proxy._playback_resume.enabled);

    auto delegate = std::make_shared<DummyDelegate>();
    // Provide a large progress so the computed start would exceed the end time.
    delegate->setProgress(2000);
    proxy._delegate = delegate;

    auto new_url = proxy.buildPlaybackUrl(url);
    assert(new_url.find("starttime=20250825T082407Z") != std::string::npos);
}

void testBuildPlaybackUrlPreservesTimezoneOffset() {
    mINI::Instance()[General::kKeepReplayProgress] = 1;
    PlayerProxy proxy(makeTuple(), makeOption());
    string url =
        "rtsp://admin:password01!@172.24.9.2:554/Streaming/tracks/201?starttime="
        "20250825T160124+08:00&endtime=20250825T162408+08:00";
    proxy.initPlaybackResume(url);
    assert(proxy._playback_resume.enabled);

    auto delegate = std::make_shared<DummyDelegate>();
    delegate->setProgress(60);
    proxy._delegate = delegate;

    auto new_url = proxy.buildPlaybackUrl(url);
    assert(new_url.find("starttime=20250825T160224+08:00") != std::string::npos);
}

} // namespace

int main() {
    testDisabledWhenConfigOff();
    testParseAndAssembleKeepsOriginalOrder();
    testBuildPlaybackUrlAdvancesStartTime();
    testBuildPlaybackUrlClampedToEndTime();
    testBuildPlaybackUrlPreservesTimezoneOffset();

    cout << "All PlayerProxy playback resume tests passed." << endl;
    return 0;
}
