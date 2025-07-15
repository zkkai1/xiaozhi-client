#include "wake_word_detect.h"
#include "application.h"

#include <esp_log.h>
#include <model_path.h>
#include <arpa/inet.h>
#include <sstream>

#define DETECTION_RUNNING_EVENT 1
#define COMMAND_DECTECTION 1

static const char* TAG = "WakeWordDetect";

WakeWordDetect::WakeWordDetect()
    : afe_data_(nullptr),
      wake_word_pcm_(),
      wake_word_opus_() {

    event_group_ = xEventGroupCreate();
}

WakeWordDetect::~WakeWordDetect() {
    if (afe_data_ != nullptr) {
        afe_iface_->destroy(afe_data_);
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    vEventGroupDelete(event_group_);
}

void WakeWordDetect::Initialize(AudioCodec* codec) {
    codec_ = codec;
    int ref_num = codec_->input_reference() ? 1 : 0;

    ESP_LOGI(TAG, "WakeWordDetect::Initialize start");
    srmodel_list_t *models = esp_srmodel_init("model");
    for (int i = 0; i < models->num; i++) {
        ESP_LOGI(TAG, "Model %d: %s", i, models->model_name[i]);
        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
            wakenet_model_ = models->model_name[i];
            auto words = esp_srmodel_get_wake_words(models, wakenet_model_);
            ESP_LOGI(TAG, "Wake words: %s", words);
            // split by ";" to get all wake words
            std::stringstream ss(words);
            std::string word;
            while (std::getline(ss, word, ';')) {
                wake_words_.push_back(word);
                ESP_LOGI(TAG, "Add wake word: %s", word.c_str());
            }
        }
#if COMMAND_DECTECTION
        else  if (strstr(models->model_name[i], ESP_MN_PREFIX) != NULL) {
            ESP_LOGI(TAG, "Found Multinet model: %s", models->model_name[i]);
            // 检查PSRAM是否可用
            ESP_LOGI("PSRAM", "Free ESP_MN_PREFIX PSRAM size: %8d bytes ",  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI("PSRAM", "Free SRAM size: %8d bytes ", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                    
            // 获取 multinet 模型句柄
            multinet = esp_mn_handle_from_name(models->model_name[i]);
            if (!multinet) {
                ESP_LOGE(TAG, "Failed to create Multinet handle");
                continue;
            }
            //ESP_LOGI("PSRAM", "Free ESP_MN_PREFIX PSRAM size: %8d bytes ",  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            //ESP_LOGI("PSRAM", "Free SRAM size create: %8d bytes ", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
               
            model_data = multinet->create(models->model_name[i], 6000); //5760  是音频帧大小  或者 6000
            if (!model_data) {
                ESP_LOGE(TAG, "Failed to create Multinet model data");
                continue;
            }
            //ESP_LOGI("PSRAM", "Free ESP_MN_PREFIX PSRAM size: %8d bytes ",  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            //ESP_LOGI("PSRAM", "Free SRAM size switch_loader_mode: %8d bytes ", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
               
            //model_data = multinet->switch_loader_mode(model_data,ESP_MN_LOAD_FROM_FLASH);
            
            int mu_chunksize = multinet->get_samp_chunksize(model_data);

            ESP_LOGI(TAG, "Multinet created, mu_chunksize: %d", mu_chunksize);
            

            // 检查PSRAM是否可用
            ESP_LOGI("PSRAM", "Free PSRAM size: %8d bytes ",  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            ESP_LOGI("PSRAM", "Free SRAM size: %8d bytes ", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            // 命令词调试打印
            ESP_LOGI(TAG, "Adding speech commands...");
            esp_mn_commands_clear();
            esp_mn_commands_add(0, "ni hao xiao le");
            esp_mn_commands_add(1, "ni hao cui hua");
            esp_mn_commands_add(2, "zeng da feng su");
            esp_mn_commands_add(3, "jian xiao feng su");
            esp_mn_commands_add(4, "sheng gao yi du");
            esp_mn_commands_add(5, "jiang di yi du");
            esp_mn_commands_add(6, "zhi re mo shi");
            esp_mn_commands_add(7, "zhi leng mo shi");
            esp_mn_commands_add(8, "song feng mo shi");
            esp_mn_commands_add(9, "jie neng mo shi");
            esp_mn_commands_add(10, "chu shi mo shi");
            esp_mn_commands_add(11, "jian kang mo shi");
            esp_mn_commands_add(12, "shui mian mo shi");
            esp_mn_commands_add(13, "da kai lan ya");
            esp_mn_commands_add(14, "guan bi lan ya");
            esp_mn_commands_add(15, "kai shi bo fang");
            esp_mn_commands_add(16, "zan ting bo fang");
            esp_mn_commands_add(17, "ding shi yi xiao shi");
            esp_mn_commands_add(18, "da kai dian deng");
            esp_mn_commands_add(19, "guan bi dian deng");
            esp_mn_commands_update();
            ESP_LOGI(TAG, "Speech commands added and updated");
        }
#endif
    }

    std::string input_format;
    for (int i = 0; i < codec_->input_channels() - ref_num; i++) {
        input_format.push_back('M');
    }
    for (int i = 0; i < ref_num; i++) {
        input_format.push_back('R');
    }
    afe_config_t* afe_config = afe_config_init(input_format.c_str(), models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = codec_->input_reference();
    afe_config->aec_mode = AEC_MODE_SR_HIGH_PERF;
    afe_config->afe_perferred_core = 1;
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    
    afe_iface_ = esp_afe_handle_from_config(afe_config);
    afe_data_ = afe_iface_->create_from_config(afe_config);

#if COMMAND_DECTECTION
    // 添加命令词
    esp_mn_active_commands_print();
    if(model_data){
        esp_mn_commands_clear();

        // 命令词添加部分已屏蔽
        // esp_mn_commands_add(1, "ni hao xiao ma");
        // esp_mn_commands_add(2, "ni hao xiao zhu");
        // esp_mn_commands_add(3, "ni hao cui hua");
        // esp_mn_commands_update();
    }
    //esp_mn_active_commands_print();
#endif

    xTaskCreate([](void* arg) {
        auto this_ = (WakeWordDetect*)arg;
        this_->AudioDetectionTask();
        vTaskDelete(NULL);
    }, "audio_detection", 4096, this, 3, nullptr);

#if COMMAND_DECTECTION   
    // 创建命令检测任务
    xTaskCreatePinnedToCore([](void* arg) {
        auto this_ = (WakeWordDetect*)arg;
        this_->CommandDetectionTask();
        vTaskDelete(NULL);
    }, "command_detection", 4096, this, 5, &command_detection_task_, 1);
#endif
}


void WakeWordDetect::OnWakeWordDetected(std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void WakeWordDetect::OnWakeCommandDetected(std::function<void(int command_id)> callback){
    wake_command_detected_callback_ = callback;
}

void WakeWordDetect::StartDetection() {
    xEventGroupSetBits(event_group_, DETECTION_RUNNING_EVENT);
}

void WakeWordDetect::StopDetection() {
    xEventGroupClearBits(event_group_, DETECTION_RUNNING_EVENT);
    if (afe_data_ != nullptr) {
        afe_iface_->reset_buffer(afe_data_);
    }
}

bool WakeWordDetect::IsDetectionRunning() {
    return xEventGroupGetBits(event_group_) & DETECTION_RUNNING_EVENT;
}

void WakeWordDetect::Feed(const std::vector<int16_t>& data) {
    if (afe_data_ == nullptr) {
        return;
    }
    afe_iface_->feed(afe_data_, data.data());
}

size_t WakeWordDetect::GetFeedSize() {
    if (afe_data_ == nullptr) {
        return 0;
    }
    return afe_iface_->get_feed_chunksize(afe_data_) * codec_->input_channels();
}

void WakeWordDetect::AudioDetectionTask() {
    auto fetch_size = afe_iface_->get_fetch_chunksize(afe_data_);
    auto feed_size = afe_iface_->get_feed_chunksize(afe_data_);
    ESP_LOGI(TAG, "Audio detection task started, feed size: %d fetch size: %d", feed_size, fetch_size);

    while (true) {
        xEventGroupWaitBits(event_group_, DETECTION_RUNNING_EVENT, pdFALSE, pdTRUE, portMAX_DELAY);
        auto res = afe_iface_->fetch_with_delay(afe_data_, portMAX_DELAY);
        if (res == nullptr || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "fetch_with_delay failed or returned nullptr");
            continue;
        }
        //ESP_LOGI(TAG, "AudioDetectionTask: got data, wakeup_state=%d", res->wakeup_state);
        StoreWakeWordData((uint16_t*)res->data, res->data_size / sizeof(uint16_t));
        this->res = res;
        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected, index=%d", res->wake_word_index);
            StopDetection();
            last_detected_wake_word_ = wake_words_[res->wake_word_index - 1];
            command_detection_enabled_ = true;
            command_detection_expire_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(5000); // 允许5秒命令检测
            if (wake_word_detected_callback_) {
                ESP_LOGI(TAG, "Calling wake_word_detected_callback_");
                wake_word_detected_callback_(last_detected_wake_word_);
            }
        } else if (res->data) {
            if (model_data) {
                if(command_detection_task_ != nullptr)
                {
                    //ESP_LOGI(TAG, "Notifying command_detection_task_");
                    xTaskNotifyGive(command_detection_task_);
                }
                else{
                    ESP_LOGW(TAG, "command_detection_task_ is nullptr, skip notify");
                }
            } 
        }
    }
}

void WakeWordDetect::CommandDetectionTask() {
    ESP_LOGI(TAG, "CommandDetectionTask started");
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        //ESP_LOGI(TAG, "CommandDetectionTask: notified, running detect");
        esp_mn_state_t mn_state;
        esp_mn_results_t *mn_result;
        if (model_data && res->data) {
            mn_state = multinet->detect(model_data, res->data);
            //ESP_LOGI(TAG, "CommandDetectionTask: mn_state=%d", mn_state);
            if (mn_state == ESP_MN_STATE_DETECTING) {
                //ESP_LOGI(TAG, "===111===");
                continue;
            } else if (mn_state == ESP_MN_STATE_DETECTED) {
                mn_result = multinet->get_results(model_data);
                //ESP_LOGI(TAG, "===222===");
                if (mn_result != nullptr) {
                    ESP_LOGI(TAG, "mn_result: num=%d", mn_result->num);
                    for (int i = 0; i < mn_result->num; ++i) {
                        ESP_LOGI(TAG, "mn_result phrase_id[%d]=%d, prob=%.3f", i, mn_result->phrase_id[i], mn_result->prob[i]);
                    }
                }
                if (mn_result != nullptr && mn_result->num > 0) {
                    int command_id = mn_result->phrase_id[0];
                    ESP_LOGI(TAG, "Command detected, id=%d", command_id);
                    if (wake_command_detected_callback_) {
                        ESP_LOGI(TAG, "Calling wake_command_detected_callback_");
                        wake_command_detected_callback_(command_id);
                    }
                } else {
                    ESP_LOGI(TAG, "mn_result is nullptr or num==0");
                }
            }
        } else {
            ESP_LOGI(TAG, "model_data or res->data is nullptr");
        }
    }
}


void WakeWordDetect::StoreWakeWordData(uint16_t* data, size_t samples) {
    // store audio data to wake_word_pcm_
    wake_word_pcm_.emplace_back(std::vector<int16_t>(data, data + samples));
    // keep about 2 seconds of data, detect duration is 32ms (sample_rate == 16000, chunksize == 512)
    while (wake_word_pcm_.size() > 2000 / 32) {
        wake_word_pcm_.pop_front();
    }
}

void WakeWordDetect::EncodeWakeWordData() {
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ = (StackType_t*)heap_caps_malloc(4096 * 8, MALLOC_CAP_SPIRAM);
    }
    wake_word_encode_task_ = xTaskCreateStatic([](void* arg) {
        auto this_ = (WakeWordDetect*)arg;
        {
            auto start_time = esp_timer_get_time();
            auto encoder = std::make_unique<OpusEncoderWrapper>(16000, 1, OPUS_FRAME_DURATION_MS);
            encoder->SetComplexity(0); // 0 is the fastest

            for (auto& pcm: this_->wake_word_pcm_) {
                encoder->Encode(std::move(pcm), [this_](std::vector<uint8_t>&& opus) {
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.emplace_back(std::move(opus));
                    this_->wake_word_cv_.notify_all();
                });
            }
            this_->wake_word_pcm_.clear();

            auto end_time = esp_timer_get_time();
            ESP_LOGI(TAG, "Encode wake word opus %zu packets in %lld ms",
                this_->wake_word_opus_.size(), (end_time - start_time) / 1000);

            std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
            this_->wake_word_opus_.push_back(std::vector<uint8_t>());
            this_->wake_word_cv_.notify_all();
        }
        vTaskDelete(NULL);
    }, "encode_detect_packets", 4096 * 8, this, 2, wake_word_encode_task_stack_, &wake_word_encode_task_buffer_);
}

bool WakeWordDetect::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() {
        return !wake_word_opus_.empty();
    });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}
