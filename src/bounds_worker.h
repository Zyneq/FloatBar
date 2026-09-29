#pragma once

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include "bounds.h"

namespace fb {

// Reads taskbar bounds through UI Automation on its own thread, so the UI
// thread (and the animation it drives) never waits on explorer. Each result is
// posted to `notify` as `message` with a heap-allocated Reply in lParam; the
// receiver takes ownership.
class BoundsWorker {
public:
    struct Reply {
        HWND taskbar = nullptr;
        BoundsResult result;
        DWORD ms = 0;  // how long the read took
    };

    ~BoundsWorker();

    // Starts the thread and its UI Automation client; fails if UIA can't start.
    HRESULT Start(HWND notify, UINT message);
    void Stop();

    // Queues a read of `taskbar` (ignored if one is already queued).
    void Request(HWND taskbar);

private:
    void Run(HANDLE ready, HRESULT* initResult);

    HWND notify_ = nullptr;
    UINT message_ = 0;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<HWND> queue_;
    bool stop_ = false;
};

}  // namespace fb
