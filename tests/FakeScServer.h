#pragma once

// ─── The team server's source control in a test (topic 199) ──────────────────
// The sync connection of CollabSession's README ("Quellcodeverwaltung"),
// served on FakeTeamServer's port as the real server does: a connection whose
// first frame is SC_HELLO with a ticket the fake team server handed out.
// Frames are decoded and encoded by hand, independently of HE::Net::ScClient.
//
// The merge is the per-file table of the design (section 5.1) with the
// simplest file class only: when both sides changed a file, the later upload
// wins whole ("replaced"), as the real server does for graph formats and
// binaries. Detail and trailer texts are the real server's (ScStore.cpp
// Commit), so the editor's parsing is tested against the real wording.
// Structural scene merges are tested against the real collab_server
// (test_team_sync.cpp, HE_TEST_TEAM_SERVER).
//
// Include after FakeTeamServer.h, once per test file.

#include "FakeTeamServer.h"

#include <Crypto/Sha256.h>

#include <ctime>

namespace {

void u64be(std::vector<std::uint8_t>& f, std::uint64_t v) {
    u32(f, std::uint32_t(v >> 32));
    u32(f, std::uint32_t(v));
}
void str16(std::vector<std::uint8_t>& f, const std::string& s) {
    f.push_back(std::uint8_t(s.size() >> 8));
    f.push_back(std::uint8_t(s.size()));
    f.insert(f.end(), s.begin(), s.end());
}
std::uint32_t getU32be(const std::vector<std::uint8_t>& f, std::size_t pos) {
    return (std::uint32_t(f.at(pos)) << 24) | (std::uint32_t(f.at(pos + 1)) << 16) |
           (std::uint32_t(f.at(pos + 2)) << 8) | std::uint32_t(f.at(pos + 3));
}
std::uint64_t getU64be(const std::vector<std::uint8_t>& f, std::size_t pos) {
    return (std::uint64_t(getU32be(f, pos)) << 32) | getU32be(f, pos + 4);
}
std::string hexOf(const std::uint8_t* p, std::size_t n) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 15]);
    }
    return out;
}
std::string sha256Hex(const std::vector<std::uint8_t>& bytes) {
    std::uint8_t d[HE::Crypto::Sha256::kDigestSize];
    HE::Crypto::Sha256::digest(bytes.data(), bytes.size(), d);
    return HE::Crypto::Sha256::toHex(d);
}
std::vector<std::string> linesOf(const std::vector<std::uint8_t>& bytes) {
    std::vector<std::string> out;
    std::string cur;
    for (std::uint8_t b : bytes) {
        if (b == '\n') { out.push_back(cur); cur.clear(); }
        else cur.push_back(char(b));
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::vector<std::string> tabsOf(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t t = line.find('\t', start);
        out.push_back(line.substr(start, t == std::string::npos ? std::string::npos : t - start));
        if (t == std::string::npos) return out;
        start = t + 1;
    }
}

struct FakeScServer {
    struct File {
        std::string   sha;
        std::uint64_t rev = 0;      // the revision that last changed it
        std::string   member;       // who saved that revision
    };
    using Tree = std::map<std::string, File>;
    struct Rev {
        std::string member, message;
        std::int64_t time = 0;
        std::vector<std::string> merges;   // "Horizon-Merge:" values
    };
    struct Sync {
        std::string member, project;
        bool        missing = false;    // sc-ready "no project yet"
        // A request whose stream is still coming in.
        bool                      collecting = false;
        std::uint32_t             id = 0;
        std::uint8_t              op = 0;
        std::vector<std::uint8_t> fields, stream;
    };

    FakeTeamServer&                                  team;
    std::map<std::string, std::vector<std::uint8_t>> blobs;     // sha256 → content
    std::map<std::string, std::vector<Tree>>         trees;     // project → tree per revision (0 = empty)
    std::map<std::string, std::vector<Rev>>          logs;      // project → revisions (0 = none)
    std::map<ConnectionId, Sync>                     syncs;
    std::vector<std::uint8_t>                        ops;       // every SC_REQUEST op
    int  puts = 0, commits = 0, hellos = 0;
    bool busyNextCommit = false;    // answer the next COMMIT with busy (7)

    explicit FakeScServer(FakeTeamServer& t) : team(t) {
        team.scFrame = [this](ConnectionId c, const std::vector<std::uint8_t>& f) { return frame(c, f); };
        team.scGone  = [this](ConnectionId c) { syncs.erase(c); };
    }
    ~FakeScServer() {
        team.scFrame = nullptr;
        team.scGone  = nullptr;
    }

    std::vector<Tree>& treesOf(const std::string& project) {
        auto& v = trees[project];
        if (v.empty()) v.emplace_back();
        return v;
    }
    std::vector<Rev>& logOf(const std::string& project) {
        auto& v = logs[project];
        if (v.empty()) v.emplace_back();
        return v;
    }
    std::uint64_t head(const std::string& project) { return treesOf(project).size() - 1; }
    // The content of `path` at the head of `project`, or "<none>".
    std::string text(const std::string& project, const std::string& path) {
        const Tree& t = treesOf(project).back();
        auto it = t.find(path);
        if (it == t.end()) return "<none>";
        const auto& b = blobs[it->second.sha];
        return std::string(b.begin(), b.end());
    }

    void send(ConnectionId c, const std::vector<std::uint8_t>& f) { team.send(c, f); }
    void status(ConnectionId c, std::uint8_t code, const std::string& text = {}) { team.status(c, code, text); }
    void data(ConnectionId c, std::uint32_t id, const std::vector<std::uint8_t>& bytes) {
        std::size_t off = 0;
        do {
            const std::size_t n = std::min<std::size_t>(1024 * 1024, bytes.size() - off);
            auto f = header(13);
            u32(f, id);
            u64be(f, off);
            f.push_back(off + n == bytes.size() ? 1 : 0);
            f.insert(f.end(), bytes.begin() + std::ptrdiff_t(off), bytes.begin() + std::ptrdiff_t(off + n));
            send(c, f);
            off += n;
        } while (off < bytes.size());
    }
    void reply(ConnectionId c, std::uint32_t id, std::uint8_t result,
               const std::vector<std::uint8_t>& body = {}, const std::string& text = {}) {
        auto f = header(12);
        u32(f, id);
        f.push_back(result);
        str16(f, text);
        f.insert(f.end(), body.begin(), body.end());
        send(c, f);
    }
    static std::vector<std::uint8_t> bytesOf(const std::string& s) { return { s.begin(), s.end() }; }

    bool frame(ConnectionId c, const std::vector<std::uint8_t>& f) {
        auto it = syncs.find(c);
        if (it == syncs.end()) {
            if (team.conns[c].loggedIn || f.size() < 6 || f[5] != 10) return false;
            hello(c, f);
            return true;
        }
        Sync& s = it->second;
        REQUIRE(f.size() >= 6);
        if (f[5] == 11) {   // SC_REQUEST
            REQUIRE_FALSE(s.collecting);
            const std::uint32_t id = getU32be(f, 6);
            const std::uint8_t  op = f.at(10);
            ops.push_back(op);
            std::vector<std::uint8_t> fields(f.begin() + 11, f.end());
            if (op == 3 || op == 5 || op == 6) {   // HAVE, PUT, COMMIT: a stream follows
                s.collecting = true;
                s.id = id;
                s.op = op;
                s.fields = std::move(fields);
                s.stream.clear();
                return true;
            }
            handle(c, s, id, op, fields, {});
            return true;
        }
        REQUIRE(f[5] == 13);  // SC_DATA
        REQUIRE(s.collecting);
        REQUIRE(getU32be(f, 6) == s.id);
        REQUIRE(getU64be(f, 10) == s.stream.size());
        const bool last = f.at(18) == 1;
        s.stream.insert(s.stream.end(), f.begin() + 19, f.end());
        if (last) {
            s.collecting = false;
            handle(c, s, s.id, s.op, s.fields, s.stream);
        }
        return true;
    }

    void hello(ConnectionId c, const std::vector<std::uint8_t>& f) {
        ++hellos;
        REQUIRE(std::string(f.begin(), f.begin() + 4) == "HERL");
        std::size_t pos = 6;
        const std::string teamName = readStr(f, pos);
        const std::string member   = readStr(f, pos);
        const std::string project  = readStr(f, pos);
        REQUIRE(f.size() == pos + 16);
        const std::string ticket(f.begin() + std::ptrdiff_t(pos), f.end());
        CHECK(teamName == "studio");
        auto t = team.scTickets.find(ticket);
        if (t == team.scTickets.end() || !sameName(t->second.member, member) || t->second.project != project) {
            status(c, 23, "unknown ticket");
            team.t->update();
            team.t->disconnect(c);
            team.refused.insert(c);
            return;
        }
        team.scTickets.erase(t);   // single use
        Sync s;
        s.member  = member;
        s.project = project;
        s.missing = !team.scProjects.count(project);
        syncs[c] = s;
        status(c, 5, s.missing ? "no project yet" : "");
    }

    void handle(ConnectionId c, Sync& s, std::uint32_t id, std::uint8_t op,
                const std::vector<std::uint8_t>& fields, const std::vector<std::uint8_t>& stream) {
        if (s.missing && op != 8) { reply(c, id, 10); return; }
        const std::string& p = s.project;
        switch (op) {
            case 1: {   // HEAD
                std::vector<std::uint8_t> body;
                u64be(body, head(p));
                str8(body, "fake-oid");
                reply(c, id, 0, body);
                return;
            }
            case 2: {   // MANIFEST
                std::uint64_t rev = getU64be(fields, 0);
                if (rev == 0) rev = head(p);
                if (rev > head(p)) { reply(c, id, 1); return; }
                std::string lines;
                for (const auto& [path, file] : treesOf(p)[rev])
                    lines += path + "\t" + file.sha + "\t" + std::to_string(blobs[file.sha].size()) + "\t100644\n";
                data(c, id, bytesOf(lines));
                std::vector<std::uint8_t> body;
                u64be(body, rev);
                reply(c, id, 0, body);
                return;
            }
            case 3: {   // HAVE
                std::string missing;
                for (const std::string& sha : linesOf(stream))
                    if (!blobs.count(sha)) missing += sha + "\n";
                data(c, id, bytesOf(missing));
                reply(c, id, 0);
                return;
            }
            case 4: {   // GET
                REQUIRE(fields.size() == 32);
                const std::string sha = hexOf(fields.data(), 32);
                auto b = blobs.find(sha);
                if (b == blobs.end()) { reply(c, id, 3); return; }
                data(c, id, b->second);
                std::vector<std::uint8_t> body;
                u64be(body, b->second.size());
                reply(c, id, 0, body);
                return;
            }
            case 5: {   // PUT
                REQUIRE(fields.size() == 40);
                const std::string sha = hexOf(fields.data(), 32);
                if (sha256Hex(stream) != sha || getU64be(fields, 32) != stream.size()) { reply(c, id, 12); return; }
                blobs[sha] = stream;
                ++puts;
                reply(c, id, 0);
                return;
            }
            case 6: commit(c, s, id, fields, stream); return;
            case 7: {   // LOG
                std::uint64_t from = getU64be(fields, 0);
                const std::uint32_t count = (std::uint32_t(fields.at(8)) << 8) | fields.at(9);
                if (from == 0) from = 1;
                std::string out;
                const auto& log = logOf(p);
                for (std::uint64_t r = from; r < log.size() && r < from + count; ++r) {
                    out += "R\t" + std::to_string(r) + "\t" + log[r].member + "\t" + std::to_string(log[r].time) +
                           "\tSave by " + log[r].member + " (rev " + std::to_string(r) + ")\n";
                    for (const std::string& m : log[r].merges) out += "M\t" + m + "\n";
                }
                data(c, id, bytesOf(out));
                reply(c, id, 0);
                return;
            }
            case 8: {   // CREATE_PROJECT
                if (!team.admins.count(s.member)) { reply(c, id, 2); return; }
                if (!s.missing) { reply(c, id, 11); return; }
                team.scProjects.insert(p);
                s.missing = false;
                reply(c, id, 0);
                return;
            }
            default:
                reply(c, id, 1);
                return;
        }
    }

    void commit(ConnectionId c, Sync& s, std::uint32_t id, const std::vector<std::uint8_t>& fields,
                const std::vector<std::uint8_t>& stream) {
        ++commits;
        if (busyNextCommit) { busyNextCommit = false; reply(c, id, 7); return; }
        const std::string& p = s.project;
        std::vector<Tree>& tv = treesOf(p);
        const std::uint64_t newRev = tv.size();
        Tree next = tv.back();
        std::string outcomes;
        std::vector<std::string> merges;
        bool changed = false;
        for (const std::string& line : linesOf(stream)) {
            const std::vector<std::string> e = tabsOf(line);
            REQUIRE(e.size() == 4);
            const std::string& path = e[1];
            const std::string b = e[2] == "-" ? "" : e[2];
            const std::string n = e[3] == "-" ? "" : e[3];
            if (!n.empty() && !blobs.count(n)) { reply(c, id, 3, {}, path); return; }
            auto hit = next.find(path);
            const std::string h = hit == next.end() ? "" : hit->second.sha;
            std::string outcome, detail, result = n;
            if (h == b) {
                outcome = "applied";
            } else if (h == n) {
                outcome = "same";
            } else if (n.empty()) {
                outcome = "delete-dropped";
                result  = h;
                detail  = "changed in r" + std::to_string(hit->second.rev) + " by " + hit->second.member;
            } else if (h.empty()) {
                outcome = "restored";
            } else {
                outcome = "replaced";
                detail  = "against r" + std::to_string(hit->second.rev) + " by " + hit->second.member +
                          " lost=horizon-conflicts:r" + std::to_string(newRev) + "/" + path;
            }
            if (outcome == "applied" || outcome == "restored" || outcome == "replaced") {
                changed = true;
                if (n.empty()) next.erase(path);
                else next[path] = File{ n, newRev, s.member };
            }
            outcomes += outcome + "\t" + path + "\t" + (result.empty() ? "-" : result) + "\t" + detail + "\n";
            if (outcome != "same")
                merges.push_back(outcome + " " + path + (detail.empty() ? "" : " " + detail));
        }
        data(c, id, bytesOf(outcomes));
        std::vector<std::uint8_t> body;
        u64be(body, changed ? newRev : head(p));
        body.push_back(changed ? 1 : 0);
        reply(c, id, 0, body);
        if (!changed) return;
        tv.push_back(std::move(next));
        Rev r;
        r.member = s.member;
        r.time   = static_cast<std::int64_t>(std::time(nullptr));
        r.message = std::string(fields.begin() + 10, fields.end());
        r.merges = std::move(merges);
        logOf(p).push_back(std::move(r));
        // Every team connection of the team, the saver's own included.
        team.sendToAll(FakeTeamServer::scHeadEvent(p, newRev, s.member));
    }
};

} // namespace
