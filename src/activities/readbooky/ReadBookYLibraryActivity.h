#pragma once

#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"
#include "readbooky/ReadBookYModels.h"

namespace readbooky {

class ReadBookYLibraryActivity final : public UiListActivity {
 public:
  ReadBookYLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void render(RenderLock&& lock) override;

 protected:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  const char* headerTitle() const override;

 private:
  void loadCached();
  void refreshFromServer();
  void downloadAndOpenBook(size_t bookIndex);
  void rebuildRowItems();

  std::vector<ReadBookYBookMeta> books_;
  std::vector<std::string> rowLabels_;
  std::vector<std::string> rowSubtitles_;
  std::vector<std::string> rowValues_;
  std::vector<freeink::ui::ListItem> rowItems_;
  std::string statusBanner_;
  OptionPopup popup_;
};

}  // namespace readbooky
