#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

struct SsdEntry {
  std::string name;
  std::vector<uint8_t> data;
};

class SsdArchive {
public:
  // разобрать нешифрованный .ssd из памяти
  void LoadSSD(const uint8_t* bytes, size_t len);

  size_t Count() const { return m_entries.size(); }
  const SsdEntry& ByIndex(size_t i) const { return m_entries.at(i); }
  const SsdEntry* Find(const std::string& name) const;

private:
  std::vector<SsdEntry> m_entries;
  std::unordered_map<std::string, size_t> m_index;
};

// Дешифрация .ssdz (ChaCha20-Poly1305 IETF, фиксированный ключ)
// Возвращает bytes внутреннего .ssd
std::vector<uint8_t> DecryptSSDZ(const uint8_t* ssdz, size_t len);
