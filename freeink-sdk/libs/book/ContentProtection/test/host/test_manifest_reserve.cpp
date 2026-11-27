#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

#define main protectedFixtureMain
#include "test_protected.cpp"
#undef main

namespace {
size_t failAllocationSize = 0;
#if defined(CONFIG_IDF_TARGET_ESP32C3) && CONFIG_IDF_TARGET_ESP32C3
constexpr size_t kExpectedContentScratch = 2048;
constexpr size_t kExpectedManifestScratch = 1024;
constexpr size_t kExpectedManifestCarry = 4608;
#else
constexpr size_t kExpectedContentScratch = 8192;
constexpr size_t kExpectedManifestScratch = 4096;
constexpr size_t kExpectedManifestCarry = 6144;
#endif

void* manifestTestMalloc(size_t size) {
  if (failAllocationSize != 0 && size == failAllocationSize) return nullptr;
  return std::malloc(size);
}

// Identity cipher checks CBC call boundaries and IV chaining, not AES itself.
class ChunkCrypto : public freeink::content::Crypto {
 public:
  uint8_t expectedIv[16];
  size_t calls = 0;
  size_t failAt = 0;
  bool expect256 = false;

  void reset(bool aes256) {
    std::memset(expectedIv, 0xA5, sizeof(expectedIv));
    calls = 0;
    failAt = 0;
    expect256 = aes256;
  }
  bool decrypt(const uint8_t* iv, const uint8_t* in, size_t len, uint8_t* out) {
    assert(len > 0 && len % 16 == 0);
    assert(std::memcmp(iv, expectedIv, sizeof(expectedIv)) == 0);
    if (++calls == failAt) return false;
    std::memcpy(expectedIv, in + len - sizeof(expectedIv), sizeof(expectedIv));
    std::memmove(out, in, len);
    return true;
  }
  int32_t rsaPrivateRaw(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t) override { return -1; }
  bool aes128CbcDecrypt(const uint8_t*, const uint8_t* iv, const uint8_t* in, size_t len, uint8_t* out) override {
    assert(!expect256);
    return decrypt(iv, in, len, out);
  }
  bool aes256CbcDecrypt(const uint8_t*, const uint8_t* iv, const uint8_t* in, size_t len, uint8_t* out) override {
    assert(expect256);
    return decrypt(iv, in, len, out);
  }
  void sha1(const uint8_t*, size_t, uint8_t*) override {}
  void sha256(const uint8_t*, size_t, uint8_t*) override {}
  bool rsaGenerate(freeink::content::RsaKeyPairDer*) override { return false; }
  bool rsaPublicEncrypt(const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*, size_t, size_t*) override {
    return false;
  }
  bool rsaPrivateSignRaw(const uint8_t*, size_t, const uint8_t*, uint8_t*) override { return false; }
  bool aes128CbcEncrypt(const uint8_t*, const uint8_t*, const uint8_t*, size_t, uint8_t*) override { return false; }
  bool pkcs12Extract(const uint8_t*, size_t, const std::string&, std::vector<uint8_t>*, std::vector<uint8_t>*) override {
    return false;
  }
  void randomBytes(uint8_t*, size_t) override {}
};

struct ContentSink {
  std::string bytes;
  size_t calls = 0;
  bool reject = false;
  static bool append(void* context, const uint8_t* data, size_t size) {
    auto& sink = *static_cast<ContentSink*>(context);
    ++sink.calls;
    if (sink.reject) return false;
    sink.bytes.append(reinterpret_cast<const char*>(data), size);
    return true;
  }
};

void verifyContentStreams() {
  const std::string plain(5003, 'x');
  for (int mode = 0; mode < 5; ++mode) {
    const bool aes256 = mode != 0 && mode != 3;
    const bool stored = mode == 2;
    const bool compressed = mode >= 3;
    std::string payload;
    payload.reserve(plain.size() + 5);
    if (compressed) {
      // Raw DEFLATE for 5003 'x' bytes exercises repeated output-buffer drains.
      static constexpr uint8_t deflated[] = {0xed, 0xc1, 0x31, 0x01, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0xda,
                                            0x8b, 0x6f, 0x0b, 0x2f, 0xa0, 0x00, 0x00, 0x00, 0x00, 0x80,
                                            0x83, 0x01};
      payload.assign(reinterpret_cast<const char*>(deflated), sizeof(deflated));
    } else if (!stored) {
      const uint16_t length = static_cast<uint16_t>(plain.size());
      payload.push_back(1);
      payload.push_back(static_cast<char>(length));
      payload.push_back(static_cast<char>(length >> 8));
      payload.push_back(static_cast<char>(~length));
      payload.push_back(static_cast<char>(~length >> 8));
    }
    if (!compressed) payload += plain;
    const uint8_t pad = static_cast<uint8_t>(16 - payload.size() % 16);
    payload.append(pad, aes256 ? 'q' : static_cast<char>(pad));
    payload.back() = static_cast<char>(pad);
    const std::string blob = std::string(16, static_cast<char>(0xA5)) + payload;
    std::string xml = "<encryption>" + encEntry("chapter.xhtml", stored ? "Method=\"0\"" : "Method=\"8\"") +
                      "</encryption>";
    if (!aes256) xml.replace(xml.find("aes256-cbc"), std::strlen("aes256-cbc"), "aes128-cbc");
    auto source = storedZip({{"META-INF/encryption.xml", xml}, {"chapter.xhtml", blob}});
    ProtectedBook book;
    assert(book.open(source));
    const uint8_t key[32]{};
    book.setContentKey(key, aes256 ? 32 : 16);
    ChunkCrypto crypto;
    ContentSink sink;
    sink.bytes.reserve(plain.size());
    crypto.reset(aes256);
    failAllocationSize = kExpectedContentScratch == 2048 ? 8192 : 2048;
    assert(book.decryptEntryToSink(source, crypto, "chapter.xhtml", ContentSink::append, &sink));
    failAllocationSize = 0;
    assert(sink.bytes == plain);
    assert(compressed ? crypto.calls == 1 : crypto.calls > 1);
    assert(sink.calls > 1);

    failAllocationSize = kExpectedContentScratch;
    crypto.reset(aes256);
    assert(!book.decryptEntryToSink(source, crypto, "chapter.xhtml", ContentSink::append, &sink));
    assert(book.lastError() == "insufficient memory for content stream");
    failAllocationSize = 0;

    sink.reject = true;
    crypto.reset(aes256);
    assert(!book.decryptEntryToSink(source, crypto, "chapter.xhtml", ContentSink::append, &sink));
    sink.reject = false;
    sink.bytes.clear();
    crypto.reset(aes256);
    crypto.failAt = compressed ? 1 : 2;
    assert(!book.decryptEntryToSink(source, crypto, "chapter.xhtml", ContentSink::append, &sink));
    assert(book.lastError() == "content read failed");
    sink.bytes.clear();
    crypto.reset(aes256);
    assert(book.decryptEntryToSink(source, crypto, "chapter.xhtml", ContentSink::append, &sink));
    assert(sink.bytes == plain);
  }
}
}  // namespace

