#ifndef EP_WEB_AUDIO_H
#define EP_WEB_AUDIO_H

#include "../../audio.h"

// Commands and playback status cross a MessagePort. The native mixer runs in
// a separate WASM instance, so slow game frames cannot stop its PCM production.
class WebAudio final : public AudioInterface {
public:
  explicit WebAudio(const Game_ConfigAudio& cfg);
  ~WebAudio() override;
  void BGM_Play(Filesystem_Stream::InputStream stream, int volume, int pitch, int fadein) override;
  void BGM_Stop() override;
  void BGM_Pause() override;
  void BGM_Resume() override;
  bool BGM_PlayedOnce() const override;
  bool BGM_IsPlaying() const override;
  int BGM_GetTicks() const override;
  void BGM_Fade(int fade) override;
  void BGM_Volume(int volume) override;
  void BGM_Pitch(int pitch) override;
  std::string BGM_GetType() const override;
  void SE_Play(std::unique_ptr<AudioSeCache> se, int volume, int pitch) override;
  void SE_Stop() override;
  bool SetManiacVolume(int volume) override;
  int GetManiacVolume() const override;
  void Update() override;
  void vGetConfig(Game_ConfigAudio&) const override {}
  void SetFluidsynthSoundfont(std::string_view sf) override;

  static bool IsProxy();
  static AudioInterface* Mixer();
  static bool CheckFluidsynth(std::string& status);
private:
  std::string last_config;
};
#endif
