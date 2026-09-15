#include "fs_ledger_storage.h"
#include <LittleFS.h>

bool FsLedgerStorage::begin() {
    if (!LittleFS.exists("/maccontrol")) LittleFS.mkdir("/maccontrol");

    // Torn-write discard (spec 5.1.1): if the last byte is not '\n', walk
    // back to the previous complete line and truncate there.
    if (LittleFS.exists(kPath)) {
        File f = LittleFS.open(kPath, "r+");
        if (!f) return false;
        const size_t size = f.size();
        if (size > 0) {
            f.seek(size - 1);
            if (f.read() != '\n') {
                long pos = (long)size - 1;
                while (pos > 0) {
                    --pos;
                    f.seek((uint32_t)pos);
                    if (f.read() == '\n') {
                        ++pos; // keep through the newline
                        break;
                    }
                }
                std::string keep;
                keep.resize((size_t)pos);
                if (pos > 0) {
                    f.seek(0);
                    if (f.read((uint8_t*)keep.data(), (size_t)pos) != pos) {
                        f.close();
                        return false;
                    }
                }
                f.close();
                File w = LittleFS.open(kPath, "w");
                if (!w) return false;
                bool ok = pos == 0 || w.write((const uint8_t*)keep.data(), (size_t)pos) == pos;
                w.close();
                if (!ok) return false;
            } else {
                f.close();
            }
        } else {
            f.close();
        }
    }

    f_ = LittleFS.open(kPath, "a"); // append mode, creates if missing
    if (!f_) f_ = LittleFS.open(kPath, "w");
    return (bool)f_;
}

bool FsLedgerStorage::append(const std::string& line) {
    Guard g(mutex_);
    if (!f_) return false;
    if (f_.write((const uint8_t*)line.data(), line.size()) != line.size()) return false;
    if (f_.write((uint8_t)'\n') != 1) return false;
    f_.flush();
    return true;
}

bool FsLedgerStorage::read_all(const std::function<void(const std::string&)>& cb) {
    Guard g(mutex_);
    File r = LittleFS.open(kPath, "r");
    if (!r) return true; // empty ledger is not an error
    std::string line;
    line.reserve(512);
    while (r.available()) {
        char buf[256];
        int n = r.read((uint8_t*)buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                if (!line.empty()) cb(line);
                line.clear();
            } else {
                line += buf[i];
            }
        }
    }
    if (!line.empty()) cb(line); // defensive: tail without newline
    r.close();
    return true;
}

bool FsLedgerStorage::replace_all(const std::vector<std::string>& lines) {
    Guard g(mutex_);
    if (f_) {
        f_.close();
        f_ = File();
    }
    File t = LittleFS.open(kTmpPath, "w");
    if (!t) return false;
    bool ok = true;
    for (const std::string& line : lines) {
        if (t.write((const uint8_t*)line.data(), line.size()) != line.size() ||
            t.write((uint8_t)'\n') != 1) {
            ok = false;
            break;
        }
    }
    t.flush();
    t.close();
    if (!ok) {
        LittleFS.remove(kTmpPath);
        return false;
    }
    if (!LittleFS.rename(kTmpPath, kPath)) return false;
    f_ = LittleFS.open(kPath, "a");
    return (bool)f_;
}
