/**
 * XtcReaderActivity.h
 *
 * XTC ebook reader activity for CrossPoint Reader
 * Displays pre-rendered XTC pages on e-ink display
 */

#pragma once

#include <Xtc.h>

#include <memory>
#include <string>
#include <utility>

#include "activities/Activity.h"

class XtcReaderActivity final : public Activity {
  std::shared_ptr<Xtc> xtc;

  uint32_t currentPage = 0;
  int pagesUntilFullRefresh = 0;

  // Cached raw page data, reused across re-renders of the same page (e.g. GlobalMenu
  // re-composites) so we don't re-read from SD or re-allocate every frame.
  std::unique_ptr<uint8_t[]> pageBuffer_;
  size_t pageBufferCap_ = 0;  // allocated capacity of pageBuffer_
  int loadedPageFor_ = -1;    // page index currently held in pageBuffer_ (-1 = none)

  enum class StatusBarOverlayPosition { Bottom, Top };
  struct StatusBarInfo {
    int currentPage;
    int pageCount;
    std::string title;
  };

  void renderPage();
  void renderStatusBarOverlay(StatusBarOverlayPosition position) const;
  StatusBarInfo getStatusBarInfo() const;
  void saveProgress() const;
  void loadProgress();

 public:
  explicit XtcReaderActivity(
    GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Xtc> xtc
  )
      : Activity("XtcReader", renderer, mappedInput), xtc(std::move(xtc)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  std::optional<GlobalMenuConfig> getGlobalMenuConfig() override {
    return GlobalMenuConfig{};
  }
  std::vector<MenuRegistryEntry> getGlobalMenuEntries() override;
  ScreenshotInfo getScreenshotInfo() const override;

 private:
  void openChapterSelection();
};
