#include "snes_text_xlate.h"

#include "snes/snes.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

extern "C" Snes* g_snes;
extern "C" Ppu* g_ppu;

namespace fs = std::filesystem;

namespace {

struct Patch {
    std::string kind;
    uint32_t address = 0;
    std::vector<uint8_t> source;
    std::map<std::string, std::vector<uint8_t>> target;
    std::map<std::string, std::string> text;
    /* Optional explicit guard for vram_patches: when set, the patch fires
     * iff the bytes at guard_address equal guard, replacing the implicit
     * current==source-or-target content check at `address`. This is what
     * makes per-screen asset paging possible: several patches can own the
     * same payload region (e.g. a shared glyph page) and be selected by a
     * screen-unique region elsewhere (e.g. one dialogue quote's tilemap
     * row), which content guards at the payload address cannot express. */
    uint32_t guard_address = 0;
    std::vector<uint8_t> guard;
};

struct Glyph {
    std::string utf8;
    std::vector<uint8_t> bytes;
};

struct State {
    std::string table_path;
    std::string default_lang = "en";
    std::string language = "off";
    std::map<std::string, std::string> language_fallbacks;
    std::string error;
    std::vector<Glyph> glyphs;
    std::vector<Patch> rom_patches;
    std::vector<Patch> ram_patches;
    std::vector<Patch> vram_patches;
    bool initialized = false;
    bool rom_dirty = true;
    bool vram_dirty = true;
    uint64_t ram_applies = 0;
    uint64_t rom_applies = 0;
    uint64_t vram_applies = 0;
};

State& state() {
    static State value;
    return value;
}

std::string trim(std::string value) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string strip_comment(const std::string& line) {
    bool quoted = false;
    bool escape = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (quoted && c == '\\') {
            escape = true;
            continue;
        }
        if (c == '"') quoted = !quoted;
        if (c == '#' && !quoted) return line.substr(0, i);
    }
    return line;
}

bool parse_string(const std::string& text, std::string& out) {
    const std::string value = trim(text);
    if (value.size() < 2 || value.front() != '"' || value.back() != '"')
        return false;
    out.clear();
    for (size_t i = 1; i + 1 < value.size(); ++i) {
        char c = value[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (++i + 1 >= value.size()) return false;
        switch (value[i]) {
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: return false;
        }
    }
    return true;
}

bool parse_u32(const std::string& text, uint32_t& out) {
    const std::string value = trim(text);
    if (value.empty()) return false;
    char* end = nullptr;
    const int base = value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0
        ? 16 : 10;
    errno = 0;
    unsigned long parsed = std::strtoul(value.c_str(), &end, base);
    if (errno || !end || *end != '\0' || parsed > 0xfffffffful)
        return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool hex_nibble(char c, uint8_t& out) {
    if (c >= '0' && c <= '9') {
        out = static_cast<uint8_t>(c - '0');
        return true;
    }
    if (c >= 'a' && c <= 'f') {
        out = static_cast<uint8_t>(10 + c - 'a');
        return true;
    }
    if (c >= 'A' && c <= 'F') {
        out = static_cast<uint8_t>(10 + c - 'A');
        return true;
    }
    return false;
}

bool parse_hex(const std::string& text, std::vector<uint8_t>& out) {
    std::string value;
    if (!parse_string(text, value)) return false;
    value.erase(std::remove_if(value.begin(), value.end(),
                               [](unsigned char c) {
                                   return std::isspace(c) || c == '_' ||
                                          c == '-' || c == ':';
                               }),
                value.end());
    if (value.size() % 2) return false;
    out.clear();
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        uint8_t hi = 0;
        uint8_t lo = 0;
        if (!hex_nibble(value[i], hi) || !hex_nibble(value[i + 1], lo))
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

std::vector<std::string> utf8_units(const std::string& text) {
    std::vector<std::string> result;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        if ((c & 0xe0) == 0xc0) len = 2;
        else if ((c & 0xf0) == 0xe0) len = 3;
        else if ((c & 0xf8) == 0xf0) len = 4;
        if (i + len > text.size()) len = 1;
        result.emplace_back(text.substr(i, len));
        i += len;
    }
    return result;
}

bool encode_text(const std::string& text, std::vector<uint8_t>& out,
                 std::string* error) {
    out.clear();
    for (const std::string& unit : utf8_units(text)) {
        const auto glyph = std::find_if(
            state().glyphs.begin(), state().glyphs.end(),
            [&](const Glyph& item) { return item.utf8 == unit; });
        if (glyph != state().glyphs.end()) {
            out.insert(out.end(), glyph->bytes.begin(), glyph->bytes.end());
            continue;
        }
        if (unit.size() == 1 && (unsigned char)unit[0] < 0x80) {
            out.push_back(static_cast<uint8_t>(unit[0]));
            continue;
        }
        if (error) *error = "missing glyph mapping for '" + unit + "'";
        return false;
    }
    return true;
}

std::vector<std::string> language_chain(const std::string& requested) {
    std::vector<std::string> chain;
    std::string current = requested.empty() ? state().default_lang : requested;
    for (int depth = 0; depth < 8; ++depth) {
        if (current.empty() || current == "off")
            break;
        if (std::find(chain.begin(), chain.end(), current) != chain.end())
            break;
        chain.push_back(current);
        const auto fallback = state().language_fallbacks.find(current);
        if (fallback == state().language_fallbacks.end())
            break;
        current = fallback->second;
    }
    return chain;
}

std::string effective_language(const std::string& requested) {
    const std::vector<std::string> chain = language_chain(requested);
    if (chain.empty())
        return requested.empty() ? state().default_lang : requested;
    return chain.back();
}

std::string json_escape(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

/* Returns the payload the language chain natively supplies for this patch,
 * or nullptr when the chain has none. `scratch` backs text-encoded payloads.
 */
const std::vector<uint8_t>* patch_native_target(const Patch& patch,
                                                const std::string& language,
                                                std::vector<uint8_t>& scratch) {
    if (language.empty() || language == "off")
        return nullptr;
    for (const std::string& candidate : language_chain(language)) {
        const auto hex = patch.target.find(candidate);
        if (hex != patch.target.end())
            return &hex->second;
        const auto text = patch.text.find(candidate);
        if (text == patch.text.end())
            continue;
        std::string error;
        if (!encode_text(text->second, scratch, &error)) {
            state().error = error;
            continue;
        }
        return &scratch;
    }
    return nullptr;
}

std::vector<uint8_t> patch_target(const Patch& patch,
                                  const std::string& language) {
    std::vector<uint8_t> scratch;
    const std::vector<uint8_t>* native =
        patch_native_target(patch, language, scratch);
    return native ? *native : patch.source;
}

bool patch_matches_any_target(const Patch& patch, const uint8_t* data,
                              size_t size) {
    if (size == patch.source.size() &&
        std::equal(patch.source.begin(), patch.source.end(), data))
        return true;
    for (const auto& [lang, bytes] : patch.target) {
        (void)lang;
        if (size == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), data))
            return true;
    }
    for (const auto& [lang, text] : patch.text) {
        (void)lang;
        std::vector<uint8_t> bytes;
        std::string error;
        if (!encode_text(text, bytes, &error)) continue;
        if (size == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), data))
            return true;
    }
    return false;
}

