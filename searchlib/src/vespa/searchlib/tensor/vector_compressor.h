// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include <vespa/eval/eval/cell_type.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace search { class BufferWriter; }

namespace search::tensor {

/**
 * Extensible interface for vector compression / quantization.
 *
 * Implementations cover the full spectrum from lossless (NONE / BFLOAT16)
 * to highly compressed (INT8, RaBitQ, BBQ).  New algorithms are added by
 * sub-classing this interface without touching the surrounding index code.
 *
 * Lifecycle during fusion:
 *   1. train()          -- optional statistical fitting over all vectors
 *   2. compress()       -- per-vector, write into fusion output
 *   3. save_params()    -- persist quantization state alongside the index
 *
 * Lifecycle during load:
 *   1. load_params()    -- restore quantization state from file
 *   2. decompress()     -- on-demand, e.g. for reranking
 */
class VectorCompressor {
public:
    using UP = std::unique_ptr<VectorCompressor>;

    enum class CompressionType { NONE, INT8, BFLOAT16, RABITQ, BBQ };

    virtual ~VectorCompressor() = default;

    /**
     * Optional statistical fitting.  Called once before compressing any
     * vector during fusion.  Algorithms that need global statistics
     * (scale factors, rotation matrices, codebooks) do their work here.
     * Stateless algorithms leave this as a no-op.
     *
     * @param vectors   pointers to float32 vectors; each has length `dims`
     * @param dims      dimensionality
     */
    virtual void train(const std::vector<const float*>& vectors, size_t dims) { (void)vectors; (void)dims; }

    /**
     * Compress one float32 vector.
     * `out` must point to at least compressed_bytes(dims) pre-allocated bytes.
     */
    virtual void compress(const float* in, void* out, size_t dims) const = 0;

    /**
     * Decompress one compressed vector back to float32.
     * Used for reranking / exact distance re-evaluation.
     */
    virtual void decompress(const void* in, float* out, size_t dims) const = 0;

    /** Number of bytes used to store one compressed vector. */
    virtual size_t compressed_bytes(size_t dims) const = 0;

    /**
     * CellType of the compressed storage.
     * Determines how distance functions interpret the stored bytes.
     */
    virtual vespalib::eval::CellType cell_type() const = 0;

    /** Serialize quantization parameters (scale, codebook, etc.) to writer. */
    virtual void save_params(BufferWriter& writer) const = 0;

    /**
     * Restore quantization parameters from a raw byte buffer.
     * @param data  pointer to serialized bytes produced by save_params()
     * @param len   byte count
     */
    virtual void load_params(const void* data, size_t len) = 0;

    /** The CompressionType enum value that identifies this compressor. */
    virtual CompressionType compression_type() const = 0;

    /** Factory: create a compressor of the given type. */
    static UP create(CompressionType type);
};

vespalib::string to_string(VectorCompressor::CompressionType t);
VectorCompressor::CompressionType compression_type_from_string(vespalib::stringref s);

}
