
#include "manager.h"
#include "../cmd/cmd.h"
#include "../view/status.h"
#include "../tools/chronos.h"
#include "../helper/settingsHelper.h"

AudioManager* audioManager = nullptr;

AudioManager::AudioManager() : drive(*this) {
    
    volumeAdjust = 1.0 / 32768.0;
    
    rData.out = new float[65536];
    
    cosine.setData( &rData );

    muteTimer.setInterval(200);

    // prevent RESID crack on power up
    muteTimer.onFinished = [this]() {
        muteTimer.setEnabled(false);
        setVolume();
    };
}

AudioManager::~AudioManager() {
    
    delete[] rData.out;
}

auto AudioManager::setLatency() -> void {

    unsigned latency = globalSettings->get<unsigned>("audio_latency", 30u, {1u, 120u});

    // video master: the emulation delivers a whole frame of audio in one burst, then waits for
    // the display; the buffer (regulated around its centre by DRC) must hold two frames + margin
    if (videoBuffer && inputFPS > 0.0)
        latency = std::max(latency, (unsigned)std::ceil(2000.0 / inputFPS) + 10u);

    // one stream rebuild for both changes (setLatency re-creates it)
    if (latency == appliedLatency && videoBuffer == appliedClientBuffer)
        return;
    appliedLatency = latency;
    appliedClientBuffer = videoBuffer;
    audioDriver->setClientBuffer(videoBuffer);
    audioDriver->setLatency( latency );
}

auto AudioManager::setFrequency() -> void {
    unsigned frequency = globalSettings->get<unsigned>("audio_frequency_v2", 48000u, {0u, 48000u});
    audioDriver->setFrequency( frequency );
}

auto AudioManager::setSynchronize() -> void {

    bool synchronize = false;
    if (cmd->debug)
        synchronize = false;
    else synchronize = !program->isMaximumSpeed() && !videoMaster;

    if (audioDriver->hasSynchronized() == synchronize)
        return;

    audioDriver->synchronize(synchronize);
    setBufferSize();
}

auto AudioManager::setResampler() -> void {
    if (!activeEmulator)
        return;
    
    stat = activeEmulator->getStatsForSelectedRegion();
    
    double inputFrequency = stat.sampleRate;
    inputFPS = stat.fps;

    measureUiUpdate.enable = false;
    
    double monitorRatio = 1.0;

    float speed;
    bool percent;
    unsigned speedProfile;

    speedProfile = program->getSpeedBySelectedProfile(speed, percent);

    if (speedProfile != 0) {
        if (percent) {
            inputFrequency = (inputFrequency * (double) speed) / 100.0;

            inputFPS = (inputFPS * (double) speed) / 100.0;

            monitorRatio = (double) speed / 100.0;

        } else {
            inputFrequency = (inputFrequency * (double) speed) / inputFPS;

            monitorRatio = (double) speed / inputFPS;

            inputFPS = speed;
        }

        measureUiUpdate.enable = monitorRatio < 0.5;

        if (measureUiUpdate.enable) {
            measureUiUpdate.lastTS = Chronos::getTimestampInMilliseconds();
        }
    }
    
    resamplerInput = inputFrequency;
    ratio = (double)audioDriver->getFrequency() / (inputFrequency * videoClock);

    cosine.reset( ratio, stat.stereoSound ? 2 : 1 );   
    
    activeEmulator->setMonitorFpsRatio( monitorRatio );

    // check changed FPS for adaptive sync
    VideoManager::setSynchronize();
}

auto AudioManager::setBufferSize() -> void {
    if (!activeEmulator)
        return;

    stat = activeEmulator->getStatsForSelectedRegion();

    if (rewind) {
        bufferSize = (stat.sampleRate / stat.fps) * 2.0 * 1.06;
        if (bufferSize > MAX_AUDIO_BUF_SIZE) {
            bufferSize = MAX_AUDIO_BUF_SIZE;
            //fprintf(stderr, "audio buf limited\n");
        }
    } else {
        bufferSize = 2048;
    
        if (audioDriver->hasSynchronized() || dynamicRateControl)
            bufferSize = 512;
    }

    if (!stat.stereoSound)
        bufferSize >>= 1;
    
    bufferPos = 0;
}

