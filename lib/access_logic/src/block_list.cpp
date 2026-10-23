#include "block_list.h"

#include <string.h>

bool BlockList::isBlocked(uint16_t id) const {
  return id < kMaxIds && (bits_[id >> 3] & (1u << (id & 7))) != 0;
}

bool BlockList::set(uint16_t id, bool blocked) {
  if (id >= kMaxIds) return false;
  if (blocked) {
    bits_[id >> 3] = static_cast<uint8_t>(bits_[id >> 3] | (1u << (id & 7)));
  } else {
    bits_[id >> 3] = static_cast<uint8_t>(bits_[id >> 3] & ~(1u << (id & 7)));
  }
  return true;
}

void BlockList::clearAll() { memset(bits_, 0, sizeof bits_); }

uint16_t BlockList::count() const {
  uint16_t n = 0;
  for (uint16_t id = 0; id < kMaxIds; ++id) {
    if (isBlocked(id)) ++n;
  }
  return n;
}

size_t BlockList::list(uint16_t* out, size_t max) const {
  size_t n = 0;
  for (uint16_t id = 0; id < kMaxIds && n < max; ++id) {
    if (isBlocked(id)) {
      if (out) out[n] = id;
      ++n;
    }
  }
  return n;
}

bool BlockList::load(const uint8_t* data, size_t len) {
  if (data == nullptr || len != kBytes) {
    clearAll();
    return false;
  }
  memcpy(bits_, data, kBytes);
  return true;
}