uint8_t vram_read_byte(uint32_t byte_addr) {
    const uint16_t word = g_ppu->vram[(byte_addr >> 1) & 0x7fff];
    return (byte_addr & 1) ? static_cast<uint8_t>(word >> 8)
                           : static_cast<uint8_t>(word & 0xff);
}

void vram_write_byte(uint32_t byte_addr, uint8_t value) {
    uint16_t& word = g_ppu->vram[(byte_addr >> 1) & 0x7fff];
    if (byte_addr & 1)
        word = static_cast<uint16_t>((word & 0x00ffu) |
                                     (static_cast<uint16_t>(value) << 8));
    else
        word = static_cast<uint16_t>((word & 0xff00u) | value);
}

void apply_ram_patch(const Patch& patch) {
    if (!g_snes || !g_snes->ram || patch.source.empty())
        return;
    uint32_t off = patch.address;
    if (off >= 0x7e0000u && off < 0x800000u)
        off &= 0x1ffffu;
    if (off + patch.source.size() > 0x20000u)
        return;
    /* RAM is live game state: only write payloads the language chain
     * natively supplies. Falling back to `source` here would restore
     * bytes into regions the game may have deliberately rewritten. */
    std::vector<uint8_t> scratch;
    const std::vector<uint8_t>* target =
        patch_native_target(patch, state().language.empty()
                            ? state().default_lang : state().language,
                            scratch);
    if (!target || target->size() != patch.source.size())
        return;
    uint8_t* base = g_snes->ram + off;
    if (!patch_matches_any_target(patch, base, patch.source.size()))
        return;
    if (!std::equal(target->begin(), target->end(), base)) {
        std::copy(target->begin(), target->end(), base);
        state().ram_applies++;
    }
}

