#include "my_switcher.h"
#include <esp_log.h>
#include <mutex>
#include "my_background.h"

#define TAG "MySwitcher"

MySwitcher::MySwitcher(gpio_num_t pin, bool active_high)
    : ctrl_pin_(pin)
    , active_high_(active_high)
{
    if (pin <= GPIO_NUM_NC || pin >= GPIO_NUM_MAX) {
        ESP_LOGE(TAG, "Invalid contrl pin: %d", pin);
        ctrl_pin_ = GPIO_NUM_NC;
        return;
    }
    gpio_config_t pin_cfg = {
        .pin_bit_mask = 1ULL << ctrl_pin_,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&pin_cfg));
    TurnOff();

    timer_handler_ = xTimerCreate(
        "Switcher",
        pdMS_TO_TICKS(1000),    // 后期是调整变化的
        pdFALSE,
        this,
        [](TimerHandle_t timer) {
            auto* switcher = static_cast<MySwitcher*>(pvTimerGetTimerID(timer));
            
            MyBackground::GetInstance().Schedule([](void* arg){
                    static_cast<MySwitcher*>(arg)->SwitchToNextState();
                },
                "switch",
                switcher
            );
        }
    );
    if (timer_handler_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create timer for contrl, switcher: ctrl_pin=%d.", pin);
        return;
    }
}

MySwitcher::~MySwitcher()
{
    if(timer_handler_) {
        xTimerStop(timer_handler_, 0);
        xTimerDelete(timer_handler_, 0);
    }
}


bool MySwitcher::Start()
{

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_info_.state != SwitcherState::kReady) {
            ESP_LOGE(TAG, "Error, Only can start switcher when it ready(%d), current:%d .", SwitcherState::kReady, status_info_.state);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock2(param_mutex_);
            status_info_.state = SwitcherState::kRun;
            status_info_.run_time_total = 0;
            status_info_.time_left = work_param_.work_time;
            status_info_.count_left = work_param_.work_count;
        }
        status_info_.start_tick = xTaskGetTickCount();
        xTimerChangePeriod(timer_handler_,
            pdMS_TO_TICKS(status_info_.time_left * 1000),
            0
        );
    }
    TurnOn();
    xTimerStart(timer_handler_, 0);
    if (start_cb_) {
        start_cb_(start_cb_arg_);
    }
    ESP_LOGI(TAG, "Switcher is started.");

    return true;
}

/// @brief 
void MySwitcher::Stop()
{
    auto tick_now = xTaskGetTickCount();
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_info_.state == SwitcherState::kRun || status_info_.state == SwitcherState::kPause) {
            xTimerStop(timer_handler_, 0);
            if (status_info_.state == SwitcherState::kRun) {
                status_info_.run_time_total += (tick_now - status_info_.start_tick) * portTICK_PERIOD_MS / 1000;
            }   
            status_info_.state = SwitcherState::kFinished;
            TurnOff();
        }
    }
    if (finished_cb_) {
        finished_cb_(finished_cb_arg_);
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_info_.state = SwitcherState::kReady;
    }
}

Time MySwitcher::GetRunTime()
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_info_.run_time_total;
}



/// @brief 返回当前的工作状态
/// @return 
std::string MySwitcher::GetWorkStateJson()
{
    std::string str;

    str = R"({"ID":)" + std::to_string(static_cast<int>(ctrl_pin_));
    SwitcherState state;
    TickType_t tick_now = xTaskGetTickCount();
    Time time_left;
    Count count_left;

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        state = status_info_.state;
        count_left = status_info_.count_left;
        if (state == SwitcherState::kRun ) {
            status_info_.time_left = (work_param_.work_time - (tick_now - status_info_.start_tick)*portTICK_PERIOD_MS/1000);
        } else if (state == SwitcherState::kPause) {
            status_info_.time_left = (work_param_.pause_time - (tick_now - status_info_.start_tick)*portTICK_PERIOD_MS/1000);
        }
        time_left = status_info_.time_left;
    }

    switch (state) {
      case SwitcherState::kIdle:
        str += R"(,"state":"idle"})";
        break;
      case SwitcherState::kReady:
        str += R"(,"state":"ready"})";
        break;
      case SwitcherState::kRun:
        str += R"(,"state":"run")"; 
        str += R"(,"time_left":)" + std::to_string(time_left);
        str += R"(,"count_left":)" + std::to_string(count_left) + R"(})";
        break;
      case SwitcherState::kPause:
        str += R"(,"state":"pause")"; 
        str += R"(,"time_left":)" + std::to_string(time_left);
        str += R"(,"count_left":)" + std::to_string(count_left) + R"(})";
        break;
      case SwitcherState::kFinished:
        str += R"(,"state":"finished"})";
        break;
      default:
        str += R"(,"state":"error"})";
        ESP_LOGE(TAG, "update json failed, invalid state: %d", state);
    }

    return str;
}
/// @brief 设置工作参数
/// @param work 工作时长（秒）
/// @param pause 暂停时长（秒）
/// @param count 工作次数
/// @return 设置成功返回true,失败返回false
bool MySwitcher::SetWorkParam(Time work, Time pause, Count count)
{

    if ( (work == 0) || (count == 0) || ((pause == 0) && (count != 1))) {
        ESP_LOGE(TAG, "Work param is invalid, work / pause / count: %lu / %lu / %lu", work, pause, count);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_info_.state > SwitcherState::kReady) {
            ESP_LOGE(TAG, "Only support set parameter when switcher is IDLE/Ready.");
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(param_mutex_);
        work_param_.work_time = work;
        work_param_.pause_time = pause;
        work_param_.work_count = count;
    }
    ESP_LOGI(TAG, "Work param is set, work/pause/count=%lu/%lu/%lu", work, pause, count);


    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        status_info_.state = SwitcherState::kReady;
    }
    return true;
}


