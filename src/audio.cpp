/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

// Headers
#include "audio.h"
#include "audio_midi.h"
#include "system.h"
#include "baseui.h"
#include "player.h"
#include "game_clock.h"
#include <algorithm>
#ifdef _WIN32
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <mmsystem.h>
#include <lcf/scope_guard.h>

namespace {
int ManiacMixerVolume(int* requested) {
	HMIXER mixer = nullptr;
	if (mixerOpen(&mixer, 0, 0, 0, 0) != MMSYSERR_NOERROR) return -1;
	const auto cleanup = lcf::makeScopeGuard([&]() { mixerClose(mixer); });
	const auto object = reinterpret_cast<HMIXEROBJ>(mixer);
	MIXERLINEW line{};
	line.cbStruct = sizeof(line);
	line.dwComponentType = MIXERLINE_COMPONENTTYPE_SRC_WAVEOUT;
	if (mixerGetLineInfoW(object, &line, MIXER_GETLINEINFOF_COMPONENTTYPE) != MMSYSERR_NOERROR) return -1;
	MIXERCONTROLW control{};
	MIXERLINECONTROLSW controls{};
	controls.cbStruct = sizeof(controls);
	controls.dwLineID = line.dwLineID;
	controls.dwControlType = MIXERCONTROL_CONTROLTYPE_VOLUME;
	controls.cControls = 1;
	// 250921 passes the ANSI size to the W API. Preserve its failure behavior.
	controls.cbmxctrl = sizeof(MIXERCONTROLA);
	controls.pamxctrl = &control;
	if (mixerGetLineControlsW(object, &controls, MIXER_GETLINECONTROLSF_ONEBYTYPE) != MMSYSERR_NOERROR) return -1;
	MIXERCONTROLDETAILS_UNSIGNED value{};
	MIXERCONTROLDETAILS details{};
	details.cbStruct = sizeof(details);
	// The fixed getter uses its zero-initialized ID; only the setter uses the
	// control returned above. Ordinary Windows rejects the preceding size.
	details.dwControlID = requested ? control.dwControlID : 0;
	details.cChannels = 1;
	details.cbDetails = sizeof(value);
	details.paDetails = &value;
	if (requested) {
		value.dwValue = static_cast<DWORD>(std::clamp(*requested, 0, 1000) * (65535.0 / 1000));
		return mixerSetControlDetails(object, &details, MIXER_SETCONTROLDETAILSF_VALUE) == MMSYSERR_NOERROR ? 0 : -1;
	}
	if (mixerGetControlDetailsW(object, &details, MIXER_GETCONTROLDETAILSF_VALUE) != MMSYSERR_NOERROR) return -1;
	return static_cast<int>(value.dwValue * (1000.0 / 65535) / 10);
}

int ManiacSessionVolume(int* requested) {
	const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(initialized)) return ManiacMixerVolume(requested);
	IMMDeviceEnumerator* enumerator = nullptr;
	IMMDevice* device = nullptr;
	IAudioSessionManager* manager = nullptr;
	ISimpleAudioVolume* volume = nullptr;
	const auto cleanup = lcf::makeScopeGuard([&]() {
		if (volume) volume->Release();
		if (manager) manager->Release();
		if (device) device->Release();
		if (enumerator) enumerator->Release();
		if (SUCCEEDED(initialized)) CoUninitialize();
	});
	if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
		__uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)))
		|| FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))
		|| FAILED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
			reinterpret_cast<void**>(&manager)))
		|| FAILED(manager->GetSimpleAudioVolume(nullptr, 0, &volume))) return ManiacMixerVolume(requested);
	if (requested) {
		const float gain = static_cast<float>(std::clamp(*requested, 0, 1000) * 0.01);
		return SUCCEEDED(volume->SetMasterVolume(gain, nullptr)) ? 0 : -1;
	}
	float gain = 0;
	if (FAILED(volume->GetMasterVolume(&gain))) return -1;
	return static_cast<int>(static_cast<double>(gain) * 100);
}
}
#endif
#ifdef EMSCRIPTEN
#include "platform/emscripten/audio.h"
#endif

