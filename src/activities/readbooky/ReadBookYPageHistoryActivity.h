#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "readbooky/ReadBookYModels.h"

namespace readbooky {

class ReadBookYPageHistoryActivity final : public UiListActivity {
 public:
  ReadBookYPageHistoryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ReadBookYSidecar sidecar);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  void loadCached();
  void refreshFromServer();
  void rebuildRows();
  void openItem(size_t idx);

  ReadBookYSidecar sidecar_;
  std::vector<ReadBookYPageItem> items_;
  std::vector<std::string> rowLabels_;
  std::vector<std::string> rowSubtitles_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::string statusNote_;
  OptionPopup popup_;
};

}  // namespace readbooky
