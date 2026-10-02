#include "../jobs/jobs.hpp"
#include "../agent/model_adapter.hpp"
#include "../agent/response_parse.hpp"
#include "api/errors.hpp"
#include "common/log.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

// Task
/**
 * Move out on_finished under lock so the callback can run without holding mutex.
 */
[[nodiscard]] std::move_only_function<void(const std::shared_ptr<Task> &)> Task::take_on_finished()
{
    std::lock_guard lock(mutex);
    return std::exchange(on_finished, nullptr);
}

[[nodiscard]] JobState Task::get_state() const
{
    std::lock_guard lock(mutex);
    return state;
}

/**
 * Set the error message
 *
 * @param msg The error message
 */
void Task::set_error_state(std::string msg, std::string code)
{
    std::lock_guard lock(mutex);
    error = std::move(msg);
    error_code = std::move(code);
    state = JobState::Error;
    finished_at = std::chrono::steady_clock::now();
}

/**
 * Set the result from the LLM
 *
 * @param text The resulting text
 * @param new_state The state of the job
 */
void Task::set_result(std::string text, const JobState new_state, ParsedAssistantActions actions)
{
    std::lock_guard lock(mutex);
    result = std::move(text);
    reasoning = std::move(actions.reasoning);
    state = new_state;
    tool_calls = std::move(actions.tool_calls);
    question = std::move(actions.question);

    // Job is finished. The token stream stays open until release_stream(), so a
    // session can record context usage before the client observes done.
    if (new_state.is_finished())
    {
        finished_at = std::chrono::steady_clock::now();
    }
}

void Task::release_stream()
{
    if (!buffer)
    {
        return;
    }
    if (is_cancel_requested() || get_state() == JobState::Cancelled)
    {
        buffer->cancel();
    }
    else
    {
        buffer->set_done();
    }
}

[[nodiscard]] std::string Task::get_result() const
{
    std::lock_guard lock(mutex);
    return result;
}

[[nodiscard]] std::string Task::get_error() const
{
    std::lock_guard lock(mutex);
    return error;
}

void Task::request_cancel()
{
    cancel_requested.store(true, std::memory_order_relaxed);
    if (buffer)
    {
        buffer->cancel();
    }
}

[[nodiscard]] bool Task::is_cancel_requested() const
{
    return cancel_requested.load(std::memory_order_relaxed);
}

// Latest KV size for this job's session. Written on the worker, read by the token stream.
void Task::note_context(int used, int size)
{
    context_used_.store(std::max(0, used), std::memory_order_relaxed);
    context_size_.store(std::max(0, size), std::memory_order_relaxed);
}

[[nodiscard]] int Task::context_used() const
{
    return context_used_.load(std::memory_order_relaxed);
}

[[nodiscard]] int Task::context_size() const
{
    return context_size_.load(std::memory_order_relaxed);
}

[[nodiscard]] MessageStatusResponse Task::to_status_unsafe() const
{
    MessageStatusResponse r;
    r.key = key;
    r.state = state;
    r.done = state.is_finished();
    r.content = result;
    r.error = error;
    r.error_code = error_code;
    r.reasoning = reasoning;
    r.tool_calls = tool_calls;
    r.question = question;
    return r;
}

[[nodiscard]] MessageStatusResponse Task::to_status() const
{
    std::lock_guard lock(mutex);
    MessageStatusResponse r;
    r.key = key;
    r.state = state;
    r.done = state.is_finished();
    r.content = result;
    r.error = error;
    r.error_code = error_code;
    r.reasoning = reasoning;
    r.tool_calls = tool_calls;
    r.question = question;
    return r;
}

/**
 * Set this task as cancelled
 */
void Task::set_cancelled()
{
    std::lock_guard lock(mutex);
    state = JobState::Cancelled;
    finished_at = std::chrono::steady_clock::now();
    buffer->cancel();
}

void Task::set_running()
{
    std::lock_guard lock(mutex);
    state = JobState::Running;
}

int Jobs::context_size() const
{
    return std::max(0, engine_.get_config().n_ctx);
}

Jobs::Jobs(LlamaEngine &engine, const ModelAdapter &adapter) : engine_(engine), adapter_(adapter)
{
    worker_ = std::jthread([this](std::stop_token stop) { worker_loop(std::move(stop)); });
}

Jobs::~Jobs()
{
    stop();
}

