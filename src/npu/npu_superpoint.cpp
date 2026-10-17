#include "npu_superpoint.h"
#include "../estimator/parameters.h"
#include <ros/ros.h>
#include <algorithm>
#include <cmath>
#include <chrono>

NpuSuperPointFrontend &NpuSuperPointFrontend::instance()
{
    static NpuSuperPointFrontend inst;
    return inst;
}

NpuSuperPointFrontend::~NpuSuperPointFrontend()
{
    running_ = false;
    for (int i = 0; i < 3; i++)
        job_cv_[i].notify_all();
    for (int i = 0; i < 3; i++)
    {
        if (workers_[i].joinable())
            workers_[i].join();
    }
}

bool NpuSuperPointFrontend::init(const std::string &model_path, float score_th)
{
    score_th_ = score_th;
    ready_ = false;
    const rknn_core_mask masks[3] = {
        RKNN_NPU_CORE_0,
        RKNN_NPU_CORE_1,
        RKNN_NPU_CORE_2};

    if (!engines_[0].init(model_path, masks[0]))
    {
        ROS_WARN("[NPU] SuperPoint init failed on core 0, fall back to OpenCV goodFeaturesToTrack");
        return false;
    }
    ROS_INFO("[NPU] SuperPoint context on NPU core 0");

    for (int i = 1; i < 3; i++)
    {
        bool ok = engines_[i].dupFrom(engines_[0], masks[i]);
        if (!ok)
            ok = engines_[i].init(model_path, masks[i]);
        if (!ok)
        {
            ROS_ERROR("[NPU] SuperPoint init failed on core %d", i);
            return false;
        }
        ROS_INFO("[NPU] SuperPoint context on NPU core %d", i);
    }

    startWorkers();
    ready_ = true;
    pickMode();
    return true;
}

void NpuSuperPointFrontend::startWorkers()
{
    running_ = true;
    for (int i = 0; i < 3; i++)
        workers_[i] = std::thread(&NpuSuperPointFrontend::workerLoop, this, i);
}

void NpuSuperPointFrontend::workerLoop(int core_id)
{
    while (running_)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lk(job_mu_[core_id]);
            job_cv_[core_id].wait(lk, [&] {
                return !running_ || !jobs_[core_id].empty();
            });
            if (!running_ && jobs_[core_id].empty())
                break;
            if (jobs_[core_id].empty())
                continue;
            job = std::move(jobs_[core_id].front());
            jobs_[core_id].pop();
        }

        std::vector<cv::Point2f> pts;
        std::vector<std::vector<float>> outs;
        const int h = engines_[core_id].input_h();
        const int w = engines_[core_id].input_w();
        const int c = std::max(1, engines_[core_id].input_c());
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(mu_[core_id]);
            if (job.need_decode)
                ok = engines_[core_id].infer(job.nn_in.data, h, w, c, &outs, 1);
            else
                ok = engines_[core_id].infer(job.nn_in.data, h, w, c, nullptr, 0);
        }
        if (ok && job.need_decode)
            pts = decode(outs, job.mask, job.img_w, job.img_h, job.max_cnt);
        if (job.prom)
            job.prom->set_value(std::move(pts));
    }
}

double NpuSuperPointFrontend::benchMs(int loops, const std::function<void()> &fn)
{
    using clock = std::chrono::steady_clock;
    fn();
    const auto t0 = clock::now();
    for (int i = 0; i < loops; i++)
        fn();
    const auto t1 = clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / loops;
}

