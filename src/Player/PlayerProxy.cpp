/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include "PlayerProxy.h"
#include "Common/config.h"
#include "Rtmp/RtmpMediaSource.h"
#include "Rtmp/RtmpPlayer.h"
#include "Rtsp/RtspMediaSource.h"
#include "Rtsp/RtspPlayer.h"
#include "Util/MD5.h"
#include "Util/logger.h"
#include "Util/mini.h"
#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdlib>
#include <limits>
#include <stdexcept>

using namespace toolkit;
using namespace std;

namespace mediakit {

namespace {

using TimezoneFormat = PlayerProxy::PlaybackResume::TimezoneFormat;

static bool isLeapYear(int year) {
    if (year % 4 != 0) {
        return false;
    }
    if (year % 100 != 0) {
        return true;
    }
    return (year % 400) == 0;
}

static int daysInMonth(int year, int month) {
    static const int kDaysInMonth[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int days = kDaysInMonth[month];
    if (month == 1 && isLeapYear(year)) {
        ++days;
    }
    return days;
}

static int64_t tmToUtcSeconds(const std::tm &time) {
    int64_t days = 0;
    int year = time.tm_year + 1900;
    if (year >= 1970) {
        for (int y = 1970; y < year; ++y) {
            days += isLeapYear(y) ? 366 : 365;
        }
    } else {
        for (int y = 1969; y >= year; --y) {
            days -= isLeapYear(y) ? 366 : 365;
        }
    }
    static const int kCumulativeDays[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    if (time.tm_mon >= 0 && time.tm_mon < 12) {
        days += kCumulativeDays[time.tm_mon];
        if (time.tm_mon > 1 && isLeapYear(year)) {
            ++days;
        }
    }
    days += time.tm_mday - 1;
    return days * 86400 + time.tm_hour * 3600 + time.tm_min * 60 + time.tm_sec;
}

static std::tm utcSecondsToTm(int64_t seconds) {
    std::tm result{};
    int64_t days = seconds / 86400;
    int64_t remain = seconds % 86400;
    if (remain < 0) {
        remain += 86400;
        --days;
    }
    result.tm_hour = static_cast<int>(remain / 3600);
    remain %= 3600;
    result.tm_min = static_cast<int>(remain / 60);
    result.tm_sec = static_cast<int>(remain % 60);

    int year = 1970;
    if (days >= 0) {
        while (true) {
            int days_in_year = isLeapYear(year) ? 366 : 365;
            if (days >= days_in_year) {
                days -= days_in_year;
                ++year;
            } else {
                break;
            }
        }
    } else {
        while (days < 0) {
            --year;
            days += isLeapYear(year) ? 366 : 365;
        }
    }

    int month = 0;
    while (month < 12) {
        int dim = daysInMonth(year, month);
        if (days >= dim) {
            days -= dim;
            ++month;
        } else {
            break;
        }
    }

    result.tm_year = year - 1900;
    result.tm_mon = month;
    result.tm_mday = static_cast<int>(days) + 1;
    return result;
}

static std::string formatPlaybackTime(int64_t utc_seconds, TimezoneFormat format, int tz_offset) {
    int64_t local_seconds = utc_seconds;
    if (format == TimezoneFormat::offset_no_colon || format == TimezoneFormat::offset_with_colon) {
        local_seconds += tz_offset;
    }
    auto local_tm = utcSecondsToTm(local_seconds);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02d",
             local_tm.tm_year + 1900,
             local_tm.tm_mon + 1,
             local_tm.tm_mday,
             local_tm.tm_hour,
             local_tm.tm_min,
             local_tm.tm_sec);
    std::string result(buf);
    switch (format) {
    case TimezoneFormat::utc_z:
        result.push_back('Z');
        break;
    case TimezoneFormat::offset_no_colon:
    case TimezoneFormat::offset_with_colon: {
        int total = tz_offset;
        char sign = total >= 0 ? '+' : '-';
        total = std::abs(total);
        int hours = total / 3600;
        int minutes = (total % 3600) / 60;
        char tz_buf[8];
        if (format == TimezoneFormat::offset_with_colon) {
            snprintf(tz_buf, sizeof(tz_buf), "%c%02d:%02d", sign, hours, minutes);
        } else {
            snprintf(tz_buf, sizeof(tz_buf), "%c%02d%02d", sign, hours, minutes);
        }
        result.append(tz_buf);
        break;
    }
    case TimezoneFormat::none:
        break;
    }
    return result;
}

static bool parsePlaybackTime(const std::string &value, int64_t &utc_seconds, TimezoneFormat &format, int &tz_offset) {
    format = TimezoneFormat::none;
    tz_offset = 0;
    if (value.size() < 15) {
        return false;
    }
    std::string work = value;
    if (!work.empty()) {
        char last = work.back();
        if (last == 'Z' || last == 'z') {
            format = TimezoneFormat::utc_z;
            work.pop_back();
        } else {
            auto pos = work.find_last_of("+-");
            if (pos != std::string::npos && pos > 8) {
                std::string tz_part = work.substr(pos);
                bool has_colon = tz_part.find(':') != std::string::npos;
                int sign = 1;
                if (tz_part[0] == '+') {
                    sign = 1;
                } else if (tz_part[0] == '-') {
                    sign = -1;
                } else {
                    return false;
                }
                std::string digits;
                for (size_t i = 1; i < tz_part.size(); ++i) {
                    if (tz_part[i] == ':') {
                        continue;
                    }
                    if (!isdigit(static_cast<unsigned char>(tz_part[i]))) {
                        return false;
                    }
                    digits.push_back(tz_part[i]);
                }
                if (digits.size() != 4) {
                    return false;
                }
                int hours = 0;
                int minutes = 0;
                try {
                    hours = std::stoi(digits.substr(0, 2));
                    minutes = std::stoi(digits.substr(2, 2));
                } catch (const std::exception &) {
                    return false;
                }
                if (minutes >= 60) {
                    return false;
                }
                tz_offset = sign * (hours * 3600 + minutes * 60);
                format = has_colon ? TimezoneFormat::offset_with_colon : TimezoneFormat::offset_no_colon;
                work = work.substr(0, pos);
            }
        }
    }
    if (work.size() != 15 || work[8] != 'T') {
        return false;
    }
    for (size_t i = 0; i < work.size(); ++i) {
        if (i == 8) {
            continue;
        }
        if (!isdigit(static_cast<unsigned char>(work[i]))) {
            return false;
        }
    }
    std::tm time{};
    try {
        time.tm_year = std::stoi(work.substr(0, 4)) - 1900;
        time.tm_mon = std::stoi(work.substr(4, 2)) - 1;
        time.tm_mday = std::stoi(work.substr(6, 2));
        time.tm_hour = std::stoi(work.substr(9, 2));
        time.tm_min = std::stoi(work.substr(11, 2));
        time.tm_sec = std::stoi(work.substr(13, 2));
    } catch (const std::exception &) {
        return false;
    }
    int year = time.tm_year + 1900;
    if (time.tm_mon < 0 || time.tm_mon > 11) {
        return false;
    }
    if (time.tm_mday < 1 || time.tm_mday > daysInMonth(year, time.tm_mon)) {
        return false;
    }
    if (time.tm_hour < 0 || time.tm_hour > 23 || time.tm_min < 0 || time.tm_min > 59 || time.tm_sec < 0 || time.tm_sec > 60) {
        return false;
    }
    utc_seconds = tmToUtcSeconds(time) - tz_offset;
    return true;
}

static std::string toLowerCopy(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
}

} // namespace

PlayerProxy::PlayerProxy(
    const MediaTuple &tuple, const ProtocolOption &option, int retry_count,
    const EventPoller::Ptr &poller, int reconnect_delay_min, int reconnect_delay_max, int reconnect_delay_step)
    : MediaPlayer(poller), _tuple(tuple), _option(option) {
    _retry_count = retry_count;

    setOnClose(nullptr);
    setOnConnect(nullptr);
    setOnDisconnect(nullptr);
    
    _reconnect_delay_min = reconnect_delay_min > 0 ? reconnect_delay_min : 2;
    _reconnect_delay_max = reconnect_delay_max > 0 ? reconnect_delay_max : 60;
    _reconnect_delay_step = reconnect_delay_step > 0 ? reconnect_delay_step : 3;
    _live_secs = 0;
    _live_status = 1;
    _repull_count = 0;
    (*this)[Client::kWaitTrackReady] = false;
}

void PlayerProxy::setPlayCallbackOnce(function<void(const SockException &ex)> cb) {
    _on_play = std::move(cb);
}

void PlayerProxy::setOnClose(function<void(const SockException &ex)> cb) {
    _on_close = cb ? std::move(cb) : [](const SockException &) {};
}

void PlayerProxy::setOnDisconnect(std::function<void()> cb) {
    _on_disconnect = cb ? std::move(cb) : [] () {};
}

void PlayerProxy::setOnConnect(std::function<void(const TranslationInfo&)> cb) {
    _on_connect = cb ? std::move(cb) : [](const TranslationInfo&) {};
}

void PlayerProxy::setTranslationInfo()
{
    _transtalion_info.byte_speed = _media_src ? _media_src->getBytesSpeed() : -1;
    _transtalion_info.start_time_stamp = _media_src ? _media_src->getCreateStamp() : 0;
    _transtalion_info.stream_info.clear();
    auto tracks = _muxer->getTracks();
    for (auto &track : tracks) {
        track->update();
        _transtalion_info.stream_info.emplace_back();
        auto &back = _transtalion_info.stream_info.back();
        back.bitrate = track->getBitRate();
        back.codec_type = track->getTrackType();
        back.codec_name = track->getCodecName();
        switch (back.codec_type) {
            case TrackAudio : {
                auto audio_track = dynamic_pointer_cast<AudioTrack>(track);
                back.audio_sample_rate = audio_track->getAudioSampleRate();
                back.audio_channel = audio_track->getAudioChannel();
                back.audio_sample_bit = audio_track->getAudioSampleBit();
                break;
            }
            case TrackVideo : {
                auto video_track = dynamic_pointer_cast<VideoTrack>(track);
                back.video_width = video_track->getVideoWidth();
                back.video_height = video_track->getVideoHeight();
                back.video_fps = video_track->getVideoFps();
                break;
            }
            default:
                break;
        }
    }
}

static int getMaxTrackSize(const std::string &url) {
    if (url.find(".m3u8") != std::string::npos || url.find(".ts") != std::string::npos) {
        // hls和ts协议才开放多track支持  [AUTO-TRANSLATED:6c5f8f04]
        // Only hls and ts protocols support multiple tracks
        return 16;
    }
    return 2;
}

void PlayerProxy::play(const string &strUrlTmp) {
    initPlaybackResume(strUrlTmp);
    _option.max_track = getMaxTrackSize(strUrlTmp);
    weak_ptr<PlayerProxy> weakSelf = shared_from_this();
    std::shared_ptr<int> piFailedCnt(new int(0)); // 连续播放失败次数
    setOnPlayResult([weakSelf, strUrlTmp, piFailedCnt](const SockException &err) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }

        if (strongSelf->_on_play) {
            strongSelf->_on_play(err);
            strongSelf->_on_play = nullptr;
        }

        if (!err) {
            // 取消定时器,避免hls拉流索引文件因为网络波动失败重连成功后出现循环重试的情况  [AUTO-TRANSLATED:91e5f0c8]
            // Cancel the timer to avoid the situation where the hls stream index file fails to reconnect due to network fluctuations and then retries in a loop after successful reconnection
            strongSelf->_timer.reset();
            strongSelf->_live_ticker.resetTime();
            strongSelf->_live_status = 0;
            // 播放成功  [AUTO-TRANSLATED:e43f9fb8]
            // Play successfully
            *piFailedCnt = 0; // 连续播放失败次数清0
            strongSelf->onPlaySuccess();
            strongSelf->setTranslationInfo();
            strongSelf->_on_connect(strongSelf->_transtalion_info);

            InfoL << "play " << strongSelf->_pull_url << " success";
        } else if (*piFailedCnt < strongSelf->_retry_count || strongSelf->_retry_count < 0) {
            // 播放失败，延时重试播放  [AUTO-TRANSLATED:d7537c9c]
            // Play failed, retry playing with delay
            strongSelf->_on_disconnect();
            strongSelf->rePlay(strUrlTmp, (*piFailedCnt)++);
        } else {
            // 达到了最大重试次数，回调关闭  [AUTO-TRANSLATED:610f31f3]
            // Reached the maximum number of retries, callback to close
            strongSelf->_on_close(err);
        }
    });
    setOnShutdown([weakSelf, strUrlTmp, piFailedCnt](const SockException &err) {
        auto strongSelf = weakSelf.lock();
        if (!strongSelf) {
            return;
        }

        // 注销直接拉流代理产生的流：#532  [AUTO-TRANSLATED:c6343a3b]
        // Unregister the stream generated by the direct stream proxy: #532
        strongSelf->setMediaSource(nullptr);

        if (strongSelf->_muxer) {
            auto tracks = strongSelf->MediaPlayer::getTracks(false);
            for (auto &track : tracks) {
                track->delDelegate(strongSelf->_muxer.get());
            }

            GET_CONFIG(bool, reset_when_replay, General::kResetWhenRePlay);
            if (reset_when_replay) {
                strongSelf->_muxer.reset();
            } else {
                strongSelf->_muxer->resetTracks();
            }
        }

        if (*piFailedCnt == 0) {
            // 第一次重拉更新时长  [AUTO-TRANSLATED:3c414b08]
            // Update the duration for the first time
            strongSelf->_live_secs += strongSelf->_live_ticker.elapsedTime() / 1000;
            strongSelf->_live_ticker.resetTime();
            TraceL << " live secs " << strongSelf->_live_secs;
        }

        // 播放异常中断，延时重试播放  [AUTO-TRANSLATED:fee316b2]
        // Play interrupted abnormally, retry playing with delay
        if (*piFailedCnt < strongSelf->_retry_count || strongSelf->_retry_count < 0) {
            strongSelf->_repull_count++;
            strongSelf->rePlay(strUrlTmp, (*piFailedCnt)++);
        } else {
            // 达到了最大重试次数，回调关闭  [AUTO-TRANSLATED:610f31f3]
            // Reached the maximum number of retries, callback to close
            strongSelf->_on_close(err);
        }
    });
    auto first_url = _playback_resume.last_url.empty() ? strUrlTmp : _playback_resume.last_url;
    try {
        MediaPlayer::play(first_url);
    } catch (std::exception &ex) {
        ErrorL << ex.what();
        onPlayResult(SockException(Err_other, ex.what()));
        return;
    }
    _pull_url = first_url;
    setDirectProxy();
}

