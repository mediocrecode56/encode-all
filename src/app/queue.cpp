#include "queue.h"

#include <windows.h>

#include <algorithm>

#include "../core/util.h"

namespace ea {

const wchar_t* stateName(TaskState s) {
    switch (s) {
        case TaskState::Waiting: return L"waiting";
        case TaskState::Running: return L"running";
        case TaskState::Paused: return L"paused";
        case TaskState::Done: return L"done";
        case TaskState::Failed: return L"failed";
        case TaskState::Canceled: return L"canceled";
    }
    return L"";
}

static std::wstring queuePath() { return joinPath(dataDir(), L"queue.json"); }

// ---------- RunCtx ----------

void RunCtx::stage(const std::wstring& s) {
    q.update(t, [&](Task& x) {
        x.stage = s;
        x.progress = -1;
        x.eta = -1;
        x.speed.clear();
    }, true);
    log(s);
}

void RunCtx::progress(double p, double eta, const std::wstring& speed) {
    q.update(t, [&](Task& x) {
        x.progress = p;
        x.eta = eta;
        x.speed = speed;
    });
}

void RunCtx::log(const std::wstring& line) {
    q.update(t, [&](Task& x) {
        x.log.push_back(line);
        if (x.log.size() > 600) x.log.erase(x.log.begin(), x.log.begin() + 100);
    });
}

void RunCtx::setClass(QualityClass c) { q.update(t, [&](Task& x) { x.cls = c; }, true); }
void RunCtx::setOutput(const std::wstring& o) { q.update(t, [&](Task& x) { x.output = o; }, true); }
void RunCtx::setTitle(const std::wstring& s) { q.update(t, [&](Task& x) { x.title = s; }, true); }
void RunCtx::saveSpec() { q.persist(); }

json::Value RunCtx::spec() {
    std::lock_guard<std::mutex> lk(q.lock());
    return t.spec;
}

void RunCtx::specSet(const std::string& key, json::Value v) {
    std::lock_guard<std::mutex> lk(q.lock());
    t.spec.set(key, std::move(v));
}

int RunCtx::run(const std::wstring& exe, const std::vector<std::wstring>& args, const ProcOpts& opts, const Process::LineFn& onLine,
                std::wstring* startErr, std::string* rawOut) {
    if (canceled()) return -1;
    Process p;
    if (rawOut) p.captureStdout(rawOut);
    log(L"> " + fileName(exe) + L" " + joinArgs(args));
    std::wstring err;
    if (!p.start(exe, args, opts, onLine, &err)) {
        if (startErr) *startErr = err;
        log(err);
        return -1;
    }
    q.attach(t, &p);
    int code = p.wait();
    q.attach(t, nullptr);
    return code;
}

// ---------- Queue ----------

void Queue::init(Deps* deps, std::function<Settings()> settings, std::function<EncoderCaps()> caps, Notify notify) {
    deps_ = deps;
    settings_ = std::move(settings);
    caps_ = std::move(caps);
    notify_ = std::move(notify);
    load();
    sched_ = std::thread(&Queue::scheduler, this);
}

void Queue::shutdown() {
    stop_ = true;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : tasks_) {
            if (t->th.joinable()) {
                t->interrupted = true;
                t->cancel = true;
                if (t->proc) t->proc->kill();
            }
        }
    }
    cv_.notify_all();
    if (sched_.joinable()) sched_.join();
    for (auto& t : tasks_)
        if (t->th.joinable()) t->th.join();
    for (auto& th : finished_)
        if (th.joinable()) th.join();
    persist();
}

