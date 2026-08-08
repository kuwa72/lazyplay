#include "bplist.h"

#include <cstring>

namespace {

uint64_t ReadBE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

void WriteBE(std::vector<uint8_t>& out, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        out.push_back(static_cast<uint8_t>((v >> ((n - 1 - i) * 8)) & 0xFF));
    }
}

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : m_data(data), m_len(len) {}

    bool Parse(BPNode& out) {
        if (m_len < 40 || memcmp(m_data, "bplist00", 8) != 0) return false;

        const uint8_t* trailer = m_data + m_len - 32;
        m_offsetSize = trailer[6];
        m_refSize = trailer[7];
        m_numObjects = ReadBE(trailer + 8, 8);
        uint64_t topObject = ReadBE(trailer + 16, 8);
        uint64_t offsetTableOffset = ReadBE(trailer + 24, 8);

        if (m_offsetSize == 0 || m_offsetSize > 8 || m_refSize == 0 || m_refSize > 8) return false;
        if (m_numObjects > (1u << 20)) return false;
        if (offsetTableOffset + m_numObjects * m_offsetSize > m_len - 32) return false;

        m_offsets = m_data + offsetTableOffset;
        return ParseObject(topObject, out, 0);
    }

private:
    bool ObjectOffset(uint64_t index, uint64_t& offset) const {
        if (index >= m_numObjects) return false;
        offset = ReadBE(m_offsets + index * m_offsetSize, m_offsetSize);
        return offset < m_len - 32;
    }

    // Reads extended length for markers with low nibble 0xF
    bool ReadLength(uint64_t markerInfo, size_t& pos, uint64_t& length) const {
        if (markerInfo != 0xF) {
            length = markerInfo;
            return true;
        }
        if (pos >= m_len) return false;
        uint8_t intMarker = m_data[pos];
        if ((intMarker >> 4) != 0x1) return false;
        size_t intSize = size_t(1) << (intMarker & 0xF);
        if (intSize > 8 || pos + 1 + intSize > m_len) return false;
        length = ReadBE(m_data + pos + 1, intSize);
        pos += 1 + intSize;
        return true;
    }

    bool ParseObject(uint64_t index, BPNode& out, int depth) const {
        if (depth > 64) return false;
        uint64_t offset;
        if (!ObjectOffset(index, offset)) return false;

        size_t pos = static_cast<size_t>(offset);
        uint8_t marker = m_data[pos++];
        uint8_t type = marker >> 4;
        uint8_t info = marker & 0xF;

        switch (type) {
        case 0x0:
            if (info == 0x0) { out.type = BPNode::Type::Null; return true; }
            if (info == 0x8) { out.type = BPNode::Type::Bool; out.boolean = false; return true; }
            if (info == 0x9) { out.type = BPNode::Type::Bool; out.boolean = true; return true; }
            return false;

        case 0x1: {
            size_t size = size_t(1) << info;
            if (size > 8 || pos + size > m_len) return false;
            out.type = BPNode::Type::Int;
            out.num = ReadBE(m_data + pos, size);
            return true;
        }

        case 0x2: {
            size_t size = size_t(1) << info;
            if (size == 4 && pos + 4 <= m_len) {
                uint32_t bits = static_cast<uint32_t>(ReadBE(m_data + pos, 4));
                float f;
                memcpy(&f, &bits, 4);
                out.type = BPNode::Type::Real;
                out.real = f;
                return true;
            }
            if (size == 8 && pos + 8 <= m_len) {
                uint64_t bits = ReadBE(m_data + pos, 8);
                double d;
                memcpy(&d, &bits, 8);
                out.type = BPNode::Type::Real;
                out.real = d;
                return true;
            }
            return false;
        }

        case 0x3: // date: treat as real
            if (pos + 8 > m_len) return false;
            out.type = BPNode::Type::Real;
            { uint64_t bits = ReadBE(m_data + pos, 8); double d; memcpy(&d, &bits, 8); out.real = d; }
            return true;

        case 0x4: {
            uint64_t count;
            if (!ReadLength(info, pos, count)) return false;
            if (count > m_len || pos + count > m_len) return false;
            out.type = BPNode::Type::Data;
            out.data.assign(m_data + pos, m_data + pos + count);
            return true;
        }

        case 0x5: {
            uint64_t count;
            if (!ReadLength(info, pos, count)) return false;
            if (count > m_len || pos + count > m_len) return false;
            out.type = BPNode::Type::String;
            out.str.assign(reinterpret_cast<const char*>(m_data + pos), count);
            return true;
        }

        case 0x6: {
            // UTF-16BE string: convert to UTF-8 (BMP only, AirPlay keys/values are ASCII)
            uint64_t count;
            if (!ReadLength(info, pos, count)) return false;
            if (count > (m_len / 2) || pos + count * 2 > m_len) return false;
            out.type = BPNode::Type::String;
            for (uint64_t i = 0; i < count; ++i) {
                uint16_t ch = static_cast<uint16_t>(ReadBE(m_data + pos + i * 2, 2));
                if (ch < 0x80) {
                    out.str.push_back(static_cast<char>(ch));
                } else if (ch < 0x800) {
                    out.str.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                    out.str.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                } else {
                    out.str.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                    out.str.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                    out.str.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
                }
            }
            return true;
        }

        case 0xA: {
            uint64_t count;
            if (!ReadLength(info, pos, count)) return false;
            if (count > m_numObjects || pos + count * m_refSize > m_len) return false;
            out.type = BPNode::Type::Array;
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t ref = ReadBE(m_data + pos + i * m_refSize, m_refSize);
                BPNode child;
                if (!ParseObject(ref, child, depth + 1)) return false;
                out.array.push_back(std::move(child));
            }
            return true;
        }

        case 0xD: {
            uint64_t count;
            if (!ReadLength(info, pos, count)) return false;
            if (count > m_numObjects || pos + 2 * count * m_refSize > m_len) return false;
            out.type = BPNode::Type::Dict;
            for (uint64_t i = 0; i < count; ++i) {
                uint64_t keyRef = ReadBE(m_data + pos + i * m_refSize, m_refSize);
                uint64_t valRef = ReadBE(m_data + pos + (count + i) * m_refSize, m_refSize);
                BPNode key, value;
                if (!ParseObject(keyRef, key, depth + 1)) return false;
                if (key.type != BPNode::Type::String) return false;
                if (!ParseObject(valRef, value, depth + 1)) return false;
                out.dict.emplace_back(std::move(key.str), std::move(value));
            }
            return true;
        }

        default:
            return false;
        }
    }

    const uint8_t* m_data;
    size_t m_len;
    const uint8_t* m_offsets = nullptr;
    uint64_t m_numObjects = 0;
    uint8_t m_offsetSize = 0;
    uint8_t m_refSize = 0;
};

