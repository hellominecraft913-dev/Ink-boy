#pragma once

#include <optional>
#include <string>

#include "ReaderProgressSaveDebouncer.h"

// Bounded, allocation-free navigation history. Page numbers have the same
// u16 limits as progress.bin and can shift if the reader layout is changed.
namespace EpubLinkReturnState {
constexpr int MAX_DEPTH = 3;
struct Position {
  int spineIndex;
  int pageNumber;
};

struct ReadingProgress {
  int spineIndex;
  int pageNumber;
  int pageCount;
};

// A full-section resume uses only a successfully rendered observation. A
// transient preview resumes its immediate origin with the chapter's metadata.
std::optional<ReadingProgress> readingProgress(const Position (&positions)[MAX_DEPTH], int depth, bool transientPreview,
                                               const ReaderProgressSaveDebouncer& progress, int lastSavedSpineIndex,
                                               int lastSavedPageCount);

void push(Position (&positions)[MAX_DEPTH], int& depth, Position origin);
int resumeDepth(int depth, bool transientPreview);
bool save(const std::string& cachePath, const Position (&positions)[MAX_DEPTH], int depth, bool transientPreview);
bool load(const std::string& cachePath, Position (&positions)[MAX_DEPTH], int& depth, int spineCount);
}  // namespace EpubLinkReturnState
