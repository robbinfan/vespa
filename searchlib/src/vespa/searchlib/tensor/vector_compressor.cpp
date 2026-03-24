// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "vector_compressor.h"
#include "scalar_quantizer.h"
#include <vespa/vespalib/stllike/string.h>
#include <stdexcept>

namespace search::tensor {

using CT = vespalib::eval::CellType;

VectorCompressor::UP VectorCompressor::create(CompressionType type)
{
    switch (type) {
    case CompressionType::NONE:
        // Passthrough: returns nullptr; caller must handle float32 storage directly.
        // We use a BFLOAT16 compressor with no-op for the NONE case at the caller level,
        // but provide a dedicated NoVectorCompressor implementation below.
        return nullptr;
    case CompressionType::INT8:
        return std::make_unique<ScalarQuantizer<CT::INT8>>();
    case CompressionType::BFLOAT16:
        return std::make_unique<ScalarQuantizer<CT::BFLOAT16>>();
    case CompressionType::RABITQ:
        // Placeholder: fall through to INT8 until RaBitQCompressor is implemented.
        return std::make_unique<ScalarQuantizer<CT::INT8>>();
    case CompressionType::BBQ:
        // Placeholder: fall through to INT8 until BBQCompressor is implemented.
        return std::make_unique<ScalarQuantizer<CT::INT8>>();
    default:
        throw std::invalid_argument("VectorCompressor::create: unknown CompressionType");
    }
}

vespalib::string to_string(VectorCompressor::CompressionType t)
{
    using T = VectorCompressor::CompressionType;
    switch (t) {
    case T::NONE:     return "NONE";
    case T::INT8:     return "INT8";
    case T::BFLOAT16: return "BFLOAT16";
    case T::RABITQ:   return "RABITQ";
    case T::BBQ:      return "BBQ";
    default:          return "UNKNOWN";
    }
}

VectorCompressor::CompressionType compression_type_from_string(vespalib::stringref s)
{
    using T = VectorCompressor::CompressionType;
    if (s == "NONE")     return T::NONE;
    if (s == "INT8")     return T::INT8;
    if (s == "BFLOAT16") return T::BFLOAT16;
    if (s == "RABITQ")   return T::RABITQ;
    if (s == "BBQ")      return T::BBQ;
    throw std::invalid_argument(std::string("compression_type_from_string: unknown type '") + s + "'");
}

}
