#include "../include/RedisDatabase.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <unordered_set>
#include <iomanip>
#include <cstdint>

// Singleton accessor
RedisDatabase& RedisDatabase::getInstance() {
    static RedisDatabase instance;
    return instance;
}

// Common Comands
bool RedisDatabase::flushAll() {
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();
    return true;
}

// Key/Value Operations
void RedisDatabase::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    kv_store[key] = value;
}

bool RedisDatabase::get(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = kv_store.find(key);
    if (it != kv_store.end()) {
        value = it->second;
        return true;
    }
    return false;
}

std::vector<std::string> RedisDatabase::keys() {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    // A key can exist in multiple stores (e.g. SET foo + LPUSH foo) — dedupe via set
    std::unordered_set<std::string> unique;
    for (const auto& pair : kv_store)   unique.insert(pair.first);
    for (const auto& pair : list_store) unique.insert(pair.first);
    for (const auto& pair : hash_store) unique.insert(pair.first);
    return std::vector<std::string>(unique.begin(), unique.end());
}

std::string RedisDatabase::type(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    if (kv_store.find(key) != kv_store.end()) 
        return "string";
    if (list_store.find(key) != list_store.end())
        return "list";
    if (hash_store.find(key) != hash_store.end()) 
        return "hash";
    else return "none";    
}

bool RedisDatabase::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool erased = false;
    erased |= kv_store.erase(key) > 0;
    erased |= list_store.erase(key) > 0;
    erased |= hash_store.erase(key) > 0;
    expiry_map.erase(key);
    return erased;
}

bool RedisDatabase::expire(const std::string& key, int seconds) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool exists = (kv_store.find(key) != kv_store.end()) ||
                  (list_store.find(key) != list_store.end()) ||
                  (hash_store.find(key) != hash_store.end());
    if (!exists)
        return false;
    
    expiry_map[key] = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    return true;
}

void RedisDatabase::purgeExpired() {
    auto now = std::chrono::steady_clock::now();
    for (auto it = expiry_map.begin(); it != expiry_map.end(); ) {
        if (now > it->second) {
            // Remove from all stores
            kv_store.erase(it->first);
            list_store.erase(it->first);
            hash_store.erase(it->first);
            it = expiry_map.erase(it);
        } else {
            ++it;
        }
    }
}

bool RedisDatabase::rename(const std::string& oldKey, const std::string& newKey) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    bool found = false;

    auto itKv = kv_store.find(oldKey);
    if (itKv != kv_store.end()) {
        kv_store[newKey] = itKv->second;
        kv_store.erase(itKv);
        found = true;
    }

    auto itList = list_store.find(oldKey);
    if (itList != list_store.end()) {
        list_store[newKey] = itList->second;
        list_store.erase(itList);
        found = true;
    }

    auto itHash = hash_store.find(oldKey);
    if (itHash != hash_store.end()) {
        hash_store[newKey] = itHash->second;
        hash_store.erase(itHash);
        found = true;
    }

    auto itExpire = expiry_map.find(oldKey);
    if (itExpire != expiry_map.end()) {
        expiry_map[newKey] = itExpire->second;
        expiry_map.erase(itExpire);
    }

    return found;
}

// List Opreations
std::vector<std::string> RedisDatabase::lget(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = list_store.find(key);
    if (it != list_store.end()) {
        return it->second; 
    }
    return {}; 
}

ssize_t RedisDatabase::llen(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = list_store.find(key);
    if (it != list_store.end())
        return it->second.size();
    return 0;
}

void RedisDatabase::lpush(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    list_store[key].insert(list_store[key].begin(), value);
}

void RedisDatabase::rpush(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    list_store[key].push_back(value);
}

bool RedisDatabase::lpop(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = list_store.find(key);
    if (it != list_store.end() && !it->second.empty()) {
        value = it->second.front();
        it->second.erase(it->second.begin());
        return true;
    }
    return false;
}

bool RedisDatabase::rpop(const std::string& key, std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = list_store.find(key);
    if (it != list_store.end() && !it->second.empty()) {
        value = it->second.back();
        it->second.pop_back();
        return true;
    }
    return false;
}

