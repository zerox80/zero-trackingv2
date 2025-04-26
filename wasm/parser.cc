/***********************************************************************
 *  Filter-List → Chrome DNR-JSON Parser  (WASM)
 *  – überarbeitete Version 2025-04-26
 *
 *  Wichtige Änderungen ggü. Original:
 *    1. Regex-Regeln werden jetzt unterstützt.
 *    2. Unsinnige Operationen (z. B. replace '^'→'^') entfernt.
 *    3. Temporäre Kopien reduziert – weite Teile arbeiten mit
 *       std::string_view.
 *    4. Kleinere Logik-Bugs behoben (leere resourceTypes, Negation).
 ***********************************************************************/

 #include <algorithm>
 #include <cctype>
 #include <optional>
 #include <sstream>
 #include <string>
 #include <string_view>
 #include <unordered_map>
 #include <unordered_set>
 #include <vector>
 
 #include "nlohmann/json.hpp"
 #include <emscripten/bind.h>
 
 using json = nlohmann::json;
 
 /* ------------------------------------------------------------------ *
  *  Hilfs-Utilities
  * ------------------------------------------------------------------ */
 
 constexpr std::string_view WHITESPACE = " \t\r\n";
 
 inline std::string_view trim(std::string_view sv) {
     const auto begin = sv.find_first_not_of(WHITESPACE);
     if (begin == std::string_view::npos) return {};
     const auto end = sv.find_last_not_of(WHITESPACE);
     return sv.substr(begin, end - begin + 1);
 }
 
 template <char Delim, typename Callback>
 inline void split_sv(std::string_view sv, Callback &&cb) {
     size_t start = 0;
     for (size_t i = 0; i < sv.size(); ++i) {
         if (sv[i] == Delim) {
             cb(sv.substr(start, i - start));
             start = i + 1;
         }
     }
     if (start < sv.size()) cb(sv.substr(start));
 }
 
 inline std::vector<std::string>
 to_string_vector_unique(const std::vector<std::string_view> &views) {
     std::vector<std::string> out;
     std::unordered_set<std::string_view> seen;
     out.reserve(views.size());
     for (auto v : views) {
         v = trim(v);
         if (!v.empty() && !seen.count(v)) {
             out.emplace_back(v);
             seen.insert(v);
         }
     }
     return out;
 }
 
 /* ------------------------------------------------------------------ *
  *  Konstanten
  * ------------------------------------------------------------------ */
 
 const std::vector<std::string> ALL_DNR_RESOURCE_TYPES = {
     "main_frame",  "sub_frame", "stylesheet",   "script",  "image",
     "font",        "object",    "xmlhttprequest","ping",    "csp_report",
     "media",       "websocket", "webtransport", "webbundle","other"};
 
 const std::unordered_map<std::string_view, std::string> RESOURCE_TYPE_MAP = {
     {"script", "script"},
     {"image", "image"},           {"img", "image"},
     {"stylesheet", "stylesheet"},
     {"xmlhttprequest", "xmlhttprequest"}, {"xhr", "xmlhttprequest"},
     {"subdocument", "sub_frame"}, {"sub_frame", "sub_frame"},
     {"document", "main_frame"},   {"main_frame", "main_frame"},
     {"websocket", "websocket"},   {"media", "media"},
     {"font", "font"},             {"ping", "ping"},
     {"other", "other"}};
 
 const std::unordered_set<std::string> SUPPORTED_METHODS = {
     "connect", "delete", "get", "head",
     "options", "patch",  "post", "put"};
 
 /* ------------------------------------------------------------------ *
  *  Datenstrukturen
  * ------------------------------------------------------------------ */
 
 struct DnrRule {
     int id;
     int priority                 = 1;
     std::string actionType       = "block";
 
     std::optional<std::string> conditionUrlFilter;
     std::optional<std::string> conditionRegexFilter;
 
     std::optional<std::vector<std::string>> conditionResourceTypes;
     std::optional<std::vector<std::string>> conditionRequestDomains;
     std::optional<std::vector<std::string>> conditionExcludedRequestDomains;
     std::optional<std::vector<std::string>> conditionInitiatorDomains;
     std::optional<std::vector<std::string>> conditionExcludedInitiatorDomains;
     std::optional<std::vector<std::string>> conditionRequestMethods;
     std::optional<std::vector<std::string>> conditionExcludedRequestMethods;
 };
 
 /* ------------------------------------------------------------------ *
  *  Optionen-Parser
  * ------------------------------------------------------------------ */
 
 static void parse_domain_option(
         std::string_view value,
         std::vector<std::string_view> &includes,
         std::vector<std::string_view> &excludes) {
 
     split_sv<'|'>(value, [&](std::string_view sub) {
         sub = trim(sub);
         if (sub.empty()) return;
         if (sub.starts_with('~')) {
             if (sub.size() > 1) excludes.emplace_back(sub.substr(1));
         } else {
             includes.emplace_back(sub);
         }
     });
 }
 
 static void parse_options(std::string_view options_sv, DnrRule &rule) {
     // Sammelcontainer
     std::vector<std::string_view> initiatorInc, initiatorExc;
     std::vector<std::string_view> requestInc,   requestExc;
     std::unordered_set<std::string> methodsInc, methodsExc;
     std::unordered_set<std::string> resTypesInc, resTypesExc;
 
     split_sv<','>(options_sv, [&](std::string_view opt) {
         opt = trim(opt);
         if (opt.empty()) return;
 
         bool neg = opt.starts_with('~');
         std::string_view keyval = neg ? opt.substr(1) : opt;
 
         std::string_view key = keyval;
         std::string_view val;
         const size_t eq = keyval.find('=');
         if (eq != std::string_view::npos) {
             key = keyval.substr(0, eq);
             val = trim(keyval.substr(eq + 1));
         }
 
         // 1. Ressource-Typ?
         if (auto it = RESOURCE_TYPE_MAP.find(key); it != RESOURCE_TYPE_MAP.end()) {
             (neg ? resTypesExc : resTypesInc).insert(it->second);
             return;
         }
 
         // 2. Domains
         if (key == "domain") {                      // initiator
             parse_domain_option(val, initiatorInc, initiatorExc);
             return;
         }
         if (key == "domains") {                     // request
             parse_domain_option(val, requestInc, requestExc);
             return;
         }
 
         // 3. Methoden
         if (key == "method" || key == "request-method") {
             split_sv<'|'>(val, [&](std::string_view m) {
                 m = trim(m);
                 if (m.empty()) return;
                 std::string low(m);
                 std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                 if (!SUPPORTED_METHODS.count(low)) return;
 
                 std::string up = low;
                 std::transform(up.begin(), up.end(), up.begin(), ::toupper);
                 (neg ? methodsExc : methodsInc).insert(std::move(up));
             });
             return;
         }
 
         // 4. Ignorierte Schlüssel (third-party/first-party etc.) -> noop
     });
 
     /* -- Resultate in Rule schreiben -------------------------------- */
 
     // Ressourcentypen
     if (!resTypesInc.empty()) {
         rule.conditionResourceTypes = {resTypesInc.begin(), resTypesInc.end()};
     } else if (!resTypesExc.empty()) {
         std::vector<std::string> final;
         final.reserve(ALL_DNR_RESOURCE_TYPES.size());
         for (const auto &t : ALL_DNR_RESOURCE_TYPES)
             if (!resTypesExc.count(t)) final.push_back(t);
         if (!final.empty() && final.size() < ALL_DNR_RESOURCE_TYPES.size())
             rule.conditionResourceTypes = std::move(final);
     }
 
     // Initiator- / Request-Domains
     if (!initiatorInc.empty())
         rule.conditionInitiatorDomains = to_string_vector_unique(initiatorInc);
     if (!initiatorExc.empty()) {
         if (rule.conditionInitiatorDomains) rule.conditionInitiatorDomains.reset();
         rule.conditionExcludedInitiatorDomains =
             to_string_vector_unique(initiatorExc);
     }
 
     if (!requestInc.empty())
         rule.conditionRequestDomains = to_string_vector_unique(requestInc);
     if (!requestExc.empty()) {
         if (rule.conditionRequestDomains) rule.conditionRequestDomains.reset();
         rule.conditionExcludedRequestDomains =
             to_string_vector_unique(requestExc);
     }
 
     // Methoden
     if (!methodsInc.empty()) {
         rule.conditionRequestMethods = {methodsInc.begin(), methodsInc.end()};
         std::sort(rule.conditionRequestMethods->begin(),
                   rule.conditionRequestMethods->end());
     }
     if (!methodsExc.empty()) {
         if (rule.conditionRequestMethods) rule.conditionRequestMethods.reset();
         rule.conditionExcludedRequestMethods =
             {methodsExc.begin(), methodsExc.end()};
         std::sort(rule.conditionExcludedRequestMethods->begin(),
                   rule.conditionExcludedRequestMethods->end());
     }
 }
 
 /* ------------------------------------------------------------------ *
  *  Einzelne Zeile parsen
  * ------------------------------------------------------------------ */
 
 static std::optional<DnrRule> parse_line(std::string_view line, int id) {
     line = trim(line);
     if (line.empty() || line.starts_with('!') || line.starts_with('[')) return {};
 
     // Cosmetic/HTML-Regeln überspringen
     if (line.find("##") != std::string_view::npos ||
         line.find("#?#") != std::string_view::npos ||
         line.find("#$#") != std::string_view::npos ||
         line.find("#@#") != std::string_view::npos)
         return {};
 
     DnrRule rule;
     rule.id = id;
 
     /* -------- Ausnahme-Regel? (allow) ----------------------------- */
     if (line.starts_with("@@")) {
         rule.actionType = "allow";
         rule.priority   = 2;
         line            = trim(line.substr(2));
         if (line.empty()) return {};
     }
 
     /* -------- $-Optionen abtrennen -------------------------------- */
     std::string_view filterPart = line, optionsPart;
     const size_t posDollar      = line.find('$');
     if (posDollar != std::string_view::npos) {
         filterPart  = line.substr(0, posDollar);
         optionsPart = line.substr(posDollar + 1);
     }
     filterPart = trim(filterPart);
     if (filterPart.empty()) return {};
 
     /* -------- Regex erkennen -------------------------------------- */
     if (filterPart.size() > 2 && filterPart.front() == '/' &&
         filterPart.back() == '/' && !filterPart.starts_with("||")) {
 
         rule.conditionRegexFilter =
             std::string(filterPart.substr(1, filterPart.size() - 2));
     } else { /* ---- URL-Filter konstruieren ------------------------ */
         if (filterPart.starts_with("||") && filterPart.ends_with('^')) {
             std::string_view domain = filterPart.substr(2, filterPart.size() - 3);
             if (!domain.empty() && domain.find('/') == std::string_view::npos &&
                 domain.find('*') == std::string_view::npos) {
                 rule.conditionUrlFilter = "||" + std::string(domain) + "/";
             } else
                 return {};
         } else if (filterPart.starts_with("||")) {
             std::string_view domain = filterPart.substr(2);
             if (!domain.empty() && domain.find('/') == std::string_view::npos &&
                 domain.find('*') == std::string_view::npos) {
                 rule.conditionUrlFilter = "||" + std::string(domain) + "^";
             } else
                 return {};
         } else {
             const bool startAnchor = filterPart.starts_with('|');
             const bool endAnchor   = filterPart.ends_with('|');
 
             std::string_view core = filterPart;
             if (startAnchor) core = core.substr(1);
             if (endAnchor && core.size()) core = core.substr(0, core.size() - 1);
 
             std::string urlFilter;
             urlFilter.reserve(core.size() + 2);
             if (!startAnchor) urlFilter.push_back('*');
             urlFilter.append(core);
             if (!endAnchor) urlFilter.push_back('*');
 
             if (urlFilter == "*" || urlFilter == "**") {
                 rule.conditionUrlFilter = "*";
             } else {
                 rule.conditionUrlFilter = std::move(urlFilter);
             }
         }
     }
 
     /* -------- Optionen verarbeiten -------------------------------- */
     if (!optionsPart.empty()) parse_options(optionsPart, rule);
 
     /* -------- Mindest-Konditionen erfüllt? ------------------------ */
     const bool hasCondition =
         rule.conditionUrlFilter.has_value() ||
         rule.conditionRegexFilter.has_value() ||
         rule.conditionResourceTypes.has_value() ||
         rule.conditionRequestDomains.has_value() ||
         rule.conditionExcludedRequestDomains.has_value() ||
         rule.conditionInitiatorDomains.has_value() ||
         rule.conditionExcludedInitiatorDomains.has_value() ||
         rule.conditionRequestMethods.has_value() ||
         rule.conditionExcludedRequestMethods.has_value();
 
     if (!hasCondition) return {};
 
     // allow-Regeln brauchen laut Chrome-DNR entweder url/regexFilter ODER
     // domains. Wenn beides fehlt, verwerfen.
     if (rule.actionType == "allow" &&
         !rule.conditionUrlFilter && !rule.conditionRegexFilter &&
         !rule.conditionRequestDomains)
         return {};
 
     return rule;
 }
 
 /* ------------------------------------------------------------------ *
  *  Serialisierung
  * ------------------------------------------------------------------ */
 
 static json rule_to_json(const DnrRule &r) {
     json j;
     j["id"]       = r.id;
     j["priority"] = r.priority;
     j["action"]   = {{"type", r.actionType}};
 
     json cond = json::object();
     if (r.conditionRegexFilter)      cond["regexFilter"]          = *r.conditionRegexFilter;
     if (r.conditionUrlFilter)        cond["urlFilter"]            = *r.conditionUrlFilter;
     if (r.conditionResourceTypes)    cond["resourceTypes"]        = *r.conditionResourceTypes;
     if (r.conditionRequestDomains)   cond["requestDomains"]       = *r.conditionRequestDomains;
     if (r.conditionExcludedRequestDomains)
         cond["excludedRequestDomains"] = *r.conditionExcludedRequestDomains;
     if (r.conditionInitiatorDomains) cond["initiatorDomains"]     = *r.conditionInitiatorDomains;
     if (r.conditionExcludedInitiatorDomains)
         cond["excludedInitiatorDomains"] = *r.conditionExcludedInitiatorDomains;
     if (r.conditionRequestMethods)   cond["requestMethods"]       = *r.conditionRequestMethods;
     if (r.conditionExcludedRequestMethods)
         cond["excludedRequestMethods"] = *r.conditionExcludedRequestMethods;
 
     if (!cond.empty()) j["condition"] = std::move(cond);
     return j;
 }
 
 /* ------------------------------------------------------------------ *
  *  Haupt-Entry-Point für JavaScript (WASM)
  * ------------------------------------------------------------------ */
 
 std::string parseFilterListWasm(std::string filterListText) {
     json rules = json::array();
 
     int   totalLines     = 0;
     int   skippedLines   = 0;
     int   processedRules = 0;
     int   id             = 1;
 
     std::stringstream ss(std::move(filterListText));
     std::string        buf;
     while (std::getline(ss, buf, '\n')) {
         ++totalLines;
         if (auto rule = parse_line(buf, id)) {
             rules.push_back(rule_to_json(*rule));
             ++processedRules;
             ++id;
         } else {
             ++skippedLines;
         }
     }
 
     json out;
     out["rules"] = std::move(rules);
     out["stats"] = {{"totalLines", totalLines},
                     {"processedRules", processedRules},
                     {"skippedLines", skippedLines}};
 
     return out.dump(-1, ' ', false, json::error_handler_t::ignore);
 }
 
 /* ------------------------------------------------------------------ *
  *  EMSCRIPTEN-Binding
  * ------------------------------------------------------------------ */
 EMSCRIPTEN_BINDINGS(filter_parser_module) {
     emscripten::function("parseFilterListWasm", &parseFilterListWasm);
 }
