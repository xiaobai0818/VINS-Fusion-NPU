#pragma once

#include "npu_rknn.h"
#include <opencv2/opencv.hpp>
#include <vector>
#include <string>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <future>
#include <atomic>
#include <functional>

// RK3588 SuperPoint:
//   combo  : one rknn_run with CORE_0_1_2 (3 cores, 1 CPU thread)
//   triple : 3 persistent NPU workers; wait only for left; core1/2 keep busy without decode
class NpuSuperPointFrontend
{
public:
    static NpuSuperPointFrontend &instance();
    bool init(const std::string &model_path, float score_th);
    bool ready() const { return ready_; }

    std::vector<cv::Point2f> detect(const cv::Mat &gray, const cv::Mat &mask, int max_cnt, int core_id);
    std::vector<cv::Point2f> detectPrimary(const cv::Mat &left, const cv::Mat &right, const cv::Mat &prev,
                                           const cv::Mat &mask, int max_cnt);

private:
    struct Job
    {
        cv::Mat nn_in;
        cv::Mat mask;
        int img_w = 0, img_h = 0, max_cnt = 0;
        bool need_decode = false;
        std::shared_ptr<std::promise<std::vector<cv::Point2f>>> prom;
    };

    NpuSuperPointFrontend() = default;
    ~NpuSuperPointFrontend();
    NpuSuperPointFrontend(const NpuSuperPointFrontend &) = delete;
    NpuSuperPointFrontend &operator=(const NpuSuperPointFrontend &) = delete;

    bool prepareNn(const cv::Mat &gray, int core_id, cv::Mat &nn_in, cv::Mat &gray1);
    std::vector<cv::Point2f> decode(const std::vector<std::vector<float>> &outs,
                                    const cv::Mat &mask, int img_w, int img_h, int max_cnt);
    void workerLoop(int core_id);
    void startWorkers();
    void pickMode();
    double benchMs(int loops, const std::function<void()> &fn);

    RknnEngine engines_[3];
    std::mutex mu_[3];
    std::mutex job_mu_[3];
    std::condition_variable job_cv_[3];
    std::queue<Job> jobs_[3];
    std::thread workers_[3];
    std::atomic<bool> running_{false};
    float score_th_ = 0.015f;
    bool ready_ = false;
    bool combo_mode_ = true;
};
