#include "jobs/jobs.hpp"
#include "agent/response_parse.hpp"
#include <cstdio>

Jobs::Jobs(LlamaEngine &engine) : engine_(engine)
{
    worker_ = std::thread([this] { worker_loop(); });
}

Jobs::~Jobs()
{
    stop();
}

void Jobs::stop()
{
    stop_ = true;
    cv_.notify_all();
    if (worker_.joinable())
    {
        worker_.join();
    }
}

void Jobs::unsafe_gc()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = tasks_.begin(); it != tasks_.end();)
    {
        auto st = it->second->get_state();
        const bool finished = st == TaskState::Done || st == TaskState::Error || st == TaskState::Cancelled;
        if (finished)
        {
            auto finished_at = it->second->finished_at;
            if (finished_at.time_since_epoch().count() != 0 && now - finished_at > kFinishedTtl)
            {
                it = tasks_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

Task::Key Jobs::next_key()
{
    return std::to_string(key_counter_.fetch_add(1, std::memory_order_relaxed));
}

optional<Task::Key> Jobs::submit(MessagesRequest request, std::function<void(const Task &)> on_finished)
{
    std::lock_guard lock(mutex_);

    // Opportunistic GC
    unsafe_gc();

    // Verify that we aren't adding too many jobs at the same time - just in case.
    size_t active = 0;
    for (const auto &kv : tasks_)
    {
        auto st = kv.second->get_state();
        if (st == TaskState::Queued || st == TaskState::Running)
        {
            ++active;
        }
    }
    if (active >= kMaxQueue)
    {
        return std::nullopt;
    }

    const auto task = std::make_shared<Task>();
    task->key = next_key();
    task->request = std::move(request);
    task->on_finished = std::move(on_finished);
    task->state = TaskState::Queued;

    tasks_[task->key] = task;
    queue_.push_back(task->key);
    cv_.notify_one();
    return task->key;
}

namespace
{
void notify_finished(const std::shared_ptr<Task> &task)
{
    if (!task || !task->on_finished)
    {
        return;
    }
    try
    {
        task->on_finished(*task);
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "[jobs] on_finished error: %s\n", e.what());
    }
    task->on_finished = nullptr;
}
} // namespace

std::shared_ptr<Task> Jobs::get_task(const std::string &key)
{
    std::lock_guard lock(mutex_);
    auto it = tasks_.find(key);
    if (it == tasks_.end())
    {
        return {};
    }
    return it->second;
}

bool Jobs::cancel(const Task::Key &key)
{
    std::shared_ptr<Task> task;
    {
        std::lock_guard lock(mutex_);
        auto it = tasks_.find(key);
        if (it == tasks_.end())
        {
            return false;
        }
        task = it->second;

        // Drop from queue if still waiting
        for (auto qit = queue_.begin(); qit != queue_.end(); ++qit)
        {
            if (*qit == key)
            {
                queue_.erase(qit);
                break;
            }
        }
    }

    // Try to cancel the task
    task->request_cancel();

    /// If the task was immediately cancelled then mark it as done
    if (task->get_state() == TaskState::Cancelled)
    {
        task->buffer->set_done();
        notify_finished(task);
    }

    return true;
}

void Jobs::gc()
{
    std::lock_guard lock(mutex_);
    unsafe_gc();
}

std::shared_ptr<Task> Jobs::unsafe_pop_next_queued()
{
    while (!queue_.empty())
    {
        auto key = queue_.front();
        queue_.pop_front();
        auto it = tasks_.find(key);
        if (it == tasks_.end())
        {
            continue;
        }
        auto task = it->second;
        if (task->is_cancel_requested() || task->get_state() == TaskState::Cancelled)
        {
            continue;
        }
        return task;
    }
    return {};
}

void Jobs::worker_loop()
{
    while (!stop_)
    {
        std::shared_ptr<Task> task;
        {
            // Wait for new tasks to be available
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_)
            {
                break;
            }
            task = unsafe_pop_next_queued();
        }
        if (!task)
        {
            continue;
        }

        // Is the task cancelled?
        if (task->is_cancel_requested())
        {
            task->set_state(TaskState::Cancelled);
            task->buffer->set_done();
            notify_finished(task);
            continue;
        }

        task->set_state(TaskState::Running);

        // Build message list: optional system + messages
        std::vector<ChatMessage> msgs;
        if (!task->request.system.empty())
        {
            msgs.push_back({"system", task->request.system});
        }
        msgs.insert(msgs.end(), task->request.messages.begin(), task->request.messages.end());

        auto buffer = task->buffer;
        std::string response;
        try
        {
            response = engine_.chat(msgs, [task, buffer](std::string_view piece) -> bool {
                if (task->is_cancel_requested() || buffer->is_cancelled())
                {
                    return false;
                }
                buffer->push(piece);
                buffer->append_result(piece);
                return true;
            });
        }
        catch (const std::exception &e)
        {
            task->set_error(e.what());
            buffer->set_done();
            notify_finished(task);
            continue;
        }

        if (task->is_cancel_requested() || buffer->is_cancelled())
        {
            task->set_state(TaskState::Cancelled);
            task->set_result(std::move(response));
            buffer->set_done();
            notify_finished(task);
            continue;
        }

        if (response.empty() && task->get_result().empty())
        {
            // May be empty on error paths inside engine; keep done with empty content
        }

        task->set_result(response.empty() ? buffer->full_result() : response);
        {
            auto actions = parse_assistant_actions(task->get_result());
            task->set_actions(std::move(actions));
        }
        task->set_state(TaskState::Done);
        buffer->set_done();
        notify_finished(task);
    }
}
