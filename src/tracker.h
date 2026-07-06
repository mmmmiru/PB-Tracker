#ifndef PBRS_TRACKER_H
#define PBRS_TRACKER_H

// Starts the polling loop that watches which book is currently open
// on the device (via the system's "current book" state file) and
// records reading sessions to the SQLite database as books are
// opened and closed.
void tracker_init();

// Called once, on demand, to force-close whatever session is open
// right now (e.g. on EVT_EXIT), so we never lose the tail end of a
// session if the service itself is killed.
void tracker_flush();

// The polling callback registered with SetHardTimer. Not normally
// called directly -- exposed mainly so main.cpp can reference it.
void tracker_poll();

#endif
