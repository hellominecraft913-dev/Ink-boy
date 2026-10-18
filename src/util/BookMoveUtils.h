#pragma once

#include <string>

namespace BookMoveUtils {

enum class RenameMigrationResult {
  Success,
  RolledBack,
  KeepRenamed,
  InvalidBookType,
  DestinationStateExists,
};

std::string buildReadFolderDestination(const std::string& srcPath);
// Renames a file and migrates supported books' path-based reader state. Book
// renames must keep the same reader format. Other files also keep pinned image
// references, with rollback if those references cannot be saved.
RenameMigrationResult renameFilePreservingBookState(const std::string& oldPath, const std::string& newPath);
// Prepares reader metadata, renames the physical book, then commits the state
// migration so one canonical metadata path is always available across resets.
RenameMigrationResult migrateRenamedBookState(const std::string& oldPath, const std::string& newPath,
                                              const std::string& oldCachePath, const std::string& title,
                                              const std::string& author, const char* bookType);
bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, bool keepInRecents);

}  // namespace BookMoveUtils