void Jobs::stop()
{
    request_shutdown();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void Jobs::request_shutdown()
{
    worker_.request_stop();
    std::vector<std::shared_ptr<Task>> tasks;
    {
        std::lock_guard lock(mutex_);
        if (current_task_)
        {
            tasks.push_back(current_task_);
        }
        tasks.reserve(tasks.size() + tasks_.size());
        for (const auto &entry : tasks_)
        {
            tasks.push_back(entry.second);
        }
    }
    for (const auto &task : tasks)
    {
        if (task)
        {
            task->request_cancel();
        }
    }
    cv_.notify_all();
}

void Jobs::unsafe_gc()
{
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(tasks_, [&](const auto &entry) {
        const auto &task = entry.second;
        if (!task->get_state().is_finished())
        {
            return false;
        }
        const auto finished_at = task->finished_at;
        return finished_at.time_since_epoch().count() != 0 && now - finished_at > kFinishedTtl;
    });
}

std::shared_ptr<Task> Jobs::submit(MessagesRequest &&request,
                              std::move_only_function<void(const std::shared_ptr<Task> &)> on_finished)
{
    const auto task = std::make_shared<Task>();
    task->request = std::move(request);
    task->on_finished = std::move(on_finished);
    return enqueue(task);
}

std::shared_ptr<Task> Jobs::enqueue(std::shared_ptr<Task> task)
{
    if (!task)
    {
        return {};
    }

    std::lock_guard lock(mutex_);

    // Opportunistic GC
    unsafe_gc();

    task->state = JobState::Queued;
    tasks_[task->key] = task;
    queue_.push_back(task->key);
    cv_.notify_one();
    return task;
}

std::shared_ptr<Task> Jobs::get_task(const JobKey key) const
{
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(key);
    if (it == tasks_.end())
    {
        return {};
    }
    return it->second;
}

namespace
{

void notify_finished(const std::shared_ptr<Task> &task)
{
    // Invoke without holding task->mutex so session callbacks can call Task getters.
    auto cb = task->take_on_finished();
    if (!cb)
    {
        return;
    }

    try
    {
        cb(task);
    }
    catch (const std::exception &e)
    {
        log_error("on_finished resulted in an unhandled error: ", e.what());
    }
}

void finish_task(const std::shared_ptr<Task> &task)
{
    notify_finished(task);
    task->release_stream();
}

} // namespace

std::shared_ptr<Task> Jobs::cancel(JobKey const id)
{
    log_info("Task(", id, ") | cancelling task");
    std::shared_ptr<Task> task;
    bool notify_immediately = false;
    {
        std::lock_guard lock(mutex_);
        const auto it = tasks_.find(id);
        if (it == tasks_.end())
        {
            return {};
        }
        task = it->second;

        if (const auto qit = std::ranges::find(queue_, task->key); qit != queue_.end())
        {
            queue_.erase(qit);
            notify_immediately = true;
        }
    }

    // If notify_immediately is true then the task has not been fetched by the job runner yet
    if (notify_immediately)
    {
        task->request_cancel();
        task->set_cancelled();
        finish_task(task);
    }
    else
    {
        task->request_cancel();
    }

    return task;
}

void Jobs::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}

void Jobs::release_session(const std::string &session_id)
{
    if (session_id.empty())
    {
        return;
    }
    // The worker is the only thread that may touch the context. A queued release would
    // never run once this thread has left the loop, so delete the file here.
    if (on_worker_thread())
    {
        engine_.release_session(session_id);
        return;
    }
    std::lock_guard lock(mutex_);
    pending_session_releases_.push_back(session_id);
    cv_.notify_one();
}

void Jobs::set_session_cache_hook(std::function<void()> hook)
{
    std::lock_guard lock(mutex_);
    session_cache_hook_ = std::move(hook);
}

bool Jobs::on_worker_thread() const
{
    return worker_.get_id() == std::this_thread::get_id();
}

std::string Jobs::active_session_id() const
{
    return engine_.active_session_id();
}

void Jobs::note_session_cache_changed()
{
    std::function<void()> hook;
    {
        std::lock_guard lock(mutex_);
        hook = session_cache_hook_;
    }
    if (!hook)
    {
        return;
    }
    try
    {
        hook();
    }
    catch (const std::exception &error)
    {
        log_error("session cache hook failed: ", error.what());
    }
}

void Jobs::clone_session(const std::string &from, const std::string &to)
{
    if (from.empty() || to.empty() || from == to)
    {
        return;
    }
    std::lock_guard lock(mutex_);
    pending_session_clones_.emplace_back(from, to);
    cv_.notify_one();
}

Jobs::NextWork Jobs::wait_next(const std::stop_token &stop)
{
    std::unique_lock lock(mutex_);
    std::stop_callback on_stop(stop, [&] { cv_.notify_all(); });
    cv_.wait(lock, [&] {
        return stop.stop_requested() || !queue_.empty() || !pending_session_releases_.empty() ||
               !pending_session_clones_.empty();
    });
    NextWork work;
    if (stop.stop_requested())
    {
        return work;
    }
    work.clones.swap(pending_session_clones_);
    work.releases.swap(pending_session_releases_);
    while (!queue_.empty())
    {
        const auto key = queue_.front();
        queue_.pop_front();
        const auto it = tasks_.find(key);
        if (it == tasks_.end())
        {
            continue;
        }
        work.task = it->second;
        break;
    }
    return work;
}

