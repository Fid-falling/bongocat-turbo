#ifndef BONGO_CAT_CUBISM_RUNTIME_HPP
#define BONGO_CAT_CUBISM_RUNTIME_HPP

#include "cubism_model.hpp"

struct BongoCatRetiredModel {
    bongo_cat::NativeModel *model = nullptr;
    unsigned frames_remaining = 0;
};

struct BongoCatLive2D {
    static constexpr unsigned retired_capacity = 4;
    bongo_cat::NativeModel *model;
    BongoCatRetiredModel retired[retired_capacity]{};
    unsigned retired_count = 0;
    int width = 612;
    int height = 354;
};

#endif
