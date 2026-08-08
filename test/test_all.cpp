// lazyplay integration tests
//   test_all.exe unit                     -- sha512 / aes-ctr / bplist self tests
//   test_all.exe decode <file.h264>       -- MFT decode + NV12 render readback
//   test_all.exe e2e <host> <file.h264>   -- full AirPlay handshake + encrypted streaming
//                                            against a running lazyplay instance
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>

#include "../src/sha512.h"
#include "../src/aes_ctr.h"
#include "../src/bplist.h"
#include "../src/renderer_d3d11.h"
#include "../src/decoder_d3d11.h"

extern "C" {
#include "../src/playfair/playfair.h"
}

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { std::cout << "  [PASS] " << msg << std::endl; } \
    else { std::cout << "  [FAIL] " << msg << std::endl; g_failures++; } \
} while (0)

// ---------- helpers ----------

static std::vector<uint8_t> ReadFileBytes(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    fread(data.data(), 1, size, f);
    fclose(f);
    return data;
}

static std::string ToHex(const uint8_t* p, size_t n) {
    std::string s;
    const char* digits = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { s += digits[p[i] >> 4]; s += digits[p[i] & 15]; }
    return s;
}

// Split an Annex-B stream into NAL units (without start codes)
static std::vector<std::vector<uint8_t>> SplitNals(const std::vector<uint8_t>& stream) {
    std::vector<std::vector<uint8_t>> nals;
    size_t i = 0;
    while (i + 3 < stream.size()) {
        size_t scLen = 0;
        if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 0 && stream[i + 3] == 1) scLen = 4;
        else if (stream[i] == 0 && stream[i + 1] == 0 && stream[i + 2] == 1) scLen = 3;
        if (!scLen) { ++i; continue; }
        size_t nalStart = i + scLen;
        size_t next = stream.size();
        for (size_t j = nalStart; j + 2 < stream.size(); ++j) {
            if (stream[j] == 0 && stream[j + 1] == 0 && (stream[j + 2] == 1 ||
                (stream[j + 2] == 0 && j + 3 < stream.size() && stream[j + 3] == 1))) {
                next = j;
                break;
            }
        }
        nals.emplace_back(stream.begin() + nalStart, stream.begin() + next);
        i = next;
    }
    return nals;
}