void PlayerProxy::setDirectProxy() {
    MediaSource::Ptr mediaSource;
    if (dynamic_pointer_cast<RtspPlayer>(_delegate)) {
        // rtsp拉流  [AUTO-TRANSLATED:189cf691]
        // Rtsp stream
        GET_CONFIG(bool, directProxy, Rtsp::kDirectProxy);
        if (directProxy && _option.enable_rtsp) {
            mediaSource = std::make_shared<RtspMediaSource>(_tuple);
        }
    } else if (dynamic_pointer_cast<RtmpPlayer>(_delegate)) {
        // rtmp拉流  [AUTO-TRANSLATED:f70a142c]
        // Rtmp stream
        GET_CONFIG(bool, directProxy, Rtmp::kDirectProxy);
        if (directProxy && _option.enable_rtmp) {
            mediaSource = std::make_shared<RtmpMediaSource>(_tuple);
        }
    }
    if (mediaSource) {
        setMediaSource(mediaSource);
    }
}

void PlayerProxy::initPlaybackResume(const std::string &url) {
    GET_CONFIG(bool, keep_replay_progress, General::kKeepReplayProgress);
    _playback_resume = PlaybackResume();
    _playback_resume.last_url = url;
    if (!keep_replay_progress) {
        return;
    }

    _playback_resume.enabled = true;
    std::string working = url;
    auto fragment_pos = working.find('#');
    if (fragment_pos != std::string::npos) {
        _playback_resume.fragment = working.substr(fragment_pos);
        working = working.substr(0, fragment_pos);
    }

    auto query_pos = working.find('?');
    if (query_pos == std::string::npos) {
        _playback_resume.base = working;
        _playback_resume.enabled = false;
        return;
    }

    _playback_resume.base = working.substr(0, query_pos);
    std::string query = working.substr(query_pos + 1);
    bool found_start = false;
    bool parse_error = false;

    size_t pos = 0;
    while (pos <= query.size()) {
        size_t next = query.find('&', pos);
        std::string token = query.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        if (!token.empty()) {
            PlaybackResume::QueryItem item;
            auto equal_pos = token.find('=');
            if (equal_pos != std::string::npos) {
                item.key = token.substr(0, equal_pos);
                item.value = token.substr(equal_pos + 1);
                item.has_value = true;
            } else {
                item.key = token;
            }

            auto lower_key = toLowerCopy(item.key);
            if (!found_start && lower_key == "starttime" && item.has_value) {
                int64_t stamp = 0;
                int tz_offset = 0;
                auto format = TimezoneFormat::none;
                if (parsePlaybackTime(item.value, stamp, format, tz_offset)) {
                    _playback_resume.initial_start = stamp;
                    _playback_resume.tz_format = format;
                    _playback_resume.tz_offset = tz_offset;
                    _playback_resume.start_index = _playback_resume.items.size();
                    found_start = true;
                } else {
                    parse_error = true;
                }
            } else if (lower_key == "endtime" && item.has_value) {
                int64_t end_stamp = 0;
                int tz_tmp = 0;
                auto format_tmp = TimezoneFormat::none;
                if (parsePlaybackTime(item.value, end_stamp, format_tmp, tz_tmp)) {
                    _playback_resume.end_stamp = end_stamp;
                    _playback_resume.end_index = _playback_resume.items.size();
                }
            }
            _playback_resume.items.emplace_back(std::move(item));
        }

        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }

    if (!found_start || parse_error) {
        _playback_resume.enabled = false;
    }
}

