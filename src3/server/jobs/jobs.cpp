#include "../jobs/jobs.hpp"
#include "../agent/response_parse.hpp"
#include "common/log.hpp"

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
    for (auto it = tasks_.begin(); it != tasks_.end();)
    {
        const auto st = it->second->get_state();
        if (st.is_finished())
        {
            const auto finished_at = it->second->finished_at;
            if (finished_at.time_since_epoch().count() != 0 && now - finished_at > kFinishedTtl)
            {
                it = tasks_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

shared_ptr<Task> Jobs::submit(MessagesRequest &&request,
                              std::function<void(const shared_ptr<Task> &)> on_finished)
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

        // Drop from queue if still waiting
        for (auto qit = queue_.begin(); qit != queue_.end(); ++qit)
        {
            if (*qit == task->key)
            {
                queue_.erase(qit);
                notify_immediately = true;
                break;
            }
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

std::shared_ptr<Task> Jobs::pop_next_queued()
{
    // Wait for new tasks to be available or shutting down
    std::unique_lock lock(mutex_);
    cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
    if (stop_)
    {
        return {};
    }

    // Get the top-most job to run
    while (!queue_.empty())
    {
        const auto key = queue_.front();
        queue_.pop_front();
        const auto it = tasks_.find(key);
        if (it == tasks_.end())
        {
            continue;
        }
        return it->second;
    }

    return {};
}

void Jobs::worker_loop()
{
    while (!stop_)
    {
        std::shared_ptr<Task> task = pop_next_queued();
        if (stop_)
        {
            break;
        }

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
            msgs.push_back(ChatMessage{.role = ChatMessage::ROLE_SYSTEM, .content = task->request.system});
        }
        msgs.insert(msgs.end(), task->request.messages.begin(), task->request.messages.end());

        // Start stream the LLM response
        const auto buffer = task->buffer;
        try
        {
            std::string response = engine_.chat(msgs, [task, buffer](std::string &&piece) {
                if (task->is_cancel_requested())
                {
                    return false;
                }
                buffer->push(piece);
                return true;
            });
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

        auto actions = parse_assistant_actions(full);
        task->set_result(full, JobState::Done, std::move(actions));
        notify_finished(task);
    }
}
