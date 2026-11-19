// Thumbnail cache: downloads on two worker threads (newest request first), decodes with WIC,
// scales once to the row size and keeps at most a few hundred bitmaps.
#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ea::ui {

class Thumbs {
public:
    void start(std::function<void()> onReady);
    void stop();
    void setSize(int w, int h);  // clears the cache when the size changes (DPI)
    HBITMAP get(const std::string& url);  // nullptr until loaded; queues the download

private:
    void worker();
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::string> todo_;
    std::map<std::string, HBITMAP> cache_;
    std::map<std::string, bool> pending_;
    std::deque<std::string> order_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::function<void()> onReady_;
    int w_ = 64, h_ = 36;
};

}  // namespace ea::ui