void NpuSuperPointFrontend::pickMode()
{
    cv::Mat dummy(ROW > 0 ? ROW : 480, COL > 0 ? COL : 640, CV_8UC1, cv::Scalar(128));
    const int loops = 6;

    engines_[0].setCore(RKNN_NPU_CORE_0_1_2);
    const double t_combo = benchMs(loops, [&] {
        (void)detect(dummy, cv::Mat(), 50, 0);
    });

    engines_[0].setCore(RKNN_NPU_CORE_0);
    const double t_core0 = benchMs(loops, [&] {
        (void)detect(dummy, cv::Mat(), 50, 0);
    });

    combo_mode_ = false;
    const double t_wait_left = benchMs(loops, [&] {
        (void)detectPrimary(dummy, dummy, dummy, cv::Mat(), 50);
    });

    ROS_INFO("[NPU] bench combo(CORE_0_1_2)=%.2f ms  single-core0=%.2f ms  triple-wait-left=%.2f ms",
             t_combo, t_core0, t_wait_left);

    if (NPU_MODE == 1)
        combo_mode_ = true;
    else if (NPU_MODE == 2)
        combo_mode_ = false;
    else
        combo_mode_ = (t_combo <= t_wait_left + 0.5) || (t_combo <= t_core0 + 0.5);

    if (combo_mode_)
    {
        engines_[0].setCore(RKNN_NPU_CORE_0_1_2);
        ROS_INFO("[NPU] using combo: rknn_set_core_mask(CORE_0_1_2), 1 thread, decode left only");
    }
    else
    {
        engines_[0].setCore(RKNN_NPU_CORE_0);
        ROS_INFO("[NPU] using triple: 3 NPU worker threads, wait left only, core1/2 no decode");
    }
}

bool NpuSuperPointFrontend::prepareNn(const cv::Mat &gray, int core_id, cv::Mat &nn_in, cv::Mat &img)
{
    if (gray.empty())
        return false;
    if (gray.channels() != 1)
        cv::cvtColor(gray, img, cv::COLOR_BGR2GRAY);
    else
        img = gray;

    const int in_h = engines_[core_id].input_h();
    const int in_w = engines_[core_id].input_w();
    const int in_c = std::max(1, engines_[core_id].input_c());
    cv::Mat resized;
    if (img.rows != in_h || img.cols != in_w)
        cv::resize(img, resized, cv::Size(in_w, in_h));
    else
        resized = img;
    if (!resized.isContinuous())
        resized = resized.clone();
    if (in_c == 3)
        cv::cvtColor(resized, nn_in, cv::COLOR_GRAY2RGB);
    else
        nn_in = resized;
    if (!nn_in.isContinuous())
        nn_in = nn_in.clone();
    return true;
}

std::vector<cv::Point2f> NpuSuperPointFrontend::detect(const cv::Mat &gray, const cv::Mat &mask,
                                                       int max_cnt, int core_id)
{
    if (!ready_ || gray.empty() || max_cnt <= 0)
        return {};
    core_id = std::max(0, std::min(2, core_id));

    cv::Mat nn_in, img;
    if (!prepareNn(gray, core_id, nn_in, img))
        return {};

    std::vector<std::vector<float>> outs;
    {
        std::lock_guard<std::mutex> lk(mu_[core_id]);
        if (!engines_[core_id].infer(nn_in.data, engines_[core_id].input_h(),
                                     engines_[core_id].input_w(),
                                     std::max(1, engines_[core_id].input_c()), &outs, 1))
        {
            ROS_WARN_THROTTLE(1.0, "[NPU] infer failed on core %d", core_id);
            return {};
        }
    }
    return decode(outs, mask, img.cols, img.rows, max_cnt);
}

std::vector<cv::Point2f> NpuSuperPointFrontend::detectPrimary(const cv::Mat &left, const cv::Mat &right,
                                                              const cv::Mat &prev, const cv::Mat &mask,
                                                              int max_cnt)
{
    if (!ready_ || left.empty() || max_cnt <= 0)
        return {};

    if (combo_mode_)
        return detect(left, mask, max_cnt, 0);

    cv::Mat nn0, g0;
    if (!prepareNn(left, 0, nn0, g0))
        return {};

    auto p0 = std::make_shared<std::promise<std::vector<cv::Point2f>>>();
    auto f0 = p0->get_future();
    {
        std::lock_guard<std::mutex> lk(job_mu_[0]);
        jobs_[0].push(Job{nn0.clone(), mask.empty() ? cv::Mat() : mask.clone(),
                          g0.cols, g0.rows, max_cnt, true, p0});
    }
    job_cv_[0].notify_one();

    const cv::Mat &r = right.empty() ? left : right;
    const cv::Mat &p = prev.empty() ? left : prev;
    cv::Mat nn1, g1, nn2, g2;
    prepareNn(r, 1, nn1, g1);
    prepareNn(p, 2, nn2, g2);
    for (int i = 1; i < 3; i++)
    {
        auto pd = std::make_shared<std::promise<std::vector<cv::Point2f>>>();
        {
            std::lock_guard<std::mutex> lk(job_mu_[i]);
            jobs_[i].push(Job{(i == 1 ? nn1 : nn2).clone(), cv::Mat(), 0, 0, 1, false, pd});
        }
        job_cv_[i].notify_one();
        // Do not wait: NPU thread blocks in rknn_run (idle CPU). Tracker only needs left.
    }

    return f0.get();
}

