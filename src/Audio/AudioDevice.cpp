
#include "Audio/AudioDevice.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <xaudio2.h>

namespace audio
{
namespace
{
constexpr uint32_t kBufferFrames = 1024;
constexpr uint32_t kCommandCapacity = 256;
constexpr uint32_t kChannelCount = 2;

std::string describeHresult(HRESULT hr)
{
    char text[32] = {};
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(text);
}

class VoiceCallback final : public IXAudio2VoiceCallback
{
public:
    using OnBufferEndFn = void (*)(void* user, uint32_t context);

    VoiceCallback(OnBufferEndFn fn, void* user) : fn_(fn), user_(user) {}

    void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
    void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
    void STDMETHODCALLTYPE OnStreamEnd() override {}
    void STDMETHODCALLTYPE OnBufferStart(void*) override {}
    void STDMETHODCALLTYPE OnBufferEnd(void* context) override
    {
        fn_(user_, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(context)));
    }
    void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
    void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}

private:
    OnBufferEndFn fn_ = nullptr;
    void* user_ = nullptr;
};

enum class CommandKind : uint32_t
{
    Play,
    StopAll,
    MasterVolume,
};

struct Command
{
    CommandKind kind = CommandKind::Play;
    ClipId clip = kInvalidClip;
    VoiceParams params;
    float volume = 1.0f;
};
} // namespace

struct AudioDevice::Impl
{
    IXAudio2* engine = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;

    AudioMixer mixer;
    uint32_t sampleRate = 48000;
    std::vector<float> buffers[kChannelCount];
    std::atomic<bool> running{ false };
    bool comInitialized = false;
    std::string error;

    Command commands[kCommandCapacity]{};
    std::atomic<uint32_t> writeIndex{ 0 };
    std::atomic<uint32_t> readIndex{ 0 };
    std::atomic<uint32_t> rejectedCommands{ 0 };

    mutable std::mutex statsMutex;
    MixStats lastStats;

    VoiceCallback callback{ &Impl::onBufferEndThunk, this };
    static void onBufferEndThunk(void* user, uint32_t context)
    {
        static_cast<Impl*>(user)->renderNext(context);
    }

    void drainCommands()
    {
        uint32_t read = readIndex.load(std::memory_order_acquire);
        const uint32_t write = writeIndex.load(std::memory_order_acquire);
        while (read != write)
        {
            const Command& command = commands[read];
            switch (command.kind)
            {
            case CommandKind::Play:
                mixer.play(command.clip, command.params);
                break;
            case CommandKind::StopAll:
                mixer.stopAll();
                break;
            case CommandKind::MasterVolume:
                mixer.setMasterVolume(command.volume);
                break;
            }
            read = (read + 1) % kCommandCapacity;
        }
        readIndex.store(read, std::memory_order_release);
    }
    void renderNext(uint32_t index)
    {
        if (!running.load(std::memory_order_acquire) || source == nullptr)
            return;
        if (index >= kChannelCount)
            return;

        drainCommands();

        std::vector<float>& buffer = buffers[index];
        const uint32_t frames = static_cast<uint32_t>(buffer.size() / kChannelCount);
        mixer.render(buffer.data(), frames, sampleRate);

        {
            std::lock_guard<std::mutex> lock(statsMutex);
            lastStats = mixer.stats();
        }

        XAUDIO2_BUFFER packet{};
        packet.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(float));
        packet.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
        packet.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(index));
        source->SubmitSourceBuffer(&packet);
    }

    bool submit(Command command)
    {
        const uint32_t write = writeIndex.load(std::memory_order_relaxed);
        const uint32_t next = (write + 1) % kCommandCapacity;
        if (next == readIndex.load(std::memory_order_acquire))
        {
            rejectedCommands.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        commands[write] = command;
        writeIndex.store(next, std::memory_order_release);
        return true;
    }
};

AudioDevice::AudioDevice() : impl_(std::make_unique<Impl>()) {}

AudioDevice::~AudioDevice()
{
    shutdown();
}