int RedisDatabase::lrem(const std::string& key, int count, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    int removed = 0;
    auto it = list_store.find(key);
    if (it == list_store.end()) 
        return 0;

    auto& lst = it->second;

    if (count == 0) {
        // Remove all occurances
        auto new_end = std::remove(lst.begin(), lst.end(), value);
        removed = std::distance(new_end, lst.end());
        lst.erase(new_end, lst.end());
    } else if (count > 0) {
        // Remove from head to tail
        for (auto iter = lst.begin(); iter != lst.end() && removed < count; ) {
            if (*iter == value) {
                iter = lst.erase(iter);
                ++removed;
            } else {
                ++iter;
            }
        }
    } else {
        // Remove from tail to head (count is negative)
        for (auto riter = lst.rbegin(); riter != lst.rend() && removed < (-count); ) {
            if (*riter == value) {
                auto fwdIter = riter.base();
                --fwdIter;
                fwdIter = lst.erase(fwdIter);
                ++removed;
                riter = std::make_reverse_iterator(fwdIter);
            } else {
                ++riter;
            }
        }
    }
    return removed;
}

bool RedisDatabase::lindex(const std::string& key, int index, std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = list_store.find(key);
    if (it == list_store.end())
        return false;

    const auto& lst = it->second;
    if (index < 0)
        index = lst.size() + index;
    if (index < 0 || index >= static_cast<int>(lst.size()))
        return false;
    
    value = lst[index];
    return true;
}

bool RedisDatabase::lset(const std::string& key, int index, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = list_store.find(key);
    if (it == list_store.end())
        return false;

    auto& lst = it->second;
    if (index < 0)
        index = lst.size() + index;
    if (index < 0 || index >= static_cast<int>(lst.size()))
        return false;
    
    lst[index] = value;
    return true;
}

// Hash Operations
bool RedisDatabase::hset(const std::string& key, const std::string& field, const std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    hash_store[key][field] = value;
    return true;
}

bool RedisDatabase::hget(const std::string& key, const std::string& field, std::string& value) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = hash_store.find(key);
    if (it != hash_store.end()) {
        auto f = it->second.find(field);
        if (f != it->second.end()) {
            value = f->second;
            return true;
        }
    }
    return false;
}

bool RedisDatabase::hexists(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = hash_store.find(key);
    if (it != hash_store.end())
        return it->second.find(field) != it->second.end();
    return false;
}

bool RedisDatabase::hdel(const std::string& key, const std::string& field) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = hash_store.find(key);
    if (it != hash_store.end())
        return it->second.erase(field) > 0;
    return false;
}

std::unordered_map<std::string, std::string> RedisDatabase::hgetall(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    if (hash_store.find(key) != hash_store.end())
        return hash_store[key];
    return {};
}

std::vector<std::string> RedisDatabase::hkeys(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    std::vector<std::string> fields;
    auto it = hash_store.find(key);
    if (it != hash_store.end()) {
        for (const auto& pair: it->second)
            fields.push_back(pair.first);
    }
    return fields;
}

std::vector<std::string> RedisDatabase::hvals(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    std::vector<std::string> values;
    auto it = hash_store.find(key);
    if (it != hash_store.end()) {
        for (const auto& pair: it->second)
            values.push_back(pair.second);
    }
    return values;
}

ssize_t RedisDatabase::hlen(const std::string& key) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    auto it = hash_store.find(key);
    return (it != hash_store.end()) ? it->second.size() : 0;
}

bool RedisDatabase::hmset(const std::string& key, const std::vector<std::pair<std::string, std::string>>& fieldValues) {
    std::lock_guard<std::mutex> lock(db_mutex);
    purgeExpired();
    for (const auto& pair: fieldValues) {
        hash_store[key][pair.first] = pair.second;
    }
    return true;
}

