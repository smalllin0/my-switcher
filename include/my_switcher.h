#ifndef MY_SWITCHER_H_
#define MY_SWITCHER_H_

#include <functional>
#include <mutex>
#include <string>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"




using Time = uint32_t;      // 时间（秒）
using Count = uint32_t;     // 工作次数
struct WorkParam {
    Time        work_time{0};
    Time        pause_time{0};
    Count       work_count{1};
};



using Callback = std::function<void(void*)>;

class MySwitcher {
public:
    MySwitcher(gpio_num_t pin, bool active_high=true);
    ~MySwitcher();
    
    MySwitcher(const MySwitcher&) = delete;
    MySwitcher& operator=(const MySwitcher&) = delete;

    bool Start();
    void Stop();
    std::string GetWorkStateJson();
    Time GetRunTime();

    bool SetWorkParam(WorkParam* arg) {
        return SetWorkParam(arg->work_time, arg->pause_time, arg->work_count);
    }

    bool SetWorkParam(Time work, Time pause = 0, Count count=1);
    /// @brief 工作开始后运行的回调
    inline bool OnStart(Callback cb, void* arg=nullptr) {
        return SetCallback(cb, arg, kStartCallback);
    }
    /// @brief 工作阶段结束后运行的回调
    inline bool OnWorkDone(Callback cb, void* arg=nullptr) {
        return SetCallback(cb, arg, kRunDoneCallback);
    }
    /// @brief 暂停阶段结束后运行的回调
    inline bool OnPauseDone(Callback cb, void* arg=nullptr) {
        return SetCallback(cb, arg, kPauseDoneCallback);
    }
    /// @brief 工作流结束后（进入就绪前）运行的回调
    inline bool OnFinished(Callback cb, void* arg=nullptr) {
        return SetCallback(cb, arg, kFinishedCallback);
    }
private:
    enum class SwitcherState : uint8_t{
        kIdle,          // 初始状态
        kReady,         // 设置参数状态
        kRun,           // 工作运行中状态
        kPause,         // 工作暂停中状态
        kFinished       // 任务结束状态
    };
    struct WorkState {
        SwitcherState       state{SwitcherState::kIdle};
        Time                run_time_total{0};
        Time                time_left{0};
        Count               count_left{0};
        TickType_t          start_tick{0};           
    };
    enum CallbackType {
        kFinishedCallback = 0,
        kRunDoneCallback,
        kPauseDoneCallback,
        kStartCallback,
        kMaxCallback
    };


    bool SetCallback(Callback cb, void* arg, CallbackType type);
    inline void TurnOn() { gpio_set_level(ctrl_pin_, active_high_ ? 1 : 0); }
    inline void TurnOff() { gpio_set_level(ctrl_pin_, active_high_ ? 0 : 1); }
    void SwitchToNextState();


    gpio_num_t      ctrl_pin_;
    bool            active_high_;
    WorkState       status_info_{};
    WorkParam       work_param_{};
    TimerHandle_t   timer_handler_{nullptr};

    std::mutex      mutex_;
    std::mutex      param_mutex_;
    std::mutex      status_mutex_;
    Callback        start_cb_{nullptr};
    void*           start_cb_arg_{nullptr};
    Callback        workdone_cb_{nullptr};
    void*           workdone_cb_arg_{nullptr};
    Callback        pausedone_cb_{nullptr};
    void*           pausedone_cb_arg_{nullptr};
    Callback        finished_cb_{nullptr};
    void*           finished_cb_arg_{nullptr};
};

#endif