std::vector<cv::Point2f> NpuSuperPointFrontend::decode(const std::vector<std::vector<float>> &outs,
                                                       const cv::Mat &mask, int img_w, int img_h, int max_cnt)
{
    if (outs.empty() || outs[0].empty())
        return {};

    const int in_h = engines_[0].input_h() > 0 ? engines_[0].input_h() : img_h;
    const int in_w = engines_[0].input_w() > 0 ? engines_[0].input_w() : img_w;
    const int hc = in_h / 8;
    const int wc = in_w / 8;
    if (hc * wc <= 0)
        return {};

    int semi_idx = -1;
    int heat_idx = -1;
    const int cell = hc * wc;
    for (size_t i = 0; i < outs.size(); i++)
    {
        const int n = static_cast<int>(outs[i].size());
        if (n == 65 * cell)
            semi_idx = static_cast<int>(i);
        else if (n == cell || n == 1 * cell)
            heat_idx = static_cast<int>(i);
    }
    const int best = semi_idx >= 0 ? semi_idx : (heat_idx >= 0 ? heat_idx : 0);
    const std::vector<float> &score = outs[best];

    const float sx = static_cast<float>(img_w) / static_cast<float>(in_w);
    const float sy = static_cast<float>(img_h) / static_cast<float>(in_h);

    std::vector<std::pair<float, cv::Point2f>> cand;
    cand.reserve(256);

    if (static_cast<int>(score.size()) >= 65 * cell)
    {
        // Cheap score: argmax 64 vs dustbin, no 65-way softmax (saves CPU).
        for (int y = 0; y < hc; y++)
        {
            for (int x = 0; x < wc; x++)
            {
                int best_k = 0;
                float best_v = -1e9f;
                for (int k = 0; k < 64; k++)
                {
                    const float v = score[k * cell + y * wc + x];
                    if (v > best_v)
                    {
                        best_v = v;
                        best_k = k;
                    }
                }
                const float dust = score[64 * cell + y * wc + x];
                const float p = 1.f / (1.f + std::exp(dust - best_v));
                if (p < score_th_)
                    continue;
                const int px = static_cast<int>(((x * 8) + (best_k % 8) + 0.5f) * sx);
                const int py = static_cast<int>(((y * 8) + (best_k / 8) + 0.5f) * sy);
                if (px < 2 || py < 2 || px >= img_w - 2 || py >= img_h - 2)
                    continue;
                if (!mask.empty() && mask.at<uchar>(py, px) == 0)
                    continue;
                cand.emplace_back(p, cv::Point2f(static_cast<float>(px), static_cast<float>(py)));
            }
        }
    }
    else if (static_cast<int>(score.size()) >= hc * wc)
    {
        for (int y = 0; y < hc; y++)
        {
            for (int x = 0; x < wc; x++)
            {
                const float p = score[y * wc + x];
                if (p < score_th_)
                    continue;
                const int px = static_cast<int>((x * 8 + 4) * sx);
                const int py = static_cast<int>((y * 8 + 4) * sy);
                if (px < 2 || py < 2 || px >= img_w - 2 || py >= img_h - 2)
                    continue;
                if (!mask.empty() && mask.at<uchar>(py, px) == 0)
                    continue;
                cand.emplace_back(p, cv::Point2f(static_cast<float>(px), static_cast<float>(py)));
            }
        }
    }

    std::sort(cand.begin(), cand.end(),
              [](const std::pair<float, cv::Point2f> &a, const std::pair<float, cv::Point2f> &b) {
                  return a.first > b.first;
              });

    std::vector<cv::Point2f> out;
    const int nms = std::max(4, MIN_DIST / 2);
    for (const auto &c : cand)
    {
        bool keep = true;
        for (const auto &q : out)
        {
            if (std::fabs(q.x - c.second.x) < nms && std::fabs(q.y - c.second.y) < nms)
            {
                keep = false;
                break;
            }
        }
        if (keep)
        {
            out.push_back(c.second);
            if (static_cast<int>(out.size()) >= max_cnt)
                break;
        }
    }
    return out;
}