void apply_rom_patch(const Patch& patch) {
    if (!g_snes || !g_snes->cart || !g_snes->cart->rom ||
        patch.source.empty())
        return;
    if (patch.address + patch.source.size() > g_snes->cart->romSize)
        return;
    const std::vector<uint8_t> target =
        patch_target(patch, state().language.empty()
                     ? state().default_lang : state().language);
    if (target.size() != patch.source.size())
        return;
    uint8_t* base = g_snes->cart->rom + patch.address;
    if (!patch_matches_any_target(patch, base, patch.source.size()))
        return;
    if (!std::equal(target.begin(), target.end(), base)) {
        std::copy(target.begin(), target.end(), base);
        state().rom_applies++;
    }
}

void apply_vram_patch(const Patch& patch) {
    if (!g_ppu || patch.source.empty())
        return;
    if (patch.address + patch.source.size() > 0x10000u)
        return;
    /* VRAM is live game state: only write payloads the language chain
     * natively supplies. Falling back to `source` would resurrect bytes
     * into regions the game has since cleared or repurposed (e.g. an
     * all-zero CJK blanking target matches the game's own cleared VRAM,
     * and writing `source` back would stamp title tilemap fragments over
     * an unrelated scene). */
    std::vector<uint8_t> scratch;
    const std::vector<uint8_t>* target =
        patch_native_target(patch, state().language.empty()
                            ? state().default_lang : state().language,
                            scratch);
    if (!target || target->size() != patch.source.size())
        return;
    std::vector<uint8_t> current(patch.source.size());
    for (size_t i = 0; i < current.size(); ++i)
        current[i] = vram_read_byte(patch.address + static_cast<uint32_t>(i));
    if (!patch.guard.empty()) {
        /* Explicit guard: fire iff the bytes at guard_address match. This
         * REPLACES the content check at `address`, because paged payload
         * regions legitimately hold another patch's page at apply time. */
        if (patch.guard_address + patch.guard.size() > 0x10000u)
            return;
        for (size_t i = 0; i < patch.guard.size(); ++i) {
            if (vram_read_byte(patch.guard_address +
                               static_cast<uint32_t>(i)) != patch.guard[i])
                return;
        }
    } else if (!patch_matches_any_target(patch, current.data(),
                                         current.size())) {
        return;
    }
    if (std::equal(target->begin(), target->end(), current.begin()))
        return;
    for (size_t i = 0; i < target->size(); ++i)
        vram_write_byte(patch.address + static_cast<uint32_t>(i),
                        (*target)[i]);
    state().vram_applies++;
}

Patch* current_patch(const std::string& section) {
    if (section == "rom_patch") {
        if (state().rom_patches.empty()) return nullptr;
        return &state().rom_patches.back();
    }
    if (section == "entry" || section == "ram_patch" ||
        section == "glyph_label") {
        if (state().ram_patches.empty()) return nullptr;
        return &state().ram_patches.back();
    }
    if (section == "vram_patch") {
        if (state().vram_patches.empty()) return nullptr;
        return &state().vram_patches.back();
    }
    return nullptr;
}

bool parse_table(const fs::path& path, std::string* error) {
    std::ifstream file(path);
    if (!file) {
        if (error) *error = "cannot open translation table: " + path.string();
        return false;
    }

    std::string section;
    std::string raw;
    size_t line_number = 0;
    while (std::getline(file, raw)) {
        ++line_number;
        const std::string line = trim(strip_comment(raw));
        if (line.empty())
            continue;
        if (line.rfind("[[", 0) == 0 &&
            line.size() >= 4 &&
            line.substr(line.size() - 2) == "]]") {
            section = trim(line.substr(2, line.size() - 4));
            if (section == "glyph") {
                state().glyphs.emplace_back();
            } else if (section == "rom_patch") {
                state().rom_patches.emplace_back();
                state().rom_patches.back().kind = section;
            } else if (section == "entry" || section == "ram_patch" ||
                       section == "glyph_label") {
                state().ram_patches.emplace_back();
                state().ram_patches.back().kind = section;
            } else if (section == "vram_patch") {
                state().vram_patches.emplace_back();
                state().vram_patches.back().kind = section;
            }
            continue;
        }

        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            if (error) *error = "invalid translation line " +
                                std::to_string(line_number);
            return false;
        }
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        std::string string_value;
        uint32_t int_value = 0;
        std::vector<uint8_t> bytes;

        if (section.empty()) {
            if (key == "default_lang" && parse_string(value, string_value)) {
                state().default_lang = string_value;
            } else if (key.rfind("fallback_", 0) == 0 &&
                       parse_string(value, string_value)) {
                state().language_fallbacks[key.substr(9)] = string_value;
            } else if (key.size() > 9 &&
                       key.substr(key.size() - 9) == "_fallback" &&
                       parse_string(value, string_value)) {
                state().language_fallbacks[key.substr(0, key.size() - 9)] =
                    string_value;
            } else if (key == "schema") {
                continue;
            }
            continue;
        }
        if (section == "glyph") {
            if (state().glyphs.empty()) continue;
            Glyph& glyph = state().glyphs.back();
            if ((key == "char" || key == "utf8") &&
                parse_string(value, string_value)) {
                glyph.utf8 = string_value;
            } else if ((key == "hex" || key == "bytes_hex") &&
                       parse_hex(value, bytes)) {
                glyph.bytes = bytes;
            }
            continue;
        }

        Patch* patch = current_patch(section);
        if (!patch) continue;
        if ((key == "address" || key == "addr") &&
            parse_u32(value, int_value)) {
            patch->address = int_value;
        } else if ((key == "source_hex" || key == "src_hex") &&
                   parse_hex(value, bytes)) {
            patch->source = bytes;
        } else if (key == "guard_address" && parse_u32(value, int_value)) {
            patch->guard_address = int_value;
        } else if (key == "guard_hex" && parse_hex(value, bytes)) {
            patch->guard = bytes;
        } else if (key.size() > 4 &&
                   key.substr(key.size() - 4) == "_hex" &&
                   parse_hex(value, bytes)) {
            patch->target[key.substr(0, key.size() - 4)] = bytes;
        } else if (parse_string(value, string_value)) {
            patch->text[key] = string_value;
        }
    }
    state().glyphs.erase(
        std::remove_if(state().glyphs.begin(), state().glyphs.end(),
                       [](const Glyph& glyph) {
                           return glyph.utf8.empty() || glyph.bytes.empty();
                       }),
        state().glyphs.end());
    return true;
}

}  // namespace

