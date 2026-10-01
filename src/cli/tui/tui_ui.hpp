#pragma once

#include "cli/tui/theme.hpp"
#include "cli/tui/transcript_scroll.hpp"
#include "cli/tui/ui.hpp"

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftxui
{
class ScreenInteractive;
}

struct Block
{
    std::string kind;
    std::string text;
    std::string caption = {};
    bool expanded = false;
};

// Full-screen session. Transcript updates, the composer, and the menus are
// separate translation units. Drawing and key handling stay in tui.cpp.
class TuiUi final : public AgentUi
{
  public:
    explicit TuiUi(Theme theme) : theme(std::move(theme))
    {
    }

    Theme theme;
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<Block> blocks;
    bool open = false;
    std::string status = "callisto";
    int context_used = 0;
    int context_size = 0;
    std::string input;
    // Byte offset into input, always on a UTF-8 boundary.
    std::size_t cursor = 0;
    bool line_ready = false;
    std::string line;
    bool quit = false;
    bool subagent_live = false;
    bool asking = false;
    bool question_mode = false;
    bool pick_mode = false;
    int ask_pick = -1;
    std::string ask_title;
    std::vector<std::string> menu;
    int menu_index = 0;
    bool ask_done = false;
    Ask ask_result = Ask::Deny;
    std::string ask_text;
    ftxui::ScreenInteractive *screen = nullptr;
    bool ready = false;
    // Hit boxes from the last frame. Empty until a folding row is drawn.
    std::vector<ftxui::Box> think_boxes;
    std::size_t revealed = static_cast<std::size_t>(-1);
    TranscriptScroll scroll;


    std::size_t latest(std::string_view kind) const;
    void toggle_fold(std::size_t index);
    void wake();
    void set_status(std::string text) override;
    void set_context(int used, int size) override;
    void note(std::string text) override;
    void set_subagent_live(bool on) override;
    void begin(std::string kind) override;
    void append(std::string text) override;
    void end() override;
    void expand(std::string_view kind) override;
    void collapse(std::string_view kind) override;
    void caption(std::string text) override;
    void show_system(std::string text) override;
    Ask ask(std::string title, std::string) override;
    std::optional<std::string> question(std::string prompt, std::vector<std::string> choices) override;
    std::optional<std::size_t> choose(std::string prompt, std::vector<std::string> choices, std::size_t selected) override;
    std::string read_line() override;
    ftxui::Element transcript();
    bool on_event(ftxui::Event event);
    ftxui::Element render();
};