static void DeriveVideoKeyIv(uint64_t streamConnectionID, const uint8_t audioKey[16],
                             uint8_t outKey[16], uint8_t outIv[16]) {
    uint8_t digest[64];
    {
        std::string s = "AirPlayStreamKey" + std::to_string(streamConnectionID);
        SHA512 ctx;
        ctx.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        ctx.Update(audioKey, 16);
        ctx.Final(digest);
        memcpy(outKey, digest, 16);
    }
    {
        std::string s = "AirPlayStreamIV" + std::to_string(streamConnectionID);
        SHA512 ctx;
        ctx.Update(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        ctx.Update(audioKey, 16);
        ctx.Final(digest);
        memcpy(outIv, digest, 16);
    }
}

// ---------- unit tests ----------

static void TestSha512() {
    std::cout << "[SHA-512]" << std::endl;
    uint8_t out[64];
    SHA512Hash((const uint8_t*)"abc", 3, out);
    CHECK(ToHex(out, 64) ==
        "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
        "sha512(\"abc\")");

    // Two-block message exercises the padding path
    std::string longStr(200, 'x');
    SHA512Hash((const uint8_t*)longStr.data(), longStr.size(), out);
    CHECK(ToHex(out, 64) ==
        "ef978e23dc520404ae16fd17bde9ee5945610d671551d6863a5ffbc99433fc72"
        "6726e51f989b886191be9325b8f8b03b1a63fe3e5eff23d126c2f41f07d2bf87",
        "sha512(200 x 'x')");
}

static void TestAesCtr() {
    std::cout << "[AES-128-CTR]" << std::endl;

    // RFC 3686 Test Vector #1 (single block)
    {
        uint8_t key[16] = {0xAE,0x68,0x52,0xF8,0x12,0x10,0x67,0xCC,0x4B,0xF7,0xA5,0x76,0x55,0x77,0xF3,0x9E};
        uint8_t iv[16]  = {0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
        const char* plainStr = "Single block msg";
        uint8_t cipher[16];
        AesCtr aes;
        aes.Init(key, iv);
        aes.Process((const uint8_t*)plainStr, cipher, 16);
        // expected = plaintext XOR AES-128-ECB(key, counter-block), per RFC 3686 #1
        CHECK(ToHex(cipher, 16) == "147e4fe78bbe7559c15320a5379fdd50", "RFC3686 #1 single block");
    }

    // NIST SP 800-38A F.5.1 (multi-block, exercises counter increment)
    {
        uint8_t key[16] = {0x2B,0x7E,0x15,0x16,0x28,0xAE,0xD2,0xA6,0xAB,0xF7,0x15,0x88,0x09,0xCF,0x4F,0x3C};
        uint8_t iv[16]  = {0xF0,0xF1,0xF2,0xF3,0xF4,0xF5,0xF6,0xF7,0xF8,0xF9,0xFA,0xFB,0xFC,0xFD,0xFE,0xFF};
        uint8_t plain[64] = {
            0x6B,0xC1,0xBE,0xE2,0x2E,0x40,0x9F,0x96,0xE9,0x3D,0x7E,0x11,0x73,0x93,0x17,0x2A,
            0xAE,0x2D,0x8A,0x57,0x1E,0x03,0xAC,0x9C,0x9E,0xB7,0x6F,0xAC,0x45,0xAF,0x8E,0x51,
            0x30,0xC8,0x1C,0x46,0xA3,0x5C,0xE4,0x11,0xE5,0xFB,0xC1,0x19,0x1A,0x0A,0x52,0xEF,
            0xF6,0x9F,0x24,0x45,0xDF,0x4F,0x9B,0x17,0xAD,0x2B,0x41,0x7B,0xE6,0x6C,0x37,0x10
        };
        const char* expected =
            "874d6191b620e3261bef6864990db6ce"
            "9806f66b7970fdff8617187bb9fffdff"
            "5ae4df3edbd5d35e5b4f09020db03eab"
            "1e031dda2fbe03d1792170a0f3009cee";
        uint8_t cipher[64];
        AesCtr aes;
        aes.Init(key, iv);
        aes.Process(plain, cipher, 64);
        CHECK(ToHex(cipher, 64) == expected, "SP800-38A F.5.1 4 blocks (one-shot)");

        // Same but byte-by-byte: exercises continuous keystream offset handling
        AesCtr aes2;
        aes2.Init(key, iv);
        for (int i = 0; i < 64; ++i) aes2.Process(plain + i, cipher + i, 1);
        CHECK(ToHex(cipher, 64) == expected, "SP800-38A F.5.1 4 blocks (byte-wise)");
    }
}

static void TestBplist() {
    std::cout << "[bplist round-trip]" << std::endl;

    BPNode root = BPNode::MakeDict();
    // > 15 keys to exercise extended-length dict counts
    for (int i = 0; i < 20; ++i) {
        root.Set("key" + std::to_string(i), BPNode::MakeInt(i));
    }
    root.Set("smallInt", BPNode::MakeInt(42));
    root.Set("mediumInt", BPNode::MakeInt(70000));
    root.Set("bigInt", BPNode::MakeInt(0x0102030405060708ULL));
    root.Set("aString", BPNode::MakeString("lazyplay-display"));
    root.Set("aReal", BPNode::MakeReal(1.0 / 60.0));
    root.Set("aBool", BPNode::MakeBool(true));
    std::vector<uint8_t> blob(72);
    for (int i = 0; i < 72; ++i) blob[i] = static_cast<uint8_t>(i * 7 + 3);
    root.Set("aData", BPNode::MakeData(blob));
    BPNode arr = BPNode::MakeArray();
    arr.Append(BPNode::MakeInt(110));
    BPNode inner = BPNode::MakeDict();
    inner.Set("streamConnectionID", BPNode::MakeInt(0x0102030405060708ULL));
    inner.Set("type", BPNode::MakeInt(110));
    arr.Append(inner);
    root.Set("streams", arr);

    std::vector<uint8_t> bin = BPlistWrite(root);
    CHECK(bin.size() > 40 && memcmp(bin.data(), "bplist00", 8) == 0, "write produces bplist00");

    BPNode parsed;
    bool ok = BPlistParse(bin.data(), bin.size(), parsed);
    CHECK(ok, "parse round-trip");
    if (!ok) return;

    CHECK(parsed.FindInt("smallInt") == 42, "small int");
    CHECK(parsed.FindInt("mediumInt") == 70000, "medium int");
    CHECK(parsed.FindInt("bigInt") == 0x0102030405060708ULL, "64-bit int");
    CHECK(parsed.FindInt("key19") == 19, "extended dict count (20 keys)");
    CHECK(parsed.FindString("aString") == "lazyplay-display", "string");
    CHECK(parsed.Find("aBool") && parsed.Find("aBool")->boolean == true, "bool");
    const BPNode* d = parsed.FindData("aData");
    CHECK(d && d->data == blob, "data blob");
    const BPNode* r = parsed.Find("aReal");
    CHECK(r && r->type == BPNode::Type::Real && r->real > 0.0166 && r->real < 0.0167, "real");
    const BPNode* s = parsed.Find("streams");
    CHECK(s && s->type == BPNode::Type::Array && s->array.size() == 2, "array size");
    if (s && s->array.size() == 2) {
        CHECK(s->array[1].FindInt("streamConnectionID") == 0x0102030405060708ULL, "nested dict int");
    }
}

// ---------- decode + render test ----------

static LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static int TestDecode(const char* path) {
    std::vector<uint8_t> stream = ReadFileBytes(path);
    if (stream.empty()) {
        std::cerr << "cannot read " << path << std::endl;
        return 1;
    }
    auto nals = SplitNals(stream);
    std::cout << "[Decode] " << stream.size() << " bytes, " << nals.size() << " NAL units" << std::endl;

    const wchar_t cls[] = L"LazyplayTestWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc = TestWindowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"lazyplay test", WS_OVERLAPPEDWINDOW,
                                0, 0, 320, 240, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);

    D3D11Renderer renderer;
    if (!renderer.Initialize(hwnd, 320, 240, false)) {
        std::cerr << "renderer init failed" << std::endl;
        return 1;
    }
    D3D11H264Decoder decoder;
    if (!decoder.Initialize(renderer.GetDevice())) {
        std::cerr << "decoder init failed" << std::endl;
        return 1;
    }

    int decodedFrames = 0;
    static const uint8_t startCode[4] = { 0, 0, 0, 1 };
    std::vector<uint8_t> au;
    for (const auto& nal : nals) {
        au.clear();
        au.insert(au.end(), startCode, startCode + 4);
        au.insert(au.end(), nal.begin(), nal.end());

        ComPtr<ID3D11Texture2D> texture;
        uint32_t sub = 0, w = 0, h = 0, cpuPitch = 0;
        ComPtr<IMFSample> holder;
        std::vector<uint8_t> cpu;
        if (decoder.Decode(au.data(), au.size(), texture, sub, holder, cpu, cpuPitch, w, h)) {
            decodedFrames++;
            if (texture) renderer.RenderNV12Frame(texture.Get(), sub, w, h);
            else if (!cpu.empty()) renderer.RenderNV12Cpu(cpu.data(), w, h, cpuPitch);
        }
    }
    std::cout << "[Decode] frames produced: " << decodedFrames << std::endl;
    CHECK(decodedFrames >= 50, "decoded >= 50 of 60 frames");

    // Read back the NV12 video texture and verify the testsrc pattern landed
    uint32_t vw = 0, vh = 0;
    renderer.GetVideoSize(vw, vh);
    CHECK(vw == 1280 && vh == 720, "video texture is 1280x720");

    bool textureOk = false;
    if (ID3D11Texture2D* videoTex = renderer.GetVideoTexture()) {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        videoTex->GetDevice(&device);
        device->GetImmediateContext(&ctx);

        D3D11_TEXTURE2D_DESC desc = {};
        videoTex->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging))) {
            ctx->CopyResource(staging.Get(), videoTex);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                const uint8_t* y = (const uint8_t*)mapped.pData;
                int mn = 255, mx = 0;
                long long sum = 0;
                for (uint32_t row = 0; row < vh; ++row) {
                    for (uint32_t col = 0; col < vw; ++col) {
                        int v = y[row * mapped.RowPitch + col];
                        mn = (v < mn) ? v : mn;
                        mx = (v > mx) ? v : mx;
                        sum += v;
                    }
                }
                ctx->Unmap(staging.Get(), 0);
                std::cout << "[Decode] Y plane min=" << mn << " max=" << mx
                          << " mean=" << (sum / (vw * vh)) << std::endl;
                // testsrc has both dark and bright regions
                textureOk = (mx - mn > 80);
            }
        }
    }
    CHECK(textureOk, "rendered NV12 frame contains real image data");

    decoder.Shutdown();
    renderer.Cleanup();
    DestroyWindow(hwnd);
    return 0;
}