/*
Persistence format: each record is type-tagged.

Memory -> File - dump()
File -> Memory - load()

K = Key/Value      (B1: length-prefixed — binary-safe)
L = List           (B2: still space-delimited — TODO)
H = Hash           (B3/B4: still space+colon-delimited — TODO)

Length-prefixed encoding: "<size> <bytes>". The size is ASCII digits followed
by exactly one space, then `size` bytes of raw data. Data may contain any
byte — spaces, colons, newlines — without ambiguity, because the parser
reads exactly `size` bytes and never scans for delimiters.
*/

namespace {
// Writes a length-prefixed string: "<size> <bytes>". Binary-safe.
void writeLP(std::ostream& os, const std::string& s) {
    os << s.size() << ' ';
    os.write(s.data(), s.size());
}

// Reads a length-prefixed string written by writeLP.
// Skips leading whitespace before the size (via operator>>),
// then consumes exactly one space and reads `size` raw bytes.
bool readLP(std::istream& is, std::string& out) {
    size_t len;
    if (!(is >> len)) return false;
    is.get();  // consume the single space separating size from bytes
    out.resize(len);
    if (len == 0) return true;
    is.read(&out[0], len);
    return static_cast<size_t>(is.gcount()) == len;
}

// CRC32 (IEEE 802.3 polynomial 0xEDB88320). Used for B6: integrity check on
// the dump file body. Cheap (~1 GB/s table-based), catches single-bit flips,
// truncations, and partial writes with ~1 - 2^-32 probability.
uint32_t crc32(const char* data, size_t len) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        init = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ static_cast<uint8_t>(data[i])) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
} // namespace

bool RedisDatabase::dump(const std::string& filename) {
    std::lock_guard<std::mutex> lock(db_mutex);

    // Build the body in memory first so we can compute its CRC before writing.
    // For a dump of size N this costs ~N transient bytes; acceptable for an
    // RDB-style snapshot. (B7's fork+COW fix would let us stream instead.)
    std::ostringstream body;

    // K records — B1 fix: length-prefixed key + value.
    for (const auto& kv : kv_store) {
        body << "K ";
        writeLP(body, kv.first);
        body << ' ';
        writeLP(body, kv.second);
        body << '\n';
    }

    // L records — B2 fix: length-prefixed key, then count, then length-prefixed items.
    for (const auto& kv : list_store) {
        body << "L ";
        writeLP(body, kv.first);
        body << ' ' << kv.second.size();
        for (const auto& item : kv.second) {
            body << ' ';
            writeLP(body, item);
        }
        body << '\n';
    }

    // H records — B3/B4 fix: length-prefixed key, count, then length-prefixed
    // field/value pairs. Eliminates the ':' delimiter ambiguity by construction.
    for (const auto& kv : hash_store) {
        body << "H ";
        writeLP(body, kv.first);
        body << ' ' << kv.second.size();
        for (const auto& fv : kv.second) {
            body << ' ';
            writeLP(body, fv.first);
            body << ' ';
            writeLP(body, fv.second);
        }
        body << '\n';
    }

    // E records — B5 fix: persist TTLs as absolute unix epoch milliseconds.
    // Why absolute, not relative: a relative duration ("expires in 10s") would
    // be reset to "10s from process start" on every restart, effectively making
    // TTLs immortal across crashes. Absolute timestamps survive restarts correctly.
    //
    // The in-memory expiry_map uses steady_clock (monotonic — won't jump on NTP
    // adjustments). For persistence we convert to system_clock by computing the
    // delta and applying it to system_clock::now(). Conversion is only done at
    // dump/load — runtime expiry checks still use steady_clock.
    auto steady_now = std::chrono::steady_clock::now();
    auto system_now = std::chrono::system_clock::now();
    for (const auto& kv : expiry_map) {
        auto delta = kv.second - steady_now;
        auto absolute = system_now + delta;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      absolute.time_since_epoch()).count();
        body << "E ";
        writeLP(body, kv.first);
        body << ' ' << ms << '\n';
    }

    std::string body_str = body.str();
    uint32_t crc = crc32(body_str.data(), body_str.size());

    // B6 fix: write magic header, then CRC, then body.
    // Header lets old binaries reject newer formats loudly; CRC catches
    // accidental corruption (disk errors, partial writes during crash).
    std::ofstream ofs(filename, std::ios::binary);
    if (!ofs) return false;
    ofs << "REDIS_DUMP_V1\n";
    ofs << std::hex << std::setw(8) << std::setfill('0') << crc << std::dec << '\n';
    ofs.write(body_str.data(), body_str.size());
    return static_cast<bool>(ofs);
}

