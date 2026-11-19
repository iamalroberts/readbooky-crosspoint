#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "readbooky/ReadBookYModels.h"

namespace readbooky {

class ReadBookYSettingsActivity final : public UiListActivity {
 public:
  ReadBookYSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  void rebuildRows();
  void runManualSync();
  void runDictionaryInstaller();
  void showRejectedOps();

  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::vector<ReadBookYDictionaryPack> fetchedDicts_;
  OptionPopup popup_;
};

}  // namespace readbooky
