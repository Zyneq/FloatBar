#include "bounds_worker.h"

#include <algorithm>

namespace fb {

BoundsWorker::~BoundsWorker() { Stop(); }

HRESULT BoundsWorker::Start(HWND notify, UINT message) {
    notify_ = notify;
    message_ = message;
    stop_ = false;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT initResult = E_FAIL;
    thread_ = std::thread(&BoundsWorker::Run, this, ready, &initResult);
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);
    if (FAILED(initResult)) Stop();
    return initResult;
}

void BoundsWorker::Stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
        queue_.clear();
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void BoundsWorker::Request(HWND taskbar) {
    {
        std::lock_guard lock(mutex_);
        if (std::find(queue_.begin(), queue_.end(), taskbar) != queue_.end()) return;
        queue_.push_back(taskbar);
    }
    wake_.notify_one();
}

void BoundsWorker::Run(HANDLE ready, HRESULT* initResult) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        BoundsReader reader;
        *initResult = FAILED(com) ? com : reader.Init();
        const bool ok = SUCCEEDED(*initResult);
        SetEvent(ready);  // `initResult` must not be touched after this
        while (ok) {
            HWND taskbar = nullptr;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                if (stop_) break;
                taskbar = queue_.front();
                queue_.pop_front();
            }
            auto* reply = new Reply;
            reply->taskbar = taskbar;
            const DWORD start = GetTickCount();
            reply->result = reader.Compute(taskbar);
            reply->ms = GetTickCount() - start;
            if (!PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(reply))) delete reply;
        }
    }  // the reader's COM objects are released before CoUninitialize
    if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace fb
