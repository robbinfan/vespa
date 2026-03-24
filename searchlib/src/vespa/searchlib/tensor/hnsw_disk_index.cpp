// Copyright Yahoo. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "hnsw_disk_index.h"
#include "scalar_quantizer.h"
#include <vespa/fastos/file.h>
#include <vespa/searchlib/util/bufferwriter.h>
#include <vespa/vespalib/io/fileutil.h>
#include <vespa/vespalib/stllike/string.h>
#include <vespa/eval/eval/typed_cells.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <vector>

#include <vespa/log/log.h>
LOG_SETUP(".tensor.hnsw_disk_index");

namespace search::tensor {

using vespalib::string;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace {

string make_dir(const string& base, uint32_t id, bool is_flush)
{
    return base + "/" + (is_flush ? "hnsw.flush." : "hnsw.fusion.") + std::to_string(id);
}

bool write_file(const string& path, const void* data, size_t bytes)
{
    FastOS_File f;
    if (!f.OpenWriteOnlyTruncate(path.c_str())) {
        LOG(error, "HnswDiskIndex: cannot open '%s' for write", path.c_str());
        return false;
    }
    if (bytes > 0 && f.Write2(data, bytes) != static_cast<ssize_t>(bytes)) {
        LOG(error, "HnswDiskIndex: write failed for '%s'", path.c_str());
        return false;
    }
    return true;
}

std::vector<uint8_t> read_file(const string& path)
{
    FastOS_File f;
    if (!f.OpenReadOnly(path.c_str())) return {};
    int64_t sz = f.GetSize();
    if (sz <= 0) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    if (f.Read(buf.data(), sz) != sz) return {};
    return buf;
}

// Simple BufferWriter backed by a std::vector for saving compressor params
class VecBufferWriter : public search::BufferWriter {
    std::vector<uint8_t>& _v;
    std::vector<uint8_t>  _tmp;
public:
    explicit VecBufferWriter(std::vector<uint8_t>& v) : _v(v) {
        _tmp.resize(65536);
        BufferWriter::setup(_tmp.data(), _tmp.size());
    }
    void flush() override {
        size_t used = BufferWriter::usedLen();
        _v.insert(_v.end(), _tmp.data(), _tmp.data() + used);
        BufferWriter::rewind();
    }
};

} // anonymous

// ---------------------------------------------------------------------------
// HnswDiskIndex
// ---------------------------------------------------------------------------

HnswDiskIndex::HnswDiskIndex(vespalib::stringref base_dir, uint32_t id)
    : _base_dir(base_dir), _id(id)
{}

HnswDiskIndex::~HnswDiskIndex() = default;

vespalib::string HnswDiskIndex::dir() const
{
    return make_dir(_base_dir, _id, true);   // always 'flush' naming; fusion uses same class
}

bool HnswDiskIndex::open()
{
    string d = dir();

    // --- meta.dat ---
    {
        auto buf = read_file(d + "/meta.dat");
        if (buf.size() < sizeof(Meta)) {
            LOG(error, "HnswDiskIndex(%s): meta.dat missing or truncated", d.c_str());
            return false;
        }
        std::memcpy(&_meta, buf.data(), sizeof(Meta));
        if (_meta.version != 1) {
            LOG(error, "HnswDiskIndex(%s): unsupported version %u", d.c_str(), _meta.version);
            return false;
        }
    }

    uint32_t n = _meta.num_docs;
    uint32_t dims = _meta.dims;

    // --- docidmap.dat ---
    {
        auto buf = read_file(d + "/docidmap.dat");
        if (buf.size() != n * sizeof(uint32_t)) {
            LOG(error, "HnswDiskIndex(%s): docidmap.dat size mismatch", d.c_str());
            return false;
        }
        _docid_map.resize(n);
        std::memcpy(_docid_map.data(), buf.data(), buf.size());
    }

    // --- compressor.dat (optional) ---
    auto ctype = static_cast<VectorCompressor::CompressionType>(_meta.compression_type);
    if (ctype != VectorCompressor::CompressionType::NONE) {
        _compressor = VectorCompressor::create(ctype);
        auto cbuf = read_file(d + "/compressor.dat");
        _compressor->load_params(cbuf.data(), cbuf.size());
    }

    // --- vectors.dat ---
    {
        size_t vbytes = n * (_compressor
            ? _compressor->compressed_bytes(dims)
            : dims * sizeof(float));
        auto buf = read_file(d + "/vectors.dat");
        if (buf.size() != vbytes) {
            LOG(error, "HnswDiskIndex(%s): vectors.dat size mismatch (got %zu, expected %zu)",
                d.c_str(), buf.size(), vbytes);
            return false;
        }
        _vectors_data = std::move(buf);
    }

    // --- graph.dat ---
    // Format: [GraphNode * n_docs] [uint32_t links_count] [uint32_t links_data...]
    {
        auto buf = read_file(d + "/graph.dat");
        if (buf.size() < n * sizeof(GraphNode)) {
            LOG(error, "HnswDiskIndex(%s): graph.dat too small", d.c_str());
            return false;
        }
        _graph_nodes.resize(n);
        std::memcpy(_graph_nodes.data(), buf.data(), n * sizeof(GraphNode));
        size_t links_bytes = buf.size() - n * sizeof(GraphNode);
        size_t links_count = links_bytes / sizeof(uint32_t);
        _links_data.resize(links_count);
        if (links_count > 0) {
            std::memcpy(_links_data.data(),
                        buf.data() + n * sizeof(GraphNode),
                        links_count * sizeof(uint32_t));
        }
    }

    // --- alive.dat ---
    {
        auto buf = read_file(d + "/alive.dat");
        if (buf.empty() && n > 0) {
            LOG(error, "HnswDiskIndex(%s): alive.dat missing", d.c_str());
            return false;
        }
        _alive = BitVector::create(n);
        if (n > 0) {
            // alive.dat stores raw BitVector words
            size_t word_bytes = ((n + 63) / 64) * 8;
            if (buf.size() < word_bytes) {
                LOG(error, "HnswDiskIndex(%s): alive.dat too small", d.c_str());
                return false;
            }
            // Set bits by copying words directly
            for (uint32_t i = 0; i < n; ++i) {
                // bit i is at byte i/8, bit i%8
                bool alive_bit = (buf[i / 8] >> (i % 8)) & 1;
                if (alive_bit) _alive->setBit(i);
            }
        }
    }

    _is_open = true;
    return true;
}

uint32_t HnswDiskIndex::alive_count() const
{
    std::lock_guard guard(_alive_mutex);
    return _alive ? _alive->countTrueBits() : 0;
}

double HnswDiskIndex::alive_ratio() const
{
    if (_meta.num_docs == 0) return 1.0;
    return static_cast<double>(alive_count()) / _meta.num_docs;
}

void HnswDiskIndex::remove_document(uint32_t global_docid)
{
    std::lock_guard guard(_alive_mutex);
    // Reverse map: scan docid_map (acceptable for delete path)
    for (uint32_t i = 0; i < _docid_map.size(); ++i) {
        if (_docid_map[i] == global_docid && _alive->testBit(i)) {
            _alive->clearBit(i);
            return;
        }
    }
}

const void* HnswDiskIndex::raw_vector(uint32_t local_id) const
{
    size_t stride = _compressor
        ? _compressor->compressed_bytes(_meta.dims)
        : _meta.dims * sizeof(float);
    return _vectors_data.data() + local_id * stride;
}

std::vector<float> HnswDiskIndex::get_float_vector(uint32_t local_id) const
{
    std::vector<float> out(_meta.dims);
    if (_compressor) {
        _compressor->decompress(raw_vector(local_id), out.data(), _meta.dims);
    } else {
        const float* src = static_cast<const float*>(raw_vector(local_id));
        std::copy(src, src + _meta.dims, out.data());
    }
    return out;
}

HnswDiskIndex::LinkRange HnswDiskIndex::get_links(uint32_t local_id, uint32_t level) const
{
    // links_data layout: level-0 links start at graph_nodes[local_id].links_offset
    // Levels 1..n-1 follow immediately; each level section is prefixed by a count.
    // Layout per node in links_data:
    //   [level0_count, level0_links...], [level1_count, level1_links...], ...
    const GraphNode& node = _graph_nodes[local_id];
    uint32_t off = node.links_offset;
    for (uint32_t lv = 0; lv <= level; ++lv) {
        if (off >= _links_data.size()) return {0, 0};
        uint32_t cnt = _links_data[off];
        if (lv == level) return {off + 1, cnt};
        off += 1 + cnt;
    }
    return {0, 0};
}

double HnswDiskIndex::calc_distance(const float* query, uint32_t local_id) const
{
    std::vector<float> vec = get_float_vector(local_id);
    // Use simple Euclidean squared distance
    double sum = 0.0;
    for (uint32_t i = 0; i < _meta.dims; ++i) {
        double d = static_cast<double>(query[i]) - vec[i];
        sum += d * d;
    }
    return sum;
}

void HnswDiskIndex::search_layer(const float* query_vec,
                                  uint32_t entry_local,
                                  int level,
                                  uint32_t ef,
                                  std::vector<std::pair<double,uint32_t>>& result,
                                  const search::BitVector* filter) const
{
    // Standard HNSW greedy search at a single layer
    using Pair = std::pair<double,uint32_t>;

    auto cmp_max = [](const Pair& a, const Pair& b){ return a.first < b.first; };
    auto cmp_min = [](const Pair& a, const Pair& b){ return a.first > b.first; };

    std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_max)> candidates(cmp_max);
    std::priority_queue<Pair, std::vector<Pair>, decltype(cmp_min)> found(cmp_min);
    std::vector<bool> visited(_meta.num_docs, false);