extern "C" int snes_text_xlate_init_c(const char* table_path,
                                       const char* language) {
    State fresh;
    state() = std::move(fresh);
    if (!table_path || !table_path[0]) {
        state().error = "missing translation table path";
        return 0;
    }
    state().table_path = table_path;
    state().language = language && language[0] ? language : "off";
    if (!parse_table(fs::path(table_path), &state().error))
        return 0;
    state().initialized = true;
    state().vram_dirty = true;
    return 1;
}

extern "C" void snes_text_xlate_set_language_c(const char* language) {
    state().language = language && language[0] ? language : "off";
    state().rom_dirty = true;
    state().vram_dirty = true;
}

extern "C" void snes_text_xlate_shutdown_c(void) {
    State fresh;
    state() = std::move(fresh);
}

extern "C" void snes_text_xlate_on_frame_c(void) {
    if (!state().initialized)
        return;
    if (state().rom_dirty) {
        for (const Patch& patch : state().rom_patches)
            apply_rom_patch(patch);
        state().rom_dirty = false;
    }
    for (const Patch& patch : state().ram_patches)
        apply_ram_patch(patch);
    if (state().vram_dirty) {
        for (const Patch& patch : state().vram_patches)
            apply_vram_patch(patch);
        state().vram_dirty = false;
    }
}

extern "C" void snes_text_xlate_on_vram_write_c(uint16_t) {
    if (state().initialized)
        state().vram_dirty = true;
}

extern "C" const char* snes_text_xlate_last_error_c(void) {
    return state().error.c_str();
}

extern "C" int snes_text_xlate_debug_json_c(const char* subcmd,
                                             char* out, int cap) {
    if (!out || cap <= 0)
        return 0;
    const char* cmd = subcmd ? subcmd : "stats";
    if (std::string(cmd) != "stats") {
        return std::snprintf(out, static_cast<size_t>(cap),
            "{\"ok\":false,\"error\":\"unsupported xlate command\"}");
    }
    const std::string language = json_escape(state().language);
    const std::string effective = json_escape(effective_language(state().language));
    const std::string table_path = json_escape(state().table_path);
    return std::snprintf(out, static_cast<size_t>(cap),
        "{\"ok\":true,\"language\":\"%s\",\"effective_language\":\"%s\","
        "\"table\":\"%s\","
        "\"rom_patches\":%zu,\"ram_patches\":%zu,\"vram_patches\":%zu,"
        "\"glyphs\":%zu,\"language_fallbacks\":%zu,"
        "\"rom_applies\":%llu,\"ram_applies\":%llu,\"vram_applies\":%llu}",
        language.c_str(), effective.c_str(), table_path.c_str(),
        state().rom_patches.size(), state().ram_patches.size(),
        state().vram_patches.size(),
        state().glyphs.size(), state().language_fallbacks.size(),
        static_cast<unsigned long long>(state().rom_applies),
        static_cast<unsigned long long>(state().ram_applies),
        static_cast<unsigned long long>(state().vram_applies));
}
