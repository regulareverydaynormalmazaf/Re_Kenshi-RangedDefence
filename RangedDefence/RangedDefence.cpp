// ============================================================================
//  Copyright (C) 2026 regulareverydaynormalmazaf
//  SPDX-License-Identifier: GPL-3.0-or-later
// ============================================================================
// ============================================================================
//  RangedDefence v12 GOG  —  KenshiLib plugin (Kenshi 1.0.65 Steam).
//
//  Даёт любому персонажу шанс защититься при попадании из ДАЛЬНОБОЙНОГО оружия
//  (турель/арбалет) — уклонением или блоком стрелы, штатными формулами игры
//  против навыка стрелка (не 100%). Настраивается в Mod Hub (Emkejs-Mod-Core) и
//  в mod-config.json.
//
//  ЗАЩИТА ПО КАТЕГОРИЯМ (v11). Для каждой из 9 категорий оружия — свой выбор поведения:
//    6 ближних (Катаны, Сабли, Дробящее, Тяжёлое, Рубящее, Древковое) + Без оружия + Дальнобойное + Турель.
//  У каждой категории: тумблер вкл/выкл (выкл = не защищается) и способ (cat_method):
//    Уклон+Блок (уклон, при неудаче — блок) / Только уклон / Только блок / Сила/Ловкость (STR>DEX -> блок).
//  ВЕС: глобальная галка «Применять вес к категориям» + порог веса у каждой из 6 ближних категорий.
//  Когда включена: оружие категории легче своего порога только УКЛОНЯЕТСЯ (без блока). Для «Без оружия»/
//  «Дальнобойное»/«Турель» вес не применяется. (Старые selection_mode / набор блок-категорий /
//  use_weapon_weight / combine / unarmed_can_block / turret_mode / «блок после уклона» заменены этой схемой.)
//
//  ТОЧКА ПЕРЕХВАТА: Character::iShotYou (единственный путь дальнобойного попадания;
//  и турель, и арбалет). При успешной защите оригинал не вызывается -> урона нет.
//
//  БЕЗОПАСНОСТЬ: хук по публичному символу; getCombatWeight/getCategory/
//  _calculateBlockChance зовём по Steam-RVA только при прохождении RVA-канарейки.
//  Mod Hub-регистрация — через публичный экспорт EMC_ModHub_GetApi (без хардкода
//  адресов хаба). Если хаб не найден — мод работает, настройки только из файла.
// ============================================================================
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <core/Functions.h>          // KenshiLib::AddHook / GetRealAddress / SUCCESS
#include <kenshi/Character.h>        // Character: iShotYou, getStats, getCurrentWeapon, isDown/isDead/getProneState
#include <kenshi/CharStats.h>        // CharStats: getStat, strengthActual, calculateDodgeChance, xp*
#include <kenshi/Enums.h>            // StatsEnumerated, ProneState
#include <kenshi/Kenshi.h>           // KenshiLib::GetKenshiVersion / BinaryVersion (Steam vs GOG, version)

#include "mod_hub_api.h"             // Emkejs Mod Hub SDK (types + EMC_ModHub_GetApi export)

#include <windows.h>
#include <tlhelp32.h>                // module enumeration (robust Mod Hub export resolution)
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

class Weapon;         // returned by getCurrentWeapon() (fwd-declared in Character.h; repeated defensively)
class Harpoon;        // projectile — only passed through
class CombatClass;    // Character::getCombatClass()
class AnimationClass; // Character::getAnimationClass()

// ----------------------------------------------------------------------------
//  Persistent state (mirrors mod-config.json AND the Mod Hub rows)
// ----------------------------------------------------------------------------
// Values kept stable so old configs don't shift. 1 (ByWeapon) and 3 (Independent) were removed from the
// UI/logic; a saved config with those values falls through to the default ("По оружию").
enum SelMode { SEL_HYBRID = 0, SEL_BY_STAT = 2, SEL_DODGE_ONLY = 4, SEL_BLOCK_ONLY = 5 };
enum { WC_UNARMED = 5, WC_BOW = 6, WC_TURRET = 7 };   // WeaponCategory ints treated as "no melee weapon"
// Player melee weapon categories (fixed engine WeaponCategory ids).
enum { WC_KATANAS = 0, WC_SABRES = 1, WC_BLUNT = 2, WC_HEAVY = 3, WC_HACKERS = 4, WC_POLEARMS = 8 };
// Per-category defence: 9 categories = 6 melee + no-weapon + ranged-in-hand + turret.
enum { CI_KATANAS=0, CI_SABRES=1, CI_BLUNT=2, CI_HEAVY=3, CI_HACKERS=4, CI_POLEARMS=5,
       CI_UNARMED=6, CI_RANGED=7, CI_TURRET=8 };
enum { RD_NCAT = 9, RD_NMELEE = 6 };   // total categories / of which are melee (have a weight threshold)
// Defence method per category (dropdown order). There is no "nothing" option: the per-category on/off
// switch disables a category. STAT = STR>DEX -> block, else dodge. DODGEBLOCK = dodge, then block on fail.
enum CatMethod { CD_DODGEBLOCK = 0, CD_DODGE = 1, CD_BLOCK = 2, CD_STAT = 3, CD_NONE = 4, CD_CUSTOM = 5 };
// CD_CUSTOM is used ONLY by the "For all categories" master dropdown (never stored in a per-category slot):
// Custom = each category keeps its own method; any other value is mass-applied to all categories.
// Map an engine WeaponCategory id to a melee category index (0..5), or -1 if not a player melee category.
static inline int catIndexOf(int cat) {
    switch (cat) {
        case WC_KATANAS:  return CI_KATANAS;  case WC_SABRES:   return CI_SABRES;
        case WC_BLUNT:    return CI_BLUNT;    case WC_HEAVY:    return CI_HEAVY;
        case WC_HACKERS:  return CI_HACKERS;  case WC_POLEARMS: return CI_POLEARMS;
        default: return -1;
    }
}

struct State {
    int32_t enabled;              // master on/off
    float   chance_multiplier;   // scale evasion chance (0..3)
    int32_t xp_enabled;          // grant dodge/block XP on attempt
    int32_t dodge_lock_ms;       // COMMITMENT (dodge): one defence per this window (ms); other arrows HIT
    int32_t block_lock_ms;       // COMMITMENT (block): usually longer than a dodge (stagger)
    int32_t skip_while_attacking;// don't defend while mid-attack (protects own offence; realistic)
    int32_t defend_while_in_melee;// 1 = defend ranged even during a melee fight; 0 = ignore ranged while in melee
    int32_t play_animations;     // EXPERIMENTAL: play the native dodge/block animation on OUR ranged defence
    int32_t debug_logging;       // verbose continuous diagnostics to RangedDefence_log.txt (temporary; OFF by default)
    int32_t lock_on_success;     // 0 = window starts on any attempt (default); 1 = only after a SUCCESSFUL defence
    int32_t cooldown_enabled;    // 1 = commitment window active (one reaction per window; lock_on_success + times apply);
                                 // 0 = NO window at all -> character may defend against every arrow (times/lock_on_success ignored)
    // ---- Per-category defence (replaces selection_mode / block set / weight / unarmed / turret / fallback) ----
    int32_t cat_method[RD_NCAT]; // per-category CatMethod (DODGEBLOCK / DODGE / BLOCK / STAT / NONE=off)
    int32_t cat_all_method;      // "For all" master dropdown (CD_CUSTOM by default). Not persisted; a UI helper
                                 // that mass-writes cat_method[] when set to a concrete method.
    int32_t apply_weight;        // 1 = apply per-category weight thresholds (below threshold -> dodge only)
    int32_t cat_weight[RD_NMELEE];// per-melee-category combat-weight threshold (0..50); non-melee ignore it
};
// dodge_lock_ms / block_lock_ms are in GAME milliseconds (scale with game speed; pause on pause).
// Defaults reproduce the previous behaviour: heavy weapons block, other melee dodge-then-block,
// no-weapon/ranged dodge, turret off. Category order: Katanas,Sabres,Blunt,Heavy,Hackers,Polearms,
// No-weapon, Ranged, Turret. Weight thresholds default to 0 (no filtering) and apply_weight is off.
static State g_state = {
    1,            // enabled
    1.0f,         // chance_multiplier
    1,            // xp_enabled
    500, 900,     // dodge_lock_ms, block_lock_ms
    1, 1,         // skip_while_attacking, defend_while_in_melee
    0, 0,         // play_animations, debug_logging
    0,            // lock_on_success
    1,            // cooldown_enabled
    { CD_DODGEBLOCK, CD_DODGEBLOCK, CD_DODGEBLOCK, CD_BLOCK, CD_DODGEBLOCK, CD_DODGEBLOCK,
      CD_DODGE, CD_DODGE, CD_NONE }, // cat_method: Heavy=block, other melee=dodge+block, no-weapon/ranged=dodge, turret=off(none)
    CD_CUSTOM,    // cat_all_method: "For all" master = Custom (categories use their own values)
    0,            // apply_weight
    { 0, 0, 0, 0, 0, 0 }             // cat_weight (per melee category); 0 = no weight filter
};

// ----------------------------------------------------------------------------
//  Localization (Mod Hub labels/descriptions). Language is chosen from the game:
//  Kenshi's settings.cfg "language=" (same value RE_Kenshi reads) — or forced via
//  the mod-config "language" key ("auto"/"en"/"ru"). Only EN + RU today.
// ----------------------------------------------------------------------------
enum Lang { LANG_EN = 0, LANG_RU = 1 };
static int         g_lang = LANG_EN;        // resolved UI language
static std::string g_langPref = "auto";     // config override: "auto" | "en" | "ru"
// Pick a string for the current language. Both operands are static string literals,
// so the returned pointer is valid for the process lifetime (safe for the hub to keep).
static inline const char* tr(const char* en, const char* ru) { return (g_lang == LANG_RU) ? ru : en; }

// per-character commitment: GAME-ms time until which this character is "committed" (can't defend again).
// Keys never dereferenced, so a reused/freed Character* is harmless (entry expires shortly).
static std::unordered_map<Character*, double> g_lockUntil;
static double      g_gameMs = 0.0;              // accumulated GAME time (ms) from the frame hook
static bool        g_useGameClock = false;      // false -> fall back to real-time (frame hook not installed)
static std::string g_dodgeAnim = "dodgeback";  // animation names (config-file only)
static std::string g_blockAnim = "";           // empty = don't play a block animation (needs a validated name)
static const size_t OFF_isAttacking = 0x140;   // CombatClass::_isAttacking (float) — >0 means attacking