auto AudioManager::setVolume() -> void {
    if (!activeEmulator)
        return;

    auto settings = Program::getSettings(activeEmulator);
    unsigned volume = settings->get<unsigned>("audio_volume", 100u, {0u, 100u});
    bool mute = muteTimer.enabled() || globalSettings->get<bool>("audio_mute", false);
        
    volumeAdjust = 0.0;
    
    if (!mute)
        volumeAdjust = ( (float)volume * 0.01 ) / 32768.0;
}

auto AudioManager::setInterference() -> void {
    if (!activeEmulator)
        return;

    LumaInterference& ali = lumaInterference;
    ali.enabled = false;

    if (dynamic_cast<LIBC64::Interface*>(activeEmulator)) {
        auto settings = Program::getSettings(activeEmulator);
        ali.enabled = settings->get<bool>("video_audio_interference", false);
    }
    updateInterference();

    ali.update = false;
    ali.lines = 0;
    ali.linePos = 0;
    ali.lineDiff = 0;
    ali.noiseDiff = 0;
    ali.noisePos = 0;
}

auto AudioManager::updateInterference() -> void {
    if (!activeEmulator)
        return;

    LumaInterference& ali = lumaInterference;
    ali.vol = 0.0f;

    if (dynamic_cast<LIBC64::Interface*>(activeEmulator)) {
        auto settings = Program::getSettings(activeEmulator);
        ali.vol = (float)settings->get<unsigned>("video_audio_interference_vol", 20, {0, 100}) / 2.0f;
    }
}

auto AudioManager::setTapeNoise( ) -> void {
    if (!activeEmulator)
        return;

    auto settings = Program::getSettings( activeEmulator );

    bool active = settings->get<bool>( "audio_tape_noise", false);

    unsigned volume = settings->get<unsigned>("audio_tape_noise_volume", 100u, {0u, 300u});

    activeEmulator->setTapeLoadingNoise( active ? volume : 0 );
}

auto AudioManager::resetDriveSounds() -> void {
    drive.unload();
    setDriveSounds(false);
}

auto AudioManager::setDriveSounds( bool init ) -> void {
    if (!activeEmulator)
        return;

    auto settings = Program::getSettings( activeEmulator );

    bool mixFloppySounds = settings->get<bool>("audio_floppy", false);
    bool mixTapeSounds = settings->get<bool>("audio_tape", false);

    mixDriveSounds = mixFloppySounds | mixTapeSounds;

    Emulator::Interface::MediaGroup* group = activeEmulator->getDiskMediaGroup();

    if (group) {
        if (mixFloppySounds) {
            unsigned volume = settings->get<unsigned>("audio_floppy_volume", 200u, {0u, 300u});
            unsigned volumeExt = settings->get<unsigned>("audio_floppy_volume_external", 200u, {0u, 300u});
            bool extIsSame = drive.getFloppyFolder(activeEmulator, false) == drive.getFloppyFolder(activeEmulator, true);

            if (!drive.loaded(activeEmulator, group))
                drive.readPack(activeEmulator, group);

            if (extIsSame)
                drive.unload(activeEmulator, group, true);
            else {
                if (!drive.loaded(activeEmulator, group, true))
                    drive.readPack(activeEmulator, group, true);
            }

            drive.setVolume(activeEmulator, group, (float) volume * 0.01, false);
            drive.setVolume(activeEmulator, group, (float) volumeExt * 0.01, true);
        } else
            drive.reset(group);
    }

    if (init)
        activeEmulator->enableFloppySounds(mixFloppySounds);

    group = activeEmulator->getTapeMediaGroup();

    if (group) {
        if (mixTapeSounds) {
            unsigned volume = settings->get<unsigned>("audio_tape_volume", 100u, {0u, 300u});

            if (!drive.loaded(activeEmulator, group))
                drive.readPack(activeEmulator, group);

            drive.setVolume(activeEmulator, group, (float) volume * 0.01, false);
        } else
            drive.reset(group);
    }

    if (init)
        activeEmulator->enableTapeSounds(mixTapeSounds);
}

auto AudioManager::setRateControl() -> void {

    // DRC only when the display clocks the emulation (video master / grid; internal, no setting)
    dynamicRateControl = videoMaster;
    if (!dynamicRateControl)
        rData.ratio = ratio;

    setBufferSize();
}

auto AudioManager::setVideoMaster(bool state, double clock, bool updateBuffer) -> void {
    if (!state)
        clock = 1.0;
    bool changed = state != videoMaster || clock != videoClock;
    if (state != videoMaster)
        drcFiltered = 0.0;
    videoMaster = state;
    videoClock = clock;
    if (updateBuffer)
        videoBuffer = state;
    // also follows emulated rate changes (NTSC <-> PAL); no-op when nothing changed
    setLatency();

    if (changed && resamplerInput > 0.0) {
        ratio = (double)audioDriver->getFrequency() / (resamplerInput * videoClock);
        rData.ratio = ratio;
    }
    setSynchronize();
    setRateControl();
}