// Writer

struct WriteContext {
    std::vector<const BPNode*> objects;               // node objects
    std::vector<const std::string*> keyStrings;       // parallel to dictKeys? no: flattened below
    std::vector<uint8_t> body;
    std::vector<uint64_t> offsets;
};

void CollectObjects(const BPNode& node, std::vector<const BPNode*>& nodes, std::vector<const std::string*>& keys) {
    nodes.push_back(&node);
    if (node.type == BPNode::Type::Dict) {
        for (const auto& kv : node.dict) keys.push_back(&kv.first);
        for (const auto& kv : node.dict) CollectObjects(kv.second, nodes, keys);
    } else if (node.type == BPNode::Type::Array) {
        for (const auto& child : node.array) CollectObjects(child, nodes, keys);
    }
}

size_t IntObjectSize(uint64_t v) {
    if (v <= 0xFF) return 1;
    if (v <= 0xFFFF) return 2;
    if (v <= 0xFFFFFFFFULL) return 4;
    return 8;
}

void WriteMarkerAndLength(std::vector<uint8_t>& out, uint8_t type, uint64_t count) {
    if (count < 0xF) {
        out.push_back(static_cast<uint8_t>((type << 4) | count));
    } else {
        out.push_back(static_cast<uint8_t>((type << 4) | 0xF));
        size_t intSize = IntObjectSize(count);
        out.push_back(static_cast<uint8_t>(0x10 | (intSize == 1 ? 0 : intSize == 2 ? 1 : intSize == 4 ? 2 : 3)));
        WriteBE(out, count, intSize);
    }
}

} // namespace

bool BPlistParse(const uint8_t* data, size_t len, BPNode& out) {
    if (!data || len == 0) return false;
    Reader r(data, len);
    return r.Parse(out);
}

