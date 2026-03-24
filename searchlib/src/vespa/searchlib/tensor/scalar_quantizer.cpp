// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "scalar_quantizer.h"
#include <vespa/searchlib/util/bufferwriter.h>
#include <vespa/vespalib/util/bfloat16.h>
#include <vespa/eval/eval/int8float.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

using vespalib::BFloat16;

namespace search::tensor {

using namespace vespalib::eval;

// ---------------------------------------------------------------------------
// INT8 specialization
// ---------------------------------------------------------------------------

template <>
void ScalarQuantizer<CellType::INT8>::train(const std::vector<const float*>& vectors, size_t dims)
{
    if (vectors.empty()) return;
    float gmin = std::numeric_limits<float>::max();
    float gmax = std::numeric_limits<float>::lowest();
    for (const float* v : vectors) {
        for (size_t i = 0; i < dims; ++i) {
            gmin = std::min(gmin, v[i]);
            gmax = std::max(gmax, v[i]);
        }
    }
    // map [gmin, gmax] -> [-127, 127]
    // compressed = round((value - gmin) / (gmax - gmin) * 254 - 127)
    float range = gmax - gmin;
    if (range < 1e-9f) range = 1.0f;   // degenerate: all values equal
    _scale  = 254.0f / range;            // float -> int8  multiply
    _offset = gmin;                      // subtract before scaling
    _trained = true;
}

template <>
void ScalarQuantizer<CellType::INT8>::compress(const float* in, void* out, size_t dims) const
{
    auto* dst = static_cast<int8_t*>(out);
    for (size_t i = 0; i < dims; ++i) {
        float v = (in[i] - _offset) * _scale - 127.0f;
        v = std::max(-127.0f, std::min(127.0f, std::roundf(v)));
        dst[i] = static_cast<int8_t>(static_cast<int>(v));
    }
}

template <>
void ScalarQuantizer<CellType::INT8>::decompress(const void* in, float* out, size_t dims) const
{
    const auto* src = static_cast<const int8_t*>(in);
    for (size_t i = 0; i < dims; ++i) {
        out[i] = (static_cast<float>(src[i]) + 127.0f) / _scale + _offset;
    }
}

template <>
size_t ScalarQuantizer<CellType::INT8>::compressed_bytes(size_t dims) const
{
    return dims * sizeof(int8_t);
}

template <>
void ScalarQuantizer<CellType::INT8>::save_params(BufferWriter& writer) const
{
    writer.write(&_scale,  sizeof(_scale));
    writer.write(&_offset, sizeof(_offset));
    writer.flush();
}

template <>
void ScalarQuantizer<CellType::INT8>::load_params(const void* data, size_t len)
{
    if (len < sizeof(float) * 2) {
        throw std::runtime_error("ScalarQuantizer<INT8>: param blob too short");
    }
    const float* p = static_cast<const float*>(data);
    _scale   = p[0];
    _offset  = p[1];
    _trained = true;
}

// ---------------------------------------------------------------------------
// BFLOAT16 specialization
// ---------------------------------------------------------------------------

template <>
void ScalarQuantizer<CellType::BFLOAT16>::train(const std::vector<const float*>&, size_t)
{
    // no statistics needed
    _trained = true;
}

template <>
void ScalarQuantizer<CellType::BFLOAT16>::compress(const float* in, void* out, size_t dims) const
{
    auto* dst = static_cast<BFloat16*>(out);
    for (size_t i = 0; i < dims; ++i) {
        dst[i] = BFloat16(in[i]);
    }
}

template <>
void ScalarQuantizer<CellType::BFLOAT16>::decompress(const void* in, float* out, size_t dims) const
{
    const auto* src = static_cast<const BFloat16*>(in);
    for (size_t i = 0; i < dims; ++i) {
        out[i] = static_cast<float>(src[i]);
    }
}

template <>
size_t ScalarQuantizer<CellType::BFLOAT16>::compressed_bytes(size_t dims) const
{
    return dims * sizeof(BFloat16);
}

template <>
void ScalarQuantizer<CellType::BFLOAT16>::save_params(BufferWriter& writer) const
{
    // BFLOAT16 has no parameters; write a zero-length sentinel
    writer.flush();
}

template <>
void ScalarQuantizer<CellType::BFLOAT16>::load_params(const void*, size_t)
{
    _trained = true;
}

// ---------------------------------------------------------------------------
// Explicit instantiations
// ---------------------------------------------------------------------------
template class ScalarQuantizer<CellType::INT8>;
template class ScalarQuantizer<CellType::BFLOAT16>;

}
