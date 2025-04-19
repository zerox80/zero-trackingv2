#include <string>
#include <vector>
#include <sstream>
#include <string_view>
#include <optional>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <cctype>

#include "nlohmann/json.hpp"
#include <emscripten/bind.h>

using json = nlohmann::json;

struct DnrRule {
    int id;
    int priority = 1;
    std::string actionType = "block";
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

std::string_view trim(std::string_view sv) {
    auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return {};
    auto end = sv.find_last_not_of(" \t\r\n");
    return sv.substr(start, end - start + 1);
}

std::vector<std::string_view> split(std::string_view sv, char delimiter) {
    std::vector<std::string_view> result;
    size_t start = 0;
    for (size_t i = 0; i < sv.length(); ++i) {
        if (sv[i] == delimiter) {
             if (i > start) result.push_back(sv.substr(start, i - start));
            start = i + 1;
        }
    }
     if (sv.length() > start) result.push_back(sv.substr(start));
    return result;
}

std::vector<std::string> toStringVectorDeduplicated(const std::vector<std::string_view>& sv_vec) {
    std::vector<std::string> str_vec;
    std::unordered_set<std::string_view> seen;
    str_vec.reserve(sv_vec.size());
    for(const auto& sv : sv_vec) {
        auto trimmed_sv = trim(sv);
        if (!trimmed_sv.empty() && seen.find(trimmed_sv) == seen.end()) {
             str_vec.emplace_back(trimmed_sv);
             seen.insert(trimmed_sv);
        }
    }
    return str_vec;
}

const std::vector<std::string> ALL_DNR_RESOURCE_TYPES = {
    "main_frame", "sub_frame", "stylesheet", "script", "image",
    "font", "object", "xmlhttprequest", "ping", "csp_report",
    "media", "websocket", "webtransport", "webbundle", "other"
};
const std::unordered_set<std::string> ALL_DNR_RESOURCE_TYPES_SET(ALL_DNR_RESOURCE_TYPES.begin(), ALL_DNR_RESOURCE_TYPES.end());

const std::unordered_map<std::string_view, std::string> RESOURCE_TYPE_MAP = {
    {"script", "script"},
    {"image", "image"}, {"img", "image"},
    {"stylesheet", "stylesheet"},
    {"xmlhttprequest", "xmlhttprequest"}, {"xhr", "xmlhttprequest"},
    {"subdocument", "sub_frame"}, {"sub_frame", "sub_frame"},
    {"document", "main_frame"}, {"main_frame", "main_frame"},
    {"websocket", "websocket"},
    {"media", "media"},
    {"font", "font"},
    {"other", "other"},
    {"ping", "ping"},
};

const std::unordered_set<std::string> SUPPORTED_METHODS = {
    "connect", "delete", "get", "head", "options", "patch", "post", "put"
};

void parseDomainOption(std::string_view value,
                       std::vector<std::string_view>& includedDomains,
                       std::vector<std::string_view>& excludedDomains) {
    auto domains = split(value, '|');
    for (const auto& domain_sv : domains) {
        auto trimmed_domain = trim(domain_sv);
        if (trimmed_domain.empty()) continue;
        if (trimmed_domain.starts_with("~")) {
             if (trimmed_domain.length() > 1) {
                excludedDomains.push_back(trimmed_domain.substr(1));
            }
        } else {
            includedDomains.push_back(trimmed_domain);
        }
    }
}

void parseOptions(std::string_view options_sv, DnrRule& rule) {
    std::unordered_set<std::string_view> initiatorDomainsIncludeSet;
    std::unordered_set<std::string_view> initiatorDomainsExcludeSet;
    std::unordered_set<std::string_view> requestDomainsIncludeSet;
    std::unordered_set<std::string_view> requestDomainsExcludeSet;
    std::unordered_set<std::string> requestMethodsIncludeSet;
    std::unordered_set<std::string> requestMethodsExcludeSet;
    std::unordered_set<std::string> specifiedResourceTypes;
    std::unordered_set<std::string> excludedResourceTypes;

    auto options = split(options_sv, ',');

    for (const auto& opt_sv : options) {
        auto trimmed_opt = trim(opt_sv);
        if (trimmed_opt.empty()) continue;

        std::string_view key = trimmed_opt;
        std::string_view value;
        size_t eq_pos = trimmed_opt.find('=');
        if (eq_pos != std::string_view::npos) {
            key = trimmed_opt.substr(0, eq_pos);
            value = trim(trimmed_opt.substr(eq_pos + 1));
        }

        bool negated = key.starts_with("~");
        std::string_view typeKey = negated ? key.substr(1) : key;

        auto it_rt = RESOURCE_TYPE_MAP.find(typeKey);
        if (it_rt != RESOURCE_TYPE_MAP.end()) {
            if (negated) {
                excludedResourceTypes.insert(it_rt->second);
            } else {
                specifiedResourceTypes.insert(it_rt->second);
            }
        }
        else if (key == "domain") {
           std::vector<std::string_view> includes, excludes;
           parseDomainOption(value, includes, excludes);
           for(const auto& inc : includes) initiatorDomainsIncludeSet.insert(inc);
           for(const auto& exc : excludes) initiatorDomainsExcludeSet.insert(exc);
        }
        else if (key == "domains") {
             std::vector<std::string_view> includes, excludes;
             parseDomainOption(value, includes, excludes);
             for(const auto& inc : includes) requestDomainsIncludeSet.insert(inc);
             for(const auto& exc : excludes) requestDomainsExcludeSet.insert(exc);
        }
        else if (key == "method" || key == "request-method") {
            auto methods = split(value, '|');
            for (const auto& method_sv : methods) {
                 auto trimmed_method_sv = trim(method_sv);
                 if(trimmed_method_sv.empty()) continue;

                 std::string lower_method(trimmed_method_sv);
                 std::transform(lower_method.begin(), lower_method.end(), lower_method.begin(), ::tolower);

                 if (SUPPORTED_METHODS.count(lower_method)) {
                     std::string upper_method = lower_method;
                     std::transform(upper_method.begin(), upper_method.end(), upper_method.begin(), ::toupper);

                     if (negated) {
                         requestMethodsExcludeSet.insert(std::move(upper_method));
                     } else {
                         requestMethodsIncludeSet.insert(std::move(upper_method));
                     }
                 }
            }
        }
        else if (key == "third-party" || key == "first-party") {
            // Ignore
        }
    }

    if (!specifiedResourceTypes.empty()) {
        rule.conditionResourceTypes = std::vector<std::string>(specifiedResourceTypes.begin(), specifiedResourceTypes.end());
    } else if (!excludedResourceTypes.empty()) {
        std::vector<std::string> finalTypes;
        for (const auto& type : ALL_DNR_RESOURCE_TYPES) {
            if (excludedResourceTypes.find(type) == excludedResourceTypes.end()) {
                finalTypes.push_back(type);
            }
        }
         if (!finalTypes.empty() && finalTypes.size() < ALL_DNR_RESOURCE_TYPES.size()) {
            rule.conditionResourceTypes = std::move(finalTypes);
        }
    }

    std::vector<std::string_view> initiatorIncludeVec(initiatorDomainsIncludeSet.begin(), initiatorDomainsIncludeSet.end());
    std::vector<std::string_view> initiatorExcludeVec(initiatorDomainsExcludeSet.begin(), initiatorDomainsExcludeSet.end());
    if (!initiatorIncludeVec.empty()) {
        rule.conditionInitiatorDomains = toStringVectorDeduplicated(initiatorIncludeVec);
    }
    if (!initiatorExcludeVec.empty()) {
        if (rule.conditionInitiatorDomains.has_value()) rule.conditionInitiatorDomains.reset();
        rule.conditionExcludedInitiatorDomains = toStringVectorDeduplicated(initiatorExcludeVec);
    }

    std::vector<std::string_view> requestIncludeVec(requestDomainsIncludeSet.begin(), requestDomainsIncludeSet.end());
    std::vector<std::string_view> requestExcludeVec(requestDomainsExcludeSet.begin(), requestDomainsExcludeSet.end());
     if (!requestIncludeVec.empty()) {
        rule.conditionRequestDomains = toStringVectorDeduplicated(requestIncludeVec);
    }
     if (!requestExcludeVec.empty()) {
        if (rule.conditionRequestDomains.has_value()) rule.conditionRequestDomains.reset();
        rule.conditionExcludedRequestDomains = toStringVectorDeduplicated(requestExcludeVec);
    }

    if (!requestMethodsIncludeSet.empty()) {
        rule.conditionRequestMethods = std::vector<std::string>(requestMethodsIncludeSet.begin(), requestMethodsIncludeSet.end());
         std::sort(rule.conditionRequestMethods->begin(), rule.conditionRequestMethods->end());
    }
     if (!requestMethodsExcludeSet.empty()) {
         if (rule.conditionRequestMethods.has_value()) rule.conditionRequestMethods.reset();
         rule.conditionExcludedRequestMethods = std::vector<std::string>(requestMethodsExcludeSet.begin(), requestMethodsExcludeSet.end());
          std::sort(rule.conditionExcludedRequestMethods->begin(), rule.conditionExcludedRequestMethods->end());
     }
}

std::optional<DnrRule> parseLine(std::string_view line, int ruleId) {
    line = trim(line);

    if (line.empty() || line.starts_with('!') || line.starts_with('[')) {
        return std::nullopt;
    }
    if (line.find("##") != std::string_view::npos ||
        line.find("#?#") != std::string_view::npos ||
        line.find("#$#") != std::string_view::npos ||
        line.find("#@#") != std::string_view::npos) {
        return std::nullopt;
    }

    DnrRule rule;
    rule.id = ruleId;

    if (line.starts_with("@@")) {
        rule.actionType = "allow";
        line = line.substr(2);
        rule.priority = 2;
        line = trim(line);
        if (line.empty()) return std::nullopt;
    }

    std::string_view filterPart = line;
    std::string_view optionsPart;
    size_t dollarPos = line.find('$');
    if (dollarPos != std::string_view::npos) {
        filterPart = line.substr(0, dollarPos);
        optionsPart = line.substr(dollarPos + 1);
    }
    filterPart = trim(filterPart);
    if (filterPart.empty()) return std::nullopt;

    bool useRegexFilter = false;
    if (filterPart.length() >= 2 && filterPart.starts_with('/') && filterPart.ends_with('/') && !filterPart.starts_with("||")) {
         // rule.conditionRegexFilter = std::string(filterPart.substr(1, filterPart.length() - 2));
         // useRegexFilter = true;
    }

    if (!useRegexFilter) {
        if (filterPart.starts_with("||") && filterPart.ends_with('^')) {
             std::string_view domain = filterPart.substr(2, filterPart.length() - 3);
             if (!domain.empty() && domain.find('/') == std::string_view::npos && domain.find('*') == std::string_view::npos) {
                rule.conditionUrlFilter = "||" + std::string(domain) + "/";
             } else { return std::nullopt; }
        } else if (filterPart.starts_with("||")) {
            std::string_view domain = filterPart.substr(2);
             if (!domain.empty() && domain.find('/') == std::string_view::npos && domain.find('*') == std::string_view::npos) {
                 rule.conditionUrlFilter = "||" + std::string(domain) + "^";
             } else { return std::nullopt; }
        } else {
            std::string urlFilterStr;
            bool startsWithAnchor = filterPart.starts_with('|');
            bool endsWithAnchor = filterPart.ends_with('|');

            std::string_view content = filterPart;
            if (startsWithAnchor) content = content.substr(1);
             if (endsWithAnchor && content.length() > 0 && content.back() == '|') {
                  content = content.substr(0, content.length() - 1);
                  endsWithAnchor = true;
             } else {
                 endsWithAnchor = false;
             }

            if (!startsWithAnchor) urlFilterStr += '*';
            urlFilterStr += content;
            if (!endsWithAnchor) urlFilterStr += '*';

            std::replace(urlFilterStr.begin(), urlFilterStr.end(), '^', '^');

             if (urlFilterStr.length() > 0 && urlFilterStr != "*" && urlFilterStr != "**") {
                 rule.conditionUrlFilter = std::move(urlFilterStr);
             } else if (urlFilterStr == "*") {
                 rule.conditionUrlFilter = "*";
             }
              else { return std::nullopt; }
        }
    }

    if (!optionsPart.empty()) {
        parseOptions(optionsPart, rule);
    }

    bool hasCondition = rule.conditionUrlFilter.has_value() ||
                        rule.conditionRegexFilter.has_value() ||
                        rule.conditionResourceTypes.has_value() ||
                        rule.conditionRequestDomains.has_value() ||
                        rule.conditionExcludedRequestDomains.has_value() ||
                        rule.conditionInitiatorDomains.has_value() ||
                        rule.conditionExcludedInitiatorDomains.has_value() ||
                        rule.conditionRequestMethods.has_value() ||
                        rule.conditionExcludedRequestMethods.has_value();

    if (!hasCondition) {
        return std::nullopt;
    }

    if (rule.actionType == "allow" &&
        !rule.conditionUrlFilter.has_value() &&
        !rule.conditionRegexFilter.has_value() &&
        !rule.conditionRequestDomains.has_value()
       )
    {
        if (!rule.conditionResourceTypes.has_value() && !rule.conditionInitiatorDomains.has_value()) {
             return std::nullopt;
        }
    }

    return rule;
}

json ruleToJson(const DnrRule& rule) {
    json j;
    j["id"] = rule.id;
    j["priority"] = rule.priority;
    j["action"] = { {"type", rule.actionType} };

    json condition = json::object();

    if (rule.conditionRegexFilter.has_value()) {
         condition["regexFilter"] = rule.conditionRegexFilter.value();
    } else if (rule.conditionUrlFilter.has_value()) {
        condition["urlFilter"] = rule.conditionUrlFilter.value();
    }

    if (rule.conditionResourceTypes.has_value()) {
        condition["resourceTypes"] = rule.conditionResourceTypes.value();
    }
    if (rule.conditionRequestDomains.has_value()) {
        condition["requestDomains"] = rule.conditionRequestDomains.value();
    }
    if (rule.conditionExcludedRequestDomains.has_value()) {
        condition["excludedRequestDomains"] = rule.conditionExcludedRequestDomains.value();
    }
    if (rule.conditionInitiatorDomains.has_value()) {
        condition["initiatorDomains"] = rule.conditionInitiatorDomains.value();
    }
     if (rule.conditionExcludedInitiatorDomains.has_value()) {
        condition["excludedInitiatorDomains"] = rule.conditionExcludedInitiatorDomains.value();
    }
     if (rule.conditionRequestMethods.has_value()) {
        condition["requestMethods"] = rule.conditionRequestMethods.value();
    }
     if (rule.conditionExcludedRequestMethods.has_value()) {
        condition["excludedRequestMethods"] = rule.conditionExcludedRequestMethods.value();
    }

    if (!condition.empty()) {
         j["condition"] = std::move(condition);
    }

    return j;
}

std::string parseFilterListWasm(std::string filterListText) {
    json jsonOutput = json::object();
    json jsonRules = json::array();
    json jsonStats = json::object();

    int totalLines = 0;
    int skippedLines = 0;
    int processedRules = 0;

    std::stringstream ss(filterListText);
    std::string line;
    int ruleId = 1;

    while (std::getline(ss, line, '\n')) {
        totalLines++;
        std::optional<DnrRule> parsedRule = parseLine(std::string_view(line), ruleId);

        if (parsedRule.has_value()) {
            json ruleJson = ruleToJson(parsedRule.value());
            if (ruleJson.contains("condition")) {
                jsonRules.push_back(std::move(ruleJson));
                ruleId++;
                processedRules++;
            } else {
                 skippedLines++;
            }
        } else {
            skippedLines++;
        }
    }

    jsonStats["totalLines"] = totalLines;
    jsonStats["processedRules"] = processedRules;
    jsonStats["skippedLines"] = skippedLines;

    jsonOutput["rules"] = std::move(jsonRules);
    jsonOutput["stats"] = std::move(jsonStats);

    return jsonOutput.dump(-1, ' ', false, json::error_handler_t::ignore);
}

EMSCRIPTEN_BINDINGS(filter_parser_module) {
    emscripten::function("parseFilterListWasm", &parseFilterListWasm);
}