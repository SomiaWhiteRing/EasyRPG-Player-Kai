#include "platform/emscripten/audio.h"
#include "audio_generic.h"
#include "audio_midi.h"
#include "filefinder.h"
#include "game_clock.h"
#include "output.h"
#include "player.h"
#include <emscripten.h>
#include <nlohmann/json.hpp>

namespace {
using Json = nlohmann::json;
class MixerAudio final : public GenericAudio {
public:
  explicit MixerAudio(int rate) : GenericAudio(Game_ConfigAudio{}) {
    SetFormat(rate, AudioDecoder::Format::S16, 2);
  }
  void LockMutex() const override {}
  void UnlockMutex() const override {}
};
std::unique_ptr<MixerAudio> mixer;
Filesystem_Stream::InputStream OpenAudio(const std::string& path) {
  return (!path.empty() && path.front() == '/' ? FileFinder::Root() : FileFinder::Game()).OpenInputStream(path);
}

void Command(const Json& command) {
  const auto text = command.dump();
  EM_ASM({ Module.audioCommand(JSON.parse(UTF8ToString($0))); }, text.c_str());
}
void Log(LogLevel level, const std::string& message, LogCallbackUserData) {
  EM_ASM({postMessage({type:'log', level:['error','warn','info','debug'][$0] || 'log', message:UTF8ToString($1)});},
    static_cast<int>(level), message.c_str());
}
}

WebAudio::WebAudio(const Game_ConfigAudio& cfg) : AudioInterface(cfg) { Update(); }
WebAudio::~WebAudio() { BGM_Stop(); SE_Stop(); }
bool WebAudio::IsProxy() { return !EM_ASM_INT({ return !!Module.audioOnly; }); }
AudioInterface* WebAudio::Mixer() { return mixer.get(); }
bool WebAudio::CheckFluidsynth(std::string& status) {
  char text[512] = {};
  const bool available = EM_ASM_INT({
    stringToUTF8(Module.audioCapabilities.fluidStatus, $0, $1);
    return Module.audioCapabilities.fluidsynth;
  }, text, sizeof(text));
  status = text;
  return available;
}
void WebAudio::BGM_Play(Filesystem_Stream::InputStream stream, int volume, int pitch, int fadein) {
  Update();
  Command({{"op","bgm"},{"path",stream.GetName()},{"volume",volume},{"pitch",pitch},{"fadein",fadein}});
}
void WebAudio::BGM_Stop() { Command({{"op","stop"}}); }
void WebAudio::BGM_Pause() { Command({{"op","pause"}}); }
void WebAudio::BGM_Resume() { Command({{"op","resume"}}); }
void WebAudio::BGM_Fade(int value) { Command({{"op","fade"},{"value",value}}); }
void WebAudio::BGM_Volume(int value) { Command({{"op","volume"},{"value",value}}); }
void WebAudio::BGM_Pitch(int value) { Command({{"op","pitch"},{"value",value}}); }
bool WebAudio::BGM_PlayedOnce() const { return EM_ASM_INT({return !!Module.audioState?.playedOnce;}); }
bool WebAudio::BGM_IsPlaying() const { return EM_ASM_INT({return !!Module.audioState?.playing;}); }
int WebAudio::BGM_GetTicks() const { return EM_ASM_INT({return Module.audioState?.ticks || 0;}); }
std::string WebAudio::BGM_GetType() const {
  // Decoder type names are short ASCII identifiers. Avoid allocating JS-owned strings.
  char value[32] = {};
  EM_ASM({stringToUTF8(Module.audioState?.musicType || String(), $0, $1);}, value, sizeof(value));
  return value;
}
void WebAudio::SE_Play(std::unique_ptr<AudioSeCache> se, int volume, int pitch) {
  Update();
  Command({{"op","se"},{"path",se->GetName()},{"volume",volume},{"pitch",pitch}});
}
void WebAudio::SE_Stop() { Command({{"op","se-stop"}}); }
bool WebAudio::SetManiacVolume(int volume) {
  const float gain = static_cast<float>(std::clamp(volume, 0, 1000) * 0.01);
  if (gain > 1.0f) return false;
  maniac_output_gain.store(gain);
  EM_ASM({ postMessage({type:'master-volume', gain:$0}); }, gain);
  return true;
}
int WebAudio::GetManiacVolume() const {
  return static_cast<int>(static_cast<double>(GetManiacOutputGain()) * 100);
}
void WebAudio::SetFluidsynthSoundfont(std::string_view sf) {
  cfg.soundfont.Set(ToString(sf));
  last_config.clear();
  Update();
}
void WebAudio::Update() {
  Json config = {{"op","config"},{"music",cfg.music_volume.Get()},{"sound",cfg.sound_volume.Get()},
    {"fluidsynth",cfg.fluidsynth_midi.Get()},{"wildmidi",cfg.wildmidi_midi.Get()},
    {"soundfont",cfg.soundfont.Get()},{"recommended",Player::player_config.extra_recommended_soundfont.Get()}};
  const auto text = config.dump();
  if (last_config != text) { last_config = text; Command(config); }
}