std::string PlayerProxy::assemblePlaybackUrl() const {
    if (!_playback_resume.enabled) {
        return _playback_resume.last_url;
    }
    _StrPrinter printer;
    printer << _playback_resume.base;
    bool appended = false;
    for (const auto &item : _playback_resume.items) {
        printer << (appended ? "&" : "?");
        appended = true;
        printer << item.key;
        if (item.has_value) {
            printer << "=" << item.value;
        }
    }
    if (!appended) {
        return _playback_resume.last_url;
    }
    printer << _playback_resume.fragment;
    return printer;
}

std::string PlayerProxy::buildPlaybackUrl(const std::string &origin_url) {
    if (!_playback_resume.enabled || _playback_resume.start_index == std::numeric_limits<size_t>::max()) {
        return !_playback_resume.last_url.empty() ? _playback_resume.last_url : origin_url;
    }

    auto delegate = getDelegate();
    uint64_t progress_seconds = delegate ? delegate->getProgressPos() : 0;
    _playback_resume.total_progress_seconds += progress_seconds;
    int64_t new_start = _playback_resume.initial_start + static_cast<int64_t>(_playback_resume.total_progress_seconds);
    if (_playback_resume.end_stamp > 0 && new_start >= _playback_resume.end_stamp) {
        if (_playback_resume.end_stamp > _playback_resume.initial_start) {
            new_start = _playback_resume.end_stamp - 1;
        } else {
            new_start = _playback_resume.initial_start;
        }
    }
    if (new_start < _playback_resume.initial_start) {
        new_start = _playback_resume.initial_start;
    }

    _playback_resume.items[_playback_resume.start_index].value =
        formatPlaybackTime(new_start, _playback_resume.tz_format, _playback_resume.tz_offset);
    _playback_resume.items[_playback_resume.start_index].has_value = true;

    auto new_url = assemblePlaybackUrl();
    if (!new_url.empty()) {
        _playback_resume.last_url = new_url;
    }
    return !_playback_resume.last_url.empty() ? _playback_resume.last_url : origin_url;
}

