
#include "../view/message.h"
#include "settingsHelper.h"
#include <algorithm>

#include "fileHelper.h"
#include "../cmd/cmd.h"

auto SettingsHelper::undockSettings() -> bool {
    for (auto settings : settingsStorage) {
        auto guid = settings->getGuid();

        if (guid) {
            auto* emulator = (Emulator::Interface*)guid;

            settings->save( program->settingsFileFromEmuFolder(emulator->ident + "_") );
        } else {
            if (!settings->save( program->settingsFileFromEmuFolder("global_") ))
                return false;
        }
    }
    program->portable = true;
    return true;
}

auto SettingsHelper::saveSettings(bool onExit) -> void {
    bool errorShown = false;

    for (auto settings : settingsStorage) {

        auto guid = settings->getGuid();

        std::string path;

        if (guid) {
            Emulator::Interface* emulator = (Emulator::Interface*)guid;

            path = cmd->getCustomConfig(emulator);
            if (!path.empty()) {
                if (onExit)
                    continue;
            } else {
                path = globalSettings->get<std::string>(emulator->ident + "_custom_settings", "");

                if (path.empty()) {
                    path = program->settingsFileFromEmuFolder(emulator->ident + "_");

                    GUIKIT::File file(path);
                    if (!file.exists())
                        path = program->settingsFile(emulator->ident + "_");

                } else if (onExit) {
                    continue;
                } else {
                    path = FileHelper::getSettingsFolder(emulator) + path;
                }
            }
        } else {
            path = program->settingsFileFromEmuFolder("global_");

            GUIKIT::File file(path);
            if (!file.exists())
                path = program->settingsFile("global_");
        }

        if (!settings->save( path )) {
            if (!errorShown) {
                appMessage->warning(GUIKIT::String::substitute("couldn't save config file at following path: %path%", {{"%path%", path}}));
                errorShown = true;
            }
        }
    }
}

auto SettingsHelper::loadSettings() -> void {

    for(auto settings : settingsStorage) {

        auto guid = settings->getGuid();

        if (guid) {
            Emulator::Interface* emulator = (Emulator::Interface*)guid;

            std::string customConfig = cmd->getCustomConfig(emulator);

            if (!customConfig.empty()) {
                if (settings->load(customConfig)) {
                    globalSettings->set("last_used_emu", emulator->ident);
                    continue;
                } else
                    cmd->removeCustomConfig(emulator);
            }

            bool lastUsed = globalSettings->get<bool>( emulator->ident + "_load_last_settings" );

            if (lastUsed) {
                std::string path = globalSettings->get<std::string>(emulator->ident + "_custom_settings", "");
                if (!path.empty()) {
                    path = FileHelper::getSettingsFolder(emulator) + path;

                    if (settings->load(path))
                        continue;
                }
            }

            globalSettings->set<std::string>(emulator->ident + "_custom_settings", "");

            if (!settings->load(program->settingsFileFromEmuFolder(emulator->ident + "_")))
                settings->load(program->settingsFile(emulator->ident + "_"));

            unsetObsoleteConfigs(settings, emulator);

        } else {
            if (!settings->load(program->settingsFileFromEmuFolder("global_"))) {
                settings->load(program->settingsFile("global_"));
                program->portable = false;
            } else
                program->portable = true;

            unsetObsoleteConfigs(settings, nullptr);
        }
    }
}

auto SettingsHelper::unsetObsoleteConfigs(GUIKIT::Settings* settings, Emulator::Interface* emulator) -> void {
    if (emulator) {
        // Denise's CPU CRT, colour spectra, VIC glitches, legacy picture settings and their
        // per-region variants (removed in M-VK3b)
        static const std::vector<std::string> obsolete = {
            "video_spectrum", "video_crt", "video_crt_legacy", "video_new_luma", "video_phase",
            "video_phase_error", "video_phase_error_use", "video_hanover_bars", "video_hanover_bars_use",
            "video_blur", "video_blur_use", "video_scanlines", "video_scanlines_use", "video_interlace",
            "video_interlace_use", "video_luma_rise_use", "video_luma_fall_use", "prepend_yuv_shader",
            "unset_video_mode_ident"
        };
        static const std::vector<std::string> regionSuffixes = { "_pal", "_ntsc", "_spectrum", "_crtcpu", "_crtgpu" };
        std::vector<std::string> remove;
        for (auto setting : settings->getList()) {
            std::string id = setting->getIdent();
            bool old = std::find(obsolete.begin(), obsolete.end(), id) != obsolete.end();
            if (!old && GUIKIT::String::startsWith(id, "video_"))
                for (auto& suffix : regionSuffixes)
                    old |= GUIKIT::String::endsWith(id, suffix);
            if (old)
                remove.push_back(id);
        }
        for (auto& id : remove)
            settings->remove(id);

        if (!settings->get("unset_mid", false)) {
            for (auto setting : settings->getList()) {
                if (GUIKIT::String::findString(setting->getIdent(), "mouse")
                    || (setting->getIdent() == "hotkey_2")) {
                    auto parts = GUIKIT::String::split(setting->value, '|', true);
                    if (parts.size() == 5) {
                        if (parts[1].size() > 1) {
                            parts[1] = "1";
                            setting->value = GUIKIT::String::unsplit(parts, "|");
                        }
                    }
                }
            }

            settings->set("unset_mid", true);
        }
    }
}

auto SettingsHelper::forceSavingSomeGlobalSettings( ) -> void {
    GUIKIT::Settings tempSettings;
    std::string path;
    bool useEmuFolder = true;

    if (!tempSettings.load(program->settingsFileFromEmuFolder("global_"))) {
        useEmuFolder = false;
        if (!tempSettings.load(program->settingsFile("global_")))
            return;
    }

    tempSettings.set<bool>("save_settings_on_exit", false);

    for( auto emulator : emulators ) {
        std::string _emuIdent = emulator->ident;

        auto state = globalSettings->get<bool>( _emuIdent + "_load_last_settings", false );
        auto customSetting = globalSettings->get<std::string>( _emuIdent + "_custom_settings", "");
        path = globalSettings->get<std::string>( _emuIdent + "_settings_path", "");

        tempSettings.set<bool>(_emuIdent + "_load_last_settings", state);
        tempSettings.set<std::string>(_emuIdent + "_custom_settings", customSetting);
        tempSettings.set<std::string>(_emuIdent + "_settings_path", path);
    }

    tempSettings.save( useEmuFolder ? program->settingsFileFromEmuFolder("global_") : program->settingsFile("global_") );
}