extern "C" {
EMSCRIPTEN_KEEPALIVE void web_audio_init(int rate) {
  Output::IgnorePause(true);
  Output::SetLogCallback(Log);
  FileFinder::SetGameFilesystem(FileFinder::Root().Create("/game"));
  Game_Clock::ResetFrame(Game_Clock::now());
  mixer = std::make_unique<MixerAudio>(rate);
  // Load the bundled soundfont before the game begins, outside playback deadlines.
  std::string status;
  const bool available = MidiDecoder::CheckFluidsynth(status);
  EM_ASM({Module.audioCapabilities = ({fluidsynth:!!$0, fluidStatus:UTF8ToString($1)});}, available, status.c_str());
}
EMSCRIPTEN_KEEPALIVE void web_audio_command(const char* text) {
  if (!mixer) return;
  Game_Clock::OnNextFrame(Game_Clock::now());
  const auto c = Json::parse(text);
  const auto op = c.at("op").get<std::string>();
  if (op == "bgm") {
    auto stream = OpenAudio(c.at("path").get<std::string>());
    mixer->BGM_Play(std::move(stream), c.at("volume"), c.at("pitch"), c.at("fadein"));
  } else if (op == "se") {
    const auto path = c.at("path").get<std::string>();
    auto se = AudioSeCache::GetCachedSe(path);
    if (!se) se = AudioSeCache::Create(OpenAudio(path), path);
    if (se) mixer->SE_Play(std::move(se), c.at("volume"), c.at("pitch"));
  } else if (op == "stop") mixer->BGM_Stop();
  else if (op == "pause") mixer->BGM_Pause();
  else if (op == "resume") mixer->BGM_Resume();
  else if (op == "fade") mixer->BGM_Fade(c.at("value"));
  else if (op == "volume") mixer->BGM_Volume(c.at("value"));
  else if (op == "pitch") mixer->BGM_Pitch(c.at("value"));
  else if (op == "se-stop") mixer->SE_Stop();
  else if (op == "config") {
    mixer->BGM_SetGlobalVolume(c.at("music"));
    mixer->SE_SetGlobalVolume(c.at("sound"));
    mixer->SetFluidsynthEnabled(c.at("fluidsynth"));
    mixer->SetWildMidiEnabled(c.at("wildmidi"));
    const bool changed = Player::player_config.extra_recommended_soundfont.Get() != c.at("recommended").get<bool>();
    Player::player_config.extra_recommended_soundfont.Set(c.at("recommended"));
    const auto sf = c.at("soundfont").get<std::string>();
    if (changed || mixer->GetFluidsynthSoundfont() != sf || c.value("reloadSoundfont", false)) mixer->SetFluidsynthSoundfont(sf);
  }
}
EMSCRIPTEN_KEEPALIVE void web_audio_render(uint8_t* buffer, int frames) {
  if (!mixer || frames <= 0 || frames > 4096) return;
  Game_Clock::OnNextFrame(Game_Clock::now());
  mixer->Decode(buffer, frames * 4);
  const auto type = mixer->BGM_GetType();
  EM_ASM({Module.audioState = ({playedOnce:!!$0, playing:!!$1, ticks:$2, musicType:UTF8ToString($3)});},
    mixer->BGM_PlayedOnce(), mixer->BGM_IsPlaying(), mixer->BGM_GetTicks(), type.c_str());
}
}
