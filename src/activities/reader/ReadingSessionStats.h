#pragma once
#include <cstdint>

#include "BookReadingStats.h"
#include "GlobalReadingStats.h"

// Adds one finished tracking session to the book and global totals. Sessions of
// at least 60 seconds count as a session; at least 10 seconds add reading time.
// sessionStart may be null when no RTC time was available at session start.
void commitReadingSession(BookReadingStats& book, GlobalReadingStats& global, uint32_t seconds,
                          const ReadingStatsDateTime* sessionStart);

// Copies the fields BookStatsActivity can edit from the on-disk copy, keeping
// the reader's live counters. The stats screen saves a preview that already
// includes the pending session, so replacing live stats would count it twice.
void importBookStatsEdits(BookReadingStats& live, const BookReadingStats& disk);

// Refreshes a reader's global stats after an overlay closes. Takes the disk
// copy only after a local reset; otherwise imports the completed-books count
// the stats screen can edit and keeps unsaved live counters.
void refreshGlobalStatsAfterOverlay(GlobalReadingStats& live, uint32_t resetRevisionAtOpen);
