#pragma once
#include <FS.h>
#include <functional>
#include <string>
#include <vector>
#include "mc_ledger.h"
#include "mc_mutex.h"

// mcco::ILedgerStorage on LittleFS: append-only JSONL at
// /maccontrol/ledger.jsonl. Every append is flushed; a torn tail line left
// by an interrupted write is discarded at open (spec 5.1.1). replace_all
// writes a temp file and renames it over the live file (atomic as far as the
// platform allows). All operations are serialized with an internal mutex —
// the ledger is touched from the HTTP task and the dispatcher task.
//
// Uses the Arduino FS (File) API rather than stdio fopen: the VFS resolves
// fopen() against registered mount prefixes, so a bare "/maccontrol/..." path
// never reaches the LittleFS partition.
class FsLedgerStorage : public mcco::ILedgerStorage {
public:
    // Opens (creating if needed) the ledger file and repairs a torn tail.
    // LittleFS must already be mounted.
    bool begin();

    bool append(const std::string& line) override;
    bool read_all(const std::function<void(const std::string& line)>& cb) override;
    bool replace_all(const std::vector<std::string>& lines) override;

private:
    static constexpr const char* kPath = "/maccontrol/ledger.jsonl";
    static constexpr const char* kTmpPath = "/maccontrol/ledger.tmp";
    File f_;
    Mutex mutex_;
};