auto AudioManager::resetDrcStats() -> void {
    drcStats = {};
}

auto AudioManager::power() -> void {
    rewind = false;
    setSynchronize();
    setResampler();
    setBufferSize();
    setVolume();
    setInterference();
    setDriveSounds();
    setTapeNoise();

    statistics.average = 0;
    statistics.sum = 0;
    statistics.count = 0;
    statistics.min = -1;
    statistics.max = 1;
    statistics.current = 0;
}

auto AudioManager::powerOff() -> void {
    drive.reset();
}

auto AudioManager::process( int16_t sampleLeft, int16_t sampleRight ) -> void {
    
    buffer[bufferPos++] = sampleLeft * volumeAdjust;
    
    if (stat.stereoSound)
        buffer[bufferPos++] = sampleRight * volumeAdjust;

    if (bufferPos < bufferSize)
        return;

    //if (rewind)
      //  fprintf(stderr, "rewind audio buf too small ");

    flush();
}

auto AudioManager::setRewind(bool state) -> void {
    rewind = state;
    reverseAndFlushBuffer();
    setBufferSize();
}

auto AudioManager::reverseAndFlushBuffer() -> void {
    if (!bufferPos)
        return;

    if (stat.stereoSound) {
        for (unsigned int i = 0; i < (bufferPos >> 1); i++) {
            if (i & 1)
                std::swap( buffer[i], buffer[bufferPos - i] );
            else
                std::swap( buffer[i], buffer[bufferPos - 2 - i] );
        }
    } else {
        for (unsigned int i = 0; i < (bufferPos >> 1); i++) {
            std::swap( buffer[i], buffer[bufferPos - 1 - i] );
        }
    }

    flush();
}

auto AudioManager::flush( ) -> void {

    rData.in = &buffer[0];
    rData.inputFrames = stat.stereoSound ? bufferPos >> 1 : bufferPos;

    bufferPos = 0;

    if (lumaInterference.enabled)
        applyInterference();

    if (dynamicRateControl || statistics.enable) {
        double deviation = audioDriver->getCenterBufferDeviation();

        if (statistics.enable)
            calcStatistics( (float)deviation );

        if (dynamicRateControl) {
            double control = deviation;
            if (videoMaster) {
                // the buffer level saw-tooths with the per-frame audio bursts; following it would
                // modulate the pitch at the frame rate: regulate the average only (per flush, so
                // the time constant is ~0.1-2 s depending on the SID sample rate; loop stays damped)
                drcFiltered += (deviation - drcFiltered) * 0.002;
                control = drcFiltered;
            }
            rData.ratio = ratio * (1.0 + rateDelta * control);

            auto& d = drcStats;
            double adj = rateDelta * control;
            if (!d.count) {
                d.devMin = d.devMax = deviation;
                d.adjMin = d.adjMax = adj;
            }
            d.devMin = std::min(d.devMin, deviation);
            d.devMax = std::max(d.devMax, deviation);
            d.adjMin = std::min(d.adjMin, adj);
            d.adjMax = std::max(d.adjMax, adj);
            d.devSum += deviation;
            d.count++;
        }
    }

    cosine.process();

    if (mixDriveSounds)
        drive.mixSound(rData.out, rData.outputFrames << 1);

    if ( audioDriver->expectFloatingPoint() ) {

        for (unsigned i = 0; i < (rData.outputFrames << 1); i++ )
            outBufferFloat[i] = *(rData.out + i);

        // 4 byte per channel, 8 byte per audio frame
        audioDriver->addSamples( (uint8_t*) outBufferFloat, rData.outputFrames << 3);

    } else {

        for (unsigned i = 0; i < (rData.outputFrames << 1); i++)
            outBuffer[i] = sclamp<16>( *(rData.out + i) * 32767.0 );

        // 2 byte per channel, 4 byte per audio frame
        audioDriver->addSamples( (uint8_t*) outBuffer, rData.outputFrames << 2);
    }

    if (measureUiUpdate.enable)
        checkIfUINeedsAnUpdate();
}