// Mod Hub attach state (retry until the hub is found — load-order independent). All touched only
// on the main game thread (startPlugin / frame hook / iShotYou), so a plain int is safe.
static int g_hubState    = 0;    // 0 = not attached, 1 = registered OK, -1 = gave up
static int g_hubTries    = 0;    // attach attempts made so far
static int g_hubFrameAcc = 0;    // frame throttle accumulator
static const int HUB_THROTTLE_FRAMES = 20;    // attempt at most once per N frames
static const int HUB_MAX_TRIES       = 400;   // ~400 * 20 frames ≈ well over a minute of retrying
static void HubTick();                        // defined in the Mod Hub section (retry driver)

static const int CALIB_LOG_LINES = 300;

// ----------------------------------------------------------------------------
//  Image RVA helpers. The build is detected in startPlugin() by the actual RVA of
//  CharStats::calculateDodgeChance (Steam 1.0.65 = 0x884EB0, GOG 1.0.65 = 0x8845D0);
//  the matching per-build RVA set (SET_STEAM / SET_GOG) is then selected. Values below are the
//  Steam 1.0.65 reference (GOG counterparts live in SET_GOG next to the detection code).
//    CharStats::calculateDodgeChance  0x884EB0  (public — build-detect reference)
//    CharStats::_calculateBlockChance 0x885400  (protected)
//    CharStats::strengthActual        0x338050  (public; called directly)
//    Weapon::getCombatWeight          0x882890
//    Weapon::getCategory              0x5C6EC0  (-> WeaponCategory int)
// ----------------------------------------------------------------------------
static uintptr_t rd_rva(size_t rva) { static uintptr_t base = (uintptr_t)GetModuleHandleA(NULL); return base + rva; }
typedef float (*blockChanceFn)(void*, float);
typedef float (*combatWeightFn)(void*);
typedef int   (*categoryFn)(void*);
typedef bool  (*handIsNullFn)(void*);
typedef bool  (*stumblingFn)(void*);
typedef int   (*numOppFn)(void*);
typedef void  (*playActionFn)(void*, const std::string&, float, float, bool);
typedef float (*estAnimFn)(void*, const std::string&);
static blockChanceFn  _blockChance  = nullptr;
static combatWeightFn _combatWeight = nullptr;
static categoryFn     _category     = nullptr;
static handIsNullFn   _handIsNull   = nullptr;
static stumblingFn    _stumbling    = nullptr;   // AnimationClass::currentlyStumbling (0x334F30)
static numOppFn       _numOpponents = nullptr;   // CombatClass::getNumOpponents (0x2B3000)
static playActionFn   _playAction   = nullptr;   // AnimationClass::playAction(string,...) (0x51F920)
static estAnimFn      _estAnim      = nullptr;   // AnimationClass::estimateAnimationTime (0x5B19C0)
static void (*frame_orig)(void*, float) = nullptr;   // GameWorld::_NV_mainLoop_GPUSensitiveStuff (0x787E70)

// Character::isUsingTurret — `hand` field @0x258 (handle to the turret being operated;
// null = not using one). Read via hand::isNull() (rva 0x36AFC0).
static const size_t OFF_isUsingTurret = 0x258;

// ----------------------------------------------------------------------------
//  Config file (mod-config.json next to this DLL) — flat JSON. The mod folder is writable on a normal
//  (NexusMods / manual) install, so the config and the log both live right next to the DLL.
// ----------------------------------------------------------------------------
static std::string g_configPath;

static std::string moduleDir() {
    char path[MAX_PATH] = { 0 };
    HMODULE h = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&rd_rva, &h);
    GetModuleFileNameA(h, path, MAX_PATH);
    std::string s(path);
    size_t p = s.find_last_of("\\/");
    return (p == std::string::npos) ? std::string(".") : s.substr(0, p);
}

static bool findNumber(const std::string& s, const char* key, double& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k); if (p == std::string::npos) return false;
    p = s.find(':', p + k.size()); if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
    if (p < s.size() && (s.compare(p,4,"true")==0))  { out = 1; return true; }
    if (p < s.size() && (s.compare(p,5,"false")==0)) { out = 0; return true; }
    size_t e = p;
    while (e < s.size() && (isdigit((unsigned char)s[e]) || s[e]=='-' || s[e]=='+' || s[e]=='.' || s[e]=='e' || s[e]=='E')) ++e;
    if (e == p) return false;
    out = atof(s.substr(p, e - p).c_str());
    return true;
}

static bool findString(const std::string& s, const char* key, std::string& out) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k); if (p == std::string::npos) return false;
    p = s.find(':', p + k.size()); if (p == std::string::npos) return false;
    p = s.find('"', p); if (p == std::string::npos) return false;
    size_t e = s.find('"', p + 1); if (e == std::string::npos) return false;
    out = s.substr(p + 1, e - (p + 1)); return true;
}

static void LoadConfig() {
    g_configPath = moduleDir() + "\\mod-config.json";
    std::ifstream f(g_configPath.c_str());
    if (!f.good()) return;                          // keep defaults; SaveConfig() will create it
    std::stringstream ss; ss << f.rdbuf(); std::string s = ss.str();
    double v;
    if (findNumber(s, "enabled", v))               g_state.enabled = (v != 0);
    if (findNumber(s, "chance_multiplier", v))     g_state.chance_multiplier = (float)v;
    if (findNumber(s, "xp_enabled", v))            g_state.xp_enabled = (v != 0);
    if (findNumber(s, "skip_while_attacking", v))  g_state.skip_while_attacking = (v != 0);
    if (findNumber(s, "defend_while_in_melee", v)) g_state.defend_while_in_melee = (v != 0);
    if (findNumber(s, "dodge_lock_ms", v))         g_state.dodge_lock_ms = (int)v;
    if (findNumber(s, "block_lock_ms", v))         g_state.block_lock_ms = (int)v;
    if (findNumber(s, "play_animations", v))       g_state.play_animations = (v != 0);
    if (findNumber(s, "debug_logging", v))         g_state.debug_logging = (v != 0);
    if (findNumber(s, "lock_on_success", v))       g_state.lock_on_success = (v != 0);
    if (findNumber(s, "cooldown_enabled", v))      g_state.cooldown_enabled = (v != 0);
    if (findNumber(s, "apply_weight", v))          g_state.apply_weight = (v != 0);
    // Per-category defence keys (cat_<name>_on / _method / _weight). Absent keys keep the defaults, which
    // reproduce the previous default behaviour. Configs from v10 (selection_mode/block_cat/turret_mode/...)
    // have no cat_* keys, so they fall back to these defaults; reconfigure once via the per-category UI.
    {
        static const char* kCat[RD_NCAT] = {
            "katanas", "sabres", "blunt", "heavy", "hackers", "polearms", "noweapon", "ranged", "turret" };
        for (int i = 0; i < RD_NCAT; ++i) {
            std::string me = std::string("cat_") + kCat[i] + "_method";
            if (findNumber(s, me.c_str(), v)) { int m = (int)v; if (m < 0 || m > 4) m = CD_DODGEBLOCK; g_state.cat_method[i] = m; }
        }
        for (int i = 0; i < RD_NMELEE; ++i) {
            std::string wk = std::string("cat_") + kCat[i] + "_weight";
            if (findNumber(s, wk.c_str(), v)) { int w = (int)v; if (w < 0) w = 0; if (w > 50) w = 50; g_state.cat_weight[i] = w; }
        }
    }
    findString(s, "dodge_anim", g_dodgeAnim);
    findString(s, "block_anim", g_blockAnim);
    findString(s, "language", g_langPref);
    if (g_langPref != "auto" && g_langPref != "en" && g_langPref != "ru") g_langPref = "auto";
    if (g_state.dodge_lock_ms < 0)    g_state.dodge_lock_ms = 0;
    if (g_state.dodge_lock_ms > 5000) g_state.dodge_lock_ms = 5000;
    if (g_state.block_lock_ms < 0)    g_state.block_lock_ms = 0;
    if (g_state.block_lock_ms > 5000) g_state.block_lock_ms = 5000;
    if (g_state.chance_multiplier < 0.0f) g_state.chance_multiplier = 0.0f;
    // No hard upper cap from the config file: the final chance is clamped to 100% at use (normChance),
    // so a large multiplier only matters for weak defenders. The Mod Hub slider goes up to 100; you can
    // still type an even larger value straight into the config if you ever want to.
}

static void writeConfigTo(std::ostream& f) {
    static const char* kCat[RD_NCAT] = {
        "katanas", "sabres", "blunt", "heavy", "hackers", "polearms", "noweapon", "ranged", "turret" };
    f << "{\n"
      << "  \"enabled\": "               << (g_state.enabled ? "true" : "false") << ",\n"
      << "  \"chance_multiplier\": "     << g_state.chance_multiplier << ",\n"
      << "  \"xp_enabled\": "            << (g_state.xp_enabled ? "true" : "false") << ",\n"
      << "  \"apply_weight\": "          << (g_state.apply_weight ? "true" : "false") << ",\n";
    for (int i = 0; i < RD_NCAT; ++i) {
        f << "  \"cat_" << kCat[i] << "_method\": " << g_state.cat_method[i] << ",\n";
        if (i < RD_NMELEE)
            f << "  \"cat_" << kCat[i] << "_weight\": " << g_state.cat_weight[i] << ",\n";
    }
    f << "  \"dodge_lock_ms\": "         << g_state.dodge_lock_ms << ",\n"
      << "  \"block_lock_ms\": "         << g_state.block_lock_ms << ",\n"
      << "  \"skip_while_attacking\": "  << (g_state.skip_while_attacking ? "true" : "false") << ",\n"
      << "  \"defend_while_in_melee\": " << (g_state.defend_while_in_melee ? "true" : "false") << ",\n"
      << "  \"play_animations\": "       << (g_state.play_animations ? "true" : "false") << ",\n"
      << "  \"debug_logging\": "         << (g_state.debug_logging ? "true" : "false") << ",\n"
      << "  \"lock_on_success\": "       << (g_state.lock_on_success ? "true" : "false") << ",\n"
      << "  \"cooldown_enabled\": "      << (g_state.cooldown_enabled ? "true" : "false") << ",\n"
      << "  \"dodge_anim\": \""          << g_dodgeAnim << "\",\n"
      << "  \"block_anim\": \""          << g_blockAnim << "\",\n"
      << "  \"language\": \""            << g_langPref << "\"\n"
      << "}\n";
}