std::vector<uint8_t> BPlistWrite(const BPNode& root) {
    // Flatten the tree: node objects and key string objects
    std::vector<const BPNode*> nodes;
    std::vector<const std::string*> keys;
    CollectObjects(root, nodes, keys);

    // Object table: index 0 = root. Then all other nodes, then key strings.
    // We need stable indices: build a vector of "objects" where each object is
    // either a BPNode* or a string*.
    struct ObjRef { const BPNode* node; const std::string* key; };
    std::vector<ObjRef> objs;
    objs.push_back({ &root, nullptr });
    for (size_t i = 1; i < nodes.size(); ++i) objs.push_back({ nodes[i], nullptr });
    for (const std::string* k : keys) objs.push_back({ nullptr, k });

    auto findNodeIndex = [&](const BPNode* n) -> uint64_t {
        for (size_t i = 0; i < objs.size(); ++i) {
            if (objs[i].node == n) return i;
        }
        return 0;
    };
    auto findKeyIndex = [&](const std::string* s) -> uint64_t {
        for (size_t i = nodes.size(); i < objs.size(); ++i) {
            if (objs[i].key == s) return i;
        }
        return 0;
    };

    uint64_t numObjects = objs.size();
    size_t refSize = numObjects < 256 ? 1 : (numObjects < 65536 ? 2 : 4);

    std::vector<uint8_t> out;
    out.insert(out.end(), { 'b', 'p', 'l', 'i', 's', 't', '0', '0' });

    std::vector<uint64_t> offsets(numObjects, 0);

    for (uint64_t i = 0; i < numObjects; ++i) {
        offsets[i] = out.size();
        const ObjRef& obj = objs[i];
        if (obj.key) {
            WriteMarkerAndLength(out, 0x5, obj.key->size());
            out.insert(out.end(), obj.key->begin(), obj.key->end());
            continue;
        }
        const BPNode& n = *obj.node;
        switch (n.type) {
        case BPNode::Type::Null:
            out.push_back(0x00);
            break;
        case BPNode::Type::Bool:
            out.push_back(n.boolean ? 0x09 : 0x08);
            break;
        case BPNode::Type::Int: {
            size_t size = IntObjectSize(n.num);
            out.push_back(static_cast<uint8_t>(0x10 | (size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3)));
            WriteBE(out, n.num, size);
            break;
        }
        case BPNode::Type::Real: {
            out.push_back(0x23);
            uint64_t bits;
            double d = n.real;
            memcpy(&bits, &d, 8);
            WriteBE(out, bits, 8);
            break;
        }
        case BPNode::Type::Data:
            WriteMarkerAndLength(out, 0x4, n.data.size());
            out.insert(out.end(), n.data.begin(), n.data.end());
            break;
        case BPNode::Type::String:
            WriteMarkerAndLength(out, 0x5, n.str.size());
            out.insert(out.end(), n.str.begin(), n.str.end());
            break;
        case BPNode::Type::Array:
            WriteMarkerAndLength(out, 0xA, n.array.size());
            for (const auto& child : n.array) WriteBE(out, findNodeIndex(&child), refSize);
            break;
        case BPNode::Type::Dict:
            WriteMarkerAndLength(out, 0xD, n.dict.size());
            for (const auto& kv : n.dict) WriteBE(out, findKeyIndex(&kv.first), refSize);
            for (const auto& kv : n.dict) WriteBE(out, findNodeIndex(&kv.second), refSize);
            break;
        }
    }

    uint64_t offsetTableOffset = out.size();
    uint64_t maxOffset = offsetTableOffset;
    size_t offsetSize = maxOffset < 256 ? 1 : (maxOffset < 65536 ? 2 : (maxOffset < 0x100000000ULL ? 4 : 8));
    for (uint64_t off : offsets) WriteBE(out, off, offsetSize);

    // Trailer (32 bytes)
    for (int i = 0; i < 6; ++i) out.push_back(0);
    out.push_back(static_cast<uint8_t>(offsetSize));
    out.push_back(static_cast<uint8_t>(refSize));
    WriteBE(out, numObjects, 8);
    WriteBE(out, 0, 8); // topObject = index 0
    WriteBE(out, offsetTableOffset, 8);

    return out;
}