void Jobs::worker_loop(std::stop_token stop)
{
    while (!stop.stop_requested())
    {
        NextWork work = wait_next(stop);
        if (stop.stop_requested())
        {
            break;
        }
        for (const auto &[from, to] : work.clones)
        {
            engine_.clone_session(from, to);
        }
        if (!work.clones.empty())
        {
            note_session_cache_changed();
        }
        for (const auto &session_id : work.releases)
        {
            engine_.release_session(session_id);
        }

        std::shared_ptr<Task> task = std::move(work.task);
        // No tasks in the queue
        if (!task)
        {
            continue;
        }

        // Is the task cancelled?
        if (task->is_cancel_requested())
        {
            task->set_cancelled();
            finish_task(task);
            continue;
        }

        // Task is now running!
        {
            std::lock_guard lock(mutex_);
            current_task_ = task;
        }
        task->set_running();

        // One leading system message. Later system roles are folded in, because the
        // Qwen and Bonsai templates reject a system message that is not first.
        std::string system = task->request.system;
        std::vector<ChatMessage> msgs;
        msgs.reserve(task->request.messages.size() + 1);
        for (const ChatMessage &message : task->request.messages)
        {
            if (message.role == ChatMessage::ROLE_SYSTEM)
            {
                if (!message.content.empty())
                {
                    if (!system.empty())
                    {
                        system.push_back('\n');
                    }
                    system += message.content;
                }
                continue;
            }
            msgs.push_back(message);
        }
        if (!system.empty())
        {
            msgs.insert(msgs.begin(),
                        ChatMessage{.role = std::string(ChatMessage::ROLE_SYSTEM), .content = std::move(system), .reasoning_content = {}});
        }

        // Start stream the LLM response
        const auto buffer = task->buffer;
        // Only families whose template opens <think> get that tag in the stream.
        // Devstral never writes </think>, so the tag would keep its tool calls in the thinking row.
        const bool opened_think = engine_.get_config().reasoning && adapter_.prompt_opens_think();
        const auto publish_context = [&] {
            const std::string &id = task->request.session_id;
            int used = engine_.session_token_count(id);
            const int live = engine_.get_used_context();
            // chat() just wrote the only live sequence. If the session id does not
            // match that sequence, the parked-file count is 0 and the offer never runs.
            if (live > used && (engine_.active_session_id() == id || used == 0))
            {
                used = live;
            }
            const int size = engine_.get_context_size();
            if (size > 0 && used > size)
            {
                used = size;
            }
            task->note_context(used, size);
        };
        try
        {
            LlamaRequest call;
            call.should_stop = [task] { return task->is_cancel_requested(); };
            call.max_tokens = task->request.max_tokens;
            call.session_id = task->request.session_id;
            call.tools = task->request.tools;
            call.on_prompt = [&] { publish_context(); };
            bool think_prefix_sent = false;
            call.token_cb = [&](std::string &&piece) {
                publish_context();
                if (task->is_cancel_requested())
                {
                    return false;
                }
                // The template already opened <think> in the prompt, so the sample does not
                // repeat it. The client only hides thinking when it sees that opener.
                if (opened_think && !think_prefix_sent)
                {
                    think_prefix_sent = true;
                    buffer->push("<think>\n");
                }
                buffer->push(std::move(piece));
                return true;
            };
            std::string response = engine_.chat(msgs, call);
            publish_context();
            buffer->set_full_result(std::move(response));
        }
        catch (const LlamaContextFull &)
        {
            publish_context();
            {
                std::lock_guard lock(mutex_);
                current_task_.reset();
            }
            task->set_error_state("context full: the latest message was rolled back", std::string(kContextFull));
            finish_task(task);
            continue;
        }
        catch (const std::exception &e)
        {
            publish_context();
            {
                std::lock_guard lock(mutex_);
                current_task_.reset();
            }
            task->set_error_state(e.what());
            finish_task(task);
            continue;
        }

        const std::string full = buffer->full_result();
        {
            std::lock_guard lock(mutex_);
            current_task_.reset();
        }
        if (task->is_cancel_requested())
        {
            task->set_result(full, JobState::Cancelled, {});
            finish_task(task);
            continue;
        }

        const ThinkingSplit split = split_thinking_channel(full, opened_think);
        ParsedAssistantActions actions;
        if (split.closed)
        {
            actions = parse_assistant_actions(split.visible, adapter_);
            coerce_tool_arguments(actions.tool_calls, task->request.tools);
        }
        actions.reasoning = split.reasoning;
        task->set_result(split.visible, JobState::Done, std::move(actions));
        finish_task(task);
    }
    // The live sequence is the only copy of the active session until this write.
    // A kill skips it; the conversation file from the last finished turn remains.
    try
    {
        engine_.park_active_session();
    }
    catch (const std::exception &e)
    {
        log_error("[llm] shutdown kv park failed: ", e.what());
    }
    note_session_cache_changed();
}
