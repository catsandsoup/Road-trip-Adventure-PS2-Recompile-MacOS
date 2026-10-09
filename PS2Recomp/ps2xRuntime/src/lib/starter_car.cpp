// GOALS Q5 starting-car chooser (see include/starter_car.h). No disc data here: body ids and guest
// addresses come from the game's own code (Q5a), the default paint word is read from guest RAM after
// the game wrote it.
#include "starter_car.h"
#include "game_hooks.h"
#include "ps2_shell.h"

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <utility>
#include <vector>

namespace ps2x::starter
{
    namespace
    {
        // FUN_00270550's six jump-table cases call 0x22b568(body); these are their return addresses.
        constexpr uint32_t kSetStarterBody = 0x0022b568u;
        constexpr uint32_t kPickerReturns[6] = {0x002705c0u, 0x002705d0u, 0x002705e0u,
                                                0x002705f0u, 0x00270600u, 0x00270610u};
        constexpr int kStarterBodies[6] = {43, 0, 117, 140, 114, 93}; // case order 0..5
        constexpr uint32_t kSaveSlot = 0x01824f80u;  // save slot 0; +0x00 car paint word
        constexpr uint32_t kBodyPaintTable = 0xd70u; // +0xd70 + body*4: per-body paint word
        constexpr int kBodyCount = 151;

        struct PaintPreset
        {
            const char *label;
            int64_t word; // low 24 bits: two tones x RGB nibbles (rtao paintShop.ts); wheel byte kept
        };
        const PaintPreset kPaints[] = {
            {"Default (original)", kDefaultPaint},
            {"Red", 0x00f00f},
            {"Blue", 0xf00f00},
            {"Yellow", 0x0ff0ff},
            {"Green", 0x0f00f0},
            {"White", 0xffffff},
            {"Black", 0x000000},
        };

        std::atomic<int> g_body{kRandom};
        std::atomic<int64_t> g_paint{kDefaultPaint};
        std::mutex g_fileMutex;

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && v[0] == '1';
        }

        std::filesystem::path configDir()
        {
            if (const char *d = std::getenv("PS2X_CONFIG_DIR"); d && *d)
                return d;
            const char *home = std::getenv("HOME");
            return std::filesystem::path(home ? home : ".") / "Library/Application Support/RoadTripAdventure";
        }

        bool parseBody(const std::string &v, int &out)
        {
            if (v.empty() || v == "random")
                return out = kRandom, true;
            char *end = nullptr;
            const long id = std::strtol(v.c_str(), &end, 0);
            if (*end || id < 0 || id >= kBodyCount)
                return false;
            out = int(id);
            return true;
        }

        bool parsePaint(const std::string &v, int64_t &out)
        {
            if (v.empty() || v == "default")
                return out = kDefaultPaint, true;
            char *end = nullptr;
            const unsigned long long w = std::strtoull(v.c_str(), &end, 16);
            if (*end)
                return false;
            out = int64_t(w & 0x00ffffffull);
            return true;
        }

        // ---- general.json: a flat top-level object; we own "starter_body" and "starter_paint" and keep
        // every other key's raw value text untouched (the disc path and future settings live there too).
        using Members = std::vector<std::pair<std::string, std::string>>;

