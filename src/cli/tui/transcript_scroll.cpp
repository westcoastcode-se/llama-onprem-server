#include "cli/tui/transcript_scroll.hpp"

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/util/autoreset.hpp>

#include <algorithm>
#include <memory>
#include <utility>

namespace
{

class TranscriptFrame : public ftxui::Node
{
  public:
    TranscriptFrame(ftxui::Element child, TranscriptScroll *scroll)
        : Node(ftxui::Elements{std::move(child)}), scroll_(scroll)
    {
    }

    void ComputeRequirement() override
    {
        children_[0]->ComputeRequirement();
        requirement_ = children_[0]->requirement();
        requirement_.min_y = 0;
        requirement_.focused = {};
        requirement_.flex_grow_x = 1;
        requirement_.flex_grow_y = 1;
        requirement_.flex_shrink_x = 1;
        requirement_.flex_shrink_y = 1;
    }

    void Check(Status *status) override
    {
        // Snapshot before this frame's layout passes. Early passes underestimate
        // the height and must not move the user's place.
        if (!snapped_)
        {
            frame_offset_ = scroll_->offset;
            const int seen_max = scroll_->max_offset;
            const bool tail_visible = !scroll_->hold && (scroll_->follow_bottom || seen_max <= 0 || frame_offset_ >= seen_max);
            pin_bottom_ = tail_visible && !scroll_->follow_reveal;
            snapped_ = true;
        }
        Node::Check(status);
    }

    void SetBox(ftxui::Box box) override
    {
        box_ = box;
        scroll_->viewport = box;

        const ftxui::Requirement child = children_[0]->requirement();
        // Same height measure as ftxui::yframe: max - min, not max - min + 1.
        const int external = std::max(0, box.y_max - box.y_min);
        const int internal = std::max(child.min_y, external);
        const int max_offset = std::max(0, internal - external - 1);

        int offset;
        if (pin_bottom_)
        {
            offset = max_offset;
        }
        else if (scroll_->follow_reveal && child.focused.enabled)
        {
            const ftxui::Box &focused = child.focused.box;
            const int span = focused.y_max - focused.y_min;
            offset = focused.y_min - external / 2 + span / 2;
        }
        else if (scroll_->follow_reveal)
        {
            offset = max_offset;
        }
        else
        {
            offset = frame_offset_;
        }
        offset = std::clamp(offset, 0, max_offset);
        if (scroll_->hold)
        {
            scroll_->follow_bottom = false;
        }
        else if (!scroll_->follow_reveal)
        {
            scroll_->follow_bottom = offset >= max_offset;
        }
        scroll_->offset = offset;
        scroll_->max_offset = max_offset;

        ftxui::Box child_box = box;
        child_box.y_min = box.y_min - offset;
        child_box.y_max = box.y_min + internal - offset;
        children_[0]->SetBox(child_box);
    }

    void Render(ftxui::Screen &screen) override
    {
        const ftxui::AutoReset<ftxui::Box> stencil(&screen.stencil, ftxui::Box::Intersection(box_, screen.stencil));
        children_[0]->Render(screen);
    }

  private:
    TranscriptScroll *scroll_;
    int frame_offset_ = 0;
    bool pin_bottom_ = false;
    bool snapped_ = false;
};

} // namespace

ftxui::Element transcript_scroll(ftxui::Element child, TranscriptScroll &scroll)
{
    return std::make_shared<TranscriptFrame>(std::move(child), &scroll);
}

bool transcript_scroll_wheel(TranscriptScroll &scroll, bool up, int step)
{
    if (scroll.max_offset <= 0 || step <= 0)
    {
        return false;
    }
    const int previous = scroll.offset;
    const bool was_following = scroll.follow_bottom || scroll.follow_reveal;
    if (up)
    {
        scroll.follow_bottom = false;
        scroll.follow_reveal = false;
        scroll.hold = true;
        scroll.offset = std::max(0, scroll.offset - step);
        return scroll.offset != previous || was_following;
    }
    if (scroll.follow_bottom)
    {
        return false;
    }
    scroll.follow_reveal = false;
    scroll.offset = std::min(scroll.max_offset, scroll.offset + step);
    if (scroll.offset >= scroll.max_offset)
    {
        scroll.follow_bottom = true;
        scroll.hold = false;
    }
    return scroll.offset != previous || was_following;
}