PlayerProxy::~PlayerProxy() {
    _timer.reset();
    // 避免析构时, 忘记回调api请求  [AUTO-TRANSLATED:1ad9ad52]
    // Avoid forgetting to callback api request when destructing
    if (_on_play) {
        try {
            _on_play(SockException(Err_shutdown, "player proxy close"));
        } catch (std::exception &ex) {
            WarnL << "Exception occurred: " << ex.what();
        }
        _on_play = nullptr;
    }
}

void PlayerProxy::rePlay(const string &strUrl, int iFailedCnt) {
    auto iDelay = MAX(_reconnect_delay_min * 1000, MIN(iFailedCnt * _reconnect_delay_step * 1000, _reconnect_delay_max * 1000));
    weak_ptr<PlayerProxy> weakSelf = shared_from_this();
    _timer = std::make_shared<Timer>(
        iDelay / 1000.0f,
        [weakSelf, strUrl, iFailedCnt]() {
            // 播放失败次数越多，则延时越长  [AUTO-TRANSLATED:5af39264]
            // The more times the playback fails, the longer the delay
            auto strongPlayer = weakSelf.lock();
            if (!strongPlayer) {
                return false;
            }
            auto retry_url = strongPlayer->buildPlaybackUrl(strUrl);
            WarnL << "重试播放[" << iFailedCnt << "]:" << retry_url;
            strongPlayer->MediaPlayer::play(retry_url);
            strongPlayer->_pull_url = retry_url;
            strongPlayer->setDirectProxy();
            return false;
        },
        getPoller());
}