bool AudioDevice::start(uint32_t sampleRate)
{
    if (impl_->running.load())
        return true;

    impl_->sampleRate = sampleRate == 0 ? 48000 : sampleRate;
    for (std::vector<float>& buffer : impl_->buffers)
        buffer.assign(static_cast<std::size_t>(kBufferFrames) * kChannelCount, 0.0f);

    const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(comResult))
        impl_->comInitialized = true;
    else if (comResult != RPC_E_CHANGED_MODE)
    {
        impl_->error = "COM init failed (" + describeHresult(comResult) + ")";
        return false;
    }

    HRESULT hr = ::XAudio2Create(&impl_->engine, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr))
    {
        impl_->error = "XAudio2Create failed (" + describeHresult(hr) + ")";
        shutdown();
        return false;
    }

    hr = impl_->engine->CreateMasteringVoice(&impl_->master, kChannelCount, impl_->sampleRate);
    if (FAILED(hr))
    {
        impl_->error = "CreateMasteringVoice failed (" + describeHresult(hr) + ")";
        shutdown();
        return false;
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(kChannelCount);
    format.nSamplesPerSec = impl_->sampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(kChannelCount * sizeof(float));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    hr = impl_->engine->CreateSourceVoice(&impl_->source, &format, 0, 2.0f, &impl_->callback);
    if (FAILED(hr))
    {
        impl_->error = "CreateSourceVoice failed (" + describeHresult(hr) + ")";
        shutdown();
        return false;
    }

    impl_->running.store(true, std::memory_order_release);
    for (uint32_t index = 0; index < kChannelCount; ++index)
        impl_->renderNext(index);

    hr = impl_->source->Start(0);
    if (FAILED(hr))
    {
        impl_->error = "Start failed (" + describeHresult(hr) + ")";
        shutdown();
        return false;
    }
    return true;
}

void AudioDevice::shutdown()
{
    if (impl_ == nullptr)
        return;

    impl_->running.store(false, std::memory_order_release);
    if (impl_->source != nullptr)
    {
        impl_->source->Stop(0);
        impl_->source->FlushSourceBuffers();
        impl_->source->DestroyVoice();
        impl_->source = nullptr;
    }
    if (impl_->master != nullptr)
    {
        impl_->master->DestroyVoice();
        impl_->master = nullptr;
    }
    if (impl_->engine != nullptr)
    {
        impl_->engine->Release();
        impl_->engine = nullptr;
    }
    if (impl_->comInitialized)
    {
        ::CoUninitialize();
        impl_->comInitialized = false;
    }
}

bool AudioDevice::running() const
{
    return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

uint32_t AudioDevice::sampleRate() const
{
    return impl_ == nullptr ? 0 : impl_->sampleRate;
}

uint32_t AudioDevice::bufferFrames() const
{
    return kBufferFrames;
}

ClipId AudioDevice::addClip(std::shared_ptr<const AudioClip> clip)
{
    if (impl_ == nullptr)
        return kInvalidClip;
    if (impl_->running.load())
    {
        std::cerr << "[Audio] addClip must be called before start(), ignored\n";
        return kInvalidClip;
    }
    return impl_->mixer.addClip(std::move(clip));
}

VoiceId AudioDevice::play(ClipId clip, const VoiceParams& params)
{
    if (impl_ == nullptr || !impl_->running.load())
        return kInvalidVoice;

    Command command;
    command.kind = CommandKind::Play;
    command.clip = clip;
    command.params = params;
    if (!impl_->submit(command))
        return kInvalidVoice;

    return static_cast<VoiceId>(impl_->writeIndex.load(std::memory_order_relaxed) + 1u);
}

void AudioDevice::stopAll()
{
    if (impl_ == nullptr || !impl_->running.load())
        return;
    Command command;
    command.kind = CommandKind::StopAll;
    impl_->submit(command);
}

void AudioDevice::setMasterVolume(float volume)
{
    if (impl_ == nullptr || !impl_->running.load())
        return;
    Command command;
    command.kind = CommandKind::MasterVolume;
    command.volume = volume;
    impl_->submit(command);
}

MixStats AudioDevice::stats() const
{
    if (impl_ == nullptr)
        return MixStats{};
    std::lock_guard<std::mutex> lock(impl_->statsMutex);
    return impl_->lastStats;
}

const std::string& AudioDevice::lastError() const
{
    static const std::string empty;
    return impl_ == nullptr ? empty : impl_->error;
}
} // namespace audio
