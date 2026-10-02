#include "session_kv_store.hpp"

#include "llama.h"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace
{

// A corrupt count must not become an allocation. Contexts in use are far below this.
constexpr uint32_t kMaxTokens = 1'048'576;

template <typename T> bool read_pod(std::istream &in, T &value)
{
    in.read(reinterpret_cast<char *>(&value), sizeof(T));
    return static_cast<bool>(in);
}

// llama_state_seq_save_file header: magic, version, token count, then the token ids.
// Reading it here avoids llama_state_seq_load_file, which would restore into the live sequence.
std::optional<uint32_t> read_token_count(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t n_tokens = 0;
    if (!in || !read_pod(in, magic) || !read_pod(in, version) || !read_pod(in, n_tokens))
    {
        return std::nullopt;
    }
    if (magic != LLAMA_STATE_SEQ_MAGIC || version != LLAMA_STATE_SEQ_VERSION || n_tokens > kMaxTokens)
    {
        return std::nullopt;
    }
    std::error_code error;
    const auto file_size = std::filesystem::file_size(path, error);
    const uint64_t header = sizeof(uint32_t) * 3;
    const uint64_t tokens_bytes = static_cast<uint64_t>(n_tokens) * sizeof(llama_token);
    if (error || static_cast<uint64_t>(file_size) < header + tokens_bytes)
    {
        return std::nullopt;
    }
    return n_tokens;
}

void remove_file(const std::filesystem::path &path)
{
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

void SessionKvStore::open(std::filesystem::path dir)
{
    if (dir.empty())
    {
        throw std::runtime_error("session directory is empty");
    }
    std::filesystem::create_directories(dir);
    dir_ = std::move(dir);
    opened_ = true;
}

bool SessionKvStore::valid_id(const std::string_view id) const
{
    if (!opened_ || id.empty() || id.size() > 32)
    {
        return false;
    }
    std::size_t i = 0;
    if (id.front() == '-')
    {
        if (id.size() == 1)
        {
            return false;
        }
        i = 1;
    }
    for (; i < id.size(); ++i)
    {
        if (id[i] < '0' || id[i] > '9')
        {
            return false;
        }
    }
    return true;
}

std::filesystem::path SessionKvStore::path_for(const std::string_view id) const
{
    return dir_ / (std::string(id) + ".kv");
}

bool SessionKvStore::save(llama_context *ctx, const std::string_view id, const std::span<const int32_t> tokens)
{
    if (ctx == nullptr || !valid_id(id) || tokens.empty() || tokens.size() > kMaxTokens)
    {
        return false;
    }
    const auto final_path = path_for(id);
    const auto tmp_path = dir_ / (std::string(id) + ".kv.tmp");
    const std::string tmp_string = tmp_path.string();
    const size_t wrote = llama_state_seq_save_file(ctx, tmp_string.c_str(), 0, tokens.data(), tokens.size());
    if (wrote == 0)
    {
        remove_file(tmp_path);
        return false;
    }
    std::error_code error;
    std::filesystem::rename(tmp_path, final_path, error);
    if (error)
    {
        remove_file(tmp_path);
        return false;
    }
    return true;
}

bool SessionKvStore::load(llama_context *ctx, const std::string_view id, std::vector<int32_t> &tokens) const
{
    tokens.clear();
    if (ctx == nullptr || !valid_id(id))
    {
        return false;
    }
    const std::optional<uint32_t> count = read_token_count(path_for(id));
    if (!count || *count == 0)
    {
        return false;
    }
    tokens.resize(*count);
    size_t loaded_count = 0;
    const std::string path = path_for(id).string();
    const size_t bytes = llama_state_seq_load_file(ctx, path.c_str(), 0, tokens.data(), tokens.size(), &loaded_count);
    if (bytes == 0 || loaded_count == 0 || loaded_count > tokens.size())
    {
        tokens.clear();
        return false;
    }
    tokens.resize(loaded_count);
    return true;
}

std::optional<size_t> SessionKvStore::token_count(const std::string_view id) const
{
    if (!valid_id(id))
    {
        return std::nullopt;
    }
    const std::optional<uint32_t> count = read_token_count(path_for(id));
    if (!count)
    {
        return std::nullopt;
    }
    return static_cast<size_t>(*count);
}

bool SessionKvStore::copy(const std::string_view from, const std::string_view to)
{
    if (!valid_id(from) || !valid_id(to) || from == to)
    {
        return false;
    }
    const auto source = path_for(from);
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error) || error)
    {
        return false;
    }
    std::filesystem::copy_file(source, path_for(to), std::filesystem::copy_options::overwrite_existing, error);
    return !error;
}

void SessionKvStore::remove(const std::string_view id)
{
    if (!valid_id(id))
    {
        return;
    }
    remove_file(path_for(id));
}