// Inject failures into parser probes without adding a production test interface.
#define malloc manifestTestMalloc
#include "../../src/ProtectedBook.cpp"
#undef malloc

int main() {
  verifyContentStreams();
  const std::string xml = "<encryption>" + encEntry("text/a.xhtml", nullptr) + "</encryption>";
  auto source = storedZip({{"META-INF/encryption.xml", xml}}, true);
  {
    ProtectedBook book;
    failAllocationSize = 6144 + 1024 + 256 * sizeof(uint64_t) +
                         256 * sizeof(std::pair<uint64_t, uint32_t>) + 512;
    assert(book.open(source));
    failAllocationSize = 0;
  }
  struct Failure {
    size_t size;
    const char* error;
  };
  constexpr Failure failures[] = {
      {kExpectedManifestCarry + 1 + 64, "manifest carry out of memory"},
      {kExpectedManifestScratch, "manifest scratch out of memory"},
      {1024 + 1 + 64, "manifest attribute out of memory"},
      {256 * sizeof(uint64_t) + 64, "manifest hashes out of memory"},
      {256 * sizeof(std::pair<uint64_t, uint32_t>) + 64, "manifest sizes out of memory"},
      {64 * sizeof(uint64_t) + 64, "manifest stored hashes out of memory"},
  };
  for (const auto& failure : failures) {
    ProtectedBook book;
    failAllocationSize = failure.size;
    assert(!book.open(source));
    assert(book.lastError() == failure.error);
    failAllocationSize = 0;
    assert(book.open(source));
    assert(book.isEncrypted("text/a.xhtml"));
  }
  assert(protectedFixtureMain() == 0);
  std::puts("test_manifest_reserve: all assertions passed");
}