bool PlayerProxy::close(MediaSource &sender) {
    // 通知其停止推流  [AUTO-TRANSLATED:d69d10d8]
    // Notify it to stop pushing the stream
    _muxer = nullptr;
    setMediaSource(nullptr);
    teardown();
    _on_close(SockException(Err_shutdown, "closed by user"));
    WarnL << "close media: " << sender.getUrl();
    return true;
}

int PlayerProxy::totalReaderCount() {
    return (_muxer ? _muxer->totalReaderCount() : 0) + (_media_src ? _media_src->readerCount() : 0);
}

int PlayerProxy::totalReaderCount(MediaSource &sender) {
    return totalReaderCount();
}

MediaOriginType PlayerProxy::getOriginType(MediaSource &sender) const {
    return MediaOriginType::pull;
}

string PlayerProxy::getOriginUrl(MediaSource &sender) const {
    return _pull_url;
}

std::shared_ptr<SockInfo> PlayerProxy::getOriginSock(MediaSource &sender) const {
    return getSockInfo();
}

float PlayerProxy::getLossRate(MediaSource &sender, TrackType type) {
    return getPacketLossRate(type);
}

toolkit::EventPoller::Ptr PlayerProxy::getOwnerPoller(MediaSource &sender) { 
    return getPoller();
}