        size_t skipWs(const std::string &s, size_t i)
        {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
                ++i;
            return i;
        }
        size_t skipString(const std::string &s, size_t i) // s[i] == '"'; returns index after the closing quote
        {
            for (++i; i < s.size(); ++i)
            {
                if (s[i] == '\\')
                    ++i;
                else if (s[i] == '"')
                    return i + 1;
            }
            return std::string::npos;
        }
        size_t skipValue(const std::string &s, size_t i)
        {
            int depth = 0;
            for (; i < s.size(); ++i)
            {
                const char c = s[i];
                if (c == '"')
                {
                    i = skipString(s, i);
                    if (i == std::string::npos)
                        return i;
                    --i;
                }
                else if (c == '{' || c == '[')
                    ++depth;
                else if (c == '}' || c == ']')
                {
                    if (depth == 0)
                        return i;
                    --depth;
                }
                else if (c == ',' && depth == 0)
                    return i;
            }
            return depth == 0 ? i : std::string::npos;
        }
        bool parseObject(const std::string &s, Members &out)
        {
            size_t i = skipWs(s, 0);
            if (i >= s.size() || s[i] != '{')
                return false;
            i = skipWs(s, i + 1);
            if (i < s.size() && s[i] == '}')
                return true;
            while (i < s.size())
            {
                if (s[i] != '"')
                    return false;
                const size_t kEnd = skipString(s, i);
                if (kEnd == std::string::npos)
                    return false;
                std::string key = s.substr(i + 1, kEnd - i - 2);
                i = skipWs(s, kEnd);
                if (i >= s.size() || s[i] != ':')
                    return false;
                i = skipWs(s, i + 1);
                const size_t vEnd = skipValue(s, i);
                if (vEnd == std::string::npos || vEnd >= s.size())
                    return false;
                std::string raw = s.substr(i, vEnd - i);
                while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back())))
                    raw.pop_back();
                if (raw.empty())
                    return false;
                out.emplace_back(std::move(key), std::move(raw));
                i = skipWs(s, vEnd);
                if (i < s.size() && s[i] == '}')
                    return true;
                if (i >= s.size() || s[i] != ',')
                    return false;
                i = skipWs(s, i + 1);
            }
            return false;
        }
        std::string unquote(const std::string &raw)
        {
            if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
                return raw.substr(1, raw.size() - 2);
            return raw;
        }

        bool readGeneral(Members &m, bool &exists)
        {
            std::ifstream in(configDir() / "general.json");
            exists = bool(in);
            if (!in)
                return true;
            std::stringstream ss;
            ss << in.rdbuf();
            return parseObject(ss.str(), m);
        }

        void writeGeneral()
        {
            std::lock_guard<std::mutex> lock(g_fileMutex);
            Members m;
            bool exists = false;
            std::error_code ec;
            const auto dir = configDir();
            const auto path = dir / "general.json";
            if (!readGeneral(m, exists))
            {
                // Never clobber a file we cannot parse (it also holds the disc path): the choice still
                // applies to this session.
                std::fprintf(stderr, "[starter] %s not understood; choice not saved\n", path.string().c_str());
                return;
            }
            const int body = g_body.load();
            const int64_t paint = g_paint.load();
            char paintText[32];
            std::snprintf(paintText, sizeof(paintText), "\"0x%06llx\"", static_cast<unsigned long long>(paint));
            const std::pair<const char *, std::string> ours[2] = {
                {"starter_body", body == kRandom ? std::string("\"random\"") : std::to_string(body)},
                {"starter_paint", paint == kDefaultPaint ? std::string("\"default\"") : std::string(paintText)},
            };
            for (const auto &[key, value] : ours)
            {
                bool found = false;
                for (auto &kv : m)
                    if (kv.first == key)
                        kv.second = value, found = true;
                if (!found)
                    m.emplace_back(key, value);
            }
            std::filesystem::create_directories(dir, ec);
            const auto tmp = dir / "general.json.tmp";
            {
                std::ofstream out(tmp, std::ios::trunc);
                if (!out)
                    return;
                out << "{\n";
                for (size_t i = 0; i < m.size(); ++i)
                    out << "  \"" << m[i].first << "\": " << m[i].second << (i + 1 < m.size() ? ",\n" : "\n");
                out << "}\n";
            }
            std::filesystem::rename(tmp, path, ec);
        }

        void loadSettings()
        {
            int body = kRandom;
            int64_t paint = kDefaultPaint;
            const char *eb = std::getenv("PS2X_STARTER_BODY");
            const char *ep = std::getenv("PS2X_STARTER_PAINT");
            const bool interactive = !envOn("PS2X_HEADLESS") && !envOn("PS2X_DETERMINISTIC");
            if (interactive && (!eb || !ep))
            {
                Members m;
                bool exists = false;
                if (readGeneral(m, exists))
                {
                    for (const auto &[key, raw] : m)
                    {
                        if (key == "starter_body" && !eb && !parseBody(unquote(raw), body))
                            body = kRandom;
                        if (key == "starter_paint" && !ep && !parsePaint(unquote(raw), paint))
                            paint = kDefaultPaint;
                    }
                }
                else
                    std::fprintf(stderr, "[starter] general.json unreadable; using the original random car\n");
            }
            if (eb && !parseBody(eb, body))
            {
                std::fprintf(stderr, "[starter] PS2X_STARTER_BODY=%s invalid (0..%d or random); random\n", eb, kBodyCount - 1);
                body = kRandom;
            }
            if (ep && !parsePaint(ep, paint))
            {
                std::fprintf(stderr, "[starter] PS2X_STARTER_PAINT=%s invalid (hex or default); default\n", ep);
                paint = kDefaultPaint;
            }
            g_body.store(body);
            g_paint.store(paint);
        }

        std::once_flag g_loadOnce;
        void ensureLoaded()
        {
            std::call_once(g_loadOnce, loadSettings);
        }

        int pickerCase(uint32_t ra)
        {
            for (int i = 0; i < 6; ++i)
                if (ra == kPickerReturns[i])
                    return i;
            return -1;
        }

        void onEnter(hooks::HookCall &call)
        {
            const int c = pickerCase(call.ra);
            if (c < 0)
                return;
            const int body = g_body.load();
            const uint32_t original = hooks::reg32(call.ctx, 4);
            if (body != kRandom)
                hooks::setReg32(call.ctx, 4, uint32_t(body));
            std::fprintf(stderr, "[starter] new Adventure: original body %u (case %d) -> %u\n", original, c,
                         body == kRandom ? original : uint32_t(body));
        }

        void onExit(hooks::HookCall &call)
        {
            if (pickerCase(call.ra) < 0)
                return;
            const int64_t paint = g_paint.load();
            if (paint == kDefaultPaint)
                return;
            const uint32_t body = call.a[0];
            if (body >= uint32_t(kBodyCount))
                return;
            const uint32_t before = hooks::read32(call.rdram, kSaveSlot);
            const uint32_t addrs[2] = {kSaveSlot + 0x00u, kSaveSlot + kBodyPaintTable + body * 4u};
            for (uint32_t a : addrs)
            {
                const uint32_t old = hooks::read32(call.rdram, a); // keep the wheel byte (top 8 bits)
                hooks::write32(call.rdram, a, (old & 0xff000000u) | (uint32_t(paint) & 0x00ffffffu));
            }
            std::fprintf(stderr, "[starter] paint word %08x -> %08x\n", before, hooks::read32(call.rdram, kSaveSlot));
        }
    }

    int optionCount() { return 7; }
    int optionBody(int index) { return index <= 0 || index > 6 ? kRandom : kStarterBodies[index - 1]; }
    std::string optionLabel(int index)
    {
        const int body = optionBody(index);
        if (body == kRandom)
            return "Random (original)";
        if (body == 43)
            return "Toyota Caldina (body 43)";
        return "Body #" + std::to_string(body);
    }

    int paintOptionCount() { return int(sizeof(kPaints) / sizeof(kPaints[0])); }
    int64_t paintOptionWord(int index) { return index < 0 || index >= paintOptionCount() ? kDefaultPaint : kPaints[index].word; }
    const char *paintOptionLabel(int index) { return index < 0 || index >= paintOptionCount() ? "" : kPaints[index].label; }

    int selectedBody() { return ensureLoaded(), g_body.load(); }
    int64_t selectedPaint() { return ensureLoaded(), g_paint.load(); }

    void selectBody(int body)
    {
        ensureLoaded();
        g_body.store(body >= 0 && body < kBodyCount ? body : kRandom);
        writeGeneral();
        std::fprintf(stderr, "[starter] starting car for the next new Adventure: %s\n",
                     body == kRandom ? "random (original)" : ("body " + std::to_string(body)).c_str());
    }

    void selectPaint(int64_t paintWord)
    {
        ensureLoaded();
        g_paint.store(paintWord < 0 ? kDefaultPaint : (paintWord & 0x00ffffff));
        writeGeneral();
    }

    void installHooks()
    {
        ensureLoaded();
        const bool menu = ps2x::shell::sdl3Window(); // the menu may set a choice later in this session
        if (g_body.load() == kRandom && g_paint.load() == kDefaultPaint && !menu)
            return; // option off: no hook, original code path untouched
        hooks::add({kSetStarterBody, &onEnter, &onExit, nullptr, "starter-car"});
        std::fprintf(stderr, "[starter] body=%d paint=%lld\n", g_body.load(), static_cast<long long>(g_paint.load()));
    }
}
