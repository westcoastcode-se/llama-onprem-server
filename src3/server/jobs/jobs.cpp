#include "../jobs/jobs.hpp"
#include "../agent/model_adapter.hpp"
#include "../agent/response_parse.hpp"
#include "common/log.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

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
    worker_.request_stop();
    {
        std::lock_guard lock(mutex_);
        if (current_task_)
        {
            current_task_->request_cancel();
        }
    }
    cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
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

shared_ptr<Task> Jobs::submit(MessagesRequest &&request,
                              std::move_only_function<void(const shared_ptr<Task> &)> on_finished)
{
    const auto task = std::make_shared<Task>();
    task->request = std::move(request);
    task->on_finished = std::move(on_finished);
    return enqueue(task);
}

shared_ptr<Task> Jobs::enqueue(shared_ptr<Task> task)
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

void notify_finished(const shared_ptr<Task> &task)
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

} // namespace

shared_ptr<Task> Jobs::cancel(JobKey const id)
{
    log_info("Task(", id, ") | cancelling task");
    shared_ptr<Task> task;
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
        notify_finished(task);
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
    std::lock_guard lock(mutex_);
    pending_session_releases_.push_back(session_id);
    cv_.notify_one();
}

Jobs::NextWork Jobs::wait_next(const std::stop_token &stop)
{
    std::unique_lock lock(mutex_);
    std::stop_callback on_stop(stop, [&] { cv_.notify_all(); });
    cv_.wait(lock, [&] { return stop.stop_requested() || !queue_.empty() || !pending_session_releases_.empty(); });
    NextWork work;
    if (stop.stop_requested())
    {
        return work;
    }
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
            notify_finished(task);
            continue;
        }

        // Task is now running!
        {
            std::lock_guard lock(mutex_);
            current_task_ = task;
        }
        task->set_running();

        // Build message list: optional system + messages
        std::vector<ChatMessage> msgs;
        if (!task->request.system.empty())
        {
            msgs.push_back(ChatMessage{.role = string(ChatMessage::ROLE_SYSTEM), .content = task->request.system});
        }
        msgs.append_range(task->request.messages);

        // Start stream the LLM response
        const auto buffer = task->buffer;
        try
        {
            LlamaRequest call;
            call.should_stop = [task] { return task->is_cancel_requested(); };
            call.max_tokens = task->request.max_tokens;
            call.session_id = task->request.session_id;
            call.tools = task->request.tools;
            call.token_cb = [task, buffer](std::string &&piece) {
                if (task->is_cancel_requested())
                {
                    return false;
                }
                buffer->push(std::move(piece));
                return true;
            };
            std::string response = engine_.chat(msgs, call);
            buffer->set_full_result(std::move(response));
        }
        catch (const std::exception &e)
        {
            {
                std::lock_guard lock(mutex_);
                current_task_.reset();
            }
            task->set_error_state(e.what());
            notify_finished(task);
            continue;
        }

        const string full = buffer->full_result();
        {
            std::lock_guard lock(mutex_);
            current_task_.reset();
        }
        if (task->is_cancel_requested())
        {
            task->set_result(full, JobState::Cancelled, {});
            notify_finished(task);
            continue;
        }

        auto actions = parse_assistant_actions(full, adapter_);
        task->set_result(full, JobState::Done, std::move(actions));
        notify_finished(task);
    }
}