auto AudioManager::applyInterference() -> void {
    LumaInterference& ali = lumaInterference;
    float sample;

    static constexpr float whiteNoise[] = {
        2.0f, 1.0f, 2.0f, 1.0f,
        2.0f, 3.0f, 1.0f, 2.0f,
        1.0f, 2.0f, 2.0f, 1.0f,
        3.0f, 2.0f, 1.0f, 2.0f,
        2.0f, 1.0f, 3.0f, 1.0f,
        2.0f, 2.0f, 1.0f, 2.0f,
        1.0f, 3.0f, 2.0f, 1.0f,
        2.0f, 1.0f, 2.0f, 3.0f
    };

    if (ali.update) {
        ali.update = false;
        updateInterference();
    }

    if (ali.lines) {
        auto _noiseFreq = static_cast<unsigned>((float) stat.cyclesPerLine * 0.19f);
        float _adjust = volumeAdjust * ali.vol;

        for (unsigned s = 0; s < bufferSize; s++) {
            sample = buffer[s];

            sample += (ali.avgLines[ali.linePos] * 1.0f) * _adjust;
            sample += ((whiteNoise[ali.noisePos] * ali.avgFrame * 0.6f) ) * _adjust;

            ali.lineDiff += static_cast<unsigned>(stat.sampleIntervall);
            if (ali.lineDiff >= stat.cyclesPerLine  ) {
                ali.lineDiff -= stat.cyclesPerLine  ;
                if (++ali.linePos >= ali.lines)
                    ali.linePos = 0;
            }

            ali.noiseDiff += stat.sampleIntervall;
            if (ali.noiseDiff >= _noiseFreq) {
                ali.noiseDiff -= _noiseFreq;
                if (++ali.noisePos >= std::size( whiteNoise ))
                    ali.noisePos = 0;
            }

            buffer[s] = sample;
        }
    }
}

auto AudioManager::calcStatistics( float adjust ) -> void {
    
    statistics.sum += adjust;
    statistics.minRaw = std::max( statistics.minRaw, adjust );
    statistics.maxRaw = std::min( statistics.maxRaw, adjust );

    if (++statistics.count == 100) {
        statistics.count = 1; // average value is the first entry
        statistics.sum = statistics.sum / 100.0;
        statistics.average = 50.0 - (statistics.sum * 50.0);
        statistics.current = 50.0 - (adjust * 50.0);
        statistics.min = 50.0 - (statistics.minRaw * 50.0);
        statistics.max = 50.0 - (statistics.maxRaw * 50.0);
        statistics.minRaw = -1;
        statistics.maxRaw = 1;
        statusHandler->setDrcBufferUpdate();
    }
}

auto AudioManager::checkIfUINeedsAnUpdate() -> void {
    unsigned ts = Chronos::getTimestampInMilliseconds();

    if ((ts - measureUiUpdate.lastTS) > 20) {
        activeEmulator->requestImmediateReturn();
        measureUiUpdate.lastTS = ts;
    }
}

auto AudioManager::initDriver() -> void {
    if (audioDriver)
        delete audioDriver;

    if (cmd->noDriver) {
        audioDriver = new DRIVER::Audio;
        return;
    }

    audioDriver = DRIVER::Audio::create( getSelectedDriver() );
    audioManager->appliedLatency = 0;
    audioManager->appliedClientBuffer = false;
    audioManager->setFrequency();
    audioManager->setLatency();
    audioManager->setSynchronize();
    audioManager->setRateControl();

    if ( !audioDriver->init( 0 ) ) {
        delete audioDriver;
        audioDriver = new DRIVER::Audio;
    }
    // driver initialization could use different frequency than user requested
    audioManager->setResampler();
    audioManager->resetDriveSounds();

}

auto AudioManager::getSelectedDriver() -> std::string {
    auto curDriver = globalSettings->get<std::string>("audio_driver", "");
    auto drivers = DRIVER::Audio::available();

    for(auto& driver : drivers) {
        if(curDriver == driver) return driver;
    }
    return DRIVER::Audio::preferred();
}

auto AudioManager::mixDriveSound( Emulator::Interface::Media* media, Emulator::Interface::DriveSound driveSound, bool alternate, uint8_t data ) -> void {
    auto& stats = activeEmulator->getStatsForSelectedRegion();

    if (stats.sampleIntervall > 2) {
        if ( (stats.sampleIntervall * bufferPos) > (256 << (uint8_t)stats.stereoSound) )
            flush();
    }

    drive.addSound( activeEmulator, media, (Mixer::Drive::DriveSound)driveSound, alternate, data );
}