void Queue::load() {
    std::string text;
    json::Value v;
    if (!readFile(queuePath(), text) || !json::parse(text, v)) return;
    for (auto& o : v["tasks"].a) {
        auto t = std::make_unique<Task>();
        t->id = nextId_++;
        t->kind = (TaskKind)std::clamp((int)o["kind"].num(0), 0, 2);
        t->state = (TaskState)std::clamp((int)o["state"].num(0), 0, 5);
        if (t->state == TaskState::Running) t->state = TaskState::Waiting;  // interrupted: resume
        t->title = o["title"].wstr();
        t->spec = o["spec"];
        t->output = o["output"].wstr();
        t->error = o["error"].wstr();
        t->hint = o["hint"].wstr();
        t->result = o["result"].wstr();
        t->attempts = (int)o["attempts"].num(0);
        t->cls = (QualityClass)std::clamp((int)o["cls"].num(0), 0, 2);
        if (t->state == TaskState::Paused) t->stage = L"paused (resume continues)";
        tasks_.push_back(std::move(t));
    }
}

void Queue::persist() {
    json::Value root = json::Value::object();
    json::Value arr = json::Value::array();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : tasks_) {
            if (t->state == TaskState::Canceled) continue;
            json::Value o = json::Value::object();
            o.set("kind", (int)t->kind);
            TaskState s = t->state == TaskState::Running ? TaskState::Waiting : t->state;
            o.set("state", (int)s);
            o.set("title", t->title);
            o.set("spec", t->spec);
            o.set("output", t->output);
            o.set("error", t->error);
            o.set("hint", t->hint);
            o.set("result", t->result);
            o.set("attempts", t->attempts);
            o.set("cls", (int)t->cls);
            arr.push(o);
        }
    }
    root.set("version", 1);
    root.set("tasks", arr);
    writeFileAtomic(queuePath(), json::dump(root, false));
    dirty_ = false;
}

uint64_t Queue::add(TaskKind kind, const std::wstring& title, const json::Value& spec) {
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto t = std::make_unique<Task>();
        t->id = id = nextId_++;
        t->kind = kind;
        t->title = title;
        t->spec = spec;
        tasks_.push_back(std::move(t));
        dirty_ = true;
    }
    cv_.notify_all();
    if (notify_) notify_(true);
    return id;
}

Task* Queue::find(uint64_t id) {
    for (auto& t : tasks_)
        if (t->id == id) return t.get();
    return nullptr;
}

void Queue::update(Task& t, const std::function<void(Task&)>& f, bool force) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        f(t);
    }
    if (notify_) notify_(force);
}

void Queue::attach(Task& t, Process* p) {
    std::lock_guard<std::mutex> lk(mu_);
    t.proc = p;
    if (p && t.pauseWanted) {
        p->suspend();
        t.state = TaskState::Paused;
    }
}

void Queue::pause(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        Task* t = find(id);
        if (!t) return;
        if (t->state == TaskState::Waiting) {
            t->state = TaskState::Paused;
        } else if (t->state == TaskState::Running) {
            t->pauseWanted = true;
            if (t->proc && t->proc->suspend()) t->state = TaskState::Paused;
        }
        dirty_ = true;
    }
    if (notify_) notify_(true);
}

void Queue::resume(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        Task* t = find(id);
        if (!t || t->state != TaskState::Paused) return;
        t->pauseWanted = false;
        if (t->th.joinable() && t->proc) {
            t->proc->resume();
            t->state = TaskState::Running;
        } else if (t->th.joinable()) {
            t->state = TaskState::Running;
        } else {
            t->state = TaskState::Waiting;
            t->stage.clear();
        }
        dirty_ = true;
    }
    cv_.notify_all();
    if (notify_) notify_(true);
}

void Queue::cancel(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        Task* t = find(id);
        if (!t) return;
        if (t->th.joinable() && (t->state == TaskState::Running || t->state == TaskState::Paused)) {
            t->cancel = true;
            if (t->proc) t->proc->kill();
        } else if (t->state == TaskState::Waiting || t->state == TaskState::Paused || t->state == TaskState::Failed) {
            t->state = TaskState::Canceled;
            t->stage = L"canceled";
        }
        dirty_ = true;
    }
    cv_.notify_all();
    if (notify_) notify_(true);
}