    auto check_alive = [&](uint32_t lid) -> bool {
        std::lock_guard guard(_alive_mutex);
        return _alive && _alive->testBit(lid);
    };

    double d_entry = calc_distance(query_vec, entry_local);
    candidates.push({d_entry, entry_local});
    visited[entry_local] = true;

    bool entry_alive = check_alive(entry_local);
    bool entry_passes = entry_alive && (!filter || filter->testBit(_docid_map[entry_local]));
    if (entry_passes) {
        found.push({d_entry, entry_local});
    }

    while (!candidates.empty()) {
        auto [c_dist, c_id] = candidates.top();
        candidates.pop();

        // Pruning: if candidate is worse than worst in found (and found is full)
        if (found.size() >= ef) {
            auto [worst_dist, _] = found.top();
            if (c_dist > worst_dist) break;
        }

        LinkRange lr = get_links(c_id, static_cast<uint32_t>(level));
        for (uint32_t k = 0; k < lr.count; ++k) {
            uint32_t nb = _links_data[lr.offset + k];
            if (nb >= _meta.num_docs || visited[nb]) continue;
            visited[nb] = true;

            double nb_dist = calc_distance(query_vec, nb);
            bool nb_alive = check_alive(nb);
            bool nb_passes = nb_alive && (!filter || filter->testBit(_docid_map[nb]));

            if (nb_passes) {
                found.push({nb_dist, nb});
                if (found.size() > ef) found.pop();
            }
            candidates.push({nb_dist, nb});
        }
    }

