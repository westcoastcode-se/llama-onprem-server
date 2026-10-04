#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// llama_state_seq_get_data writes a uint32 magic and an int32 sequence id before the bytes
// llama_state_seq_save_file stores after its token header. Those 8 bytes are not part of the file.
inline constexpr std::size_t kSeqMemoryPrefix = sizeof(std::uint32_t) + sizeof(std::int32_t);

struct llama_context;

// Files named <session id>.kv. llama_state_seq_save_file writes the sequence state and the token
// ids straight to the file, one tensor slice at a time, so the whole snapshot is not a RAM buffer.
// An id is one path segment: letters, digits, '.', '_', and '-'. Decimal session ids stay valid.
// A name cannot escape the directory.
class SessionKvStore
{
  public:
    SessionKvStore() = default;

    // Create the directory. Throws std::runtime_error when the path cannot be used.
    void open(std::filesystem::path dir);

    [[nodiscard]] bool is_open() const
    {
        return opened_;
    }

    [[nodiscard]] const std::filesystem::path &dir() const
    {
        return dir_;
    }

    // Stream sequence 0 to <id>.kv. llama_state_seq_save_file truncates its path immediately, so the
    // write goes to <id>.kv.tmp and replaces <id>.kv only after it succeeds. False leaves the previous file.
    bool save(llama_context *ctx, std::string_view id, std::span<const int32_t> tokens);

    // Stream <id>.kv back into sequence 0. tokens receives the ids stored in the file.
    // False when the file is missing, foreign, or rejected. A rejected file may already have cleared sequence 0.
    bool load(llama_context *ctx, std::string_view id, std::vector<int32_t> &tokens) const;

    // Token count from the file header. Empty when the session has no readable file.
    [[nodiscard]] std::optional<size_t> token_count(std::string_view id) const;

    // Copy one session file onto another id. False when the source is missing.
    bool copy(std::string_view from, std::string_view to);

    // Write a snapshot taken with llama_state_seq_get_data. The first 8 bytes are that
    // function's magic and sequence id; the file stores the same body llama_state_seq_save_file writes.
    // False leaves the previous file in place.
    bool save_captured(std::string_view id, std::span<const int32_t> tokens, std::span<const uint8_t> state);

    // Delete parked files older than max_age_days while the directory is over max_bytes.
    // max_age_days < 0 deletes nothing. Returns the ids removed, oldest first.
    [[nodiscard]] std::vector<std::string> trim_expired(int max_age_days, uint64_t max_bytes);

    void remove(std::string_view id);

  private:
    [[nodiscard]] bool valid_id(std::string_view id) const;
    [[nodiscard]] std::filesystem::path path_for(std::string_view id) const;

    std::filesystem::path dir_;
    bool opened_ = false;
};