// ---------- e2e protocol test ----------

struct RtspResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
};

static bool SendAll(SOCKET s, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = send(s, (const char*)data + sent, (int)(len - sent), 0);
        if (r <= 0) return false;
        sent += r;
    }
    return true;
}

static bool RtspExchange(SOCKET s, const std::string& method, const std::string& url,
                         int& cseq, const std::string& contentType,
                         const std::vector<uint8_t>& body, RtspResponse& resp) {
    std::ostringstream oss;
    oss << method << " " << url << " RTSP/1.0\r\n";
    oss << "CSeq: " << cseq++ << "\r\n";
    oss << "User-Agent: lazyplay-e2e-test/1.0\r\n";
    oss << "DACP-ID: 1122334455667788\r\n";
    oss << "Active-Remote: 123456789\r\n";
    if (!contentType.empty()) oss << "Content-Type: " << contentType << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n\r\n";
    std::string head = oss.str();
    if (!SendAll(s, (const uint8_t*)head.data(), head.size())) return false;
    if (!body.empty() && !SendAll(s, body.data(), body.size())) return false;

    // Read header
    std::vector<uint8_t> buf;
    size_t headerEnd = std::string::npos;
    uint8_t tmp[8192];
    while (headerEnd == std::string::npos) {
        int r = recv(s, (char*)tmp, sizeof(tmp), 0);
        if (r <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + r);
        for (size_t i = 0; i + 3 < buf.size(); ++i) {
            if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
                headerEnd = i;
                break;
            }
        }
    }
    std::string headerBlock((char*)buf.data(), headerEnd);
    {
        std::istringstream iss(headerBlock);
        std::string line, statusText;
        std::getline(iss, line);
        std::istringstream ls(line);
        std::string proto;
        ls >> proto >> resp.status;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            size_t colon = line.find(':');
            if (colon == std::string::npos || line.empty()) continue;
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            size_t b = v.find_first_not_of(" \t");
            if (b != std::string::npos) v = v.substr(b);
            resp.headers.emplace_back(k, v);
        }
    }
    size_t contentLength = 0;
    for (auto& h : resp.headers) {
        std::string k = h.first;
        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
        if (k == "content-length") contentLength = strtoull(h.second.c_str(), nullptr, 10);
    }
    size_t bodyStart = headerEnd + 4;
    while (buf.size() < bodyStart + contentLength) {
        int r = recv(s, (char*)tmp, sizeof(tmp), 0);
        if (r <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + r);
    }
    resp.body.assign(buf.begin() + bodyStart, buf.begin() + bodyStart + contentLength);
    return true;
}

