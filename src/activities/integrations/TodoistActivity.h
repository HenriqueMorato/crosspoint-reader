#pragma once

#include "activities/Activity.h"
#include "MappedInputManager.h"
#include "integrations/todoist/TodoistClient.h"
#include "integrations/todoist/TodoistTask.h"
#include "util/ButtonNavigator.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <vector>

class TodoistActivity : public Activity {
 public:
  TodoistActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("Todoist", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;

 private:
  enum class State {
    Loading,        // initial fetch in progress / awaiting WiFi
    ShowingTasks,
    ShowingError,
  };

  // Step 1 of fetch: ensure WiFi. If already connected, calls
  // proceedWithFetch() directly. Otherwise launches WifiSelectionActivity
  // and re-enters proceedWithFetch() on its result.
  void startFetch();

  // Step 2 of fetch: NTP + TodoistClient::fetchToday + populate state.
  // Called once WiFi is up.
  void proceedWithFetch();

  // Maps a FetchResult to the corresponding StrId for an error message.
  StrId fetchResultToStrId(todoist::FetchResult r) const;

  void renderLoading();
  void renderError();
  void renderTaskList();

  void captureSnapshotIfNeeded();
  bool writeSnapshotMeta(GfxRenderer::Orientation o);

  std::vector<todoist::TodoistTask> _tasks;
  State _state = State::Loading;
  int _scrollOffset = 0;
  StrId _errorStrId = StrId::STR_TODOIST_FETCH_FAILED;
  uint8_t _capturedHour = 0;
  uint8_t _capturedMin = 0;
  ButtonNavigator _navigator;
};
