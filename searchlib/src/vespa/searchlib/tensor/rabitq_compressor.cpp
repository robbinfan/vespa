// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "rabitq_compressor.h"
#include <vespa/searchlib/util/bufferwriter.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <numeric>
#include <random>
#include <stdexcept>

namespace search::tensor {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

// Pack d binary bits (0/1 in float array) into ceil(d/8) bytes
void pack_bits(const float* bits, size_t dims, uint8_t* out)
{
    size_t nbytes = (dims + 7) / 8;
    std::fill(out, out + nbytes, uint8_t(0));
    for (size_t i = 0; i < dims; ++i) {
        if (bits[i] >= 0.0f) {
            out[i / 8] |= uint8_t(1u << (i % 8));
        }
    }
}

// Count bits set (popcount across a byte array)
uint32_t popcount_bytes(const uint8_t* a, size_t nbytes)
{
    uint32_t cnt = 0;
    for (size_t i = 0; i < nbytes; ++i) {
        cnt += static_cast<uint32_t>(__builtin_popcount(a[i]));
    }
    return cnt;
}

// Read/write little-endian float32
float read_f32(const uint8_t* p) {
    float v;
    std::memcpy(&v, p, sizeof(float));
    return v;
}
void write_f32(uint8_t* p, float v) {
    std::memcpy(p, &v, sizeof(float));
}

} // anonymous

// ---------------------------------------------------------------------------
// RaBitQCompressor
// ---------------------------------------------------------------------------

void RaBitQCompressor::project(const float* v_centered, float* out_projected) const
{
    // Structured ±1/sqrt(d) Rademacher sketch: P[i][j] = _projection[i*_dims+j]
    // out[i] = sum_j P[i][j] * v_centered[j]
    float scale = 1.0f / std::sqrt(static_cast<float>(_dims));
    for (size_t i = 0; i < _dims; ++i) {
        float acc = 0.0f;
        const float* row = _projection.data() + i * _dims;
        for (size_t j = 0; j < _dims; ++j) {
            acc += row[j] * v_centered[j];
        }
        out_projected[i] = acc * scale;
    }
}

void RaBitQCompressor::train(const std::vector<const float*>& vectors, size_t dims)
{
    if (vectors.empty() || dims == 0) return;
    _dims = dims;

    // 1. Compute centroid
    _centroid.assign(dims, 0.0f);
    for (const float* v : vectors) {
        for (size_t j = 0; j < dims; ++j) _centroid[j] += v[j];
    }
    float inv_n = 1.0f / static_cast<float>(vectors.size());
    for (float& c : _centroid) c *= inv_n;

    // 2. Generate Rademacher ±1 random matrix (seeded for reproducibility)
    // Stored as ±1.0f; scaling by 1/sqrt(d) happens at project() time
    _projection.resize(dims * dims);
    std::mt19937_64 rng(0xDEADBEEF42ULL);
    std::bernoulli_distribution coin(0.5);
    for (float& p : _projection) {
        p = coin(rng) ? 1.0f : -1.0f;
    }
}

void RaBitQCompressor::compress(const float* in, void* out, size_t dims) const
{
    assert(dims == _dims && !_centroid.empty());
    size_t nbits_bytes = (dims + 7) / 8;
    auto* dst = static_cast<uint8_t*>(out);

    // Center the vector
    std::vector<float> centered(dims);
    for (size_t i = 0; i < dims; ++i) {
        centered[i] = in[i] - _centroid[i];
    }

    // Original-space norm (before projection)
    float norm = 0.0f;
    for (float c : centered) norm += c * c;
    norm = std::sqrt(norm);

    // Project
    std::vector<float> projected(dims);
    project(centered.data(), projected.data());

    // Binarise and pack bits
    pack_bits(projected.data(), dims, dst);
    dst += nbits_bytes;

    // Write norm
    write_f32(dst, norm);
    dst += sizeof(float);

    // Quantisation correction factor:
    // factor = (1/||projected||) * sum_i |projected_i| / sqrt(d)
    float proj_norm = 0.0f;
    float abs_sum = 0.0f;
    for (float p : projected) {
        proj_norm += p * p;
        abs_sum   += std::fabs(p);
    }
    proj_norm = std::sqrt(proj_norm);
    float factor = (proj_norm > 1e-9f)
        ? abs_sum / (proj_norm * std::sqrt(static_cast<float>(dims)))
        : 1.0f;
    write_f32(dst, factor);
}

void RaBitQCompressor::decompress(const void* in, float* out, size_t dims) const
{
    assert(dims == _dims && !_centroid.empty());
    size_t nbits_bytes = (dims + 7) / 8;
    const auto* src = static_cast<const uint8_t*>(in);

    // Reconstruct signed binary vector from packed bits
    std::vector<float> binary(dims);
    for (size_t i = 0; i < dims; ++i) {
        binary[i] = (src[i / 8] >> (i % 8)) & 1u ? 1.0f : -1.0f;
    }
    src += nbits_bytes;

    float norm = read_f32(src);
    // factor is unused in decompress; skip
    // src += sizeof(float); src += sizeof(float);

    // Reconstruct: v ≈ c + norm * P^T binary / sqrt(d)
    float scale = norm / std::sqrt(static_cast<float>(dims));
    for (size_t j = 0; j < dims; ++j) {
        float acc = 0.0f;
        for (size_t i = 0; i < dims; ++i) {
            acc += _projection[i * dims + j] * binary[i];
        }
        out[j] = _centroid[j] + acc * scale;
    }
}

void RaBitQCompressor::save_params(BufferWriter& writer) const
{
    // dims (uint64_t)
    uint64_t d = static_cast<uint64_t>(_dims);
    writer.write(&d, sizeof(d));
    // centroid
    if (_dims > 0) {
        writer.write(_centroid.data(), _dims * sizeof(float));
        writer.write(_projection.data(), _dims * _dims * sizeof(float));
    }
    writer.flush();
}

void RaBitQCompressor::load_params(const void* data, size_t len)
{
    if (len < sizeof(uint64_t)) throw std::runtime_error("RaBitQ: param blob too short");
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t d;
    std::memcpy(&d, p, sizeof(d));
    p += sizeof(d);
    _dims = static_cast<size_t>(d);
    size_t expected = sizeof(uint64_t) + _dims * sizeof(float) + _dims * _dims * sizeof(float);
    if (len < expected) throw std::runtime_error("RaBitQ: param blob truncated");
    _centroid.resize(_dims);
    std::memcpy(_centroid.data(), p, _dims * sizeof(float));
    p += _dims * sizeof(float);
    _projection.resize(_dims * _dims);
    std::memcpy(_projection.data(), p, _dims * _dims * sizeof(float));
}

}