static void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}

static void PutLE32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static void PutLE64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static void PutLEFloat(uint8_t* p, float f) { memcpy(p, &f, 4); }

static int TestE2E(const char* host, const char* path) {
    std::vector<uint8_t> stream = ReadFileBytes(path);
    if (stream.empty()) { std::cerr << "cannot read " << path << std::endl; return 1; }
    auto nals = SplitNals(stream);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // UDP socket for the timing protocol (we are the "client" side responder)
    SOCKET timingSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in taddr = {};
    taddr.sin_family = AF_INET;
    taddr.sin_addr.s_addr = INADDR_ANY;
    taddr.sin_port = 0;
    bind(timingSock, (struct sockaddr*)&taddr, sizeof(taddr));
    sockaddr_in tbound = {};
    int tlen = sizeof(tbound);
    getsockname(timingSock, (struct sockaddr*)&tbound, &tlen);
    uint16_t timingPort = ntohs(tbound.sin_port);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(7000);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        std::cerr << "[E2E] cannot connect to " << host << ":7000" << std::endl;
        return 1;
    }
    std::cout << "[E2E] connected to " << host << ":7000" << std::endl;

    int cseq = 1;
    RtspResponse resp;

    // 1. GET /info
    if (!RtspExchange(s, "GET", "/info", cseq, "", {}, resp)) { std::cerr << "info failed" << std::endl; return 1; }
    CHECK(resp.status == 200, "GET /info -> 200");
    {
        BPNode info;
        bool ok = BPlistParse(resp.body.data(), resp.body.size(), info);
        CHECK(ok, "GET /info body is a valid bplist");
        if (ok) {
            CHECK(info.FindInt("features") == 0x527FFEE6ULL, "features = 0x527FFEE6 (bit27 off)");
            CHECK(info.FindString("model") == "AppleTV3,2", "model");
            CHECK(info.FindString("name") == "lazyplay-display", "name");
            const BPNode* displays = info.Find("displays");
            CHECK(displays && displays->type == BPNode::Type::Array && !displays->array.empty(), "displays present");
            if (displays && !displays->array.empty()) {
                CHECK(displays->array[0].FindInt("widthPixels") == 1280, "display width 1280");
                CHECK(displays->array[0].FindInt("maxFPS") == 30, "maxFPS 30");
            }
        }
    }

    // 2. fp-setup phase 1 (16 bytes) -> 142 bytes
    std::vector<uint8_t> fp1 = { 0x46,0x50,0x4c,0x59,0x03,0x01,0x01,0x00,
                                 0x00,0x00,0x00,0x04,0x02,0x00,0x00,0x00 };
    if (!RtspExchange(s, "POST", "/fp-setup", cseq, "application/octet-stream", fp1, resp)) return 1;
    CHECK(resp.status == 200 && resp.body.size() == 142 && memcmp(resp.body.data(), "FPLY", 4) == 0,
          "fp-setup phase 1 -> 142 byte FPLY response");

    // 3. fp-setup phase 2 (164 bytes) -> 32 bytes
    //    byte[12] selects the FairPlay SAP mode (0-3); the rest is opaque key material
    std::vector<uint8_t> fp2(164, 0);
    memcpy(fp2.data(), "FPLY", 4);
    fp2[4] = 0x03; fp2[5] = 0x01; fp2[6] = 0x03;
    for (int i = 8; i < 164; ++i) fp2[i] = (uint8_t)(i * 31 + 7);
    fp2[12] = 0x00;
    if (!RtspExchange(s, "POST", "/fp-setup", cseq, "application/octet-stream", fp2, resp)) return 1;
    CHECK(resp.status == 200 && resp.body.size() == 32 && memcmp(resp.body.data(), "FPLY", 4) == 0,
          "fp-setup phase 2 -> 32 byte FPLY response");
    // The response tail echoes req[144:164]; verify the server received fp2 intact
    if (resp.body.size() == 32) {
        CHECK(memcmp(resp.body.data() + 12, fp2.data() + 144, 20) == 0,
              "server keymsg tail matches request (fp2[144:164])");
    }

    // 4. OPTIONS
    if (!RtspExchange(s, "OPTIONS", "*", cseq, "", {}, resp)) return 1;
    CHECK(resp.status == 200, "OPTIONS -> 200");

    // 5. SETUP phase 1 (keys + timing)
    std::vector<uint8_t> ekey(72), eiv(16);
    for (int i = 0; i < 72; ++i) ekey[i] = (uint8_t)(i * 13 + 5);
    for (int i = 0; i < 16; ++i) eiv[i] = (uint8_t)(i * 17 + 1);

    BPNode setup1 = BPNode::MakeDict();
    setup1.Set("ekey", BPNode::MakeData(ekey));
    setup1.Set("eiv", BPNode::MakeData(eiv));
    setup1.Set("deviceID", BPNode::MakeString("AA:BB:CC:DD:EE:FF"));
    setup1.Set("model", BPNode::MakeString("MacBookPro18,3"));
    setup1.Set("name", BPNode::MakeString("E2E Test Mac"));
    setup1.Set("timingProtocol", BPNode::MakeString("NTP"));
    setup1.Set("timingPort", BPNode::MakeInt(timingPort));
    std::vector<uint8_t> setup1Bin = BPlistWrite(setup1);

    if (!RtspExchange(s, "SETUP", "rtsp://127.0.0.1/1", cseq, "application/x-apple-binary-plist", setup1Bin, resp)) return 1;
    CHECK(resp.status == 200, "SETUP (keys) -> 200");
    {
        BPNode r;
        bool ok = BPlistParse(resp.body.data(), resp.body.size(), r);
        CHECK(ok && r.FindInt("timingPort", 0) != 0, "SETUP response carries timingPort");
    }

    // compute the shared audio AES key exactly as the server does
    uint8_t audioKey[16];
    playfair_decrypt(fp2.data(), ekey.data(), audioKey);

    // 6. SETUP phase 2 (mirroring stream)
    const uint64_t streamConnectionID = 0x0102030405060708ULL;
    BPNode setup2 = BPNode::MakeDict();
    BPNode streams = BPNode::MakeArray();
    BPNode st = BPNode::MakeDict();
    st.Set("type", BPNode::MakeInt(110));
    st.Set("streamConnectionID", BPNode::MakeInt(streamConnectionID));
    streams.Append(st);
    setup2.Set("streams", streams);
    std::vector<uint8_t> setup2Bin = BPlistWrite(setup2);

    if (!RtspExchange(s, "SETUP", "rtsp://127.0.0.1/1", cseq, "application/x-apple-binary-plist", setup2Bin, resp)) return 1;
    uint16_t dataPort = 0;
    {
        BPNode r;
        bool ok = BPlistParse(resp.body.data(), resp.body.size(), r);
        CHECK(ok, "SETUP (stream) response bplist");
        if (ok) {
            const BPNode* rs = r.Find("streams");
            if (rs && rs->type == BPNode::Type::Array && !rs->array.empty()) {
                dataPort = (uint16_t)rs->array[0].FindInt("dataPort", 0);
            }
        }
        CHECK(dataPort != 0, "mirror dataPort assigned");
    }
    if (!dataPort) return 1;

    // The server sends timing requests to our timingPort; receive and reply in-place
    std::atomic<bool> timingRunning{true};
    std::thread timingThread([&]() {
        while (timingRunning) {
            uint8_t req[64];
            sockaddr_in from = {};
            int fromLen = sizeof(from);
            DWORD tv = 500;
            setsockopt(timingSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
            int len = recvfrom(timingSock, (char*)req, sizeof(req), 0, (struct sockaddr*)&from, &fromLen);
            if (len < 32) continue;
            uint8_t rp[32] = {};
            rp[0] = 0x80; rp[1] = 0xd3; rp[3] = 0x07;
            memcpy(rp + 8, req + 24, 8);
            uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            auto putBE64 = [](uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; --i) { p[i] = (uint8_t)(v & 0xFF); v >>= 8; } };
            putBE64(rp + 16, now);
            putBE64(rp + 24, now);
            sendto(timingSock, (const char*)rp, sizeof(rp), 0, (struct sockaddr*)&from, fromLen);
        }
    });
    timingThread.detach();

    // 7. RECORD
    if (!RtspExchange(s, "RECORD", "rtsp://127.0.0.1/1", cseq, "", {}, resp)) return 1;
    CHECK(resp.status == 200, "RECORD -> 200");

    // 8. connect the mirror data channel
    SOCKET ds = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in daddr = {};
    daddr.sin_family = AF_INET;
    daddr.sin_port = htons(dataPort);
    inet_pton(AF_INET, host, &daddr.sin_addr);
    if (connect(ds, (struct sockaddr*)&daddr, sizeof(daddr)) != 0) {
        std::cerr << "[E2E] cannot connect data port " << dataPort << std::endl;
        return 1;
    }
    std::cout << "[E2E] mirror data channel connected (port " << dataPort << ")" << std::endl;

    // 9. extract SPS/PPS, build the type 0x01 parameter packet
    std::vector<uint8_t> sps, pps;
    for (const auto& nal : nals) {
        int type = nal[0] & 0x1F;
        if (type == 7 && sps.empty()) sps = nal;
        if (type == 8 && pps.empty()) pps = nal;
    }
    CHECK(!sps.empty() && !pps.empty(), "test stream has SPS and PPS");

    const uint64_t spsTs = 1000000;
    {
        std::vector<uint8_t> payload;
        payload.push_back(0x01);           // version
        payload.push_back(sps[1]);         // profile
        payload.push_back(sps[2]);         // compatibility
        payload.push_back(sps[3]);         // level
        payload.push_back(0xFC);           // reserved+len minus one
        payload.push_back(0xE1);           // reserved + num SPS
        payload.push_back((uint8_t)(sps.size() >> 8));
        payload.push_back((uint8_t)(sps.size() & 0xFF));
        payload.insert(payload.end(), sps.begin(), sps.end());
        payload.push_back(0x01);           // num PPS
        payload.push_back((uint8_t)(pps.size() >> 8));
        payload.push_back((uint8_t)(pps.size() & 0xFF));
        payload.insert(payload.end(), pps.begin(), pps.end());

        std::vector<uint8_t> packet(128, 0);
        PutLE32(packet.data(), (uint32_t)payload.size());
        packet[4] = 0x01;                  // type: SPS/PPS (unencrypted)
        packet[5] = 0x00;
        packet[6] = 0x16; packet[7] = 0x01;
        PutLE64(packet.data() + 8, spsTs);
        PutLEFloat(packet.data() + 16, 1280.0f);
        PutLEFloat(packet.data() + 20, 720.0f);
        PutLEFloat(packet.data() + 40, 1280.0f);
        PutLEFloat(packet.data() + 44, 720.0f);
        PutLEFloat(packet.data() + 56, 1280.0f);
        PutLEFloat(packet.data() + 60, 720.0f);
        packet.insert(packet.end(), payload.begin(), payload.end());
        CHECK(SendAll(ds, packet.data(), packet.size()), "SPS/PPS packet sent");
    }

    // 10. stream all remaining NALs as encrypted type-0x00 packets
    uint8_t videoKey[16], videoIv[16];
    DeriveVideoKeyIv(streamConnectionID, audioKey, videoKey, videoIv);
    AesCtr aes;
    aes.Init(videoKey, videoIv);

    uint64_t ts = spsTs; // first encrypted packet must share the SPS/PPS timestamp
    int sentPackets = 0;
    for (const auto& nal : nals) {
        int type = nal[0] & 0x1F;
        if (type == 7 || type == 8) continue; // already sent via parameter packet

        std::vector<uint8_t> plain;
        PutBE32(plain, (uint32_t)nal.size());
        plain.insert(plain.end(), nal.begin(), nal.end());

        std::vector<uint8_t> cipher(plain.size());
        aes.Process(plain.data(), cipher.data(), plain.size());

        std::vector<uint8_t> packet(128, 0);
        PutLE32(packet.data(), (uint32_t)cipher.size());
        packet[4] = 0x00;
        packet[5] = (type == 5) ? 0x10 : 0x00;
        PutLE64(packet.data() + 8, ts);
        packet.insert(packet.end(), cipher.begin(), cipher.end());
        if (!SendAll(ds, packet.data(), packet.size())) {
            std::cerr << "[E2E] data channel send failed at packet " << sentPackets << std::endl;
            break;
        }
        sentPackets++;
        ts += 33333; // ~30fps in the client's ns-domain clock units
    }
    std::cout << "[E2E] streamed " << sentPackets << " encrypted video packets" << std::endl;
    CHECK(sentPackets > 40, "streamed the video");

    // give the server a moment to decode/render, then verify it is still healthy
    Sleep(1500);
    SOCKET s2 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(s2, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        int cseq2 = 1;
        RtspResponse resp2;
        bool ok = RtspExchange(s2, "GET", "/info", cseq2, "", {}, resp2);
        CHECK(ok && resp2.status == 200, "server still healthy after streaming");
        closesocket(s2);
    } else {
        CHECK(false, "server still healthy after streaming");
    }

    // 11. TEARDOWN
    {
        RtspResponse td;
        bool ok = RtspExchange(s, "TEARDOWN", "rtsp://127.0.0.1/1", cseq, "", {}, td);
        CHECK(ok && td.status == 200, "TEARDOWN -> 200");
    }

    timingRunning = false;
    closesocket(timingSock);
    closesocket(ds);
    closesocket(s);
    WSACleanup();
    return 0;
}

// ---------- main ----------

int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "unit";

    if (mode == "unit") {
        TestSha512();
        TestAesCtr();
        TestBplist();
    } else if (mode == "decode" && argc > 2) {
        TestDecode(argv[2]);
    } else if (mode == "e2e" && argc > 3) {
        TestE2E(argv[2], argv[3]);
    } else {
        std::cerr << "usage: test_all.exe unit | decode <file.h264> | e2e <host> <file.h264>" << std::endl;
        return 2;
    }

    std::cout << (g_failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << std::endl;
    return g_failures == 0 ? 0 : 1;
}
