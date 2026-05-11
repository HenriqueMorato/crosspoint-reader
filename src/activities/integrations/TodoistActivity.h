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

  // Step 2 of fetch: NTP + TodoistClient::fetch + populate state.
  // Called once WiFi is up.
  void proceedWithFetch();

  // Maps a FetchResult to the corresponding StrId for an error message.
  StrId fetchResultToStrId(todoist::FetchResult r) const;

  void renderLoading();
  void renderError();
  // When drawHints is false, the bottom hint bar is omitted and the list
  // expands into that space. Used by the sleep-screen snapshot, which has
  // no buttons and shouldn't waste pixels on hints. Dispatches to the
  // active design mode (Minimal or Daily).
  void renderTaskList(bool drawHints = true);
  void renderMinimal(bool drawHints);
  void renderDaily(bool drawHints);
  // Shared task-list body used by both designs. The caller passes the
  // vertical band (top, height) and horizontal band (tileX, tileWidth)
  // already adjusted for the design's header and any landscape hint
  // reserve. Returns nothing — updates _lastVisibleIndex as a side effect.
  void drawTaskRows(int contentTop, int contentHeight, int tileX, int tileWidth,
                    bool drawHints, int pageWidth);

  void captureSnapshotIfNeeded();
  bool writeSnapshotMeta(GfxRenderer::Orientation o);

  std::vector<todoist::TodoistTask> _tasks;
  State _state = State::Loading;
  // First task index in the visible window. Advances only when the cursor
  // crosses out of the window — i.e. window is cursor-driven, not free-scroll.
  int _scrollOffset = 0;
  // Highlighted task. Independent of _scrollOffset so the user can move the
  // cursor through the visible band without scrolling.
  int _selectedIndex = 0;
  // Index of the last task that fully rendered in the previous frame. Used by
  // the navigator to decide whether moving the cursor forward also has to
  // advance _scrollOffset. Updated at the end of each renderTaskList().
  int _lastVisibleIndex = -1;
  StrId _errorStrId = StrId::STR_TODOIST_FETCH_FAILED;
  uint8_t _capturedHour = 0;
  uint8_t _capturedMin = 0;
  // dd/mm at fetch time, in the user's configured timezone. Stored
  // separately from _today (which is YYYY-MM-DD and used for the
  // future-vs-today comparison) so the header can render quickly without
  // re-parsing.
  uint8_t _capturedDay = 0;
  uint8_t _capturedMonth = 0;
  // Full year and day-of-week (0=Sunday..6=Saturday) at fetch time. Used
  // by the Daily design to render the top "May 11 2026 / Monday" block;
  // the Minimal design ignores these.
  uint16_t _capturedYear = 0;
  uint8_t _capturedDow = 0;
  // "YYYY-MM-DD" snapshot of today's date at fetch time. Used to classify
  // tasks as future (dueDate strictly greater than this) so they render
  // with a distinct glyph and a date suffix.
  char _today[11] = "";
  // Snapshot of renderer orientation at onEnter() so we can restore it on
  // exit. Without this, switching activity orientation in TodoistConfig
  // leaks into HomeActivity (and corrupts its cached coverBuffer).
  GfxRenderer::Orientation _entryOrientation = GfxRenderer::Orientation::Portrait;
  ButtonNavigator _navigator;
};