AudioInterface& Audio() {
#ifdef EMSCRIPTEN
	if (auto* mixer = WebAudio::Mixer()) return *mixer;
#endif
	static Game_ConfigAudio cfg;
	static EmptyAudio default_(cfg);
#ifdef SUPPORT_AUDIO
	if (!Player::no_audio_flag && DisplayUi)
		return DisplayUi->GetAudio();
#endif
	return default_;
}

void EmptyAudio::BGM_Play(Filesystem_Stream::InputStream, int, int, int) {
	bgm_starttick = Player::GetFrames();
	playing = true;
}

void EmptyAudio::BGM_Stop() {
	playing = false;
}

int EmptyAudio::BGM_GetTicks() const {
	if (!playing) {
		return 0;
	}

	// Time since BGM_Play was called, works for everything except MIDI
	return (Player::GetFrames() - bgm_starttick + 1) / Game_Clock::GetTargetGameFps();
}

void EmptyAudio::vGetConfig(Game_ConfigAudio&) const {
	// Not supported. The audio menu is disabled.
}

bool EmptyAudio::BGM_PlayedOnce() const {
	// 5 seconds, arbitrary
	return BGM_GetTicks() > (Game_Clock::GetTargetGameFps() * 5);
}

EmptyAudio::EmptyAudio(const Game_ConfigAudio& cfg) : AudioInterface(cfg) {

}

AudioInterface::AudioInterface(const Game_ConfigAudio& cfg) : cfg(cfg) {

}

Game_ConfigAudio AudioInterface::GetConfig() const {
	auto acfg = cfg;
	acfg.Hide();

#if !defined(HAVE_FLUIDSYNTH) && !defined(HAVE_FLUIDLITE)
	acfg.fluidsynth_midi.SetOptionVisible(false);
	acfg.soundfont.SetOptionVisible(false);
#endif
#ifndef HAVE_LIBWILDMIDI
	acfg.wildmidi_midi.SetOptionVisible(false);
#endif
#ifndef HAVE_NATIVE_MIDI
	acfg.native_midi.SetOptionVisible(false);
#endif
#ifndef WANT_FMMIDI
	acfg.fmmidi_midi.SetOptionVisible(false);
#endif

#ifdef __ANDROID__
	// FIXME: URI encoded SAF paths are not supported
	acfg.soundfont.SetOptionVisible(false);
#endif

	vGetConfig(acfg);
	return acfg;
}

int AudioInterface::BGM_GetGlobalVolume() const {
	return cfg.music_volume.Get();
}

void AudioInterface::BGM_SetGlobalVolume(int volume) {
	cfg.music_volume.Set(volume);
}

int AudioInterface::SE_GetGlobalVolume() const {
	return cfg.sound_volume.Get();
}

void AudioInterface::SE_SetGlobalVolume(int volume) {
	cfg.sound_volume.Set(volume);
}

bool AudioInterface::SetManiacVolume(int volume) {
#ifdef _WIN32
	return ManiacSessionVolume(&volume) == 0;
#else
	return false;
#endif
}

int AudioInterface::GetManiacVolume() const {
#ifdef _WIN32
	return ManiacSessionVolume(nullptr);
#else
	return -1;
#endif
}

bool AudioInterface::GetFluidsynthEnabled() const {
	return cfg.fluidsynth_midi.Get();
}

void AudioInterface::SetFluidsynthEnabled(bool enable) {
	cfg.fluidsynth_midi.Set(enable);
}

bool AudioInterface::GetWildMidiEnabled() const {
	return cfg.wildmidi_midi.Get();
}

void AudioInterface::SetWildMidiEnabled(bool enable) {
	cfg.wildmidi_midi.Set(enable);
}

bool AudioInterface::GetNativeMidiEnabled() const {
	return cfg.native_midi.Get();
}

void AudioInterface::SetNativeMidiEnabled(bool enable) {
	cfg.native_midi.Set(enable);
}

std::string AudioInterface::GetFluidsynthSoundfont() const {
	return cfg.soundfont.Get();
}

void AudioInterface::SetFluidsynthSoundfont(std::string_view sf) {
	cfg.soundfont.Set(ToString(sf));
	MidiDecoder::ChangeFluidsynthSoundfont(sf);
}