    result.clear();
    while (!found.empty()) {
        result.push_back(found.top());
        found.pop();
    }
    std::sort(result.begin(), result.end());
}

std::vector<NearestNeighborIndex::Neighbor>
HnswDiskIndex::find_top_k(uint32_t k, vespalib::eval::TypedCells query,
                           uint32_t explore_k, double distance_threshold,
                           const search::BitVector* filter) const
{
    if (!_is_open || _meta.entry_level < 0 || _meta.num_docs == 0) return {};

    // Convert query to float32
    std::vector<float> q_float(_meta.dims);
    if (query.type == vespalib::eval::CellType::FLOAT) {
        const float* src = static_cast<const float*>(query.data);
        std::copy(src, src + _meta.dims, q_float.data());
    } else if (query.type == vespalib::eval::CellType::DOUBLE) {
        const double* src = static_cast<const double*>(query.data);
        for (uint32_t i = 0; i < _meta.dims; ++i) q_float[i] = static_cast<float>(src[i]);
    } else {
        LOG(warning, "HnswDiskIndex::find_top_k: unsupported query cell type");
        return {};
    }

    // Find entry local id
    uint32_t entry_local = 0;
    for (uint32_t i = 0; i < _docid_map.size(); ++i) {
        if (_docid_map[i] == _meta.entry_docid) { entry_local = i; break; }
    }

    // Multi-level greedy descend
    uint32_t cur_entry = entry_local;
    for (int lv = _meta.entry_level; lv > 0; --lv) {
        std::vector<std::pair<double,uint32_t>> tmp;
        search_layer(q_float.data(), cur_entry, lv, 1, tmp, nullptr);
        if (!tmp.empty()) cur_entry = tmp[0].second;
    }

    // Full search at level 0
    std::vector<std::pair<double,uint32_t>> candidates;
    search_layer(q_float.data(), cur_entry, 0, explore_k, candidates, filter);

    std::vector<NearestNeighborIndex::Neighbor> result;
    result.reserve(std::min<size_t>(k, candidates.size()));
    for (auto& [dist, lid] : candidates) {
        if (dist > distance_threshold) break;
        result.emplace_back(_docid_map[lid], dist);
        if (result.size() >= k) break;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Static write
// ---------------------------------------------------------------------------

bool HnswDiskIndex::write(const vespalib::string& base_dir,
                           uint32_t id,
                           const std::vector<uint32_t>& global_docids,
                           const search::BitVector& alive,
                           const std::vector<float>& vectors,
                           uint32_t dims,
                           uint32_t entry_local_id,
                           int32_t  entry_level,
                           const std::vector<GraphNode>& graph_nodes,
                           const std::vector<uint32_t>&  links_data,
                           const VectorCompressor* compressor)
{
    uint32_t n = static_cast<uint32_t>(global_docids.size());
    assert(vectors.size() == static_cast<size_t>(n) * dims);
    assert(graph_nodes.size() == n);

    string d = make_dir(base_dir, id, true);
    vespalib::mkdir(d, false);

    // --- meta.dat ---
    Meta meta;
    meta.num_docs         = n;
    meta.dims             = dims;
    meta.entry_docid      = (entry_local_id < n) ? global_docids[entry_local_id] : 0;
    meta.entry_level      = entry_level;
    meta.compression_type = static_cast<uint32_t>(
        compressor ? compressor->cell_type() == vespalib::eval::CellType::INT8
                         ? VectorCompressor::CompressionType::INT8
                         : VectorCompressor::CompressionType::BFLOAT16
                   : VectorCompressor::CompressionType::NONE);
    if (!write_file(d + "/meta.dat", &meta, sizeof(meta))) return false;

    // --- docidmap.dat ---
    if (!write_file(d + "/docidmap.dat", global_docids.data(),
                    n * sizeof(uint32_t))) return false;

    // --- vectors.dat ---
    if (compressor) {
        size_t cbytes = compressor->compressed_bytes(dims);
        std::vector<uint8_t> cvecs(n * cbytes);
        for (uint32_t i = 0; i < n; ++i) {
            compressor->compress(vectors.data() + i * dims,
                                 cvecs.data() + i * cbytes, dims);
        }
        if (!write_file(d + "/vectors.dat", cvecs.data(), cvecs.size())) return false;
    } else {
        if (!write_file(d + "/vectors.dat", vectors.data(),
                        n * dims * sizeof(float))) return false;
    }

    // --- graph.dat ---
    {
        std::vector<uint8_t> gbuf;
        gbuf.resize(n * sizeof(GraphNode));
        std::memcpy(gbuf.data(), graph_nodes.data(), gbuf.size());
        size_t prev = gbuf.size();
        gbuf.resize(prev + links_data.size() * sizeof(uint32_t));
        if (!links_data.empty()) {
            std::memcpy(gbuf.data() + prev, links_data.data(),
                        links_data.size() * sizeof(uint32_t));
        }
        if (!write_file(d + "/graph.dat", gbuf.data(), gbuf.size())) return false;
    }

    // --- alive.dat (packed bits) ---
    {
        size_t bytes = (n + 7) / 8;
        std::vector<uint8_t> abuf(bytes, 0);
        for (uint32_t i = 0; i < n; ++i) {
            if (alive.testBit(i)) abuf[i / 8] |= uint8_t(1 << (i % 8));
        }
        if (!write_file(d + "/alive.dat", abuf.data(), abuf.size())) return false;
    }

    // --- compressor.dat ---
    if (compressor) {
        std::vector<uint8_t> pbuf;
        VecBufferWriter w(pbuf);
        compressor->save_params(w);
        if (!write_file(d + "/compressor.dat", pbuf.data(), pbuf.size())) return false;
    }

    LOG(info, "HnswDiskIndex: wrote %u docs to %s", n, d.c_str());
    return true;
}

}
