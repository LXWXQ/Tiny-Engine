#include "Audio/AudioService.h"

#include "Audio/AudioClip.h"
#include "Core/AssetPack.h"

#include <iostream>
#include <vector>

namespace audio
{
AudioService& AudioService::instance()
{
    static AudioService service;
    return service;
}

bool AudioService::ensureStarted()
{
    if (startAttempted_)
        return available();

    startAttempted_ = true;
    device_ = std::make_unique<AudioDevice>();
    if (device_->start())
        return true;

    error_ = device_->lastError();
    std::cout << "[Audio] 设备不可用：" << error_ << "（引擎继续无声运行）\n";
    device_.reset();
    return false;
}

bool AudioService::available() const
{
    return device_ != nullptr && device_->running();
}

const std::string& AudioService::lastError() const
{
    return error_;
}

ClipId AudioService::loadClip(const std::string& path)
{
    if (path.empty())
        return kInvalidClip;

    const auto cached = clipCache_.find(path);
    if (cached != clipCache_.end())
        return cached->second;

    std::vector<uint8_t> bytes;
    if (!assets::mounts().readFile(path, bytes))
    {
        std::cerr << "[Audio] 读不到片段：" << path << "\n";
        return kInvalidClip;
    }

    AudioClip clip;
    std::string parseError;
    if (!loadWavFromMemory(bytes.data(), bytes.size(), clip, parseError))
    {
        std::cerr << "[Audio] 解析失败：" << path << " | " << parseError << "\n";
        return kInvalidClip;
    }

    if (!ensureStarted())
        return kInvalidClip;

    const ClipId id = device_->addClip(std::make_shared<AudioClip>(std::move(clip)));
    if (id == kInvalidClip)
    {
        std::cerr << "[Audio] 片段槽位已满：" << path << "\n";
        return kInvalidClip;
    }

    clipCache_.emplace(path, id);
    return id;
}

PlayRequest AudioService::play(ClipId clip, const VoiceParams& params)
{
    if (clip == kInvalidClip || !ensureStarted())
        return kInvalidPlayRequest;
    return device_->play(clip, params);
}

bool AudioService::updateVoice(PlayRequest request, const VoiceParams& params)
{
    if (!available())
        return false;
    return device_->updateVoice(request, params);
}

void AudioService::stop(PlayRequest request)
{
    if (available())
        device_->stop(request);
}

void AudioService::stopAll()
{
    if (available())
        device_->stopAll();
}

void AudioService::setMasterVolume(float volume)
{
    if (ensureStarted())
        device_->setMasterVolume(volume);
}

MixStats AudioService::stats() const
{
    return available() ? device_->stats() : MixStats{};
}

uint32_t AudioService::sampleRate() const
{
    return available() ? device_->sampleRate() : 0;
}

void AudioService::shutdown()
{
    if (device_)
    {
        device_->shutdown();
        device_.reset();
    }
    clipCache_.clear();
    startAttempted_ = false;
}
} // namespace audio