static void SaveConfig() {
    if (g_configPath.empty()) g_configPath = moduleDir() + "\\mod-config.json";
    std::ofstream f(g_configPath.c_str(), std::ios::out | std::ios::trunc);
    if (f.good()) writeConfigTo(f);
}

// ----------------------------------------------------------------------------
//  Language detection. Priority: mod-config "language" override, else Kenshi's
//  settings.cfg "language=" (read from the process cwd = game dir, exactly like
//  RE_Kenshi does). Value is a locale-ish code ("ru"/"ru_RU"/"russian"/"Русский"…);
//  anything Russian -> RU, otherwise EN.
// ----------------------------------------------------------------------------
static bool looksRussian(const std::string& raw) {
    std::string low = raw;
    for (size_t i = 0; i < low.size(); ++i) low[i] = (char)tolower((unsigned char)low[i]);
    if (low == "ru" || low.rfind("ru", 0) == 0 || low.find("russ") != std::string::npos) return true;
    for (size_t i = 0; i < raw.size(); ++i) {           // UTF-8 Cyrillic lead bytes -> "Русский"
        unsigned char c = (unsigned char)raw[i];
        if (c == 0xD0 || c == 0xD1) return true;
    }
    return false;
}
static void DetectLanguage() {
    if (g_langPref == "ru") { g_lang = LANG_RU; return; }
    if (g_langPref == "en") { g_lang = LANG_EN; return; }
    g_lang = LANG_EN;                                    // default
    std::ifstream f("settings.cfg");                    // Kenshi's, in the game dir (process cwd)
    if (!f.good()) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#' || line[0] == '[') continue;
        size_t sep = line.find_first_of("=:\t");
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        // trim key
        size_t ka = key.find_first_not_of(" \t\r\n"); size_t kb = key.find_last_not_of(" \t\r\n");
        if (ka == std::string::npos) continue;
        key = key.substr(ka, kb - ka + 1);
        for (size_t i = 0; i < key.size(); ++i) key[i] = (char)tolower((unsigned char)key[i]);
        if (key != "language") continue;
        std::string val = line.substr(sep + 1);
        size_t va = val.find_first_not_of(" \t\r\n"); size_t vb = val.find_last_not_of(" \t\r\n");
        if (va == std::string::npos) continue;
        val = val.substr(va, vb - va + 1);
        if (looksRussian(val)) g_lang = LANG_RU;
        return;                                         // first "language" line wins
    }
}

// ----------------------------------------------------------------------------
//  Calibration log
// ----------------------------------------------------------------------------
static std::ofstream* g_log = nullptr;
static int g_logged = 0;
// Log is active when: verbose diagnostics are ON (debug_logging -> unlimited), OR we are still
// within the bounded startup/calibration budget (so key startup lines are always captured, even
// if the user turns diagnostics off). Toggled live from the Mod Hub "Diagnostic logging" row.
static inline bool rdlogActive() { return g_log && (g_state.debug_logging || g_logged < CALIB_LOG_LINES); }
static void rdlog(const std::string& s) { if (rdlogActive()) { (*g_log) << s << "\n"; g_log->flush(); ++g_logged; } }

// ----------------------------------------------------------------------------
//  Roll helpers
// ----------------------------------------------------------------------------
static float normChance(float chance) {
    float p = chance;
    if (p > 1.5f) p *= 0.01f;                       // percent -> fraction
    p *= g_state.chance_multiplier;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    return p;
}
static bool rollDodge(CharStats* def, float shooter, float& outChance) {
    outChance = def->calculateDodgeChance(shooter, false);
    float p = normChance(outChance);
    return ((float)rand() / (float)RAND_MAX) < p;
}
static bool rollBlock(CharStats* def, float shooter, float& outChance) {
    if (!_blockChance) { outChance = 0; return false; }
    outChance = _blockChance((void*)def, shooter);
    float p = normChance(outChance);
    return ((float)rand() / (float)RAND_MAX) < p;
}

// ----------------------------------------------------------------------------
//  Hook: Character::iShotYou(attacker, projectile, onPurpose)
// ----------------------------------------------------------------------------
// GAME-time clock: accumulate the per-frame delta the game feeds its own update/animations,
// so our windows are in the SAME time-base as animations (scale with game speed, freeze on pause).
void frame_hook(void* gw, float time) {
    if (time > 0.0f && time < 5.0f) g_gameMs += (double)time * 1000.0;   // clamp out load-spike jumps
    HubTick();                          // load-order-independent Mod Hub attach retry (no-op once attached)
    frame_orig(gw, time);
}

bool (*iShotYou_orig)(Character*, Character*, Harpoon*, bool) = nullptr;

