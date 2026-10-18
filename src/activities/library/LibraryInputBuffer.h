#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Main-loop-owned events. Fixed storage keeps polling allocation-free while the
// render task owns the UI; FIFO order preserves navigation followed by Select.
class LibraryInputBuffer {
 public:
  enum class Type : uint8_t {
    TouchPress,
    TouchRelease,
    TouchLongPress,
    ConfirmLongPress,
    ConfirmRelease,
    BackRelease,
    LeftRelease,
    RightRelease,
    Next,
    Previous,
    NextPage,
    PreviousPage,
    SwipeUp,
    SwipeDown,
    SwipeLeft,
    SwipeRight
  };
  struct Event {
    Type type = Type::TouchRelease;
    int16_t x = -1;
    int16_t y = -1;
  };
  static constexpr size_t CAPACITY = 32;

  bool push(const Event event) {
    if (count == CAPACITY) return false;
    events[(head + count) % CAPACITY] = event;
    ++count;
    return true;
  }
  bool pop(Event& event) {
    if (!count) return false;
    event = events[head];
    head = (head + 1) % CAPACITY;
    --count;
    return true;
  }
  size_t size() const { return count; }
  void clear() { head = count = 0; }

 private:
  std::array<Event, CAPACITY> events{};
  uint8_t head = 0;
  uint8_t count = 0;
};
