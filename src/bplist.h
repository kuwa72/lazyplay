#ifndef BPLIST_H
#define BPLIST_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <map>

// Minimal binary plist (bplist00) DOM, reader and writer.
// Supports the subset used by the AirPlay protocol:
// dict / array / ascii string / uint / real / data / bool.

struct BPNode {
    enum class Type { Null, Bool, Int, Real, Data, String, Array, Dict };

    Type type = Type::Null;
    bool boolean = false;
    uint64_t num = 0;
    double real = 0.0;
    std::vector<uint8_t> data;
    std::string str;
    std::vector<BPNode> array;
    std::vector<std::pair<std::string, BPNode>> dict;

    static BPNode MakeBool(bool v) { BPNode n; n.type = Type::Bool; n.boolean = v; return n; }
    static BPNode MakeInt(uint64_t v) { BPNode n; n.type = Type::Int; n.num = v; return n; }
    static BPNode MakeReal(double v) { BPNode n; n.type = Type::Real; n.real = v; return n; }
    static BPNode MakeData(const uint8_t* p, size_t len) { BPNode n; n.type = Type::Data; n.data.assign(p, p + len); return n; }
    static BPNode MakeData(const std::vector<uint8_t>& v) { BPNode n; n.type = Type::Data; n.data = v; return n; }
    static BPNode MakeString(const std::string& v) { BPNode n; n.type = Type::String; n.str = v; return n; }
    static BPNode MakeArray() { BPNode n; n.type = Type::Array; return n; }
    static BPNode MakeDict() { BPNode n; n.type = Type::Dict; return n; }

    void Set(const std::string& key, const BPNode& value) { dict.emplace_back(key, value); }
    void Append(const BPNode& value) { array.push_back(value); }

    const BPNode* Find(const std::string& key) const {
        if (type != Type::Dict) return nullptr;
        for (const auto& kv : dict) {
            if (kv.first == key) return &kv.second;
        }
        return nullptr;
    }
    uint64_t FindInt(const std::string& key, uint64_t fallback = 0) const {
        const BPNode* n = Find(key);
        return (n && n->type == Type::Int) ? n->num : fallback;
    }
    std::string FindString(const std::string& key) const {
        const BPNode* n = Find(key);
        return (n && n->type == Type::String) ? n->str : std::string();
    }
    const BPNode* FindData(const std::string& key) const {
        const BPNode* n = Find(key);
        return (n && n->type == Type::Data) ? n : nullptr;
    }
};

// Returns false on malformed input
bool BPlistParse(const uint8_t* data, size_t len, BPNode& out);
std::vector<uint8_t> BPlistWrite(const BPNode& root);

#endif // BPLIST_H