void Queue::retry(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        Task* t = find(id);
        if (!t || t->th.joinable()) return;
        if (t->state != TaskState::Failed && t->state != TaskState::Canceled) return;
        t->state = TaskState::Waiting;
        t->error.clear();
        t->hint.clear();
        t->stage.clear();
        t->progress = -1;
        t->cancel = false;
        t->retryAt = 0;
        t->autoRetries = 0;
        t->triedUpdate = false;
        dirty_ = true;
    }
    cv_.notify_all();
    if (notify_) notify_(true);
}

void Queue::remove(uint64_t id) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = tasks_.begin(); it != tasks_.end(); ++it) {
            Task* t = it->get();
            if (t->id != id) continue;
            if (t->th.joinable()) {
                // Running: cancel; it is dropped once its thread ends.
                t->cancel = true;
                if (t->proc) t->proc->kill();
                t->spec.set("_remove", true);
            } else {
                tasks_.erase(it);
            }
            break;
        }
        dirty_ = true;
    }
    cv_.notify_all();
    if (notify_) notify_(true);
}

void Queue::clearFinished() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        tasks_.remove_if([](const std::unique_ptr<Task>& t) {
            return !t->th.joinable() && (t->state == TaskState::Done || t->state == TaskState::Canceled);
        });
        dirty_ = true;
    }
    if (notify_) notify_(true);
}

void Queue::pauseAll() {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : tasks_)
            if (t->state == TaskState::Running || t->state == TaskState::Waiting) ids.push_back(t->id);
    }
    for (auto id : ids) pause(id);
}

void Queue::resumeAll() {
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& t : tasks_)
            if (t->state == TaskState::Paused) ids.push_back(t->id);
    }
    for (auto id : ids) resume(id);
}

std::vector<TaskView> Queue::snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TaskView> r;
    r.reserve(tasks_.size());
    for (auto& t : tasks_) {
        if (t->spec["_remove"].boolean()) continue;
        TaskView v;
        v.id = t->id;
        v.kind = t->kind;
        v.state = t->state;
        v.title = t->title;
        v.stage = t->stage;
        v.output = t->output;
        v.error = t->error;
        v.hint = t->hint;
        v.result = t->result;
        v.speed = t->speed;
        v.progress = t->progress;
        v.eta = t->eta;
        v.attempts = t->attempts;
        v.cls = t->cls;
        r.push_back(std::move(v));
    }
    return r;
}

std::vector<std::wstring> Queue::taskLog(uint64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    Task* t = find(id);
    return t ? t->log : std::vector<std::wstring>();
}

int Queue::running() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto& t : tasks_)
        if (t->th.joinable()) n++;
    return n;
}

int Queue::waiting() const {
    std::lock_guard<std::mutex> lk(mu_);
    int n = 0;
    for (auto& t : tasks_)
        if (t->state == TaskState::Waiting) n++;
    return n;
}

bool Queue::busy() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& t : tasks_)
        if (t->th.joinable() || t->state == TaskState::Waiting) return true;
    return false;
}

void Queue::kick() { cv_.notify_all(); }

int Queue::limitFor(TaskKind k) const {
    Settings s = settings_ ? settings_() : Settings();
    switch (k) {
        case TaskKind::Download: return s.maxDownloads;
        case TaskKind::Convert: return s.maxEncodes;
        case TaskKind::Remux: return s.maxRemux;
    }
    return 1;
}