TranslationInfo PlayerProxy::getTranslationInfo() {
    return _transtalion_info;
}

void PlayerProxy::onPlaySuccess() {
    GET_CONFIG(bool, reset_when_replay, General::kResetWhenRePlay);
    if (dynamic_pointer_cast<RtspMediaSource>(_media_src)) {
        // rtsp拉流代理  [AUTO-TRANSLATED:3935cf68]
        // Rtsp stream proxy
        if (reset_when_replay || !_muxer) {
            auto old = _option.enable_rtsp;
            _option.enable_rtsp = false;
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, getDuration(), _option);
            _option.enable_rtsp = old;
        }
    } else if (dynamic_pointer_cast<RtmpMediaSource>(_media_src)) {
        // rtmp拉流代理  [AUTO-TRANSLATED:21173335]
        // Rtmp stream proxy
        if (reset_when_replay || !_muxer) {
             auto old = _option.enable_rtmp;
            _option.enable_rtmp = false;
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, getDuration(), _option);
             _option.enable_rtmp = old;
        }
    } else {
        // 其他拉流代理  [AUTO-TRANSLATED:e5f2e45d]
        // Other stream proxies
        if (reset_when_replay || !_muxer) {
            _muxer = std::make_shared<MultiMediaSourceMuxer>(_tuple, getDuration(), _option);
        }
    }
    _muxer->setMediaListener(shared_from_this());

    auto videoTrack = getTrack(TrackVideo, false);
    if (videoTrack) {
        // 添加视频  [AUTO-TRANSLATED:afc7e0f7]
        // Add video
        _muxer->addTrack(videoTrack);
        // 视频数据写入_mediaMuxer  [AUTO-TRANSLATED:fc07e1c9]
        // Write video data to _mediaMuxer
        videoTrack->addDelegate(_muxer);
    }

    auto audioTrack = getTrack(TrackAudio, false);
    if (audioTrack) {
        // 添加音频  [AUTO-TRANSLATED:e08e79ce]
        // Add audio
        _muxer->addTrack(audioTrack);
        // 音频数据写入_mediaMuxer  [AUTO-TRANSLATED:69911524]
        // Write audio data to _mediaMuxer
        audioTrack->addDelegate(_muxer);
    }

    // 添加完毕所有track，防止单track情况下最大等待3秒  [AUTO-TRANSLATED:8908bc01]
    // After adding all tracks, prevent the maximum waiting time of 3 seconds in the case of a single track
    _muxer->addTrackCompleted();

    if (_media_src) {
        // 让_muxer对象拦截一部分事件(比如说录像相关事件)  [AUTO-TRANSLATED:7d27c400]
        // Let the _muxer object intercept some events (such as recording related events)
        _media_src->setListener(_muxer);
    }
}

int PlayerProxy::getStatus() {
    return _live_status.load();
}
uint64_t PlayerProxy::getLiveSecs() {
    if (_live_status == 0) {
        return _live_secs + _live_ticker.elapsedTime() / 1000;
    }
    return _live_secs;
}

uint64_t PlayerProxy::getRePullCount() {
    return _repull_count;
}

} /* namespace mediakit */
