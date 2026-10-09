// PK1: first-run disc setup (see ps2_disc_setup.h). Plain C++; the macOS dialogs live in
// ps2_disc_picker_mac.mm and run in a child process so no AppKit state exists in the game process
// before SDL3/raylib create their own NSApplication.
#include "ps2_disc_setup.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace ps2x::disc
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint32_t kSector = 2048u;
        constexpr uint32_t kRawSector = 2352u;
        constexpr const char *kBootElf = "SLES_513.56";
        constexpr const char *kMarkerName = ".ps2x_extracted";

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && v[0] == '1';
        }

        // ISO (2048) or raw 2352-byte sectors (MODE1 user data at +16, MODE2 form 1 at +24).
        class Image
        {
        public:
            ~Image()
            {
                if (m_f)
                    std::fclose(m_f);
            }

            bool open(const fs::path &path)
            {
                m_f = std::fopen(path.c_str(), "rb");
                if (!m_f)
                    return false;
                std::error_code ec;
                const uint64_t size = fs::file_size(path, ec);
                if (ec)
                    return false;
                static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
                uint8_t head[16] = {};
                if (size % kRawSector == 0u && readRaw(0, head, sizeof(head)) && std::memcmp(head, sync, 12) == 0)
                {
                    m_raw = true;
                    m_dataOffset = head[15] == 1u ? 16u : 24u;
                    m_sectors = size / kRawSector;
                }
                else
                {
                    m_sectors = size / kSector;
                }
                return true;
            }

            bool raw() const { return m_raw; }
            uint64_t sectors() const { return m_sectors; }

            bool readSectors(uint64_t lba, uint64_t count, uint8_t *dst)
            {
                if (lba + count > m_sectors)
                    return false;
                if (!m_raw)
                    return readRaw(lba * kSector, dst, count * kSector);
                for (uint64_t i = 0; i < count; ++i)
                    if (!readRaw((lba + i) * kRawSector + m_dataOffset, dst + i * kSector, kSector))
                        return false;
                return true;
            }

            // Reads `bytes` starting at the beginning of sector lba (the tail of the last sector is dropped).
            bool readExtent(uint64_t lba, uint64_t bytes, std::vector<uint8_t> &out)
            {
                const uint64_t count = (bytes + kSector - 1u) / kSector;
                std::vector<uint8_t> buf(count * kSector);
                if (count != 0u && !readSectors(lba, count, buf.data()))
                    return false;
                buf.resize(bytes);
                out.swap(buf);
                return true;
            }

        private:
            bool readRaw(uint64_t offset, void *dst, uint64_t bytes)
            {
                return ::fseeko(m_f, static_cast<off_t>(offset), SEEK_SET) == 0 &&
                       std::fread(dst, 1, bytes, m_f) == bytes;
            }

            std::FILE *m_f = nullptr;
            bool m_raw = false;
            uint32_t m_dataOffset = 0u;
            uint64_t m_sectors = 0u;
        };

        uint32_t le32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        struct DirEntry
        {
            std::string name; // version suffix (";1") removed
            uint32_t lba = 0;
            uint32_t size = 0;
            bool dir = false;
        };

        bool readDirectory(Image &img, uint32_t lba, uint32_t size, std::vector<DirEntry> &out)
        {
            if (size == 0u || size > (16u << 20))
                return false;
            std::vector<uint8_t> d;
            if (!img.readExtent(lba, size, d))
                return false;
            for (size_t pos = 0; pos < d.size();)
            {
                const uint8_t len = d[pos];
                if (len == 0u)
                {
                    pos = (pos / kSector + 1u) * kSector; // records never span sectors
                    continue;
                }
                if (len < 34u || pos + len > d.size())
                    return false;
                const uint8_t nameLen = d[pos + 32];
                if (33u + nameLen > len)
                    return false;
                std::string name(reinterpret_cast<const char *>(&d[pos + 33]), nameLen);
                if (!(nameLen == 1u && (name[0] == '\0' || name[0] == '\1')))
                {
                    if (const size_t semi = name.find(';'); semi != std::string::npos)
                        name.resize(semi);
                    if (!name.empty() && name.back() == '.')
                        name.pop_back();
                    DirEntry e;
                    e.name = name;
                    e.lba = le32(&d[pos + 2]);
                    e.size = le32(&d[pos + 10]);
                    e.dir = (d[pos + 25] & 0x02u) != 0u;
                    if (e.name.empty() || e.name == "." || e.name == ".." || e.name.find('/') != std::string::npos ||
                        e.name.find('\\') != std::string::npos)
                        return false;
                    out.push_back(std::move(e));
                }
                pos += len;
            }
            return true;
        }

        struct Volume
        {
            uint32_t rootLba = 0;
            uint32_t rootSize = 0;
            uint32_t volumeSectors = 0;
        };

        bool readVolume(Image &img, Volume &v)
        {
            std::array<uint8_t, kSector> pvd{};
            if (!img.readSectors(16, 1, pvd.data()) || pvd[0] != 1u || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
                return false;
            v.volumeSectors = le32(pvd.data() + 80);
            v.rootLba = le32(pvd.data() + 156 + 2);
            v.rootSize = le32(pvd.data() + 156 + 10);
            return true;
        }

        const DirEntry *findEntry(const std::vector<DirEntry> &dir, const std::string &name)
        {
            for (const DirEntry &e : dir)
                if (e.name == name)
                    return &e;
            return nullptr;
        }

        Result fail(Status s, std::string msg) { return Result{s, std::move(msg)}; }

        Result validateOpen(Image &img, const fs::path &image, Volume &vol, std::vector<DirEntry> &root)
        {
            const std::string shown = image.filename().string();
            if (!img.open(image))
                return fail(Status::FailedToOpen, "\"" + shown + "\" could not be opened.");
            if (!readVolume(img, vol) || !readDirectory(img, vol.rootLba, vol.rootSize, root))
                return fail(Status::NotAPs2Disc,
                            "\"" + shown + "\" is not a CD/DVD image (no ISO 9660 file system). Choose an .iso, or the .cue/.bin of your disc.");
            const DirEntry *cnf = findEntry(root, "SYSTEM.CNF");
            std::vector<uint8_t> cnfBytes;
            if (!cnf || cnf->dir || cnf->size > 4096u || !img.readExtent(cnf->lba, cnf->size, cnfBytes))
                return fail(Status::NotAPs2Disc, "\"" + shown + "\" is not a PlayStation 2 disc (no SYSTEM.CNF).");
            const std::string cnfText(cnfBytes.begin(), cnfBytes.end());
            std::string boot;
            std::istringstream lines(cnfText);
            for (std::string line; std::getline(lines, line);)
                if (line.rfind("BOOT2", 0) == 0)
                    boot = line;
            if (boot.find(kBootElf) == std::string::npos)
            {
                std::string serial = boot.empty() ? std::string("unknown") : boot;
                while (!serial.empty() && (serial.back() == '\r' || serial.back() == ' '))
                    serial.pop_back();
                return fail(Status::IncorrectGame,
                            "This is a different PlayStation 2 game (" + serial +
                                "). Road Trip Adventure needs the PAL disc SLES-51356 (Europe/Australia).");
            }
            const DirEntry *elf = findEntry(root, kBootElf);
            std::vector<uint8_t> magic;
            if (!elf || elf->dir || elf->size < 4096u || !img.readExtent(elf->lba, 4, magic) ||
                std::memcmp(magic.data(), "\x7f" "ELF", 4) != 0)
                return fail(Status::NotAPs2Disc, "\"" + shown + "\" has no readable SLES_513.56 boot file.");
            if (vol.volumeSectors == 0u || img.sectors() < vol.volumeSectors)
                return fail(Status::Truncated,
                            "\"" + shown + "\" is incomplete (" + std::to_string(img.sectors()) + " of " +
                                std::to_string(vol.volumeSectors) + " sectors). Make a new copy of the disc.");
            return Result{Status::Good, {}};
        }

        bool extractTree(Image &img, uint32_t lba, uint32_t size, const fs::path &dst, int depth)
        {
            if (depth > 16)
                return false;
            std::vector<DirEntry> entries;
            if (!readDirectory(img, lba, size, entries))
                return false;
            std::error_code ec;
            fs::create_directories(dst, ec);
            if (ec)
                return false;
            std::vector<uint8_t> buf;
            for (const DirEntry &e : entries)
            {
                const fs::path target = dst / e.name;
                if (e.dir)
                {
                    if (!extractTree(img, e.lba, e.size, target, depth + 1))
                        return false;
                    continue;
                }
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;
                constexpr uint64_t kChunkSectors = 512u; // 1 MiB
                uint64_t remaining = e.size;
                uint64_t sector = e.lba;
                while (remaining != 0u)
                {
                    const uint64_t bytes = std::min<uint64_t>(remaining, kChunkSectors * kSector);
                    if (!img.readExtent(sector, bytes, buf))
                        return false;
                    out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(bytes));
                    remaining -= bytes;
                    sector += kChunkSectors;
                }
                if (!out.good())
                    return false;
            }
            return true;
        }

        bool convertToIso(Image &img, uint64_t sectors, const fs::path &dst)
        {
            std::ofstream out(dst, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            std::vector<uint8_t> buf;
            for (uint64_t s = 0; s < sectors; s += 512u)
            {
                const uint64_t n = std::min<uint64_t>(512u, sectors - s);
                buf.resize(n * kSector);
                if (!img.readSectors(s, n, buf.data()))
                    return false;
                out.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
            }
            return out.good();
        }

        std::string markerText(const fs::path &image)
        {
            std::error_code ec;
            const uint64_t size = fs::file_size(image, ec);
            const auto mtime = fs::last_write_time(image, ec).time_since_epoch().count();
            std::ostringstream s;
            s << "ps2x-disc v1\n" << image.string() << "\n" << size << "\n" << static_cast<long long>(mtime) << "\n";
            return s.str();
        }

        std::string readText(const fs::path &p)
        {
            std::ifstream in(p, std::ios::binary);
            std::ostringstream s;
            s << in.rdbuf();
            return s.str();
        }

        bool writeTextAtomic(const fs::path &p, const std::string &text)
        {
            const fs::path tmp = p.string() + ".tmp";
            {
                std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                if (!out)
                    return false;
                out << text;
                if (!out.good())
                    return false;
            }
            std::error_code ec;
            fs::rename(tmp, p, ec);
            return !ec;
        }

        std::string jsonEscape(const std::string &s)
        {
            std::string o;
            for (const char c : s)
            {
                if (c == '"' || c == '\\')
                {
                    o += '\\';
                    o += c;
                }
                else if (static_cast<unsigned char>(c) < 0x20u)
                {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    o += b;
                }
                else
                    o += c;
            }
            return o;
        }

        // Minimal reader for the "disc" string value (escapes \" \\ \/ \n \t \uXXXX<0x80).
        bool jsonString(const std::string &s, const char *key, std::string &out)
        {
            const size_t k = s.find(std::string("\"") + key + "\"");
            if (k == std::string::npos)
                return false;
            size_t i = s.find(':', k);
            if (i == std::string::npos)
                return false;
            i = s.find('"', i);
            if (i == std::string::npos)
                return false;
            out.clear();
            for (++i; i < s.size(); ++i)
            {
                char c = s[i];
                if (c == '"')
                    return true;
                if (c == '\\' && i + 1 < s.size())
                {
                    c = s[++i];
                    if (c == 'n')
                        c = '\n';
                    else if (c == 't')
                        c = '\t';
                    else if (c == 'u' && i + 4 < s.size())
                    {
                        c = static_cast<char>(std::strtol(s.substr(i + 1, 4).c_str(), nullptr, 16) & 0x7F);
                        i += 4;
                    }
                }
                out += c;
            }
            return false;
        }

#if defined(__APPLE__)
        fs::path executablePath()
        {
            uint32_t size = 0;
            _NSGetExecutablePath(nullptr, &size);
            std::string buf(size, '\0');
            if (_NSGetExecutablePath(buf.data(), &size) != 0)
                return {};
            buf.resize(std::strlen(buf.c_str()));
            std::error_code ec;
            const fs::path canon = fs::weakly_canonical(buf, ec);
            return ec ? fs::path(buf) : canon;
        }

        // Runs the picker child; 0 = a disc was chosen, prepared and stored.
        int spawnPicker(const std::string &message)
        {
            const std::string exe = executablePath().string();
            std::vector<char *> args{const_cast<char *>(exe.c_str()), const_cast<char *>(kPickerArg)};
            if (!message.empty())
                args.push_back(const_cast<char *>(message.c_str()));
            args.push_back(nullptr);
            pid_t pid = 0;
            if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, args.data(), environ) != 0)
                return -1;
            int status = 0;
            while (waitpid(pid, &status, 0) < 0)
                if (errno != EINTR)
                    return -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
