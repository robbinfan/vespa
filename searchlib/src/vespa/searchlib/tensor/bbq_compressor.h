// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "vector_compressor.h"
#include <cstdint>
#include <vector>

namespace search::tensor {

/**
 * BBQ: Better Binary Quantization via multi-bit binary decomposition.
 *
 * Decomposes each (centered, scaled) float dimension into `bits_per_dim`
 * binary planes.  This is essentially a uniform scalar quantizer with
 * `2^bits_per_dim` levels stored as bit-planes, enabling fast inner-product
 * estimation via popcount operations without a lookup table.
 *
 * Algorithm:
 *   Training:
 *     - Compute centroid c and per-dimension standard deviation σ_j.
 *     - Scale: s_j = (2^bits - 1) / (k_sigma * σ_j)  (k_sigma = 3 by default)
 *
 *   Compression (per vector v):
 *     - r_j = clamp(round((v_j - c_j) * s_j), 0, 2^bits - 1)  — integer code
 *     - Pack each bit-plane b (bit b of r_j) into a uint64 word stream.
 *     - Layout: [bit-plane 0 packed bits] [bit-plane 1 packed bits] ... [bit-plane B-1]
 *     - Each plane occupies ceil(d/8) bytes; total = B * ceil(d/8) bytes.
 *
 *   Distance (approximate inner product / Euclidean):
 *     - Dequantise: v_j ≈ c_j + r_j / s_j
 *     - For fast search, asymmetric distance uses popcount across bit-planes.
 *
 * compressed_bytes = bits_per_dim * ceil(dims / 8)
 *
 * Default bits_per_dim = 4 gives ~8x compression vs float32 with good recall.
 */
class BBQCompressor : public VectorCompressor {
public:
    static constexpr uint32_t DEFAULT_BITS = 4;
    static constexpr float    DEFAULT_SIGMA_MULTIPLIER = 3.0f;

    explicit BBQCompressor(uint32_t bits_per_dim = DEFAULT_BITS,
                           float sigma_multiplier = DEFAULT_SIGMA_MULTIPLIER);
    ~BBQCompressor() override = default;

    void train(const std::vector<const float*>& vectors, size_t dims) override;
    void compress(const float* in, void* out, size_t dims) const override;
    void decompress(const void* in, float* out, size_t dims) const override;

    size_t compressed_bytes(size_t dims) const override {
        return _bits_per_dim * ((dims + 7) / 8);
    }

    vespalib::eval::CellType cell_type() const override {
        return vespalib::eval::CellType::INT8;  // raw bytes
    }

    void save_params(BufferWriter& writer) const override;
    void load_params(const void* data, size_t len) override;

    uint32_t bits_per_dim() const { return _bits_per_dim; }

private:
    uint32_t           _bits_per_dim;
    float              _sigma_multiplier;
    size_t             _dims{0};
    std::vector<float> _centroid;    // length = dims
    std::vector<float> _scale;       // per-dimension quantisation scale s_j
};

}
