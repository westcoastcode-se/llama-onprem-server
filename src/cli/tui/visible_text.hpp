#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// One family's protocol markers in an assistant stream.
// find_open, open_length, and held_prefix describe where a span starts.
// classify commits to the open that is complete in the current buffer.
class Markup
{
  public:
    virtual ~Markup();

    // How a committed open is finished.
    // Paired tags drop `skip` bytes and then wait for `close`.
    // A whole span keeps the open in the buffer and measures itself with span_end.
    struct OpenSpan
    {
        std::string_view close;
        std::size_t skip = 0;
        bool whole = false;
    };

    [[nodiscard]] virtual std::size_t find_open(std::string_view text) const = 0;
    // Length of the open at text[0]. Zero when text does not start with one.
    [[nodiscard]] virtual std::size_t open_length(std::string_view text) const = 0;
    // Longest suffix of text that is a proper prefix of an open.
    [[nodiscard]] virtual std::size_t held_prefix(std::string_view text) const = 0;
    [[nodiscard]] virtual std::optional<OpenSpan> classify(std::string_view text) const = 0;
    // Length of a whole span that starts at text[0]. npos when it is unfinished.
    [[nodiscard]] virtual std::size_t span_end(std::string_view text) const;

  protected:
    Markup() = default;
};

// Case-insensitive open and close tags. The longest open at a position wins,
// so <tool_calls> is not read as <tool_call>.
class PairedTagMarkup : public Markup
{
  public:
    struct Tag
    {
        std::string_view open;
        std::string_view close;
    };

    [[nodiscard]] std::size_t find_open(std::string_view text) const override;
    [[nodiscard]] std::size_t open_length(std::string_view text) const override;
    [[nodiscard]] std::size_t held_prefix(std::string_view text) const override;
    [[nodiscard]] std::optional<OpenSpan> classify(std::string_view text) const override;

  protected:
    explicit PairedTagMarkup(std::span<const Tag> tags) : tags_(tags)
    {
    }

  private:
    [[nodiscard]] const Tag *longest_at(std::string_view text, std::size_t pos) const;

    std::span<const Tag> tags_;
};

// <tool_call><function=name><parameter=key>value</parameter></function></tool_call>
// Bonsai writes the same tags.
class QwenMarkup final : public PairedTagMarkup
{
  public:
    QwenMarkup();
};

// <｜tool▁calls▁begin｜> … <｜tool▁call▁begin｜> … <｜tool▁call▁end｜>
class DeepseekMarkup final : public PairedTagMarkup
{
  public:
    DeepseekMarkup();
};

// [TOOL_CALLS]name[ARGS]{json} through the end of the JSON value.
class DevstralMarkup final : public Markup
{
  public:
    [[nodiscard]] std::size_t find_open(std::string_view text) const override;
    [[nodiscard]] std::size_t open_length(std::string_view text) const override;
    [[nodiscard]] std::size_t held_prefix(std::string_view text) const override;
    [[nodiscard]] std::optional<OpenSpan> classify(std::string_view text) const override;
    [[nodiscard]] std::size_t span_end(std::string_view text) const override;
};

// <question>, <answer>, and <tool_response>. Shared by every model.
class ProtocolMarkup final : public PairedTagMarkup
{
  public:
    ProtocolMarkup();
};

// Assistant text as the user should see it. Every family is applied, because a
// transcript can mix turns from more than one server. The earliest span wins,
// so a marker inside another family's span stays inside that span.
class VisibleText
{
  public:
    std::string feed(std::string_view piece);
    std::string finish();

  private:
    enum class Family
    {
        None,
        Protocol,
        Qwen,
        Deepseek,
        Devstral
    };

    std::string drain(bool end);
    void stop_hiding();
    [[nodiscard]] std::array<const Markup *, 4> families() const;
    [[nodiscard]] Family family_of(const Markup *markup) const;
    [[nodiscard]] const Markup &markup(Family family) const;
    [[nodiscard]] std::size_t held(std::string_view text) const;

    ProtocolMarkup protocol_;
    QwenMarkup qwen_;
    DeepseekMarkup deepseek_;
    DevstralMarkup devstral_;
    std::string pending_;
    bool hiding_ = false;
    bool whole_ = false;
    Family family_ = Family::None;
    std::string_view close_;
};