#endif
    }

    fs::path configDir()
    {
        if (const char *d = std::getenv("PS2X_CONFIG_DIR"); d && *d)
            return d;
        const char *home = std::getenv("HOME");
        return fs::path(home ? home : ".") / "Library/Application Support/RoadTripAdventure";
    }

    fs::path resolveImagePath(const fs::path &picked)
    {
        std::string ext = picked.extension().string();
        for (char &c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".cue")
            return picked;
        std::istringstream lines(readText(picked));
        for (std::string line; std::getline(lines, line);)
        {
            const size_t f = line.find("FILE");
            if (f == std::string::npos)
                continue;
            const size_t q1 = line.find('"', f);
            const size_t q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
            std::string name;
            if (q2 != std::string::npos)
                name = line.substr(q1 + 1, q2 - q1 - 1);
            else
            {
                std::istringstream w(line.substr(f + 4));
                w >> name;
            }
            if (!name.empty())
                return picked.parent_path() / name;
        }
        return picked;
    }

    Result validate(const fs::path &image)
    {
        Image img;
        Volume vol;
        std::vector<DirEntry> root;
        return validateOpen(img, image, vol, root);
    }

    namespace
    {
        // Strict skipper for one JSON value starting at s[i] (after whitespace); false = malformed.
        void skipWs(const std::string &s, size_t &i)
        {
            while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
                ++i;
        }

        bool skipString(const std::string &s, size_t &i, std::string *raw = nullptr)
        {
            if (i >= s.size() || s[i] != '"')
                return false;
            const size_t start = ++i;
            for (; i < s.size(); ++i)
            {
                if (s[i] == '\\')
                {
                    ++i;
                    continue;
                }
                if (static_cast<unsigned char>(s[i]) < 0x20u)
                    return false;
                if (s[i] == '"')
                {
                    if (raw)
                        *raw = s.substr(start, i - start);
                    ++i;
                    return true;
                }
            }
            return false;
        }

        bool skipValue(const std::string &s, size_t &i, int depth)
        {
            if (depth > 64 || i >= s.size())
                return false;
            const char c = s[i];
            if (c == '"')
                return skipString(s, i);
            if (c == '{' || c == '[')
            {
                const char close = c == '{' ? '}' : ']';
                ++i;
                skipWs(s, i);
                if (i < s.size() && s[i] == close)
                {
                    ++i;
                    return true;
                }
                for (;;)
                {
                    if (c == '{')
                    {
                        if (!skipString(s, i))
                            return false;
                        skipWs(s, i);
                        if (i >= s.size() || s[i++] != ':')
                            return false;
                        skipWs(s, i);
                    }
                    if (!skipValue(s, i, depth + 1))
                        return false;
                    skipWs(s, i);
                    if (i >= s.size())
                        return false;
                    if (s[i] == close)
                    {
                        ++i;
                        return true;
                    }
                    if (s[i++] != ',')
                        return false;
                    skipWs(s, i);
                }
            }
            const size_t start = i;
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.'))
                ++i;
            const std::string tok = s.substr(start, i - start);
            if (tok == "true" || tok == "false" || tok == "null")
                return true;
            if (tok.empty())
                return false;
            char *end = nullptr;
            std::strtod(tok.c_str(), &end);
            return end && *end == '\0';
        }

        struct Member
        {
            std::string key; // raw (escaped) key text
            size_t valueBegin = 0, valueEnd = 0;
        };

        // Top-level object members of a JSON document; false when the document is not one valid object.
        bool parseTopObject(const std::string &s, std::vector<Member> &members, size_t &closeBrace)
        {
            size_t i = 0;
            skipWs(s, i);
            if (i >= s.size() || s[i] != '{')
                return false;
            ++i;
            skipWs(s, i);
            if (i < s.size() && s[i] == '}')
                closeBrace = i++;
            else
                for (;;)
                {
                    Member m;
                    if (!skipString(s, i, &m.key))
                        return false;
                    skipWs(s, i);
                    if (i >= s.size() || s[i++] != ':')
                        return false;
                    skipWs(s, i);
                    m.valueBegin = i;
                    if (!skipValue(s, i, 1))
                        return false;
                    m.valueEnd = i;
                    members.push_back(m);
                    skipWs(s, i);
                    if (i >= s.size())
                        return false;
                    if (s[i] == '}')
                    {
                        closeBrace = i++;
                        break;
                    }
                    if (s[i++] != ',')
                        return false;
                    skipWs(s, i);
                }
            skipWs(s, i);
            return i == s.size();
        }
    }

    bool loadStoredDisc(fs::path &imageOut)
    {
        std::error_code ec;
        const fs::path p = configDir() / "general.json";
        if (!fs::is_regular_file(p, ec))
            return false;
        const std::string text = readText(p);
        std::vector<Member> members;
        size_t closeBrace = 0;
        if (!parseTopObject(text, members, closeBrace))
            return false; // corrupt -> treated as "no disc chosen" (the file itself is left alone)
        for (const Member &m : members)
        {
            std::string disc;
            if (m.key == "disc" &&
                jsonString("{\"disc\":" + text.substr(m.valueBegin, m.valueEnd - m.valueBegin) + "}", "disc", disc) && !disc.empty())
            {
                imageOut = disc;
                return true;
            }
        }
        return false;
    }

    // Merges "version" and "disc" into general.json, keeping every other key (e.g. Q5b's starter_*).
    // A file that exists but is not a valid JSON object is left untouched (returns false).
    bool storeDisc(const fs::path &image)
    {
        std::error_code ec;
        fs::create_directories(configDir(), ec);
        const fs::path p = configDir() / "general.json";
        const std::pair<std::string, std::string> wanted[] = {
            {"version", "1"}, {"disc", "\"" + jsonEscape(image.string()) + "\""}};
        if (!fs::exists(p, ec))
            return writeTextAtomic(p, "{\n  \"version\": 1,\n  \"disc\": " + wanted[1].second + "\n}\n");

        std::string text = readText(p);
        std::vector<Member> members;
        size_t closeBrace = 0;
        if (!parseTopObject(text, members, closeBrace))
        {
            std::cerr << "[disc] " << p.string() << " is not valid JSON; not overwriting it" << std::endl;
            return false;
        }
        std::string additions;
        for (const auto &[key, value] : wanted)
        {
            bool found = false;
            for (const Member &m : members)
                found = found || m.key == key;
            if (!found)
                additions += std::string(members.empty() && additions.empty() ? "\n" : ",\n") + "  \"" + key + "\": " + value;
        }
        if (!additions.empty())
        {
            size_t insertAt = closeBrace; // after the last member's value (before trailing whitespace)
            if (!members.empty())
                insertAt = members.back().valueEnd;
            text.insert(insertAt, additions + (members.empty() ? "\n" : ""));
        }
        // Replace existing values back to front so earlier offsets stay valid.
        for (size_t k = members.size(); k-- > 0;)
            for (const auto &[key, value] : wanted)
                if (members[k].key == key)
                    text.replace(members[k].valueBegin, members[k].valueEnd - members[k].valueBegin, value);
        return writeTextAtomic(p, text);
    }

    Result prepare(const fs::path &imageIn, fs::path &elfOut, fs::path &isoOut)
    {
        const fs::path image = resolveImagePath(imageIn);
        Image img;
        Volume vol;
        std::vector<DirEntry> root;
        if (Result r = validateOpen(img, image, vol, root); !r.ok())
            return r;

        const fs::path cfg = configDir();
        const fs::path dataDir = cfg / "disc";
        const fs::path convertedIso = cfg / "disc.iso";
        const std::string marker = markerText(image);
        elfOut = dataDir / kBootElf;
        isoOut = img.raw() ? convertedIso : image;

        std::error_code ec;
        if (readText(dataDir / kMarkerName) == marker && fs::is_regular_file(elfOut, ec) &&
            (!img.raw() || fs::file_size(convertedIso, ec) == static_cast<uint64_t>(vol.volumeSectors) * kSector))
            return Result{Status::Good, {}};

        std::cerr << "[disc] preparing " << image.string() << " -> " << dataDir.string() << std::endl;
        fs::create_directories(cfg, ec);
        const fs::path tmpDir = cfg / "disc.partial";
        fs::remove_all(tmpDir, ec);
        if (!extractTree(img, vol.rootLba, vol.rootSize, tmpDir, 0))
        {
            fs::remove_all(tmpDir, ec);
            return fail(Status::PrepareFailed, "Could not copy the game files from the disc image into " + cfg.string() +
                                                   " (is the disk full?).");
        }
        if (img.raw())
        {
            const fs::path tmpIso = cfg / "disc.iso.partial";
            if (!convertToIso(img, vol.volumeSectors, tmpIso) || (fs::rename(tmpIso, convertedIso, ec), ec))
            {
                fs::remove(tmpIso, ec);
                fs::remove_all(tmpDir, ec);
                return fail(Status::PrepareFailed, "Could not convert the BIN image into " + convertedIso.string() +
                                                       " (is the disk full?).");
            }
        }
        fs::remove_all(dataDir, ec);
        fs::rename(tmpDir, dataDir, ec);
        if (ec || !writeTextAtomic(dataDir / kMarkerName, marker))
            return fail(Status::PrepareFailed, "Could not finish preparing the disc in " + dataDir.string() + ".");
        std::cerr << "[disc] ready (" << (img.raw() ? "raw BIN converted" : "ISO") << ")" << std::endl;
        return Result{Status::Good, {}};
    }

    bool runningFromAppBundle()
    {
#if defined(__APPLE__)
        const fs::path exe = executablePath();
        const fs::path contents = exe.parent_path().parent_path();
        return exe.parent_path().filename() == "MacOS" && contents.filename() == "Contents" &&
               contents.parent_path().extension() == ".app";
#else
        return false;
#endif
    }

    bool resolveLaunch(std::string &elfPathOut)
    {
        const bool headless = envOn("PS2X_HEADLESS");
        const char *envImage = std::getenv("PS2X_CD_IMAGE");
        const bool fromEnv = envImage && envImage[0] != '\0';
        std::string pickerMessage;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            fs::path image;
            if (fromEnv)
                image = envImage;
            else if (!loadStoredDisc(image))
                image.clear();

            Result r{Status::FailedToOpen, "No disc has been chosen yet."};
            fs::path elf, iso;
            if (!image.empty())
            {
                r = prepare(image, elf, iso);
                if (!r.ok() && r.status == Status::FailedToOpen && !fromEnv)
                    r.message = "The disc image chosen earlier is missing or unreadable:\n" + image.string();
            }
            if (r.ok())
            {
                ::setenv("PS2X_CD_IMAGE", iso.c_str(), 1);
                const char *mc = std::getenv("PS2X_MC_ROOT");
                if (!mc || mc[0] == '\0')
                {
                    const fs::path mcRoot = configDir() / "mc0";
                    std::error_code ec;
                    fs::create_directories(mcRoot, ec);
                    ::setenv("PS2X_MC_ROOT", mcRoot.c_str(), 1);
                }
                elfPathOut = elf.string();
                std::cerr << "[disc] boot " << elfPathOut << " image=" << iso.string() << std::endl;
                return true;
            }
            if (fromEnv || headless)
            {
                std::cerr << "[disc] " << r.message << "\n[disc] No usable disc: pass the ELF as argv[1], set PS2X_CD_IMAGE, or put "
                          << "{\"disc\": \"/path/to/image.iso\"} in " << (configDir() / "general.json").string() << std::endl;
                return false;
            }
#if defined(__APPLE__)
            pickerMessage = image.empty() ? std::string() : r.message;
            const int rc = spawnPicker(pickerMessage);
            if (rc != 0)
            {
                std::cerr << "[disc] no disc chosen (picker exit " << rc << ")" << std::endl;
                return false;
            }
#else
            std::cerr << "[disc] " << r.message << std::endl;
            return false;
#endif
        }
        return false;
    }

#if !defined(__APPLE__)
    int runPickerProcess(int, char *[]) { return 1; }
#endif
}