bool MySwitcher::SetCallback(Callback cb, void* arg, CallbackType type)
{

    if (type >= kMaxCallback) {
        ESP_LOGE(TAG, "Callback type unknow, type: %d", type);
        return false;
    }

    if (cb == nullptr) {
        ESP_LOGW(TAG, "Warnning, the callback will be set nullptr!");
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_info_.state > SwitcherState::kReady) {
            ESP_LOGE(TAG, "Only support set parameter when switcher is IDLE/Ready.");
            return false;
        }
    }

    std::lock_guard<std::mutex>  lock(mutex_);

    switch (type) {
      case kFinishedCallback:
        finished_cb_ = cb;
        finished_cb_arg_ = arg;
        break;
      case kRunDoneCallback:
        workdone_cb_ = cb;
        workdone_cb_arg_ = arg;
        break;
      case kPauseDoneCallback:
        pausedone_cb_ = cb;
        pausedone_cb_arg_ = arg;
        break;
      case kStartCallback:
        start_cb_ = cb;
        start_cb_arg_ = arg;
        break;  
      default:
        ESP_LOGE(TAG, "Type arg checkout failed.");
        return false;
    }
    return true;
}


void MySwitcher::SwitchToNextState()
{
    auto finshed_flag = false;
    auto workdone_flag = false;
    auto pausedone_flag = false;

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        switch (status_info_.state) {
        case SwitcherState::kRun:
            TurnOff();
            status_info_.count_left --;
            status_info_.run_time_total += work_param_.work_time;
            if (status_info_.count_left == 0) {
                // 进入stop
                ESP_LOGI(TAG, "Switcher is stop.");
                status_info_.state = SwitcherState::kFinished;
                xTimerStop(timer_handler_, 0);
                finshed_flag = true;
            } else {
                // 进入pause
                ESP_LOGI(TAG, "Switcher is Pause.");
                status_info_.state = SwitcherState::kPause;
                status_info_.time_left = work_param_.pause_time;
                status_info_.start_tick = xTaskGetTickCount();
                workdone_flag = true;
                xTimerChangePeriod(timer_handler_,
                    pdMS_TO_TICKS(status_info_.time_left * 1000),
                    0
                );
            }
            break;
        case SwitcherState::kPause:
            ESP_LOGI(TAG, "Switcher is Resume.");
            TurnOn();
            status_info_.state = SwitcherState::kRun;
            status_info_.time_left = work_param_.work_time;
            status_info_.start_tick = xTaskGetTickCount();
            pausedone_flag = true;
            xTimerChangePeriod(timer_handler_,
                pdMS_TO_TICKS(status_info_.time_left * 1000),
                0
            );
            break;
        default:
            TurnOff();
            ESP_LOGE(TAG, "Error, can not use this fun in state: %d", status_info_.state);
        }
    }
    

    if (finshed_flag && finished_cb_) {
        finished_cb_(finished_cb_arg_);
        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            status_info_.state = SwitcherState::kReady;
        }
        
    }
    if (workdone_flag && workdone_cb_) {
        workdone_cb_(workdone_cb_arg_);
    }
    if (pausedone_flag && pausedone_cb_) {
        pausedone_cb_(pausedone_cb_arg_);
    } 
}