void Queue::schedule() {
    // Caller holds mu_.
    int64_t now = (int64_t)GetTickCount64();
    int counts[3] = {0, 0, 0};
    for (auto& t : tasks_)
        if (t->th.joinable()) counts[(int)t->kind]++;
    int limits[3] = {limitFor(TaskKind::Download), limitFor(TaskKind::Convert), limitFor(TaskKind::Remux)};
    for (auto& up : tasks_) {
        Task* t = up.get();
        if (t->state != TaskState::Waiting || t->th.joinable()) continue;
        if (t->retryAt && now < t->retryAt) continue;
        int k = (int)t->kind;
        if (counts[k] >= limits[k]) continue;
        counts[k]++;
        t->state = TaskState::Running;
        t->cancel = false;
        t->pauseWanted = false;
        t->retryAt = 0;
        t->error.clear();
        t->hint.clear();
        t->stage = L"starting";
        t->th = std::thread(&Queue::runTask, this, t);
    }
}

void Queue::scheduler() {
    int64_t lastSave = 0;
    while (!stop_) {
        std::vector<std::thread> toJoin;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(500));
            if (stop_) break;
            // Join finished runners; drop tasks removed while running.
            for (auto it = tasks_.begin(); it != tasks_.end();) {
                Task* t = it->get();
                if (t->th.joinable() && t->state != TaskState::Running && t->state != TaskState::Paused) toJoin.push_back(std::move(t->th));
                if (!t->th.joinable() && t->spec["_remove"].boolean()) {
                    it = tasks_.erase(it);
                    dirty_ = true;
                    continue;
                }
                ++it;
            }
            schedule();
        }
        for (auto& th : toJoin) th.join();
        int64_t now = (int64_t)GetTickCount64();
        if (dirty_ && now - lastSave > 1000) {
            persist();
            lastSave = now;
        }
    }
}

void Queue::runTask(Task* t) {
    RunCtx ctx{*this, *t, *deps_, settings_ ? settings_() : Settings(), caps_ ? caps_() : EncoderCaps()};
    RunResult r;
    Runner& run = runners_[(int)t->kind];
    if (run) r = run(ctx);
    else r.error = L"No runner";
    {
        std::lock_guard<std::mutex> lk(mu_);
        t->proc = nullptr;
        t->speed.clear();
        t->eta = -1;
        if (t->interrupted) {
            // App shutdown: keep the task (a paused one stays paused) so the next start continues it.
            t->state = t->state == TaskState::Paused || t->pauseWanted ? TaskState::Paused : TaskState::Waiting;
            t->stage = t->state == TaskState::Paused ? L"paused (resume continues)" : L"interrupted, resumes";
            t->progress = -1;
        } else if (t->cancel) {
            t->state = TaskState::Canceled;
            t->stage = L"canceled";
            t->progress = -1;
        } else if (r.ok) {
            t->state = TaskState::Done;
            t->stage = L"done";
            t->progress = 1;
            t->result = r.result;
        } else if (r.retryNow) {
            t->state = TaskState::Waiting;
            t->attempts++;
            t->stage = L"retrying";
            t->error = r.error;
        } else if (r.retryAfterMs > 0 && t->autoRetries < 2) {
            t->autoRetries++;
            t->attempts++;
            t->state = TaskState::Waiting;
            t->retryAt = (int64_t)GetTickCount64() + r.retryAfterMs;
            t->stage = L"retrying in " + std::to_wstring(r.retryAfterMs / 1000) + L" s";
            t->error = r.error;
            t->hint = r.hint;
        } else {
            t->state = TaskState::Failed;
            t->stage = L"failed";
            t->error = r.error.empty() ? L"Failed" : r.error;
            t->hint = r.hint;
            t->progress = -1;
        }
        if (t->state == TaskState::Done && !t->result.empty()) t->log.push_back(L"done: " + t->result);
        if (t->state == TaskState::Failed) t->log.push_back(L"failed: " + t->error + (t->hint.empty() ? L"" : L"  (" + t->hint + L")"));
        dirty_ = true;
        logLine(std::wstring(stateName(t->state)) + L": " + t->title + (t->error.empty() || t->state == TaskState::Done ? L"" : L" - " + t->error));
    }
    cv_.notify_all();
    if (notify_) notify_(true);
}

}  // namespace ea
