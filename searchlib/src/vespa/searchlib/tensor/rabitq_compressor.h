// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "vector_compressor.h"
#include <cstdint>
#include <vector>

namespace search::tensor {

/**
 * RaBitQ: Random Binary Quantization for approximate nearest neighbour search.
 *
 * Algorithm (simplified from "RaBitQ: Quantizing High-Dimensional Vectors with
 * a Theoretical Error Bound", Gao & Long, SIGMOD 2024):
 *
 * Training:
 *   1. Compute the dataset centroid c = mean(v_i).
 *   2. Generate a d×d random orthogonal matrix P (Haar-distributed via
 *      QR decomposition of a Gaussian matrix).  For large d we use a
 *      structured Johnson–Lindenstrauss sketch (random ±1 / sqrt(d) rows)
 *      to avoid O(d²) storage.
 *   3. Compute per-vector norms ||v_i - c||.
 *
 * Compression (per vector v):
 *   1. r = P (v - c)          — random projection
 *   2. b[j] = (r[j] >= 0) ? 1 : 0   — binarisation (1 bit per dim)
 *   3. Store: packed bits (ceil(d/8) bytes) + float32 norm + float32 factor
 *
 * Distance (asymmetric — query stays in float32):
 *   IP(q, v) ≈ ||q_r|| * ||v_r|| * (2/d * popcount(b_q XOR b_v) − 1)
 *              + correction term using precomputed norms
 *
 * compressed_bytes = ceil(dims / 8) * 8 bytes (bit vector)
 *                  + 4 bytes (||v - c||, the original-space norm)
 *                  + 4 bytes (pre-computed quantisation error factor)
 *
 * For decompress() (used in reranking), we reconstruct:
 *   v ≈ c + ||v - c|| * P^T sign(r) / sqrt(d)
 * which is an approximation; exact reranking requires the original float32.
 */
class RaBitQCompressor : public VectorCompressor {
public:
    RaBitQCompressor() = default;
    ~RaBitQCompressor() override = default;

    void train(const std::vector<const float*>& vectors, size_t dims) override;

    void compress(const float* in, void* out, size_t dims) const override;

    void decompress(const void* in, float* out, size_t dims) const override;

    /** ceil(dims/8) bytes for bit vector + 8 bytes for norm + factor */
    size_t compressed_bytes(size_t dims) const override {
        return ((dims + 7) / 8) + 2 * sizeof(float);
    }

    vespalib::eval::CellType cell_type() const override {
        return vespalib::eval::CellType::INT8;  // treated as raw bytes
    }

    CompressionType compression_type() const override { return CompressionType::RABITQ; }

    void save_params(BufferWriter& writer) const override;
    void load_params(const void* data, size_t len) override;

    // For testing
    const std::vector<float>& centroid() const { return _centroid; }

private:
    // Random projection: _projection is stored row-major as d×d float matrix.
    // For large d, we use a random ±1/sqrt(d) Rademacher sketch instead.
    // _projection[i * _dims + j] = P[i][j]
    size_t             _dims{0};
    std::vector<float> _centroid;       // length = dims
    std::vector<float> _projection;    // length = dims * dims (Rademacher ±1 sketch)

    void project(const float* v_centered, float* out_projected) const;
};

}