bool iShotYou_hook(Character* thisptr, Character* attacker, Harpoon* poon, bool onPurpose) {
    HubTick();   // secondary Mod Hub attach retry (covers the case where the frame hook did not install)
    if (g_state.enabled && thisptr && attacker && thisptr != attacker
        && !thisptr->isDown() && !thisptr->isDead()) {
        // Оператор турели: по умолчанию не защищается (прикован к турели). Настройкой turret_mode
        // можно разрешить ему уклоняться/блокировать — тогда он идёт по обычному конвейеру, но тип
        // защиты форсируется (см. выбор основной защиты ниже).
        bool onTurret = (_handIsNull && !_handIsNull((void*)((char*)thisptr + OFF_isUsingTurret)));
        if (onTurret && g_state.cat_method[CI_TURRET] == CD_NONE) {
            if (rdlogActive()) rdlog("iShotYou -> target is using a turret; Turret category = Nothing -> no defence.");
            return iShotYou_orig(thisptr, attacker, poon, onPurpose);
        }

        // ---- COMMITMENT: одна защита за окно (dodge_lock_ms / block_lock_ms) ----
        // как в ближнем бою: увернулся/блокнул ОДНУ атаку и «занят» — параллельные стрелы в это
        // окно ПОПАДАЮТ. Чинит и «уклонение от бесконечного числа стрел», и вечно-защищающегося босса.
        // Всё окно кулдауна можно ОТКЛЮЧИТЬ (cooldown_enabled = 0): тогда проверка/установка окна не
        // выполняются, и персонаж может защищаться от КАЖДОЙ стрелы (время окна и «только при успехе» не
        // применяются).
        double now = g_useGameClock ? g_gameMs : (double)GetTickCount();   // game-ms (or real-ms fallback)
        if (g_state.cooldown_enabled) {
            std::unordered_map<Character*, double>::iterator it = g_lockUntil.find(thisptr);
            if (it != g_lockUntil.end() && now < it->second)
                return iShotYou_orig(thisptr, attacker, poon, onPurpose);   // ещё в окне коммита -> стрела попадает
        }
        // Не защищаемся во время СВОЕЙ атаки (не отменяем собственный удар — важно для босса) и
        // если уже в стаггере/блоке от реального боя (нативный коммит).
        if (g_state.skip_while_attacking) {
            CombatClass* cc = thisptr->getCombatClass();
            if (cc && *reinterpret_cast<const float*>((const char*)cc + OFF_isAttacking) > 0.0f) {
                if (rdlogActive()) rdlog("skip: mid-attack (own attack in progress) -> arrow hits");
                return iShotYou_orig(thisptr, attacker, poon, onPurpose);
            }
        }
        {
            AnimationClass* ac = thisptr->getAnimationClass();
            if (ac && _stumbling && _stumbling((void*)ac)) {
                if (rdlogActive()) rdlog("skip: stumbling (native stagger) -> arrow hits");
                return iShotYou_orig(thisptr, attacker, poon, onPurpose);
            }
        }
        // Опционально: РЕАЛЬНО в ближней схватке (есть ближние противники) -> дальнюю защиту не применяем.
        // Именно getNumOpponents, а НЕ isInCombatMode: бегущий к стрелку «в боевом режиме», но противников
        // в ближнем у него нет -> он продолжает уклоняться от болтов.
        if (!g_state.defend_while_in_melee) {
            CombatClass* cc2 = thisptr->getCombatClass();
            if (cc2 && _numOpponents && _numOpponents((void*)cc2) > 0) {
                if (rdlogActive()) { std::ostringstream o; o << "skip: in melee (opp=" << _numOpponents((void*)cc2) << ") -> arrow hits"; rdlog(o.str()); }
                return iShotYou_orig(thisptr, attacker, poon, onPurpose);
            }
        }

        ProneState ps = thisptr->getProneState();
        if (ps != PS_KO && ps != PS_PLAYING_DEAD) {
            CharStats* def = thisptr->getStats();
            CharStats* atk = attacker->getStats();
            if (def && atk) {
                float shooter = atk->getStat(STAT_TURRETS, false);
                float cb      = atk->getStat(STAT_CROSSBOWS, false);
                if (cb > shooter) shooter = cb;

                Weapon* w  = thisptr->getCurrentWeapon();
                bool hasWeapon = (w != 0);
                int  cat   = (hasWeapon && _category) ? _category((void*)w) : -1;
                float wt   = (hasWeapon && _combatWeight) ? _combatWeight((void*)w) : 0.0f;
                float str  = def->strengthActual();
                float dex  = def->getStat(STAT_DEXTERITY, false);

                // ---- Per-category defence: pick the category, then its method (and weight filter) ----
                // Category: turret operator -> Turret; no weapon / unarmed -> No-weapon; crossbow/turret
                // weapon in hand -> Ranged; otherwise the melee category of the wielded weapon.
                int ci;
                if (onTurret)                                 ci = CI_TURRET;
                else if (!hasWeapon || cat == WC_UNARMED)     ci = CI_UNARMED;
                else if (cat == WC_BOW || cat == WC_TURRET)   ci = CI_RANGED;
                else { ci = catIndexOf(cat); if (ci < 0) ci = CI_UNARMED; }

                // Category method = Nothing -> no ranged defence at all (arrow behaves as vanilla).
                if (g_state.cat_method[ci] == CD_NONE) {
                    if (rdlogActive()) { std::ostringstream o; o << "iShotYou -> category #" << ci << " = Nothing -> no defence."; rdlog(o.str()); }
                    return iShotYou_orig(thisptr, attacker, poon, onPurpose);
                }

                // ---- choose PRIMARY defense + whether a block fallback is allowed ----
                bool primaryBlock = false;
                bool fallbackBlock = false;          // only meaningful when primary == dodge
                // Weight filter (melee categories only): below the category threshold -> dodge only (no block).
                bool weightOK = true;
                if (g_state.apply_weight && ci < RD_NMELEE)
                    weightOK = (wt >= (float)g_state.cat_weight[ci]);
                if (weightOK) {
                    switch (g_state.cat_method[ci]) {
                        case CD_DODGE:  primaryBlock = false; break;                       // Только уклон
                        case CD_BLOCK:  primaryBlock = true;  break;                       // Только блок
                        case CD_STAT:   primaryBlock = (str > dex); break;                 // Сила/Ловкость
                        case CD_DODGEBLOCK:
                        default:        primaryBlock = false; fallbackBlock = true; break; // Уклон + Блок
                    }
                }
                // (weightOK == false -> primaryBlock=false, fallbackBlock=false -> dodge only)

                bool evaded = false;
                const char* how = "none";
                float chDodge = 0, chBlock = 0;
                if (primaryBlock) {
                    if (rollBlock(def, shooter, chBlock)) { evaded = true; how = "block(primary)"; }
                    // block-first is terminal: no dodge fallback
                    // XP only on a SUCCESSFUL block (a failed block trains nothing, like a failed melee block)
                    if (g_state.xp_enabled && evaded) def->xpStat_eventBased(STAT_MELEE_DEFENCE, 1.0f);
                } else {
                    if (rollDodge(def, shooter, chDodge)) { evaded = true; how = "dodge(primary)"; }
                    // Dodge XP uses the game's OWN dodge-event function -> identical to vanilla melee dodging.
                    if (g_state.xp_enabled) def->xpDodgeEvent(shooter, evaded);
                    if (!evaded && fallbackBlock) {
                        if (rollBlock(def, shooter, chBlock)) { evaded = true; how = "block(fallback)"; }
                        if (g_state.xp_enabled && evaded) def->xpStat_eventBased(STAT_MELEE_DEFENCE, 1.0f);
                    }
                }

                // ---- COMMITMENT window (game-ms) ----
                // Skipped entirely when cooldown_enabled = 0 (no window -> defends against every arrow).
                // Default (lock_on_success = 0): ANY attempt starts the window — one reaction per window,
                //   so simultaneous arrows in it still hit (can't dodge a whole volley at once).
                // lock_on_success = 1: only a SUCCESSFUL defence starts the window; a failed attempt leaves
                //   the character free to try the next arrow. Duration follows the defence actually used.
                if (g_state.cooldown_enabled && (!g_state.lock_on_success || evaded)) {
                    bool blk = g_state.lock_on_success ? (how[0] == 'b') : primaryBlock;
                    double dur = blk ? (double)g_state.block_lock_ms : (double)g_state.dodge_lock_ms;
                    g_lockUntil[thisptr] = now + dur;
                    if (g_lockUntil.size() > 4096) {
                        for (std::unordered_map<Character*, double>::iterator it = g_lockUntil.begin(); it != g_lockUntil.end(); ) {
                            if (now - it->second > 5000.0) it = g_lockUntil.erase(it); else ++it;   // истёк >5с назад
                        }
                    }
                }

                if (rdlogActive()) {
                    // DIAGNOSTIC: opp = CombatClass::getNumOpponents(). Проверь: если по цели стреляют
                    // ТОЛЬКО издалека (мили-противников нет), opp должен быть 0. Если >0 — метод считает
                    // и дальних, тогда сменим сигнал детекта ближней схватки.
                    int opp = -1; float atkf = -1.0f;
                    { CombatClass* lc = thisptr->getCombatClass(); if (lc) { if (_numOpponents) opp = _numOpponents((void*)lc);
                        atkf = *reinterpret_cast<const float*>((const char*)lc + OFF_isAttacking); } }
                    std::ostringstream o;
                    o << "iShotYou this=" << (void*)thisptr << " shooter=" << shooter
                      << " cat=" << cat << " wt=" << wt
                      << " ci=" << ci << " method=" << g_state.cat_method[ci]
                      << " applyWt=" << g_state.apply_weight << " catWt=" << (ci < RD_NMELEE ? g_state.cat_weight[ci] : -1)
                      << " weightOK=" << (int)weightOK
                      << " str=" << str << " dex=" << dex << " opp=" << opp << " atk=" << atkf
                      << " turret=" << (int)onTurret
                      << " primary=" << (primaryBlock ? "block" : "dodge")
                      << " fb=" << (int)fallbackBlock << " cd=" << g_state.cooldown_enabled << " los=" << g_state.lock_on_success
                      << " win=" << (!g_state.cooldown_enabled ? "off" : ((!g_state.lock_on_success || evaded) ? ((g_state.lock_on_success ? (how[0]=='b') : primaryBlock) ? "block" : "dodge") : "none"))
                      << " chDodge=" << chDodge << " chBlock=" << chBlock
                      << " -> " << (evaded ? how : "hit");
                    rdlog(o.str());
                }

                // НАТИВНАЯ АНИМАЦИЯ (экспериментально, только для НАШЕЙ дальней защиты).
                // Проигрываем настоящую анимацию уклона/блока на этом персонаже; на вражескую/ближнюю
                // боёвку не влияем (вызываем только из этого хука). how[0]=='b' -> это был блок.
                if (g_state.play_animations && evaded && _playAction) {
                    AnimationClass* ac = thisptr->getAnimationClass();
                    if (ac) {
                        bool blk = (how[0] == 'b');
                        const std::string& nm = blk ? g_blockAnim : g_dodgeAnim;
                        if (!nm.empty()) {
                            _playAction((void*)ac, nm, 1.0f, 1.0f, blk);
                            static bool loggedDur = false;
                            if (!loggedDur && _estAnim) {
                                loggedDur = true;
                                std::ostringstream d; d << "anim '" << nm << "' estimated game-time="
                                                        << _estAnim((void*)ac, nm) << "s";
                                rdlog(d.str());
                            }
                        }
                    }
                }
                if (evaded) return false;             // защита сработала — выстрел аннулирован
            }
        }
    }
    return iShotYou_orig(thisptr, attacker, poon, onPurpose);
}

// ============================================================================
//  Mod Hub (Emkejs-Mod-Core) — register settings via the public API export.
//  user_data of each row points at the matching field in g_state; set-callbacks
//  update it + persist mod-config.json.
// ============================================================================
static EMC_Result __cdecl cbGetBool (void* ud, int32_t* out) { *out = (*(int32_t*)ud) ? 1 : 0; return EMC_OK; }
static EMC_Result __cdecl cbSetBool (void* ud, int32_t v, char*, uint32_t) { *(int32_t*)ud = (v != 0); SaveConfig(); return EMC_OK; }
static EMC_Result __cdecl cbGetInt  (void* ud, int32_t* out) { *out = *(int32_t*)ud; return EMC_OK; }
static EMC_Result __cdecl cbSetInt  (void* ud, int32_t v, char*, uint32_t) { *(int32_t*)ud = v; SaveConfig(); return EMC_OK; }
static EMC_Result __cdecl cbGetFloat(void* ud, float* out) { *out = *(float*)ud; return EMC_OK; }
static EMC_Result __cdecl cbSetFloat(void* ud, float v, char*, uint32_t) {
    // Only guard against negatives; no upper cap (the slider already limits its own range, and the
    // config file is allowed to go higher — the final chance is clamped to 100% at use anyway).
    if (v < 0.0f) v = 0.0f; *(float*)ud = v; SaveConfig(); return EMC_OK; }
static EMC_Result __cdecl cbGetSel  (void* ud, int32_t* out) { *out = *(int32_t*)ud; return EMC_OK; }
static EMC_Result __cdecl cbSetSel  (void* ud, int32_t v, char*, uint32_t) {
    if (v < 0 || v > 5) return EMC_ERR_INVALID_ARGUMENT; *(int32_t*)ud = v; SaveConfig(); return EMC_OK; }
// Per-category method setter: also flips the "For all" master to Custom (this category is now individual).
static EMC_Result __cdecl cbSetCatMethod(void* ud, int32_t v, char*, uint32_t) {
    if (v < 0 || v > 4) return EMC_ERR_INVALID_ARGUMENT;
    *(int32_t*)ud = v; g_state.cat_all_method = CD_CUSTOM; SaveConfig(); return EMC_OK; }
// "For all categories" master setter: Custom = leave each category as-is; any other value is written to ALL
// categories at once (the per-category rows now hold that value too).
static EMC_Result __cdecl cbSetAllMethod(void* ud, int32_t v, char*, uint32_t) {
    if (v != CD_CUSTOM && (v < 0 || v > 3)) return EMC_ERR_INVALID_ARGUMENT;
    (void)ud; g_state.cat_all_method = v;
    if (v != CD_CUSTOM) for (int i = 0; i < RD_NCAT; ++i) g_state.cat_method[i] = v;
    SaveConfig(); return EMC_OK; }

// Register under Emkejs' own "Emkej QoL" tab (namespace_id "emkej.qol" — from Emkejs-Mod-Core's
// hub_menu_bridge.cpp). The hub keys tabs by namespace_id, so sharing the id puts "Ranged Defence"
// under that existing tab; the first-registered display name is canonical, ours is ignored (fine).
// mod_id stays unique ("ranged_defence"). No "proton.combat" namespace is created -> that tab is gone.
// Built at runtime so the mod display name can be localized. Static -> valid for the process lifetime.
static EMC_ModDescriptorV1 g_modDesc;

