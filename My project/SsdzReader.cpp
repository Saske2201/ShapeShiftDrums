#include "SsdzReader.h"
#include <sodium.h>
#include <cstring>

static uint16_t rd_u16(const uint8_t* p){ uint16_t v; memcpy(&v,p,2); return v; }
static uint32_t rd_u32(const uint8_t* p){ uint32_t v; memcpy(&v,p,4); return v; }
static uint64_t rd_u64(const uint8_t* p){ uint64_t v; memcpy(&v,p,8); return v; }

void SsdArchive::LoadSSD(const uint8_t* b, size_t len)
{
  if (len < 8 || memcmp(b, "SSD1", 4) != 0)
    throw std::runtime_error("SSD: bad magic");
  const uint8_t* p = b + 4;
  uint32_t count = rd_u32(p); p += 4;

  m_entries.clear();
  m_index.clear();
  m_entries.reserve(count);

  for (uint32_t i=0; i<count; ++i){
    if ((size_t)(p - b) + 12 > len) throw std::runtime_error("SSD: truncated header");
    uint32_t name_len = rd_u32(p); p += 4;
    uint64_t data_len = rd_u64(p); p += 8;
    if ((size_t)(p - b) + name_len > len) throw std::runtime_error("SSD: truncated name");
    std::string name(reinterpret_cast<const char*>(p), name_len); p += name_len;
    if ((size_t)(p - b) + data_len > len) throw std::runtime_error("SSD: truncated data");
    std::vector<uint8_t> data(p, p + data_len); p += data_len;

    m_index[name] = m_entries.size();
    m_entries.push_back({std::move(name), std::move(data)});
  }
}

const SsdEntry* SsdArchive::Find(const std::string& name) const
{
    // быстрый путь: через индекс
    auto it = m_index.find(name);
    if (it != m_index.end())
        return &m_entries[it->second];

    // запасной вариант: линейный поиск (если индекс не заполнился по какой-то причине)
    for (size_t i = 0; i < m_entries.size(); ++i)
        if (m_entries[i].name == name)
            return &m_entries[i];

    return nullptr;
}

// === тот самый фиксированный ключ (должен совпасть с KEY_HEX в Python) ===
static const uint8_t kKey[32] = {
  0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff,
  0xfe,0xdc,0xba,0x98,0x76,0x54,0x32,0x10,0x01,0x23,0x45,0x67,0x89,0xab,0xcd,0xef
};

std::vector<uint8_t> DecryptSSDZ(const uint8_t* buf, size_t len)
{
  if (len < 28 || memcmp(buf, "SSDX", 4) != 0)
    throw std::runtime_error("SSDZ: bad magic/length");

  uint16_t ver = rd_u16(buf+4);
  uint16_t alg = rd_u16(buf+6);
  if (ver != 1 || alg != 1)
    throw std::runtime_error("SSDZ: unsupported version/alg");

  const uint8_t* nonce = buf + 8;   // 12 bytes
  uint64_t ct_len = rd_u64(buf + 20);
  if (28 + ct_len > len)
    throw std::runtime_error("SSDZ: truncated payload");
  const uint8_t* ct = buf + 28;

  unsigned long long mlen = ct_len - crypto_aead_chacha20poly1305_ietf_ABYTES;
  std::vector<uint8_t> plain(mlen);
  unsigned long long outlen = 0;
  const uint8_t aad[] = "SSDXv1";

  if (crypto_aead_chacha20poly1305_ietf_decrypt(
        plain.data(), &outlen, nullptr,
        ct, ct_len,
        aad, sizeof(aad)-1,
        nonce, kKey) != 0)
  {
    throw std::runtime_error("SSDZ: decrypt/auth failed (bad key/corrupted file)");
  }
  plain.resize(outlen);

  if (plain.size() < 4 || memcmp(plain.data(), "SSD1", 4) != 0)
    throw std::runtime_error("SSDZ: inner payload is not SSD");

  return plain;
}
