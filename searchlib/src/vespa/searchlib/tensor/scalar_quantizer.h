// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "vector_compressor.h"
#include <vespa/vespalib/util/bfloat16.h>
#include <limits>

namespace search::tensor {

/**
 * Scalar (per-dimension uniform) quantizer.
 *
 * INT8 variant:
 *   During train(), finds global min/max across all training vectors.
 *   compress() linearly maps [min, max] → [-127, 127] and stores int8.
 *   decompress() reverses the mapping.
 *   compressed_bytes = dims * sizeof(int8_t)
 *
 * BFLOAT16 variant:
 *   No training required; simply truncates the lower 16 bits of each float.
 *   compressed_bytes = dims * sizeof(BFloat16)
 */
template <vespalib::eval::CellType CT>
class ScalarQuantizer : public VectorCompressor {
    static_assert(CT == vespalib::eval::CellType::INT8 ||
                  CT == vespalib::eval::CellType::BFLOAT16,
                  "ScalarQuantizer only supports INT8 and BFLOAT16");

    // INT8 state: global scale computed during train()
    float _scale{1.0f};
    float _offset{0.0f};   // maps compressed value 0 → original value _offset
    bool  _trained{false};

public:
    ScalarQuantizer() = default;
    ~ScalarQuantizer() override = default;

    // --- VectorCompressor interface ---

    void train(const std::vector<const float*>& vectors, size_t dims) override;

    void compress(const float* in, void* out, size_t dims) const override;

    void decompress(const void* in, float* out, size_t dims) const override;

    size_t compressed_bytes(size_t dims) const override;

    vespalib::eval::CellType cell_type() const override { return CT; }

    CompressionType compression_type() const override {
        if constexpr (CT == vespalib::eval::CellType::INT8) return CompressionType::INT8;
        else return CompressionType::BFLOAT16;
    }

    void save_params(BufferWriter& writer) const override;

    void load_params(const void* data, size_t len) override;

    // For testing
    float scale()  const { return _scale; }
    float offset() const { return _offset; }
};

}