/*
Key-Value (K)
kv_store["name"] = "Alice";
kv_store["city"] = "Berlin";

List (L)
list_store["fruits"] = {"apple", "banana", "orange"};
list_store["colors"] = {"red", "green", "blue"};

Hash (H)
hash_store["user:100"] = {
    {"name", "Bob"},
    {"age", "30"},
    {"email", "bob@example.com"}
};

hash_store["user:200"] = {
    {"name", "Eve"},
    {"age", "25"},
    {"email", "eve@example.com"}
};
*/
bool RedisDatabase::load(const std::string& filename) {
    std::lock_guard<std::mutex> lock(db_mutex);
    std::ifstream ifs(filename, std::ios::binary);
    if (!ifs) return false;  // file doesn't exist — normal on first run

    // B6: verify magic header.
    std::string header;
    std::getline(ifs, header);
    if (header != "REDIS_DUMP_V1") return false;

    // B6: read expected CRC, then read body, then verify.
    std::string crc_line;
    if (!std::getline(ifs, crc_line)) return false;
    uint32_t expected_crc;
    try {
        expected_crc = static_cast<uint32_t>(std::stoul(crc_line, nullptr, 16));
    } catch (const std::exception&) {
        return false;
    }
    std::ostringstream body_ss;
    body_ss << ifs.rdbuf();
    std::string body = body_ss.str();
    if (crc32(body.data(), body.size()) != expected_crc) return false;

    // Only clear in-memory state AFTER integrity checks pass — fail-loud
    // principle. A corrupted file shouldn't wipe a known-good in-memory DB.
    kv_store.clear();
    list_store.clear();
    hash_store.clear();
    expiry_map.clear();

    // Parse the body stream-based. operator>> skips whitespace between
    // records (the trailing '\n' after each record's payload).
    std::istringstream stream(body);
    char type;
    while (stream >> type) {
        if (type == 'K') {
            std::string key, value;
            if (!readLP(stream, key) || !readLP(stream, value)) return false;
            kv_store[std::move(key)] = std::move(value);
        } else if (type == 'L') {
            std::string key;
            if (!readLP(stream, key)) return false;
            size_t count;
            if (!(stream >> count)) return false;
            std::vector<std::string> list;
            list.reserve(count);
            for (size_t i = 0; i < count; ++i) {
                std::string item;
                if (!readLP(stream, item)) return false;
                list.push_back(std::move(item));
            }
            list_store[std::move(key)] = std::move(list);
        } else if (type == 'H') {
            std::string key;
            if (!readLP(stream, key)) return false;
            size_t count;
            if (!(stream >> count)) return false;
            std::unordered_map<std::string, std::string> hash;
            for (size_t i = 0; i < count; ++i) {
                std::string field, value;
                if (!readLP(stream, field) || !readLP(stream, value)) return false;
                hash[std::move(field)] = std::move(value);
            }
            hash_store[std::move(key)] = std::move(hash);
        } else if (type == 'E') {
            // B5: TTL record. Stored as absolute unix epoch ms; convert back
            // to a steady_clock::time_point relative to the current process.
            std::string key;
            if (!readLP(stream, key)) return false;
            int64_t ms;
            if (!(stream >> ms)) return false;
            auto system_target = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(ms));
            auto delta = system_target - std::chrono::system_clock::now();
            // If delta is negative the key has already expired; record it
            // anyway — purgeExpired() will clean it up on next access.
            expiry_map[std::move(key)] = std::chrono::steady_clock::now() + delta;
        } else {
            return false;  // unknown record type — file is corrupt or newer-format
        }
    }
    return true;
}
