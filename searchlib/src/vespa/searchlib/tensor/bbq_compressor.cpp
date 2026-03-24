// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "bbq_compressor.h"
#include <vespa/searchlib/util/bufferwriter.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace search::tensor {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

void write_u32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
void write_f32(uint8_t* p, float v)   { std::memcpy(p, &v, 4); }
uint32_t read_u32(const uint8_t* p)   { uint32_t v; std::memcpy(&v, p, 4); return v; }
float    read_f32(const uint8_t* p)   { float v;    std::memcpy(&v, p, 4); return v; }

} // anonymous

// ---------------------------------------------------------------------------
// BBQCompressor
// ---------------------------------------------------------------------------

BBQCompressor::BBQCompressor(uint32_t bits_per_dim, float sigma_multiplier)
    : _bits_per_dim(std::clamp(bits_per_dim, 1u, 8u)),
      _sigma_multiplier(sigma_multiplier)
{}

void BBQCompressor::train(const std::vector<const float*>& vectors, size_t dims)
{
    if (vectors.empty() || dims == 0) return;
    _dims = dims;
    size_t n = vectors.size();

    // Centroid
    _centroid.assign(dims, 0.0f);
    for (const float* v : vectors) {
        for (size_t j = 0; j < dims; ++j) _centroid[j] += v[j];
    }
    float inv_n = 1.0f / static_cast<float>(n);
    for (float& c : _centroid) c *= inv_n;

    // Per-dimension standard deviation
    _scale.assign(dims, 1.0f);
    for (size_t j = 0; j < dims; ++j) {
        float var = 0.0f;
        for (const float* v : vectors) {
            float d = v[j] - _centroid[j];
            var += d * d;
        }
        float sigma = std::sqrt(var * inv_n);
        // range = k_sigma * sigma, map [−range, +range] → [0, 2^bits − 1]
        float range = _sigma_multiplier * sigma;
        if (range < 1e-9f) range = 1.0f;
        float max_code = static_cast<float>((1u << _bits_per_dim) - 1u);
        _scale[j] = max_code / (2.0f * range);   // float → code
    }
}

void BBQCompressor::compress(const float* in, void* out, size_t dims) const
{
    assert(dims == _dims && !_centroid.empty());
    size_t plane_bytes = (dims + 7) / 8;
    auto* dst = static_cast<uint8_t*>(out);
    std::fill(dst, dst + _bits_per_dim * plane_bytes, uint8_t(0));

    uint32_t max_code = (1u << _bits_per_dim) - 1u;
    float half = static_cast<float>(max_code) / 2.0f;

    for (size_t j = 0; j < dims; ++j) {
        // Center and scale to [0, max_code]
        float centered = (in[j] - _centroid[j]) * _scale[j] + half;
        int32_t code = static_cast<int32_t>(std::roundf(centered));
        code = std::clamp(code, 0, static_cast<int32_t>(max_code));
        uint32_t ucode = static_cast<uint32_t>(code);

        // Scatter bits into planes
        size_t byte_idx = j / 8;
        uint32_t bit_idx = static_cast<uint32_t>(j % 8);
        for (uint32_t b = 0; b < _bits_per_dim; ++b) {
            if ((ucode >> b) & 1u) {
                dst[b * plane_bytes + byte_idx] |= uint8_t(1u << bit_idx);
            }
        }
    }
}

void BBQCompressor::decompress(const void* in, float* out, size_t dims) const
{
    assert(dims == _dims && !_centroid.empty());
    size_t plane_bytes = (dims + 7) / 8;
    const auto* src = static_cast<const uint8_t*>(in);

    uint32_t max_code = (1u << _bits_per_dim) - 1u;
    float half = static_cast<float>(max_code) / 2.0f;

    for (size_t j = 0; j < dims; ++j) {
        size_t byte_idx = j / 8;
        uint32_t bit_idx = static_cast<uint32_t>(j % 8);
        uint32_t ucode = 0;
        for (uint32_t b = 0; b < _bits_per_dim; ++b) {
            if ((src[b * plane_bytes + byte_idx] >> bit_idx) & 1u) {
                ucode |= (1u << b);
            }
        }
        float centered = (static_cast<float>(ucode) - half) / _scale[j];
        out[j] = _centroid[j] + centered;
    }
}

void BBQCompressor::save_params(BufferWriter& writer) const
{
    // bits_per_dim (uint32)
    uint32_t bits = _bits_per_dim;
    writer.write(&bits, sizeof(bits));
    // sigma_multiplier (float)
    writer.write(&_sigma_multiplier, sizeof(_sigma_multiplier));
    // dims (uint64)
    uint64_t d = static_cast<uint64_t>(_dims);
    writer.write(&d, sizeof(d));
    if (_dims > 0) {
        writer.write(_centroid.data(), _dims * sizeof(float));
        writer.write(_scale.data(),    _dims * sizeof(float));
    }
    writer.flush();
}

void BBQCompressor::load_params(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    const uint8_t* end = p + len;
    if (p + 4 + 4 + 8 > end) throw std::runtime_error("BBQ: param blob too short");
    _bits_per_dim = read_u32(p); p += 4;
    _sigma_multiplier = read_f32(p); p += 4;
    uint64_t d; std::memcpy(&d, p, 8); p += 8;
    _dims = static_cast<size_t>(d);
    size_t expected_left = _dims * 2 * sizeof(float);
    if (static_cast<size_t>(end - p) < expected_left)
        throw std::runtime_error("BBQ: param blob truncated");
    _centroid.resize(_dims);
    std::memcpy(_centroid.data(), p, _dims * sizeof(float)); p += _dims * sizeof(float);
    _scale.resize(_dims);
    std::memcpy(_scale.data(), p, _dims * sizeof(float));
}

}