// Per-category defence-method options (shared by all 9 category selects; labels localized, values = CatMethod).
static const EMC_SelectOptionV1 kMethodOptions_en[] = {
    { CD_DODGEBLOCK, "Dodge + Block" },
    { CD_DODGE, "Dodge only" },
    { CD_BLOCK, "Block only" },
    { CD_STAT, "STR / DEX" },
    { CD_NONE, "Nothing" },
};
static const EMC_SelectOptionV1 kMethodOptions_ru[] = {
    { CD_DODGEBLOCK, "\xd0\xa3\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd + \xd0\x91\xd0\xbb\xd0\xbe\xd0\xba" },
    { CD_DODGE, "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd" },
    { CD_BLOCK, "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba" },
    { CD_STAT, "\xd0\xa1\xd0\xb8\xd0\xbb\xd0\xb0/\xd0\x9b\xd0\xbe\xd0\xb2\xd0\xba\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c" },
    { CD_NONE, "\xd0\x9d\xd0\xb8\xd1\x87\xd0\xb5\xd0\xb3\xd0\xbe" },
};
// "For all categories" master options: same methods, but "Custom" replaces "Nothing". Custom = per-category.
static const EMC_SelectOptionV1 kAllOptions_en[] = {
    { CD_DODGEBLOCK, "Dodge + Block" },
    { CD_DODGE, "Dodge only" },
    { CD_BLOCK, "Block only" },
    { CD_STAT, "STR / DEX" },
    { CD_CUSTOM, "Custom" },
};
static const EMC_SelectOptionV1 kAllOptions_ru[] = {
    { CD_DODGEBLOCK, "\xd0\xa3\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd + \xd0\x91\xd0\xbb\xd0\xbe\xd0\xba" },
    { CD_DODGE, "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd" },
    { CD_BLOCK, "\xd0\xa2\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba" },
    { CD_STAT, "\xd0\xa1\xd0\xb8\xd0\xbb\xd0\xb0/\xd0\x9b\xd0\xbe\xd0\xb2\xd0\xba\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c" },
    { CD_CUSTOM, "Custom" },
};

typedef EMC_Result(__cdecl* GetApiFn)(uint32_t, uint32_t, const EMC_HubApiV1**, uint32_t*);

