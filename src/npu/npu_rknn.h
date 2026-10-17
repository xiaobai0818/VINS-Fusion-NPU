#pragma once

#include "rknn_api.h"
#include <string>
#include <vector>
#include <cstdint>

class RknnEngine
{
public:
    RknnEngine();
    ~RknnEngine();
    RknnEngine(const RknnEngine &) = delete;
    RknnEngine &operator=(const RknnEngine &) = delete;

    bool init(const std::string &model_path, rknn_core_mask core_mask);
    // Share weights with an already-loaded context (3 cores, 1 weight copy).
    bool dupFrom(RknnEngine &src, rknn_core_mask core_mask);
    bool ready() const { return ctx_ != 0; }
    bool infer(const uint8_t *nhwc, int h, int w, int c,
               std::vector<std::vector<float>> &outputs);
    // outputs==nullptr: run NPU but skip float copy (keeps core busy, little CPU).
    bool infer(const uint8_t *nhwc, int h, int w, int c,
               std::vector<std::vector<float>> *outputs, int want_float);
    bool setCore(rknn_core_mask core_mask) { return bindCore(core_mask); }

    int input_h() const { return in_h_; }
    int input_w() const { return in_w_; }
    int input_c() const { return in_c_; }
    int output_count() const { return n_out_; }

private:
    bool queryIo();
    bool bindCore(rknn_core_mask core_mask);
    rknn_context ctx_;
    rknn_input_output_num io_num_;
    std::vector<rknn_tensor_attr> in_attr_;
    std::vector<rknn_tensor_attr> out_attr_;
    int in_h_, in_w_, in_c_, n_out_;
};