// Resolve the hub's EMC_ModHub_GetApi export from ANY loaded module. This is deliberately NOT
// GetModuleHandleA("Emkejs-Mod-Core.dll"): that hardcodes one filename and returns null if the hub
// module has a different on-disk name OR simply has not been loaded yet when our startPlugin runs
// (load order). We first try the common names, then fall back to scanning every loaded module for
// the export, so we attach regardless of the hub's filename. Returns null while the hub is absent;
// the caller retries (see HubTick) so a late-loading hub is still picked up.
static GetApiFn ResolveHubGetApi(const void** whereModule) {
    static const char* kNames[] = { "Emkejs-Mod-Core.dll", "Emkejs-Mod-Core" };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i) {
        HMODULE h = GetModuleHandleA(kNames[i]);
        if (!h) continue;
        FARPROC p = GetProcAddress(h, EMC_MOD_HUB_GET_API_EXPORT_NAME);
        if (!p) p = GetProcAddress(h, EMC_MOD_HUB_GET_API_COMPAT_EXPORT_NAME);
        if (p) { if (whereModule) *whereModule = (const void*)h; return (GetApiFn)p; }
    }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return 0;
    MODULEENTRY32 me; me.dwSize = sizeof(me);       // maps to MODULEENTRY32W under Unicode; we only read hModule/dwSize
    GetApiFn fn = 0;
    if (Module32First(snap, &me)) {
        do {
            FARPROC p = GetProcAddress(me.hModule, EMC_MOD_HUB_GET_API_EXPORT_NAME);
            if (!p) p = GetProcAddress(me.hModule, EMC_MOD_HUB_GET_API_COMPAT_EXPORT_NAME);
            if (p) { if (whereModule) *whereModule = (const void*)me.hModule; fn = (GetApiFn)p; break; }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return fn;
}

// One attach+registration attempt. Returns true if the mod is now registered with the hub (so the
// caller stops retrying); false means "hub not available yet, try again later". A hard failure that
// won't improve on retry (API found but register_mod rejected us) also returns true — with g_hubState
// set to -1 by the caller — so we don't spin forever.
// ---- Registration helpers -------------------------------------------------------------------
// Each row = a label + one short single-line description (the hub draws the description as a single
// non-wrapping line and has no hover-tooltip rendering on the shipping build, so all detail lives in
// that one line and in the docs). regIntV2 keeps the fine +/- step buttons (that V2 row IS supported).
static void regBool(const EMC_HubApiV1* api, uint32_t apiSize, EMC_ModHandle mod,
                    const char* id, const char* label, const char* desc, int32_t* field) {
    (void)apiSize;
    EMC_BoolSettingDefV1 d = { id, label, desc, field, &cbGetBool, &cbSetBool };
    api->register_bool_setting(mod, &d);
}
static void regInt(const EMC_HubApiV1* api, uint32_t apiSize, EMC_ModHandle mod,
                   const char* id, const char* label, const char* desc,
                   int32_t lo, int32_t hi, int32_t step, int32_t* field) {
    (void)apiSize;
    EMC_IntSettingDefV1 d = { id, label, desc, field, lo, hi, step, &cbGetInt, &cbSetInt };
    api->register_int_setting(mod, &d);
}
// Like regInt, but registers via the V2 int row so we control the +/- quick-step buttons.
// A weapon category is a discrete id, so coarse (+/-10, +/-5) jumps are meaningless: expose only a
// single fine step. Falls back to the V1 row (hub's default coarse buttons) on older hubs.
static void regIntV2(const EMC_HubApiV1* api, uint32_t apiSize, EMC_ModHandle mod,
                     const char* id, const char* label, const char* desc,
                     int32_t lo, int32_t hi, int32_t step, int32_t* field) {
    if (apiSize >= EMC_HUB_API_V1_INT_SETTING_V2_MIN_SIZE && api->register_int_setting_v2) {
        EMC_IntSettingDefV2 d;
        d.setting_id = id; d.label = label; d.description = desc; d.user_data = field;
        d.min_value = lo; d.max_value = hi; d.step = step;
        d.dec_button_deltas[0] = step; d.dec_button_deltas[1] = 0; d.dec_button_deltas[2] = 0;
        d.inc_button_deltas[0] = step; d.inc_button_deltas[1] = 0; d.inc_button_deltas[2] = 0;
        d.get_value = &cbGetInt; d.set_value = &cbSetInt;
        api->register_int_setting_v2(mod, &d);
    } else {
        EMC_IntSettingDefV1 d = { id, label, desc, field, lo, hi, step, &cbGetInt, &cbSetInt };
        api->register_int_setting(mod, &d);
    }
}
static void regFloat(const EMC_HubApiV1* api, uint32_t apiSize, EMC_ModHandle mod,
                     const char* id, const char* label, const char* desc,
                     float lo, float hi, float step, uint32_t dec, float* field) {
    (void)apiSize;
    EMC_FloatSettingDefV1 d = { id, label, desc, field, lo, hi, step, dec, &cbGetFloat, &cbSetFloat };
    api->register_float_setting(mod, &d);
}
static void regSelect(const EMC_HubApiV1* api, uint32_t apiSize, EMC_ModHandle mod,
                      const char* id, const char* label, const char* desc,
                      int32_t* field, const EMC_SelectOptionV1* opts, uint32_t count) {
    if (apiSize >= EMC_HUB_API_V1_SELECT_SETTING_MIN_SIZE && api->register_select_setting) {
        EMC_SelectSettingDefV1 d = { id, label, desc, field, opts, count, &cbGetSel, &cbSetSel };
        api->register_select_setting(mod, &d);
    }
}
// Like regSelect but with a caller-supplied set callback (get is always cbGetSel).
static void regSelectCb(const EMC_HubApiV1* api, uint32_t apiSize, EMC_ModHandle mod,
                        const char* id, const char* label, const char* desc,
                        int32_t* field, const EMC_SelectOptionV1* opts, uint32_t count, EMC_SetSelectCallback setcb) {
    if (apiSize >= EMC_HUB_API_V1_SELECT_SETTING_MIN_SIZE && api->register_select_setting) {
        EMC_SelectSettingDefV1 d = { id, label, desc, field, opts, count, &cbGetSel, setcb };
        api->register_select_setting(mod, &d);
    }
}

static bool g_hubHardFail = false;   // set by DoRegisterModHub on a non-retryable failure
static bool DoRegisterModHub() {
    const void* where = 0;
    GetApiFn getApi = ResolveHubGetApi(&where);
    if (!getApi) return false;                       // hub not loaded yet -> retry later
    const EMC_HubApiV1* api = 0; uint32_t apiSize = 0;
    // Request MIN_SIZE (not sizeof): the hub validates caller_api_size as a *minimum* and reports
    // its real size back in apiSize, which we use to gate optional (select/…) rows below.
    EMC_Result gr = getApi(EMC_HUB_API_VERSION_1, EMC_HUB_API_V1_MIN_SIZE, &api, &apiSize);
    if (gr != EMC_OK || !api || apiSize < EMC_HUB_API_V1_MIN_SIZE) {
        { std::ostringstream o; o << "ModHub: GetApi failed (result=" << gr << " apiSize=" << apiSize << ")."; rdlog(o.str()); }
        g_hubHardFail = true; return true;           // API present but handshake rejected -> don't spin
    }
    // Build the descriptor now so the mod display name follows the chosen language. namespace_id is
    // Emkejs' "emkej.qol" so we land under their "Emkej QoL" tab; namespace display is ignored by the
    // hub (canonical wins) but set for correctness. IDs stay ASCII; display names may be UTF-8.
    g_modDesc.namespace_id           = "emkej.qol";
    g_modDesc.namespace_display_name = "Emkej QoL";
    g_modDesc.mod_id                 = "ranged_defence";
    g_modDesc.mod_display_name       = tr("Ranged Defence (v12)", "\xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8f\xd1\x8f \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 (v12)");
    g_modDesc.mod_user_data          = &g_state;
    EMC_ModHandle mod = 0;
    EMC_Result rr = api->register_mod(&g_modDesc, &mod);
    if (rr == EMC_ERR_CONFLICT) { rdlog("ModHub: register_mod -> already registered (conflict treated as OK)."); return true; }
    if (rr != EMC_OK || !mod) {
        { std::ostringstream o; o << "ModHub: register_mod failed (result=" << rr << ")."; rdlog(o.str()); }
        g_hubHardFail = true; return true;
    }
    { std::ostringstream o; o << "ModHub: attached (api@" << api << " module@" << where << " apiSize=" << apiSize << ")."; rdlog(o.str()); }

    regBool(api, apiSize, mod, "enabled",
        tr("Enabled", "\xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb5\xd0\xbd\xd0\xbe"),
        tr("Turn the whole mod on or off.", "\xd0\x92\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb0\xd0\xb5\xd1\x82 \xd0\xb8\xd0\xbb\xd0\xb8 \xd0\xb2\xd1\x8b\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd0\xb0\xd0\xb5\xd1\x82 \xd0\xb2\xd0\xb5\xd1\x81\xd1\x8c \xd0\xbc\xd0\xbe\xd0\xb4."),
        &g_state.enabled);

    regFloat(api, apiSize, mod, "chance_multiplier",
        tr("Chance multiplier", "\xd0\x9c\xd0\xbd\xd0\xbe\xd0\xb6\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c \xd1\x88\xd0\xb0\xd0\xbd\xd1\x81\xd0\xb0"),
        tr("Multiplies dodge/block chance (1.0 = like melee).", "\xd0\x9c\xd0\xbd\xd0\xbe\xd0\xb6\xd0\xb8\xd1\x82\xd0\xb5\xd0\xbb\xd1\x8c \xd1\x88\xd0\xb0\xd0\xbd\xd1\x81\xd0\xb0 \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd0\xb0/\xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0 (1.0 = \xd0\xba\xd0\xb0\xd0\xba \xd0\xb2 \xd0\xb1\xd0\xbb\xd0\xb8\xd0\xb6\xd0\xbd\xd0\xb5\xd0\xbc)."),
        0.0f, 100.0f, 0.05f, 2u, &g_state.chance_multiplier);

    regBool(api, apiSize, mod, "cooldown_enabled",
        tr("Cooldown window", "\xd0\x9e\xd0\xba\xd0\xbd\xd0\xbe \xd0\xba\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd\xd0\xb0"),
        tr("On: one defence per cooldown window. Off: defends every arrow (cooldowns ignored).",
           "\xd0\x92\xd0\xba\xd0\xbb: \xd0\xbe\xd0\xb4\xd0\xbd\xd0\xb0 \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 \xd0\xb7\xd0\xb0 \xd0\xbe\xd0\xba\xd0\xbd\xd0\xbe \xd0\xba\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd\xd0\xb0. \xd0\x92\xd1\x8b\xd0\xba\xd0\xbb: \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x89\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f \xd0\xbe\xd1\x82 \xd0\xba\xd0\xb0\xd0\xb6\xd0\xb4\xd0\xbe\xd0\xb9 \xd1\x81\xd1\x82\xd1\x80\xd0\xb5\xd0\xbb\xd1\x8b (\xd0\xba\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd\xd1\x8b \xd0\xb8\xd0\xb3\xd0\xbd\xd0\xbe\xd1\x80\xd0\xb8\xd1\x80\xd1\x83\xd1\x8e\xd1\x82\xd1\x81\xd1\x8f)."),
        &g_state.cooldown_enabled);

    regInt(api, apiSize, mod, "dodge_lock_ms",
        tr("Dodge cooldown (game time)", "\xd0\x9a\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd0\xb0 (\xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb5 \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f)"),
        tr("Cooldown after a dodge, in game-time ms; no defence during it.", "\xd0\x9a\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd \xd0\xbf\xd0\xbe\xd1\x81\xd0\xbb\xd0\xb5 \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd0\xb0, \xd0\xbc\xd1\x81 \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8; \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 \xd0\xbd\xd0\xb5 \xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd0\xb2\xd0\xbd\xd0\xb0."),
        0, 5000, 50, &g_state.dodge_lock_ms);

    regInt(api, apiSize, mod, "block_lock_ms",
        tr("Block cooldown (game time)", "\xd0\x9a\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0 (\xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb5 \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f)"),
        tr("Cooldown after a block, in game-time ms; no defence during it.", "\xd0\x9a\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd \xd0\xbf\xd0\xbe\xd1\x81\xd0\xbb\xd0\xb5 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0, \xd0\xbc\xd1\x81 \xd0\xb8\xd0\xb3\xd1\x80\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd0\xb5\xd0\xbd\xd0\xb8; \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 \xd0\xbd\xd0\xb5 \xd0\xb0\xd0\xba\xd1\x82\xd0\xb8\xd0\xb2\xd0\xbd\xd0\xb0."),
        0, 5000, 50, &g_state.block_lock_ms);

    regBool(api, apiSize, mod, "lock_on_success",
        tr("Cooldown only on success", "\xd0\x9a\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd0\xbf\xd1\x80\xd0\xb8 \xd1\x83\xd1\x81\xd0\xbf\xd0\xb5\xd1\x85\xd0\xb5"),
        tr("Off: any attempt starts the cooldown. On: only a successful defence does.",
           "\xd0\x92\xd1\x8b\xd0\xba\xd0\xbb: \xd0\xba\xd1\x83\xd0\xbb\xd0\xb4\xd0\xb0\xd1\x83\xd0\xbd \xd0\xb7\xd0\xb0\xd0\xbf\xd1\x83\xd1\x81\xd0\xba\xd0\xb0\xd0\xb5\xd1\x82 \xd0\xbb\xd1\x8e\xd0\xb1\xd0\xb0\xd1\x8f \xd0\xbf\xd0\xbe\xd0\xbf\xd1\x8b\xd1\x82\xd0\xba\xd0\xb0. \xd0\x92\xd0\xba\xd0\xbb: \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd1\x81\xd0\xbf\xd0\xb5\xd1\x88\xd0\xbd\xd0\xb0\xd1\x8f \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0."),
        &g_state.lock_on_success);

    regBool(api, apiSize, mod, "xp_enabled",
        tr("Grant skill XP", "\xd0\x9d\xd0\xb0\xd1\x87\xd0\xb8\xd1\x81\xd0\xbb\xd1\x8f\xd1\x82\xd1\x8c \xd0\xbe\xd0\xbf\xd1\x8b\xd1\x82"),
        tr("Give Dodge / Melee Defence XP on attempts.", "\xd0\x9e\xd0\xbf\xd1\x8b\xd1\x82 \xd0\xa3\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd0\xb5\xd0\xbd\xd0\xb8\xd1\x8f/\xd0\x97\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd1\x8b \xd0\xb7\xd0\xb0 \xd0\xbf\xd0\xbe\xd0\xbf\xd1\x8b\xd1\x82\xd0\xba\xd0\xb8."),
        &g_state.xp_enabled);

    regBool(api, apiSize, mod, "skip_while_attacking",
        tr("No defence mid-attack", "\xd0\x91\xd0\xb5\xd0\xb7 \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd1\x8b \xd0\xb2 \xd0\xb0\xd1\x82\xd0\xb0\xd0\xba\xd0\xb5"),
        tr("While attacking, this character does not defend against arrows.", "\xd0\x9f\xd0\xbe\xd0\xba\xd0\xb0 \xd0\xbf\xd0\xb5\xd1\x80\xd1\x81\xd0\xbe\xd0\xbd\xd0\xb0\xd0\xb6 \xd0\xb2\xd1\x8b\xd0\xbf\xd0\xbe\xd0\xbb\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82 \xd1\x83\xd0\xb4\xd0\xb0\xd1\x80 \xe2\x80\x94 \xd0\xbe\xd0\xbd \xd0\xbd\xd0\xb5 \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x89\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f \xd0\xbe\xd1\x82 \xd1\x81\xd1\x82\xd1\x80\xd0\xb5\xd0\xbb."),
        &g_state.skip_while_attacking);

    regBool(api, apiSize, mod, "defend_while_in_melee",
        tr("Defend ranged in melee", "\xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd1\x8f\xd1\x8f \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x82\xd0\xb0 \xd0\xb2 \xd0\xbc\xd0\xb8\xd0\xbb\xd0\xb8"),
        tr("On: defends vs arrows during a fight. Off: no ranged defence while fighting.", "\xd0\x92\xd0\xba\xd0\xbb: \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x89\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f \xd0\xbe\xd1\x82 \xd1\x81\xd1\x82\xd1\x80\xd0\xb5\xd0\xbb \xd0\xb2\xd0\xbe \xd0\xb2\xd1\x80\xd0\xb5\xd0\xbc\xd1\x8f \xd0\xb1\xd0\xbe\xd1\x8f. \xd0\x92\xd1\x8b\xd0\xba\xd0\xbb: \xd0\xb2 \xd0\xb1\xd0\xbe\xd1\x8e \xd0\xbd\xd0\xb5 \xd0\xb7\xd0\xb0\xd1\x89\xd0\xb8\xd1\x89\xd0\xb0\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f \xd0\xbe\xd1\x82 \xd1\x81\xd1\x82\xd1\x80\xd0\xb5\xd0\xbb."),
        &g_state.defend_while_in_melee);

    // ---- Per-category defence method (Nothing = this category takes no ranged defence) ----
    // The method legend used to be repeated on every category row. It now appears ONCE, as the
    // single-line description of the first "For all categories" row. The 9 category rows carry only
    // their label — the method is self-evident from the dropdown itself.
    {
        const EMC_SelectOptionV1* mopts = (g_lang == LANG_RU) ? kMethodOptions_ru : kMethodOptions_en;
        const uint32_t mcount = (uint32_t)(sizeof(kMethodOptions_en)/sizeof(kMethodOptions_en[0]));
        // Master: set the method for ALL categories at once. "Custom" = per-category values below.
        // Its description carries the shared method legend (shown once, at the top of the block).
        {
            const EMC_SelectOptionV1* aopts = (g_lang == LANG_RU) ? kAllOptions_ru : kAllOptions_en;
            regSelectCb(api, apiSize, mod, "cat_all_method",
                tr("For all categories", "\xd0\x94\xd0\xbb\xd1\x8f \xd0\xb2\xd1\x81\xd0\xb5\xd1\x85 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb9"),
                tr("Dodge+Block / Dodge / Block / STR-DEX / Nothing \xe2\x80\x94 sets ALL; Custom = per-category below.", "\xd0\xa3\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\x2b\xd0\x91\xd0\xbb\xd0\xbe\xd0\xba\x20\x2f\x20\xd0\xa3\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\x20\x2f\x20\xd0\x91\xd0\xbb\xd0\xbe\xd0\xba\x20\x2f\x20\xd0\xa1\xd0\xb8\xd0\xbb\xd0\xb0\x2f\xd0\x9b\xd0\xbe\xd0\xb2\xd0\xba\xd0\xbe\xd1\x81\xd1\x82\xd1\x8c\x20\x2f\x20\xd0\x9d\xd0\xb8\xd1\x87\xd0\xb5\xd0\xb3\xd0\xbe\x20\xe2\x80\x94\x20\xd0\xb7\xd0\xb0\xd0\xb4\xd0\xb0\xd1\x91\xd1\x82\x20\xd0\x92\xd0\xa1\xd0\x95\x3b\x20\x43\x75\x73\x74\x6f\x6d\x20\x3d\x20\xd0\xbf\xd0\xbe\x20\xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\x2e"),
                &g_state.cat_all_method, aopts, (uint32_t)(sizeof(kAllOptions_en)/sizeof(kAllOptions_en[0])), &cbSetAllMethod);
        }
        regSelectCb(api, apiSize, mod, "cat_katanas_method",
            tr("Katanas: method", "\xd0\x9a\xd0\xb0\xd1\x82\xd0\xb0\xd0\xbd\xd1\x8b: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[0], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_sabres_method",
            tr("Sabres: method", "\xd0\xa1\xd0\xb0\xd0\xb1\xd0\xbb\xd0\xb8: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[1], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_blunt_method",
            tr("Blunt: method", "\xd0\x94\xd1\x80\xd0\xbe\xd0\xb1\xd1\x8f\xd1\x89\xd0\xb5\xd0\xb5: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[2], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_heavy_method",
            tr("Heavy: method", "\xd0\xa2\xd1\x8f\xd0\xb6\xd1\x91\xd0\xbb\xd0\xbe\xd0\xb5: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[3], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_hackers_method",
            tr("Hackers: method", "\xd0\xa0\xd1\x83\xd0\xb1\xd1\x8f\xd1\x89\xd0\xb5\xd0\xb5: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[4], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_polearms_method",
            tr("Polearms: method", "\xd0\x94\xd1\x80\xd0\xb5\xd0\xb2\xd0\xba\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb5: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[5], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_noweapon_method",
            tr("No weapon: method", "\xd0\x91\xd0\xb5\xd0\xb7 \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd1\x8f: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[6], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_ranged_method",
            tr("Ranged: method", "\xd0\x94\xd0\xb0\xd0\xbb\xd1\x8c\xd0\xbd\xd0\xbe\xd0\xb1\xd0\xbe\xd0\xb9\xd0\xbd\xd0\xbe\xd0\xb5: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[7], mopts, mcount, &cbSetCatMethod);
        regSelectCb(api, apiSize, mod, "cat_turret_method",
            tr("Turret: method", "\xd0\xa2\xd1\x83\xd1\x80\xd0\xb5\xd0\xbb\xd1\x8c: \xd1\x81\xd0\xbf\xd0\xbe\xd1\x81\xd0\xbe\xd0\xb1"),
            "",
            &g_state.cat_method[8], mopts, mcount, &cbSetCatMethod);
    }

    regBool(api, apiSize, mod, "apply_weight",
        tr("Apply weight to categories", "\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc"),
        tr("Take per-category weight thresholds into account.", "\xd0\xa3\xd1\x87\xd0\xb8\xd1\x82\xd1\x8b\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3\xd0\xb8 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x83 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb9."),
        &g_state.apply_weight);

    // ---- Per-melee-category weight thresholds (used only when Apply weight is on) ----
    {
        regInt(api, apiSize, mod, "cat_katanas_weight",
            tr("Katanas: weight threshold", "\xd0\x9a\xd0\xb0\xd1\x82\xd0\xb0\xd0\xbd\xd1\x8b: \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0"),
            tr("With 'Apply weight to categories' on: weapons of this category lighter than this combat weight only dodge (no block). 0 = no filter.", "\xd0\x9f\xd1\x80\xd0\xb8 \xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb9 \xc2\xab\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\xc2\xbb: \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8 \xd0\xbb\xd0\xb5\xd0\xb3\xd1\x87\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0). 0 = \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x84\xd0\xb8\xd0\xbb\xd1\x8c\xd1\x82\xd1\x80\xd0\xb0."),
            0, 50, 1, &g_state.cat_weight[0]);
        regInt(api, apiSize, mod, "cat_sabres_weight",
            tr("Sabres: weight threshold", "\xd0\xa1\xd0\xb0\xd0\xb1\xd0\xbb\xd0\xb8: \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0"),
            tr("With 'Apply weight to categories' on: weapons of this category lighter than this combat weight only dodge (no block). 0 = no filter.", "\xd0\x9f\xd1\x80\xd0\xb8 \xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb9 \xc2\xab\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\xc2\xbb: \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8 \xd0\xbb\xd0\xb5\xd0\xb3\xd1\x87\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0). 0 = \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x84\xd0\xb8\xd0\xbb\xd1\x8c\xd1\x82\xd1\x80\xd0\xb0."),
            0, 50, 1, &g_state.cat_weight[1]);
        regInt(api, apiSize, mod, "cat_blunt_weight",
            tr("Blunt: weight threshold", "\xd0\x94\xd1\x80\xd0\xbe\xd0\xb1\xd1\x8f\xd1\x89\xd0\xb5\xd0\xb5: \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0"),
            tr("With 'Apply weight to categories' on: weapons of this category lighter than this combat weight only dodge (no block). 0 = no filter.", "\xd0\x9f\xd1\x80\xd0\xb8 \xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb9 \xc2\xab\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\xc2\xbb: \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8 \xd0\xbb\xd0\xb5\xd0\xb3\xd1\x87\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0). 0 = \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x84\xd0\xb8\xd0\xbb\xd1\x8c\xd1\x82\xd1\x80\xd0\xb0."),
            0, 50, 1, &g_state.cat_weight[2]);
        regInt(api, apiSize, mod, "cat_heavy_weight",
            tr("Heavy: weight threshold", "\xd0\xa2\xd1\x8f\xd0\xb6\xd1\x91\xd0\xbb\xd0\xbe\xd0\xb5: \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0"),
            tr("With 'Apply weight to categories' on: weapons of this category lighter than this combat weight only dodge (no block). 0 = no filter.", "\xd0\x9f\xd1\x80\xd0\xb8 \xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb9 \xc2\xab\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\xc2\xbb: \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8 \xd0\xbb\xd0\xb5\xd0\xb3\xd1\x87\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0). 0 = \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x84\xd0\xb8\xd0\xbb\xd1\x8c\xd1\x82\xd1\x80\xd0\xb0."),
            0, 50, 1, &g_state.cat_weight[3]);
        regInt(api, apiSize, mod, "cat_hackers_weight",
            tr("Hackers: weight threshold", "\xd0\xa0\xd1\x83\xd0\xb1\xd1\x8f\xd1\x89\xd0\xb5\xd0\xb5: \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0"),
            tr("With 'Apply weight to categories' on: weapons of this category lighter than this combat weight only dodge (no block). 0 = no filter.", "\xd0\x9f\xd1\x80\xd0\xb8 \xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb9 \xc2\xab\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\xc2\xbb: \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8 \xd0\xbb\xd0\xb5\xd0\xb3\xd1\x87\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0). 0 = \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x84\xd0\xb8\xd0\xbb\xd1\x8c\xd1\x82\xd1\x80\xd0\xb0."),
            0, 50, 1, &g_state.cat_weight[4]);
        regInt(api, apiSize, mod, "cat_polearms_weight",
            tr("Polearms: weight threshold", "\xd0\x94\xd1\x80\xd0\xb5\xd0\xb2\xd0\xba\xd0\xbe\xd0\xb2\xd0\xbe\xd0\xb5: \xd0\xbf\xd0\xbe\xd1\x80\xd0\xbe\xd0\xb3 \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0"),
            tr("With 'Apply weight to categories' on: weapons of this category lighter than this combat weight only dodge (no block). 0 = no filter.", "\xd0\x9f\xd1\x80\xd0\xb8 \xd0\xb2\xd0\xba\xd0\xbb\xd1\x8e\xd1\x87\xd1\x91\xd0\xbd\xd0\xbd\xd0\xbe\xd0\xb9 \xc2\xab\xd0\x9f\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x8f\xd1\x82\xd1\x8c \xd0\xb2\xd0\xb5\xd1\x81 \xd0\xba \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd1\x8f\xd0\xbc\xc2\xbb: \xd0\xbe\xd1\x80\xd1\x83\xd0\xb6\xd0\xb8\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb9 \xd0\xba\xd0\xb0\xd1\x82\xd0\xb5\xd0\xb3\xd0\xbe\xd1\x80\xd0\xb8\xd0\xb8 \xd0\xbb\xd0\xb5\xd0\xb3\xd1\x87\xd0\xb5 \xd1\x8d\xd1\x82\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb1\xd0\xbe\xd0\xb5\xd0\xb2\xd0\xbe\xd0\xb3\xd0\xbe \xd0\xb2\xd0\xb5\xd1\x81\xd0\xb0 \xd1\x82\xd0\xbe\xd0\xbb\xd1\x8c\xd0\xba\xd0\xbe \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd1\x8f\xd0\xb5\xd1\x82\xd1\x81\xd1\x8f (\xd0\xb1\xd0\xb5\xd0\xb7 \xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0). 0 = \xd0\xb1\xd0\xb5\xd0\xb7 \xd1\x84\xd0\xb8\xd0\xbb\xd1\x8c\xd1\x82\xd1\x80\xd0\xb0."),
            0, 50, 1, &g_state.cat_weight[5]);
    }

    regBool(api, apiSize, mod, "debug_logging",
        tr("Diagnostic logging", "\xd0\x94\xd0\xb8\xd0\xb0\xd0\xb3\xd0\xbd\xd0\xbe\xd1\x81\xd1\x82\xd0\xb8\xd1\x87\xd0\xb5\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9 \xd0\xbb\xd0\xbe\xd0\xb3"),
        tr("Write RangedDefence_log.txt.", "\xd0\x9f\xd0\xb8\xd1\x81\xd0\xb0\xd1\x82\xd1\x8c RangedDefence_log.txt."),
        &g_state.debug_logging);

    regBool(api, apiSize, mod, "play_animations",
        tr("Play animation (experimental)", "\xd0\x90\xd0\xbd\xd0\xb8\xd0\xbc\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8f (\xd1\x8d\xd0\xba\xd1\x81\xd0\xbf\xd0\xb5\xd1\x80\xd0\xb8\xd0\xbc\xd0\xb5\xd0\xbd\xd1\x82)"),
        tr("Play the dodge/block animation.", "\xd0\x9f\xd1\x80\xd0\xbe\xd0\xb8\xd0\xb3\xd1\x80\xd1\x8b\xd0\xb2\xd0\xb0\xd1\x82\xd1\x8c \xd0\xb0\xd0\xbd\xd0\xb8\xd0\xbc\xd0\xb0\xd1\x86\xd0\xb8\xd1\x8e \xd1\x83\xd0\xba\xd0\xbb\xd0\xbe\xd0\xbd\xd0\xb0/\xd0\xb1\xd0\xbb\xd0\xbe\xd0\xba\xd0\xb0."),
        &g_state.play_animations);

    { std::ostringstream o; o << "ModHub: registered all rows (lang=" << (g_lang == LANG_RU ? "ru" : "en")
                              << ", namespace=emkej.qol)."; rdlog(o.str()); }
    return true;
}

// Retry driver. Called every frame from the frame hook (and, as a secondary path, from iShotYou in
// case the frame hook did not install). Attempts registration at most once per HUB_THROTTLE_FRAMES
// until it succeeds or the retry budget is exhausted — making attach independent of whether the hub
// loaded before or after us. Once g_hubState != 0 this is a single int read (effectively free).
static void HubTick() {
    if (g_hubState != 0) return;                     // already registered, or gave up
    if (++g_hubFrameAcc < HUB_THROTTLE_FRAMES) return;
    g_hubFrameAcc = 0;
    if (++g_hubTries > HUB_MAX_TRIES) {
        g_hubState = -1;
        rdlog("\x4d\x6f\x64\x48\x75\x62\x3a\x20\x67\x61\x76\x65\x20\x75\x70\x20\xe2\x80\x94\x20\x45\x4d\x43\x5f\x4d\x6f\x64\x48\x75\x62\x5f\x47\x65\x74\x41\x70\x69\x20\x65\x78\x70\x6f\x72\x74\x20\x6e\x6f\x74\x20\x66\x6f\x75\x6e\x64\x20\x69\x6e\x20\x61\x6e\x79\x20\x6c\x6f\x61\x64\x65\x64\x20\x6d\x6f\x64\x75\x6c\x65\x20\x28\x68\x75\x62\x20\x6e\x6f\x74\x20\x69\x6e\x73\x74\x61\x6c\x6c\x65\x64\x2f\x6c\x6f\x61\x64\x65\x64\x3f\x29\x2e\x20\x46\x69\x6c\x65\x2d\x63\x6f\x6e\x66\x69\x67\x20\x73\x74\x69\x6c\x6c\x20\x61\x70\x70\x6c\x69\x65\x73\x2e");
        return;
    }
    if (DoRegisterModHub()) g_hubState = g_hubHardFail ? -1 : 1;   // success (1) or non-retryable failure (-1); else keep retrying
}

// ----------------------------------------------------------------------------
//  Game version / platform detection (Steam vs GOG). Log-only: the iShotYou hook is symbol-based
//  (cross-version via KenshiLib), and the RVA helpers are gated by the RVA canary below — so on a
//  non-matching build the mod does not crash, it just disables RVA-only features (dodge still works).
//  GetKenshiVersion() can fault on some builds, so the actual call is wrapped in SEH under MSVC.
//  (SEH must live in a function with no unwinding C++ objects -> the _impl/_fault split avoids C2712.)
// ----------------------------------------------------------------------------
static void DetectGameVersion_impl() {
    KenshiLib::BinaryVersion gv = KenshiLib::GetKenshiVersion();
    std::ostringstream o; o << "game version: " << gv.GetPlatformStr() << " " << gv.GetVersion();
    rdlog(o.str());
}
static void DetectGameVersion_fault() { rdlog("game version: detection faulted (ignored)"); }
static void DetectGameVersion() {
#ifdef _MSC_VER
    __try { DetectGameVersion_impl(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { DetectGameVersion_fault(); }
#else
    DetectGameVersion_impl();
#endif
}

// ----------------------------------------------------------------------------
//  Entry point
// ----------------------------------------------------------------------------
// NB: NO extern "C". RE_Kenshi's loader (Plugins.cpp) resolves the entry point by its C++ *mangled*
// name — GetProcAddress(plugin, "?startPlugin@@YAXXZ") — which is what MSVC emits for a plain global
// `void __cdecl startPlugin(void)`. Declaring it extern "C" exports the undecorated name "startPlugin"
// instead, so RE_Kenshi never finds it and never calls it (no log, no hooks, mod invisible). Match the
// canonical KenshiLib form (HelloWorld / Emkejs-Mod-Core): __declspec(dllexport) void startPlugin().
__declspec(dllexport) void startPlugin() {
    LoadConfig();
    SaveConfig();                                   // ensure the file exists with all keys
    DetectLanguage();                               // resolve UI language before registering Mod Hub rows

    // Log in the GAME ROOT (process cwd = game dir, same place RE_Kenshi reads settings.cfg): this is
    // writable even on a Steam Workshop subscription, where the mod's own folder can be read-only.
    g_log = new std::ofstream("RangedDefence_log.txt", std::ios::out | std::ios::trunc);
    rdlog("RangedDefence v12 GOG loaded.");
    { std::ostringstream o; o << "language: pref=" << g_langPref << " -> " << (g_lang == LANG_RU ? "ru" : "en"); rdlog(o.str()); }
    DetectGameVersion();                             // log Steam/GOG + version (diagnostics; see note above)

    // Detect the build by the ACTUAL RVA of a reference function (CharStats::calculateDodgeChance),
    // then pick that build's whole RVA set. Steam 1.0.65 and GOG 1.0.65 share identical class/member
    // layouts (so OFF_isAttacking / OFF_isUsingTurret carry over) — only the function RVAs differ.
    // Any other build -> no set -> dodge-only fallback (still safe, no crash).
    struct RvaSet { size_t dodgeRef, block, weight, cat, handNull, stumbling, numOpp, playAction, estAnim, frame; };
    static const RvaSet SET_STEAM = { 0x884EB0, 0x885400, 0x882890, 0x5C6EC0, 0x36AFC0, 0x334F30, 0x2B3000, 0x51F920, 0x5B19C0, 0x787E70 };
    static const RvaSet SET_GOG   = { 0x8845D0, 0x884B20, 0x881FB0, 0x5C71D0, 0x36ABE0, 0x334AC0, 0x2B2B90, 0x51FC30, 0x5B1CD0, 0x7877A0 };
    uintptr_t base     = rd_rva(0);
    uintptr_t dodgeOff = (uintptr_t)KenshiLib::GetRealAddress(&CharStats::calculateDodgeChance) - base;
    const RvaSet* S = (dodgeOff == SET_STEAM.dodgeRef) ? &SET_STEAM
                    : (dodgeOff == SET_GOG.dodgeRef)   ? &SET_GOG   : (const RvaSet*)0;
    const char* rvaPlat = (S == &SET_STEAM) ? "Steam 1.0.65" : (S == &SET_GOG) ? "GOG 1.0.65" : "unknown";
    bool rvaOK = (S != 0);
    { std::ostringstream o; o << "RVA set: dodgeOff=" << (void*)dodgeOff << " -> " << rvaPlat << " (rvaOK=" << (int)rvaOK << ")"; rdlog(o.str()); }
    if (rvaOK) {
        _blockChance  = (blockChanceFn) rd_rva(S->block);
        _combatWeight = (combatWeightFn)rd_rva(S->weight);
        _category     = (categoryFn)    rd_rva(S->cat);
        _handIsNull   = (handIsNullFn)  rd_rva(S->handNull);    // hand::isNull
        _stumbling    = (stumblingFn)   rd_rva(S->stumbling);   // AnimationClass::currentlyStumbling
        _numOpponents = (numOppFn)      rd_rva(S->numOpp);      // CombatClass::getNumOpponents
        _playAction   = (playActionFn)  rd_rva(S->playAction);  // AnimationClass::playAction(string,...)
        _estAnim      = (estAnimFn)     rd_rva(S->estAnim);     // AnimationClass::estimateAnimationTime
    } else {
        rdlog("RVA set: unrecognised build -> block/category/weight/turret disabled; dodge-only fallback.");
    }

    bool ok = (KenshiLib::SUCCESS == KenshiLib::AddHook(
        KenshiLib::GetRealAddress(&Character::iShotYou), &iShotYou_hook, &iShotYou_orig));
    rdlog(ok ? "hook installed: Character::iShotYou" : "HOOK FAIL: Character::iShotYou");
    if (!ok) OutputDebugStringA("RangedDefence: failed to hook Character::iShotYou\n");

    // GAME-time clock: hook the per-frame update by RVA (no GameWorld.h needed). If it fails
    // (e.g. another mod already hooked it), fall back to real-time so commitment still works.
    if (rvaOK) {
        bool okf = (KenshiLib::SUCCESS == KenshiLib::AddHook((intptr_t)rd_rva(S->frame), &frame_hook, &frame_orig));
        g_useGameClock = okf;
        rdlog(okf ? "frame clock hook installed (game-time windows active)"
                  : "frame clock hook FAILED -> real-time fallback for commitment windows");
    } else {
        rdlog("RVA canary off -> real-time fallback for commitment windows");
    }

    // Try to attach to the Mod Hub now (common case: the hub is already loaded). If it is not yet
    // loaded, HubTick() from the frame hook / iShotYou keeps retrying until it is — so visibility no
    // longer depends on load order or on the hub DLL's exact filename.
    if (DoRegisterModHub()) {
        g_hubState = g_hubHardFail ? -1 : 1;
    } else {
        rdlog("ModHub: not attached at startup (hub not loaded yet) -> will retry each frame.");
    }
    srand((unsigned)GetTickCount());
}
