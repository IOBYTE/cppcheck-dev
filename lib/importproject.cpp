/*
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2026 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "importproject.h"

#include "path.h"
#include "pathmatch.h"
#include "settings.h"
#include "standards.h"
#include "suppressions.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

#include "xml.h"

#include "json.h"


std::string ImportProject::collectArgs(const std::string &cmd, std::vector<std::string> &args)
{
    args.clear();

    std::string::size_type pos = 0;
    const std::string::size_type end = cmd.size();
    std::string arg;

    bool inDoubleQuotes = false;
    bool inSingleQuotes = false;

    while (pos < end) {
        char c = cmd[pos++];

        if (c == ' ') {
            if (inDoubleQuotes || inSingleQuotes) {
                arg.push_back(c);
                continue;
            }

            if (!arg.empty())
                args.push_back(arg);
            arg.clear();

            pos = cmd.find_first_not_of(' ', pos);

            continue;
        }

        if (c == '\"' && !inSingleQuotes) {
            inDoubleQuotes = !inDoubleQuotes;
            continue;
        }

        if (c == '\'' && !inDoubleQuotes) {
            inSingleQuotes = !inSingleQuotes;
            continue;
        }

        if (c == '\\' && !inSingleQuotes) {
            if (pos == end) {
                arg.push_back('\\');
                break;
            }

            c = cmd[pos++];

            if (!std::strchr("\\\"\' ", c))
                arg.push_back('\\');

            arg.push_back(c);
            continue;
        }

        arg.push_back(c);
    }

    if (inSingleQuotes || inDoubleQuotes)
        return "Missing closing quote in command string";

    if (!arg.empty())
        args.push_back(std::move(arg));

    return "";
}

void ImportProject::parseArgs(FileSettings &fs, const std::vector<std::string> &args)
{
    const auto getOptArg = [&args](std::initializer_list<std::string> optNames,
                                   std::size_t &i) {
        const auto &arg = args[i];
        const auto *const it = std::find_if(optNames.begin(),
                                            optNames.end(),
                                            [&arg] (const std::string &optName) {
            return startsWith(arg, optName);
        });

        if (it == optNames.end())
            return std::string();

        const std::size_t optLen = it->size();
        if (arg.size() == optLen)
            return ++i >= args.size() ? std::string() : args[i];

        return arg.substr(optLen);
    };

    std::string defs;
    for (std::size_t i = 0; i < args.size(); i++) {
        std::string optArg;

        if (!(optArg = getOptArg({ "-I", "/I" }, i)).empty()) {
            if (std::none_of(fs.includePaths.cbegin(), fs.includePaths.cend(),
                             [&](const std::string &path) {
                return path == optArg;
            }))
                fs.includePaths.push_back(std::move(optArg));
            continue;
        }

        if (!(optArg = getOptArg({ "-isystem" }, i)).empty()) {
            fs.systemIncludePaths.push_back(std::move(optArg));
            continue;
        }

        if (!(optArg = getOptArg({ "-include", "/FI", "-FI" }, i)).empty()) {
            fs.forcedIncludes.push_back(std::move(optArg));
            continue;
        }

        if (!(optArg = getOptArg({ "-D", "/D" }, i)).empty()) {
            defs += optArg + ";";
            continue;
        }

        if (!(optArg = getOptArg({ "-U", "/U" }, i)).empty()) {
            fs.undefs.insert(std::move(optArg));
            continue;
        }

        if (!(optArg = getOptArg({ "-std=", "/std:" }, i)).empty()) {
            fs.standard = std::move(optArg);
            continue;
        }

        if (!(optArg = getOptArg({ "-f" }, i)).empty()) {
            if (optArg == "pic")
                defs += "__pic__;";
            else if (optArg == "PIC")
                defs += "__PIC__;";
            else if (optArg == "pie")
                defs += "__pie__;";
            else if (optArg == "PIE")
                defs += "__PIE__;";
            continue;
        }

        if (!(optArg = getOptArg({ "-m" }, i)).empty()) {
            if (optArg == "unicode")
                defs += "UNICODE;";
            continue;
        }
    }

    fsSetDefines(fs, std::move(defs));
}

void ImportProject::ignorePaths(const std::vector<std::string> &ipaths, bool debug)
{
    PathMatch matcher(ipaths, Path::getCurrentPath());
    for (auto it = fileSettings.cbegin(); it != fileSettings.cend();) {
        if (matcher.match(it->filename())) {
            if (debug)
                std::cout << "ignored path: " << it->filename() << std::endl;
            it = fileSettings.erase(it);
        }
        else
            ++it;
    }
}

void ImportProject::ignoreOtherConfigs(const std::string &cfg)
{
    for (auto it = fileSettings.cbegin(); it != fileSettings.cend();) {
        if (it->cfg != cfg)
            it = fileSettings.erase(it);
        else
            ++it;
    }
}

void ImportProject::fsSetDefines(FileSettings& fs, std::string defs)
{
    while (defs.find(";%(") != std::string::npos) {
        const std::string::size_type pos1 = defs.find(";%(");
        const std::string::size_type pos2 = defs.find(';', pos1+1);
        defs.erase(pos1, pos2 == std::string::npos ? pos2 : (pos2-pos1));
    }
    while (defs.find(";;") != std::string::npos)
        defs.erase(defs.find(";;"),1);
    while (!defs.empty() && defs[0] == ';')
        defs.erase(0, 1);
    while (!defs.empty() && endsWith(defs,';'))
        defs.pop_back();
    bool eq = false;
    for (std::size_t pos = 0; pos < defs.size(); ++pos) {
        if (defs[pos] == '(' || defs[pos] == '=')
            eq = true;
        else if (defs[pos] == ';') {
            if (!eq) {
                defs.insert(pos,"=1");
                pos += 3;
            }
            if (pos < defs.size())
                eq = false;
        }
    }
    if (!eq && !defs.empty())
        defs += "=1";
    fs.defines.swap(defs);
}

// Find the ')' that matches the '(' at position parenPos, handling nested '$(' pairs.
static std::string::size_type findMatchingParen(const std::string &s, std::string::size_type parenPos)
{
    int depth = 0;
    for (std::string::size_type i = parenPos; i < s.size(); ++i) {
        if (s.compare(i, 2, "$(") == 0) {
            ++depth;
            ++i;  // skip the '(' on next iteration increment
        } else if (s[i] == ')') {
            if (depth == 0)
                return i;
            --depth;
        }
    }
    return std::string::npos;
}

// Apply an MSBuild property string method (ToLower, Replace, etc.).
// Used by both the condition evaluator and the property value expander.
static std::string applyPropertyMethod(std::string value,
                                       const std::string &method,
                                       const std::vector<std::string> &args)
{
    if (caseInsensitiveStringCompare(method, "ToUpper") == 0) {
        if (!args.empty())
            throw std::runtime_error("ToUpper takes no arguments");
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return std::toupper(c);
        });
        return value;
    }

    if (caseInsensitiveStringCompare(method, "ToLower") == 0) {
        if (!args.empty())
            throw std::runtime_error("ToLower takes no arguments");
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        return value;
    }

    if (caseInsensitiveStringCompare(method, "Contains") == 0) {
        if (args.size() != 1)
            throw std::runtime_error("Contains requires one argument");
        return value.find(args[0]) != std::string::npos ? "True" : "False";
    }

    if (caseInsensitiveStringCompare(method, "StartsWith") == 0) {
        if (args.size() != 1)
            throw std::runtime_error("StartsWith requires one argument");
        return startsWith(value, args[0]) ? "True" : "False";
    }

    if (caseInsensitiveStringCompare(method, "EndsWith") == 0) {
        if (args.size() != 1)
            throw std::runtime_error("EndsWith requires one argument");
        return endsWith(value, args[0].c_str(), args[0].size()) ? "True" : "False";
    }

    if (caseInsensitiveStringCompare(method, "Trim") == 0) {
        if (args.empty()) {
            const std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return "";
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            return value.substr(first, last - first + 1);
        }
        std::string chars;
        for (const std::string &arg : args)
            chars += arg;
        const std::size_t first = value.find_first_not_of(chars);
        if (first == std::string::npos)
            return "";
        const std::size_t last = value.find_last_not_of(chars);
        return value.substr(first, last - first + 1);
    }

    if (caseInsensitiveStringCompare(method, "TrimStart") == 0) {
        if (args.empty()) {
            const std::size_t first = value.find_first_not_of(" \t\r\n");
            return first == std::string::npos ? "" : value.substr(first);
        }
        std::string chars;
        for (const std::string &arg : args)
            chars += arg;
        const std::size_t first = value.find_first_not_of(chars);
        return first == std::string::npos ? "" : value.substr(first);
    }

    if (caseInsensitiveStringCompare(method, "TrimEnd") == 0) {
        if (args.empty()) {
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            return last == std::string::npos ? "" : value.substr(0, last + 1);
        }
        std::string chars;
        for (const std::string &arg : args)
            chars += arg;
        const std::size_t last = value.find_last_not_of(chars);
        return last == std::string::npos ? "" : value.substr(0, last + 1);
    }

    if (caseInsensitiveStringCompare(method, "Substring") == 0) {
        if (args.size() != 1 && args.size() != 2)
            throw std::runtime_error("Substring requires one or two arguments");
        char *end = nullptr;
        const long start = std::strtol(args[0].c_str(), &end, 10);
        if (end == args[0].c_str() || *end != '\0')
            throw std::runtime_error("Invalid Substring start index");
        if (start < 0 || static_cast<unsigned long>(start) > value.size())
            throw std::runtime_error("Substring start index out of range");
        const auto index = static_cast<std::size_t>(start);
        if (args.size() == 1)
            return value.substr(index);
        end = nullptr;
        const long length = std::strtol(args[1].c_str(), &end, 10);
        if (end == args[1].c_str() || *end != '\0')
            throw std::runtime_error("Invalid Substring length");
        if (length < 0 || static_cast<unsigned long>(length) > value.size() - index)
            throw std::runtime_error("Substring length out of range");
        return value.substr(index, static_cast<std::size_t>(length));
    }

    if (caseInsensitiveStringCompare(method, "Replace") == 0) {
        if (args.size() != 2)
            throw std::runtime_error("Replace requires two arguments");
        if (args[0].empty())
            throw std::runtime_error("Replace search string cannot be empty");
        std::size_t pos = 0;
        while ((pos = value.find(args[0], pos)) != std::string::npos) {
            value.replace(pos, args[0].size(), args[1]);
            pos += args[1].size();
        }
        return value;
    }

    if (caseInsensitiveStringCompare(method, "IndexOf") == 0) {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("IndexOf requires one or two arguments");
        std::size_t from = 0;
        if (args.size() == 2) {
            char *end = nullptr;
            const long idx = std::strtol(args[1].c_str(), &end, 10);
            if (end == args[1].c_str() || *end != '\0')
                throw std::runtime_error("Invalid IndexOf start index");
            if (idx < 0 || static_cast<unsigned long>(idx) > value.size())
                throw std::runtime_error("IndexOf start index out of range");
            from = static_cast<std::size_t>(idx);
        }
        const std::size_t found = value.find(args[0], from);
        return std::to_string(found == std::string::npos ? -1L : static_cast<long>(found));
    }

    if (caseInsensitiveStringCompare(method, "LastIndexOf") == 0) {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error("LastIndexOf requires one or two arguments");
        std::size_t from = std::string::npos;
        if (args.size() == 2) {
            char *end = nullptr;
            const long idx = std::strtol(args[1].c_str(), &end, 10);
            if (end == args[1].c_str() || *end != '\0')
                throw std::runtime_error("Invalid LastIndexOf start index");
            if (idx < 0 || static_cast<unsigned long>(idx) > value.size())
                throw std::runtime_error("LastIndexOf start index out of range");
            from = static_cast<std::size_t>(idx);
        }
        const std::size_t found = value.rfind(args[0], from);
        return std::to_string(found == std::string::npos ? -1L : static_cast<long>(found));
    }

    if (caseInsensitiveStringCompare(method, "PadLeft") == 0 ||
        caseInsensitiveStringCompare(method, "PadRight") == 0) {
        if (args.empty() || args.size() > 2)
            throw std::runtime_error(method + " requires one or two arguments");
        char *end = nullptr;
        const long totalWidth = std::strtol(args[0].c_str(), &end, 10);
        if (end == args[0].c_str() || *end != '\0' || totalWidth < 0)
            throw std::runtime_error(method + " totalWidth must be a non-negative integer");
        const char padChar = (args.size() == 2 && !args[1].empty()) ? args[1][0] : ' ';
        const auto width = static_cast<std::size_t>(totalWidth);
        if (value.size() >= width)
            return value;
        const std::string padding(width - value.size(), padChar);
        const bool left = caseInsensitiveStringCompare(method, "PadLeft") == 0;
        return left ? padding + value : value + padding;
    }

    throw std::runtime_error("Unhandled method '" + method + "'");
}

static std::string findFile(const std::string &startDirectory, const std::string &file)
{
    // startDirectory comes from MSBuildThisFileDirectory which is already
    // normalized to '/' separators by Path::simplifyPath.
    std::string currentDir = startDirectory;
    if (currentDir.back() == '/' && currentDir.size() > 1 && currentDir[currentDir.size() - 2] != ':')
        currentDir.pop_back();

    while (!currentDir.empty()) {
        std::string targetFile = Path::join(currentDir, file);
        if (Path::isFile(targetFile))
            return targetFile;
        if (currentDir.back() == '/' || (currentDir.back() == ':' && currentDir.size() == 2))
            break;
        size_t lastSlash = currentDir.rfind('/');
        if (lastSlash == std::string::npos)
            break;
        currentDir.resize(lastSlash);
    }

    return "";
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-format-attribute"
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif

template<typename ... Args>
static std::string safeFormat(const char *fmt, Args... args) {
    const int needed = std::snprintf(nullptr, 0, fmt, args ...);
    if (needed < 0)
        return std::string();
    std::vector<char> buf(static_cast<std::size_t>(needed) + 1);
    std::snprintf(buf.data(), buf.size(), fmt, args ...);
    return std::string(buf.data());
}

#if defined(__clang__)
#pragma clang diagnostic pop
#endif

static std::string getRelativePath(const std::string &absolutePath, const std::vector<std::string> &basePaths) {
    const std::string normAbs = Path::fromNativeSeparators(absolutePath);

    // Split a forward-slash path into non-empty components, skipping any
    // leading drive prefix (e.g. "C:") and the following '/'.
    const auto split = [](const std::string &s) {
        std::vector<std::string> parts;
        std::size_t pos = 0;
        if (s.size() >= 2 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':')
            pos = 2;
        if (pos < s.size() && s[pos] == '/')
            ++pos;
        while (pos < s.size()) {
            const std::size_t slash = s.find('/', pos);
            std::string seg = s.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            if (!seg.empty())
                parts.push_back(std::move(seg));
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        return parts;
    };

    for (const std::string &bp : basePaths) {
        if (absolutePath == bp || bp.empty()) // Seems to be a file, or path is empty
            continue;

        const std::string normBase = Path::fromNativeSeparators(bp);

        // Fast path: absolutePath is directly under basePath — strip the prefix.
        if (normAbs.compare(0, normBase.length(), normBase) == 0) {
            if (endsWith(normBase, '/'))
                return normAbs.substr(normBase.length());
            if (normAbs.size() > normBase.size() && normAbs[normBase.size()] == '/')
                return normAbs.substr(normBase.size() + 1);
        }

        // Slow path: build a relative path using ".." when absolutePath is above
        // or beside basePath but shares a common ancestor.
        std::string base = normBase;
        if (!base.empty() && base.back() != '/')
            base += '/';

        const std::vector<std::string> absParts = split(normAbs);
        const std::vector<std::string> baseParts_ = split(base);

        // Find the length of the common component prefix.
        std::size_t common = 0;
        while (common < absParts.size() && common < baseParts_.size() &&
               Path::sameFileName(absParts[common], baseParts_[common]))
            ++common;

        if (common == 0)
            continue; // truly different roots — skip this basePath

        // One ".." for every extra component in base beyond the common prefix,
        // then the remaining components of absolutePath.
        std::string rel;
        for (std::size_t i = common; i < baseParts_.size(); ++i) {
            if (!rel.empty()) rel += '/';
            rel += "..";
        }
        for (std::size_t i = common; i < absParts.size(); ++i) {
            if (!rel.empty()) rel += '/';
            rel += absParts[i];
        }
        if (!rel.empty())
            return rel;
    }
    return absolutePath;
}

// Evaluate a $([ClassName]::Method(args)) static property function.
// Returns an empty string for unknown or unimplementable functions rather
// than throwing, so import can continue gracefully.
std::string ImportProject::applyMSBuildStaticFunction(const std::string &className,
                                                      const std::string &member,
                                                      const std::vector<std::string> &args) {
    const auto toInt = [](const std::string &s, long &out) -> bool {
        if (s.empty()) return false;
        char *end = nullptr;
        out = std::strtol(s.c_str(), &end, 10);
        return end != s.c_str() && *end == '\0';
    };

    if (caseInsensitiveStringCompare(className, "MSBuild") == 0) {

        // $([MSBuild]::IsOSPlatform('Windows'|'Linux'|'OSX'))
        if (caseInsensitiveStringCompare(member, "IsOSPlatform") == 0 && args.size() == 1) {
#if defined(_WIN32)
            const bool onWindows = true, onLinux = false, onOSX = false;
#elif defined(__APPLE__)
            const bool onWindows = false, onLinux = false, onOSX = true;
#else
            const bool onWindows = false, onLinux = true, onOSX = false;
#endif
            if (caseInsensitiveStringCompare(args[0], "Windows") == 0)
                return onWindows ? "True" : "False";
            if (caseInsensitiveStringCompare(args[0], "Linux") == 0)
                return onLinux ? "True" : "False";
            if (caseInsensitiveStringCompare(args[0], "OSX") == 0 ||
                caseInsensitiveStringCompare(args[0], "MacOS") == 0)
                return onOSX ? "True" : "False";
            return "False";
        }

        // Arithmetic: Add, Subtract, Multiply, Divide, Modulo
        if (args.size() == 2) {
            long a = 0, b = 0;
            if (toInt(args[0], a) && toInt(args[1], b)) {
                if (caseInsensitiveStringCompare(member, "Add") == 0)
                    return std::to_string(a + b);
                if (caseInsensitiveStringCompare(member, "Subtract") == 0)
                    return std::to_string(a - b);
                if (caseInsensitiveStringCompare(member, "Multiply") == 0)
                    return std::to_string(a * b);
                if (caseInsensitiveStringCompare(member, "Divide") == 0 && b != 0)
                    return std::to_string(a / b);
                if (caseInsensitiveStringCompare(member, "Modulo") == 0 && b != 0)
                    return std::to_string(a % b);
            }
            // $([MSBuild]::ValueOrDefault(value, default))
            if (caseInsensitiveStringCompare(member, "ValueOrDefault") == 0)
                return args[0].empty() ? args[1] : args[0];
            // $([MSBuild]::MakeRelative(basePath, path))
            // Returns path expressed relative to basePath.  If the two paths
            // share no common root (e.g. different drive letters) path is
            // returned unchanged.
            if (caseInsensitiveStringCompare(member, "MakeRelative") == 0)
                return getRelativePath(args[1], {args[0]});
            // $([MSBuild]::GetDirectoryNameOfFileAbove(startingDirectory, fileName))
            // Walks up from startingDirectory looking for fileName; returns the
            // containing directory (no trailing separator) or "" if not found.
            if (caseInsensitiveStringCompare(member, "GetDirectoryNameOfFileAbove") == 0) {
                const std::string found = findFile(args[0], args[1]);
                if (found.empty())
                    return "";
                std::string dir = Path::getPathFromFilename(found);
                if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
                    dir.pop_back();
                return dir;
            }
            // $([MSBuild]::GetPathOfFileAbove(file, startingDirectory))
            // Walks up from startingDirectory looking for file; returns the full
            // path of the file or "" if not found.
            if (caseInsensitiveStringCompare(member, "GetPathOfFileAbove") == 0)
                return findFile(args[1], args[0]);
        }

        // $([MSBuild]::NormalizePath(seg1[, seg2, ...])) — join segments (Path.Combine
        // semantics: an absolute segment resets the accumulated path), normalize \ to /,
        // and resolve . and .. components.  The result is absolute only when the first
        // evaluated segment is itself absolute; relative inputs stay relative.
        if (caseInsensitiveStringCompare(member, "NormalizePath") == 0) {
            if (args.empty()) {
                debugs.emplace_back("NormalizePath: called with no arguments");
                return "";
            }
            // If any arg still contains an unexpanded $(…) reference, the property was
            // not defined at evaluation time.  Log it and bail out rather than treating
            // the raw reference text as a literal path component.
            for (const std::string &a : args) {
                // cppcheck-suppress useStlAlgorithm
                if (a.find("$(") != std::string::npos) {
                    debugs.emplace_back("NormalizePath: arg contains unexpanded property reference: " + a);
                    return "";
                }
            }
            // Join: an absolute segment resets the accumulated path (Path.Combine semantics).
            std::string result = args[0];
            for (std::size_t i = 1; i < args.size(); ++i) {
                const std::string &seg = args[i];
                const bool segAbsolute = !seg.empty() &&
                                         (seg[0] == '/' || seg[0] == '\\' ||
                                          (seg.size() >= 2 && std::isalpha(static_cast<unsigned char>(seg[0])) && seg[1] == ':'));
                if (segAbsolute) {
                    result = seg;
                } else {
                    if (!result.empty() && result.back() != '/' && result.back() != '\\')
                        result += '/';
                    result += seg;
                }
            }
            // Unify separators.
            // cppcheck-suppress useStlAlgorithm
            for (char &c : result) if (c == '\\') c = '/';
            // Extract drive-letter or leading-slash prefix.
            std::string prefix;
            std::size_t pos = 0;
            if (result.size() >= 2 && std::isalpha(static_cast<unsigned char>(result[0])) && result[1] == ':') {
                prefix = result.substr(0, 2) + '/';
                pos = (result.size() > 2 && result[2] == '/') ? 3 : 2;
            } else if (!result.empty() && result[0] == '/') {
                prefix = "/";
                pos = 1;
            }
            // Resolve . and .. components.
            std::vector<std::string> parts;
            while (pos < result.size()) {
                const std::size_t slash = result.find('/', pos);
                const std::string seg = result.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
                pos = (slash == std::string::npos) ? result.size() : slash + 1;
                if (seg.empty() || seg == ".")
                    continue;
                if (seg == "..") {
                    if (!parts.empty()) parts.pop_back();
                } else {
                    parts.push_back(seg);
                }
            }
            std::string normalized = prefix;
            for (std::size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) normalized += '/';
                normalized += parts[i];
            }
            return normalized;
        }

        // $([MSBuild]::NormalizeDirectory(seg1[, seg2, ...])) — same as NormalizePath
        // but always returns a path with a trailing slash.
        if (caseInsensitiveStringCompare(member, "NormalizeDirectory") == 0) {
            if (args.empty()) {
                debugs.emplace_back("NormalizeDirectory: called with no arguments");
                return "";
            }
            // Reuse NormalizePath logic via recursive call with renamed member.
            const std::string normalized = applyMSBuildStaticFunction(className, "NormalizePath", args);
            if (!normalized.empty() && normalized.back() != '/')
                return normalized + '/';
            return normalized;
        }

        if (args.size() == 1) {
            // $([MSBuild]::EnsureTrailingSlash(path))
            if (caseInsensitiveStringCompare(member, "EnsureTrailingSlash") == 0) {
                std::string s = args[0];
                if (!s.empty() && s.back() != '/' && s.back() != '\\')
                    s += '/';
                return s;
            }
            // $([MSBuild]::GetTargetPlatformVersion(version)) — pass through
            if (caseInsensitiveStringCompare(member, "GetTargetPlatformVersion") == 0)
                return args[0];
        }

        // $([MSBuild]::VersionGreaterThan / VersionGreaterThanOrEquals /
        //           VersionLessThan    / VersionLessThanOrEquals    /
        //           VersionEquals      / VersionNotEquals(v1, v2))
        // Component-by-component numeric comparison; missing trailing components
        // are treated as 0 (so '1.0' == '1.0.0'), which differs from the
        // condition-expression < / > operators that use -1 for missing parts.
        if (args.size() == 2 &&
            (caseInsensitiveStringCompare(member, "VersionGreaterThan") == 0 ||
             caseInsensitiveStringCompare(member, "VersionGreaterThanOrEquals") == 0 ||
             caseInsensitiveStringCompare(member, "VersionLessThan") == 0 ||
             caseInsensitiveStringCompare(member, "VersionLessThanOrEquals") == 0 ||
             caseInsensitiveStringCompare(member, "VersionEquals") == 0 ||
             caseInsensitiveStringCompare(member, "VersionNotEquals") == 0)) {
            const auto parseVer = [](const std::string &s) -> std::vector<int> {
                std::vector<int> parts;
                std::size_t pos = 0;
                while (pos <= s.size()) {
                    char *end = nullptr;
                    const long v = std::strtol(s.c_str() + pos, &end, 10);
                    if (end == s.c_str() + pos)
                        break;
                    parts.push_back(static_cast<int>(v));
                    pos = static_cast<std::size_t>(end - s.c_str());
                    if (pos < s.size() && s[pos] == '.')
                        ++pos;
                    else
                        break;
                }
                return parts;
            };
            const std::vector<int> lv = parseVer(args[0]);
            const std::vector<int> rv = parseVer(args[1]);
            const std::size_t count = std::max(lv.size(), rv.size());
            int cmp = 0;
            for (std::size_t i = 0; i < count && cmp == 0; ++i) {
                const int l = (i < lv.size()) ? lv[i] : 0;
                const int r = (i < rv.size()) ? rv[i] : 0;
                if (l < r) cmp = -1;
                else if (l > r) cmp = 1;
            }
            bool result = false;
            if (caseInsensitiveStringCompare(member, "VersionGreaterThan") == 0)
                result = cmp > 0;
            else if (caseInsensitiveStringCompare(member, "VersionGreaterThanOrEquals") == 0)
                result = cmp >= 0;
            else if (caseInsensitiveStringCompare(member, "VersionLessThan") == 0)
                result = cmp < 0;
            else if (caseInsensitiveStringCompare(member, "VersionLessThanOrEquals") == 0)
                result = cmp <= 0;
            else if (caseInsensitiveStringCompare(member, "VersionEquals") == 0)
                result = cmp == 0;
            else // VersionNotEquals
                result = cmp != 0;
            return result ? "True" : "False";
        }

        if (args.empty()) {
            if (caseInsensitiveStringCompare(member, "GetCurrentToolsVersion") == 0)
                return "Current";
        }
    }

    if (caseInsensitiveStringCompare(className, "System.Environment") == 0) {
        // $([System.Environment]::GetEnvironmentVariable('NAME'))
        if (caseInsensitiveStringCompare(member, "GetEnvironmentVariable") == 0 && args.size() == 1) {
            const char *env = std::getenv(args[0].c_str());
            return env ? env : "";
        }
        // $([System.Environment]::GetFolderPath(SpecialFolder.X))
        if (caseInsensitiveStringCompare(member, "GetFolderPath") == 0 && args.size() == 1) {
            const char *pf = std::getenv("ProgramFiles");
            if ((caseInsensitiveStringCompare(args[0], "ProgramFiles") == 0 ||
                 caseInsensitiveStringCompare(args[0], "ProgramFilesX86") == 0) && pf)
                return pf;
            return "";
        }
    }

    if (caseInsensitiveStringCompare(className, "System.IO.Path") == 0) {
        if (args.size() == 1) {
            if (caseInsensitiveStringCompare(member, "GetFileName") == 0) {
                const auto slash = args[0].find_last_of("/\\");
                return slash != std::string::npos ? args[0].substr(slash + 1) : args[0];
            }
            if (caseInsensitiveStringCompare(member, "GetFileNameWithoutExtension") == 0) {
                const auto slash = args[0].find_last_of("/\\");
                std::string name = slash != std::string::npos ? args[0].substr(slash + 1) : args[0];
                const auto dot = name.rfind('.');
                return dot != std::string::npos ? name.substr(0, dot) : name;
            }
            if (caseInsensitiveStringCompare(member, "GetDirectoryName") == 0) {
                const auto slash = args[0].find_last_of("/\\");
                return slash != std::string::npos ? args[0].substr(0, slash) : "";
            }
            if (caseInsensitiveStringCompare(member, "GetExtension") == 0) {
                const auto dot = args[0].rfind('.');
                return dot != std::string::npos ? args[0].substr(dot) : "";
            }
            if (caseInsensitiveStringCompare(member, "IsPathRooted") == 0) {
                const std::string &p = args[0];
                const bool rooted = !p.empty() &&
                                    (p[0] == '/' || p[0] == '\\' ||
                                     (p.size() >= 2 &&
                                      std::isalpha(static_cast<unsigned char>(p[0])) &&
                                      p[1] == ':'));
                return rooted ? "True" : "False";
            }
            if (caseInsensitiveStringCompare(member, "GetFullPath") == 0) {
                if (args.size() != 1)
                    return "";

                const std::string &path = args[0];

                if (Path::isAbsolute(path))
                    return Path::simplifyPath(Path::fromNativeSeparators(path));

                return Path::simplifyPath(toAbsolute(path));
            }
        }
        if (!args.empty() && caseInsensitiveStringCompare(member, "Combine") == 0) {
            // Path.Combine(seg1[, seg2, ...]): an absolute segment resets the
            // accumulated path; relative segments are joined with '/'.
            std::string result = args[0];
            for (std::size_t i = 1; i < args.size(); ++i) {
                const std::string &seg = args[i];
                const bool segAbsolute = !seg.empty() &&
                                         (seg[0] == '/' || seg[0] == '\\' ||
                                          (seg.size() >= 2 && std::isalpha(static_cast<unsigned char>(seg[0])) && seg[1] == ':') ||
                                          Path::isAbsolute(seg));
                if (segAbsolute) {
                    result = seg;
                    continue;
                }
                if (!result.empty() && result.back() != '/' && result.back() != '\\')
                    result += '/';
                result += args[i];
            }
            return result;
        }
        if (args.size() == 2) {
            if (caseInsensitiveStringCompare(member, "GetFullPath") == 0) {
                const std::string path = Path::fromNativeSeparators(args[0]);
                const std::string basePath = Path::fromNativeSeparators(args[1]);
                if (Path::isAbsolute(path))
                    return Path::simplifyPath(path);
                if (!Path::isAbsolute(basePath))
                    return "";
                std::string combined = basePath;
                if (!combined.empty() && combined.back() != '/')
                    combined += '/';
                combined += path;
                return Path::simplifyPath(combined);
            }
        }
    }

    if (caseInsensitiveStringCompare(className, "System.String") == 0) {
        if (caseInsensitiveStringCompare(member, "IsNullOrEmpty") == 0 && args.size() == 1)
            return args[0].empty() ? "True" : "False";
        if (caseInsensitiveStringCompare(member, "IsNullOrWhiteSpace") == 0 && args.size() == 1) {
            for (const char c : args[0])
                // cppcheck-suppress useStlAlgorithm
                if (!std::isspace(static_cast<unsigned char>(c))) return "False";
            return "True";
        }
        if (caseInsensitiveStringCompare(member, "Concat") == 0) {
            std::string result;
            for (const std::string &a : args) result += a;
            return result;
        }
        if (caseInsensitiveStringCompare(member, "Join") == 0 && args.size() >= 2) {
            std::string result;
            for (std::size_t i = 1; i < args.size(); ++i) {
                if (i > 1) result += args[0];
                result += args[i];
            }
            return result;
        }
        // Format: replace {n} and {n:spec} placeholders; {{ and }} are literal braces.
        if (caseInsensitiveStringCompare(member, "Format") == 0 && !args.empty()) {
            // Apply a .NET composite-format specifier to a string value.
            const auto applySpec = [](const std::string &arg, const std::string &spec) -> std::string {
                if (spec.empty())
                    return arg;
                const char specChar = spec[0];

                int width = -1;
                if (spec.size() > 1) {
                    char *wend = nullptr;
                    const long w = std::strtol(spec.c_str() + 1, &wend, 10);
                    if (wend != spec.c_str() + 1 && *wend == '\0' && w >= 0)
                        width = static_cast<int>(w);
                }

                char *iend = nullptr;
                const long lval = std::strtol(arg.c_str(), &iend, 10);
                const bool isInt = !arg.empty() && iend != arg.c_str() && *iend == '\0';

                char *dend = nullptr;
                const double dval = std::strtod(arg.c_str(), &dend);
                const bool isDouble = !arg.empty() && dend != arg.c_str() && *dend == '\0';

                if (specChar == 'D' || specChar == 'd') {
                    if (!isInt)
                        return arg;
                    if (width > 0) {
                        // Handle negative numbers manually to keep '-' outside the zero padding
                        if (lval == LONG_MIN) {
                            // Prevents -lval overflow bug. LONG_MIN is 20 chars long.
                            const std::string magnitude = "9223372036854775808";
                            const int padding = width - static_cast<int>(magnitude.size());

                            if (padding > 0)
                                return safeFormat("-%s%s", std::string(padding, '0').c_str(), magnitude.c_str());

                            return safeFormat("-%s", magnitude.c_str());
                        }
                        if (lval < 0)
                            return safeFormat("-%0*ld", width, -lval);
                        return safeFormat("%0*ld", width, lval);
                    }
                    return safeFormat("%ld", lval);
                }

                if (specChar == 'X' || specChar == 'x') {
                    if (!isInt)
                        return arg;
                    const auto uval = static_cast<unsigned long>(lval);
                    if (width > 0) {
                        return specChar == 'X'
                            ? safeFormat("%0*lX", width, uval)
                            : safeFormat("%0*lx", width, uval);
                    }
                    return specChar == 'X'
                        ? safeFormat("%lX", uval)
                        : safeFormat("%lx", uval);
                }

                if (specChar == 'F' || specChar == 'f') {
                    if (!isDouble)
                        return arg;
                    const int p = width >= 0 ? width : 2;
                    return safeFormat("%.*f", p, dval);
                }

                if (specChar == 'E' || specChar == 'e') {
                    if (!isDouble)
                        return arg;
                    const int p = width >= 0 ? width : 6;
                    return specChar == 'E'
                        ? safeFormat("%.*E", p, dval)
                        : safeFormat("%.*e", p, dval);
                }

                if (specChar == 'G' || specChar == 'g') {
                    if (!isDouble)
                        return arg;
                    if (width > 0) {
                        return specChar == 'G'
                            ? safeFormat("%.*G", width, dval)
                            : safeFormat("%.*g", width, dval);
                    }
                    return specChar == 'G'
                        ? safeFormat("%G", dval)
                        : safeFormat("%g", dval);
                }
                return arg;
            };

            const std::string &fmt = args[0];
            std::string result;
            result.reserve(fmt.size());
            for (std::size_t i = 0; i < fmt.size(); ++i) {
                if (fmt[i] == '{') {
                    if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                        result += '{'; ++i;
                        continue;
                    }
                    const std::size_t close = fmt.find('}', i + 1);
                    if (close == std::string::npos) { result += '{'; continue; }
                    const std::string inner = fmt.substr(i + 1, close - i - 1);
                    const std::size_t colon = inner.find(':');
                    const std::string indexStr = inner.substr(0, colon == std::string::npos ? inner.size() : colon);
                    const std::string spec = colon != std::string::npos ? inner.substr(colon + 1) : "";
                    char *end = nullptr;
                    const long idx = std::strtol(indexStr.c_str(), &end, 10);
                    if (end != indexStr.c_str() && *end == '\0' && idx >= 0) {
                        const std::size_t argIdx = static_cast<std::size_t>(idx) + 1;
                        result += applySpec(argIdx < args.size() ? args[argIdx] : "", spec);
                        i = close;
                    } else {
                        result += '{';
                    }
                } else if (fmt[i] == '}' && i + 1 < fmt.size() && fmt[i + 1] == '}') {
                    result += '}'; ++i;
                } else {
                    result += fmt[i];
                }
            }
            return result;
        }
    }

    if (caseInsensitiveStringCompare(className, "System.Math") == 0) {
        const auto toDouble = [](const std::string &s, double &out) -> bool {
            if (s.empty()) return false;
            char *end = nullptr;
            out = std::strtod(s.c_str(), &end);
            return end != s.c_str() && *end == '\0';
        };
        // Format a double as an integer string when the value is whole,
        // otherwise use std::to_string (which gives 6 decimal places).
        const auto fmtDouble = [](double d) -> std::string {
            const auto i = static_cast<long long>(d);
            if (!(static_cast<double>(i) < d) && !(static_cast<double>(i) > d))
                return std::to_string(i);
            return std::to_string(d);
        };
        if (args.size() == 1) {
            double x = 0;
            if (toDouble(args[0], x)) {
                if (caseInsensitiveStringCompare(member, "Abs") == 0)
                    return fmtDouble(x < 0 ? -x : x);
                if (caseInsensitiveStringCompare(member, "Floor") == 0)
                    return std::to_string(static_cast<long long>(x >= 0 ? x : x - 1));
                if (caseInsensitiveStringCompare(member, "Ceiling") == 0)
                    return std::to_string(static_cast<long long>(x <= 0 ? x : x + 1));
                if (caseInsensitiveStringCompare(member, "Round") == 0)
                    return std::to_string(static_cast<long long>(x >= 0 ? x + 0.5 : x - 0.5));
                if (caseInsensitiveStringCompare(member, "Sqrt") == 0 && x >= 0)
                    return fmtDouble(std::sqrt(x));
                if (caseInsensitiveStringCompare(member, "Log") == 0 && x > 0)
                    return fmtDouble(std::log(x));
                if (caseInsensitiveStringCompare(member, "Log10") == 0 && x > 0)
                    return fmtDouble(std::log10(x));
            }
        }
        if (args.size() == 2) {
            double a = 0, b = 0;
            if (toDouble(args[0], a) && toDouble(args[1], b)) {
                if (caseInsensitiveStringCompare(member, "Max") == 0)
                    return fmtDouble(a > b ? a : b);
                if (caseInsensitiveStringCompare(member, "Min") == 0)
                    return fmtDouble(a < b ? a : b);
                if (caseInsensitiveStringCompare(member, "Pow") == 0)
                    return fmtDouble(std::pow(a, b));
            }
        }
    }

    // $([MSBuild]::Escape / Unescape) — encode/decode MSBuild special chars as %XX
    if (caseInsensitiveStringCompare(className, "MSBuild") == 0 && args.size() == 1) {
        if (caseInsensitiveStringCompare(member, "Escape") == 0) {
            static const char special[] = "%$@';?*!";
            std::string result;
            for (const unsigned char c : args[0]) {
                if (std::strchr(special, static_cast<char>(c))) {
                    const char hex[] = "0123456789ABCDEF";
                    result += '%';
                    result += hex[(c >> 4) & 0xF];
                    result += hex[c & 0xF];
                } else {
                    result += static_cast<char>(c);
                }
            }
            return result;
        }
        if (caseInsensitiveStringCompare(member, "Unescape") == 0) {
            std::string result;
            const std::string &s = args[0];
            for (std::size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '%' && i + 2 < s.size() &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                    std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
                    const auto nibble = [](char c) -> unsigned char {
                        if (c >= '0' && c <= '9') return static_cast<unsigned char>(c - '0');
                        if (c >= 'a' && c <= 'f') return static_cast<unsigned char>(c - 'a' + 10);
                        return static_cast<unsigned char>(c - 'A' + 10);
                    };
                    result += static_cast<char>((nibble(s[i + 1]) << 4) | nibble(s[i + 2]));
                    i += 2;
                } else {
                    result += s[i];
                }
            }
            return result;
        }
        // Bitwise operations
        {
            long a = 0;
            if (toInt(args[0], a)) {
                if (caseInsensitiveStringCompare(member, "BitwiseNot") == 0)
                    return std::to_string(~a);
            }
        }
    }

    if (caseInsensitiveStringCompare(className, "MSBuild") == 0 && args.size() == 2) {
        long a = 0, b = 0;
        if (toInt(args[0], a) && toInt(args[1], b)) {
            if (caseInsensitiveStringCompare(member, "BitwiseAnd") == 0)
                return std::to_string(a & b);
            if (caseInsensitiveStringCompare(member, "BitwiseOr") == 0)
                return std::to_string(a | b);
            if (caseInsensitiveStringCompare(member, "BitwiseXor") == 0)
                return std::to_string(a ^ b);
        }
    }

    // $([MSBuild]::GetRegistryValue / GetRegistryValueFromView)
    // Returns empty on non-Windows; on Windows would need registry access.
    if (caseInsensitiveStringCompare(className, "MSBuild") == 0 &&
        (caseInsensitiveStringCompare(member, "GetRegistryValue") == 0 ||
         caseInsensitiveStringCompare(member, "GetRegistryValueFromView") == 0))
        return "";

    // $([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(...))
    // Arg is itself a static property like $([...OSPlatform]::Windows) which
    // expands to the platform name string via the same mechanism.
    if (caseInsensitiveStringCompare(className, "System.Runtime.InteropServices.RuntimeInformation") == 0 &&
        caseInsensitiveStringCompare(member, "IsOSPlatform") == 0 && args.size() == 1)
        return applyMSBuildStaticFunction("MSBuild", "IsOSPlatform", args);

    // $([System.Runtime.InteropServices.OSPlatform]::Windows|Linux|OSX) — static property
    if (caseInsensitiveStringCompare(className, "System.Runtime.InteropServices.OSPlatform") == 0)
        return member;  // return the platform name ("Windows", "Linux", "OSX") as a string

    // $([System.IO.FileInfo]::new('path')) / $([System.IO.DirectoryInfo]::new('path'))
    // Model as a normalized path string so .FullName / .DirectoryName / .Name chains work.
    if ((caseInsensitiveStringCompare(className, "System.IO.FileInfo") == 0 ||
         caseInsensitiveStringCompare(className, "System.IO.DirectoryInfo") == 0) &&
        caseInsensitiveStringCompare(member, "new") == 0 && !args.empty())
        return Path::simplifyPath(args[0]);

    // Unknown class or method — return empty so import continues
    debugs.emplace_back("unknown class " + className + " or member " + member);
    return "";
}

// Expands $(Name) and $(Name.Method(args)) references in property value strings.
// Unknown variables are left unexpanded. Use expandPropertyValue() to invoke.
struct ImportProject::PropertyValueExpander {
    ImportProject &mProject;
    const PropertiesMap &mVars;
    std::string mStr;
    std::size_t mPos{0};
    bool mChanged{false};
    bool mReplaceUnknown{false};  // if true, unknown variables expand to ""

    PropertyValueExpander(ImportProject &project, const PropertiesMap &vars, std::string str)
        : mProject(project),mVars(vars), mStr(std::move(str)) {}

    bool isKnown(const std::string &name) const {
        if (mVars.count(name)) return true;
        return std::getenv(name.c_str()) != nullptr;
    }

    std::string lookup(const std::string &name) const {
        const auto it = mVars.find(name);
        if (it != mVars.end())
            return it->second;
        const char *env = std::getenv(name.c_str());
        return env ? env : std::string();
    }

    // Parses an identifier, handling nested $(...) within the name.
    std::string parseIdentifier() {
        std::string result;
        while (mPos < mStr.size()) {
            if (mStr.compare(mPos, 2, "$(") == 0) {
                result += tryParseExpr();
                continue;
            }
            const auto c = static_cast<unsigned char>(mStr[mPos]);
            if (!std::isalnum(c) && c != '_' && c != '-') break;
            result += mStr[mPos++];
        }
        return result;
    }

    // Parses one method argument: a quoted string literal (with inner $(…)
    // expansion) or an unquoted $(…) reference or bare word.
    std::string parseArg() {
        while (mPos < mStr.size() && std::isspace(static_cast<unsigned char>(mStr[mPos])))
            ++mPos;
        if (mPos < mStr.size() && mStr[mPos] == '\'') {
            ++mPos;
            std::string s;
            // Expand $(…) references embedded inside the quoted string so that
            // e.g. '$(MSBuildThisFileDirectory)' is resolved before being passed
            // to NormalizePath / NormalizeDirectory / Path.Combine, etc.
            while (mPos < mStr.size() && mStr[mPos] != '\'') {
                if (mStr.compare(mPos, 2, "$(") == 0)
                    s += tryParseExpr();
                else
                    s += mStr[mPos++];
            }
            if (mPos < mStr.size()) ++mPos;  // consume closing '\''
            return s;
        }
        if (mStr.compare(mPos, 2, "$(") == 0) {
            std::string s = tryParseExpr();
            // Append any trailing bare-word text so that unquoted args like
            // $(ReactNativeDir)..\..\node_modules become one concatenated arg
            // instead of two separate args.
            while (mPos < mStr.size() && mStr[mPos] != ',' && mStr[mPos] != ')')
                s += mStr[mPos++];
            return s;
        }
        // Bare word — consume until ',' or ')'.
        std::string s;
        while (mPos < mStr.size() && mStr[mPos] != ',' && mStr[mPos] != ')')
            s += mStr[mPos++];
        return s;
    }

    // Parses and evaluates $(Name[.Method(args)…]) starting at mPos.
    // Also handles $([ClassName]::Method(args)) static property functions.
    // If the variable is unknown the token is left unchanged and mPos advances past it.
    std::string tryParseExpr() {
        const std::size_t start = mPos;
        mPos += 2;  // skip "$("

        // $([ClassName]::Method(args)) — static property function
        if (mPos < mStr.size() && mStr[mPos] == '[') {
            ++mPos;  // skip '['
            std::string className;
            while (mPos < mStr.size() && mStr[mPos] != ']')
                className += mStr[mPos++];
            if (mPos < mStr.size()) ++mPos;  // skip ']'
            if (mPos + 1 < mStr.size() && mStr[mPos] == ':' && mStr[mPos + 1] == ':')
                mPos += 2;  // skip '::'
            std::string member;
            while (mPos < mStr.size()) {
                const auto c = static_cast<unsigned char>(mStr[mPos]);
                if (!std::isalnum(c) && c != '_') break;
                member += mStr[mPos++];
            }
            std::vector<std::string> args;
            // Skip optional whitespace between method name and '(' — legal in MSBuild.
            while (mPos < mStr.size() && std::isspace(static_cast<unsigned char>(mStr[mPos])))
                ++mPos;
            if (mPos < mStr.size() && mStr[mPos] == '(') {
                ++mPos;  // skip '('
                while (mPos < mStr.size() && mStr[mPos] != ')') {
                    args.push_back(parseArg());
                    while (mPos < mStr.size() && std::isspace(static_cast<unsigned char>(mStr[mPos])))
                        ++mPos;
                    if (mPos < mStr.size() && mStr[mPos] == ',') ++mPos;
                }
                if (mPos < mStr.size()) ++mPos;  // skip inner ')'
            }
            mChanged = true;
            std::string value = mProject.applyMSBuildStaticFunction(className, member, args);
            // Handle optional .Property or .Method(args) chain on the result,
            // e.g. $([System.IO.FileInfo]::new('path').DirectoryName).
            while (mPos < mStr.size() && mStr[mPos] == '.') {
                ++mPos;
                std::string chainMethod;
                while (mPos < mStr.size()) {
                    const auto c = static_cast<unsigned char>(mStr[mPos]);
                    if (!std::isalnum(c) && c != '_') break;
                    chainMethod += mStr[mPos++];
                }
                if (mPos >= mStr.size() || mStr[mPos] != '(') {
                    if (caseInsensitiveStringCompare(chainMethod, "Length") == 0)
                        value = std::to_string(value.size());
                    else if (caseInsensitiveStringCompare(chainMethod, "FullName") == 0)
                        value = Path::simplifyPath(value);
                    else if (caseInsensitiveStringCompare(chainMethod, "DirectoryName") == 0)
                        value = Path::getPathFromFilename(value);
                    else if (caseInsensitiveStringCompare(chainMethod, "Name") == 0) {
                        const std::string p = Path::fromNativeSeparators(value);
                        const auto sl = p.rfind('/');
                        value = (sl != std::string::npos) ? p.substr(sl + 1) : p;
                    } else {
                        mProject.debugs.emplace_back("unhandled property access '." + chainMethod + "' after static function");
                    }
                    continue;  // allow further .Method() segments on the result
                }
                ++mPos;  // skip '('
                std::vector<std::string> chainArgs;
                while (mPos < mStr.size() && mStr[mPos] != ')') {
                    chainArgs.push_back(parseArg());
                    while (mPos < mStr.size() && std::isspace(static_cast<unsigned char>(mStr[mPos])))
                        ++mPos;
                    if (mPos < mStr.size() && mStr[mPos] == ',') ++mPos;
                }
                if (mPos < mStr.size()) ++mPos;  // skip ')'
                try {
                    value = applyPropertyMethod(value, chainMethod, chainArgs);
                } catch (const std::exception &e) {
                    mProject.debugs.emplace_back(std::string("applyPropertyMethod (chained): ") + e.what());
                } catch (...) {
                    mProject.debugs.emplace_back("applyPropertyMethod (chained): unknown error for method '" + chainMethod + "'");
                }
            }
            if (mPos < mStr.size() && mStr[mPos] == ')') ++mPos;  // skip outer ')'
            return value;
        }

        const std::string name = parseIdentifier();
        if (name.empty() || !isKnown(name)) {
            const std::size_t end = findMatchingParen(mStr, start + 2);
            mPos = (end != std::string::npos) ? end + 1 : mStr.size();
            if (mReplaceUnknown) {
                mChanged = true;
                return std::string();
            }
            return mStr.substr(start, mPos - start);
        }
        mChanged = true;
        std::string value = lookup(name);
        // Parse optional .Method(args) chain.
        while (mPos < mStr.size() && mStr[mPos] == '.') {
            ++mPos;
            std::string method;
            while (mPos < mStr.size()) {
                const auto c = static_cast<unsigned char>(mStr[mPos]);
                if (!std::isalnum(c) && c != '_') break;
                method += mStr[mPos++];
            }
            if (mPos >= mStr.size() || mStr[mPos] != '(') {
                // Property access without parentheses (e.g. $(Foo.Length)).
                if (caseInsensitiveStringCompare(method, "Length") == 0)
                    value = std::to_string(value.size());
                else if (caseInsensitiveStringCompare(method, "FullName") == 0)
                    value = Path::simplifyPath(value);
                else if (caseInsensitiveStringCompare(method, "DirectoryName") == 0)
                    value = Path::getPathFromFilename(value);
                else if (caseInsensitiveStringCompare(method, "Name") == 0) {
                    const std::string p = Path::fromNativeSeparators(value);
                    const auto sl = p.rfind('/');
                    value = (sl != std::string::npos) ? p.substr(sl + 1) : p;
                } else {
                    mProject.debugs.emplace_back("unhandled property access '." + method + "' on '" + name + "'");
                }
                continue;  // allow further .Method() segments on the result
            }
            ++mPos;  // skip '('
            std::vector<std::string> args;
            while (mPos < mStr.size() && mStr[mPos] != ')') {
                args.push_back(parseArg());
                while (mPos < mStr.size() && std::isspace(static_cast<unsigned char>(mStr[mPos])))
                    ++mPos;
                if (mPos < mStr.size() && mStr[mPos] == ',') ++mPos;
            }
            if (mPos < mStr.size()) ++mPos;  // skip ')'
            try {
                value = applyPropertyMethod(value, method, args);
            } catch (const std::exception &e) {
                mProject.debugs.emplace_back(std::string("applyPropertyMethod: ") + e.what());
            } catch (...) {
                mProject.debugs.emplace_back("applyPropertyMethod: unknown error for method '" + method + "'");
            }
        }
        if (mPos < mStr.size() && mStr[mPos] == ')') ++mPos;  // skip closing ')'
        return value;
    }

    // Expand all property expressions in mStr, multi-pass (capped at 50).
    std::string expand() {
        const int maxPasses = 50;
        for (int pass = 0; pass < maxPasses; ++pass) {
            mChanged = false;
            mPos = 0;
            std::string result;
            result.reserve(mStr.size());
            while (mPos < mStr.size()) {
                if (mStr.compare(mPos, 2, "$(") == 0)
                    result += tryParseExpr();
                else
                    result += mStr[mPos++];
            }
            mStr = std::move(result);
            if (!mChanged) break;
        }
        return mStr;
    }
};

void ImportProject::expandMSBuildVariables(std::string &s, const PropertiesMap &properties)
{
    PropertyValueExpander expander{*this, properties, s};
    s = expander.expand();
}

ImportProject::Type ImportProject::import(const std::string &filename, Settings *settings, Suppressions *supprs)
{
    std::ifstream fin(filename);
    if (!fin.is_open())
        return ImportProject::Type::MISSING;

    mPath = Path::getPathFromFilename(Path::fromNativeSeparators(filename));
    if (!mPath.empty() && !endsWith(mPath,'/'))
        mPath += '/';

    const std::vector<std::string> fileFilters =
        settings ? settings->fileFilters : std::vector<std::string>();

    if (endsWith(filename, ".json")) {
        if (importCompileCommands(fin)) {
            setRelativePaths(filename);
            return ImportProject::Type::COMPILE_DB;
        }
    } else if (endsWith(filename, ".sln")) {
        if (importSln(fin, filename, fileFilters)) {
            setRelativePaths(filename);
            return ImportProject::Type::VS_SLN;
        }
    } else if (endsWith(filename, ".slnx")) {
        if (importSlnx(filename, fileFilters)) {
            setRelativePaths(filename);
            return ImportProject::Type::VS_SLNX;
        }
    } else if (endsWith(filename, ".vcxproj")) {
        PropertiesMap mVariables;
        if (importVcxproj(toAbsolute(filename), mVariables, fileFilters)) {
            setRelativePaths(filename);
            return ImportProject::Type::VS_VCXPROJ;
        }
    } else if (endsWith(filename, ".bpr")) {
        if (importBcb6Prj(filename)) {
            setRelativePaths(filename);
            return ImportProject::Type::BORLAND;
        }
    } else if (settings && supprs && endsWith(filename, ".cppcheck")) {
        if (importCppcheckGuiProject(fin, *settings, *supprs)) {
            setRelativePaths(filename);
            return ImportProject::Type::CPPCHECK_GUI;
        }
    } else {
        return ImportProject::Type::UNKNOWN;
    }
    return ImportProject::Type::FAILURE;
}

bool ImportProject::importCompileCommands(std::istream &istr)
{
    picojson::value compileCommands;
    istr >> compileCommands;
    if (!compileCommands.is<picojson::array>()) {
        errors.emplace_back("compilation database is not a JSON array");
        return false;
    }

    std::map<std::string, std::size_t> fsFileIds;

    for (const picojson::value &fileInfo : compileCommands.get<picojson::array>()) {
        picojson::object obj = fileInfo.get<picojson::object>();

        if (obj.count("directory") == 0) {
            errors.emplace_back("'directory' field in compilation database entry missing");
            return false;
        }

        if (!obj["directory"].is<std::string>()) {
            errors.emplace_back("'directory' field in compilation database entry is not a string");
            return false;
        }

        std::string dirpath = Path::fromNativeSeparators(obj["directory"].get<std::string>());

        /* CMAKE produces the directory without trailing / so add it if not
         * there - it is needed by setIncludePaths() */
        if (!endsWith(dirpath, '/'))
            dirpath += '/';

        const std::string directory = std::move(dirpath);

        std::vector<std::string> arguments;
        if (obj.count("arguments")) {
            if (obj["arguments"].is<picojson::array>()) {
                for (const picojson::value& arg : obj["arguments"].get<picojson::array>()) {
                    if (arg.is<std::string>())
                        arguments.push_back(arg.get<std::string>());
                }
            } else {
                errors.emplace_back("'arguments' field in compilation database entry is not a JSON array");
                return false;
            }
        } else if (obj.count("command")) {
            std::string command;
            if (obj["command"].is<std::string>()) {
                command = obj["command"].get<std::string>();
            } else {
                errors.emplace_back("'command' field in compilation database entry is not a string");
                return false;
            }

            std::string error = collectArgs(command, arguments);
            if (!error.empty()) {
                errors.emplace_back(error);
                return false;
            }
        } else {
            errors.emplace_back("no 'arguments' or 'command' field found in compilation database entry");
            return false;
        }

        if (!obj.count("file") || !obj["file"].is<std::string>()) {
            errors.emplace_back("skip compilation database entry because it does not have a proper 'file' field");
            continue;
        }

        std::string file = Path::fromNativeSeparators(obj["file"].get<std::string>());

        // Accept file?
        if (!Path::acceptFile(file))
            continue;

        std::string path;
        if (Path::isAbsolute(file))
            path = Path::simplifyPath(std::move(file));
#ifdef _WIN32
        else if (file[0] == '/' && directory.size() > 2 && std::isalpha(directory[0]) && directory[1] == ':')
            // directory: C:\foo\bar
            // file: /xy/z.c
            // => c:/xy/z.c
            path = Path::simplifyPath(directory.substr(0,2) + file);
#endif
        else
            path = Path::simplifyPath(directory + file);
        FileSettings fs{path, Standards::Language::None, 0}; // file will be identified later on
        parseArgs(fs, arguments);
        PropertiesMap properties;
        fsSetIncludePaths(fs, directory, fs.includePaths, properties);
        // Assign a unique index to each file path. If the file path already exists in the map,
        // increment the index to handle duplicate file entries.
        fs.file.setFsFileId(fsFileIds[path]++);
        fileSettings.push_back(std::move(fs));
    }

    return true;
}

void ImportProject::setSolution(const std::string &filename, PropertiesMap &properties) {
    const std::string absolutePath = toAbsolute(filename);
    properties["SolutionDir"] = Path::getPathFromFilename(absolutePath);
    properties["SolutionExt"] = Path::getFilenameExtensionInLowerCase(absolutePath);
    properties["SolutionPath"] = absolutePath;

    // Path::stripDirectoryPart doesn't work on windows with unix paths
    // absolutePath is already normalized to '/' by toAbsolute()
    const auto slash = absolutePath.rfind('/');
    properties["SolutionFileName"] = (slash != std::string::npos) ? absolutePath.substr(slash + 1) : absolutePath;

    std::string temp = properties["SolutionFileName"];
    findAndReplace(temp, Path::getFilenameExtension(temp), "");
    properties["SolutionName"] = temp;
}

bool ImportProject::importDirectorySolutionProps(PropertiesMap &properties)
{
    const std::string directorySolutionProps = findFile(properties["ProjectDir"], "Directory.Solution.props");
    if (!directorySolutionProps.empty()) {
        MetadataMap data;
        std::unordered_set<std::string> stack;
        std::list<ProjectConfiguration> projectConfigurationList;
        std::list<ItemGroupClCompile> compileList;
        const ImportResult result = importPropsOrTargets(directorySolutionProps, properties, data, compileList, projectConfigurationList, stack);
        if (result > ImportResult::NotResolvable) {
            errors.emplace_back("Could not import \"" + directorySolutionProps + "\" - " + importResultStr(result));
            return false;
        }
    }
    return true;
}

bool ImportProject::importSln(std::istream &istr, const std::string &filename, const std::vector<std::string> &fileFilters)
{
    PropertiesMap mVariables;
    std::string line;

    debugs.clear();

    if (!std::getline(istr,line)) {
        errors.emplace_back("Visual Studio solution file is empty");
        return false;
    }

    if (!startsWith(line, "Microsoft Visual Studio Solution File")) {
        // Skip BOM
        if (!std::getline(istr, line) || !startsWith(line, "Microsoft Visual Studio Solution File")) {
            errors.emplace_back("Visual Studio solution file header not found");
            return false;
        }
    }

    PropertiesMap solutionVariables;
    setSolution(filename, solutionVariables);

    solutionVariables["VisualStudioVersion"] = "17.0";

    const std::string solutionDir = solutionVariables["SolutionDir"];

    bool found = false;
    while (std::getline(istr,line)) {
        if (startsWith(line, "VisualStudioVersion = ")) {
            solutionVariables["VisualStudioVersion"] = line.substr(std::strlen("VisualStudioVersion = "));
            continue;
        }
        if (startsWith(line, "MinimumVisualStudioVersion = ")) {
            solutionVariables["MinimumVisualStudioVersion"] = line.substr(std::strlen("MinimumVisualStudioVersion = "));
            continue;
        }
        if (!startsWith(line,"Project("))
            continue;
        const std::string::size_type pos = line.find(".vcxproj");
        if (pos == std::string::npos)
            continue;
        const std::string::size_type pos1 = line.rfind('\"',pos);
        if (pos1 == std::string::npos)
            continue;
        std::string vcxproj(line.substr(pos1+1, pos-pos1+7));
        vcxproj = Path::toNativeSeparators(std::move(vcxproj));
        vcxproj = toAbsolute(vcxproj, solutionDir, solutionVariables);
        vcxproj = Path::fromNativeSeparators(std::move(vcxproj));

        mVariables = solutionVariables;
        if (!importVcxproj(vcxproj, mVariables, fileFilters)) {
            errors.emplace_back("failed to load '" + vcxproj + "' from Visual Studio solution");
            return false;
        }
        found = true;
    }

    if (!found) {
        errors.emplace_back("no projects found in Visual Studio solution file");
        return false;
    }

    return importDirectorySolutionProps(mVariables);
}

bool ImportProject::importSlnx(const std::string& filename, const std::vector<std::string>& fileFilters)
{
    PropertiesMap mVariables;
    debugs.clear();

    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError error = doc.LoadFile(filename.c_str());
    if (error != tinyxml2::XML_SUCCESS) {
        errors.emplace_back(std::string("Visual Studio solution file is not a valid XML - ") + tinyxml2::XMLDocument::ErrorIDToName(error));
        return false;
    }

    const tinyxml2::XMLElement* const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr) {
        errors.emplace_back("Visual Studio solution file has no XML root node");
        return false;
    }

    if (std::strcmp(rootnode->Name(), "Solution") != 0) {
        errors.emplace_back("Invalid Visual Studio solution file format");
        return false;
    }

    PropertiesMap solutionVariables;
    setSolution(filename, solutionVariables);

    solutionVariables["VisualStudioVersion"] = "18.0";

    bool found = false;

    auto processProject = [&](const tinyxml2::XMLElement* projectNode) -> bool {
        const char* pathAttribute = projectNode->Attribute("Path");
        if (pathAttribute == nullptr)
            return true;

        std::string vcxproj(pathAttribute);
        vcxproj = Path::toNativeSeparators(std::move(vcxproj));

        if (Path::getFilenameExtensionInLowerCase(vcxproj) != ".vcxproj")
            return true; // skip other project types

        vcxproj = toAbsolute(vcxproj, solutionVariables["SolutionDir"], solutionVariables);

        vcxproj = Path::fromNativeSeparators(std::move(vcxproj));

        mVariables = solutionVariables;
        if (!importVcxproj(vcxproj, mVariables, fileFilters)) {
            errors.emplace_back("failed to load '" + vcxproj + "' from Visual Studio solution");
            return false;
        }
        found = true;
        return true;
    };

    for (const tinyxml2::XMLElement* node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        const char* name = node->Name();
        if (std::strcmp(name, "Project") == 0) {
            if (!processProject(node))
                return false;
        } else if (std::strcmp(name, "Folder") == 0) {
            // Walk nested Folder/Project nodes recursively
            std::function<bool(const tinyxml2::XMLElement *)> processFolder;
            processFolder = [&](const tinyxml2::XMLElement *folder) -> bool {
                for (const tinyxml2::XMLElement *child = folder->FirstChildElement(); child; child = child->NextSiblingElement()) {
                    const char *childName = child->Name();
                    if (std::strcmp(childName, "Project") == 0) {
                        if (!processProject(child))
                            return false;
                    } else if (std::strcmp(childName, "Folder") == 0) {
                        if (!processFolder(child))
                            return false;
                    }
                }
                return true;
            };
            if (!processFolder(node))
                return false;
        }
    }

    if (!found) {
        errors.emplace_back("no projects found in Visual Studio solution file");
        return false;
    }

    return importDirectorySolutionProps(mVariables);
}

ImportProject::ProjectConfiguration::ProjectConfiguration(const tinyxml2::XMLElement *cfg) {
    const char *a = cfg->Attribute("Include");
    if (a)
        name = a;
    for (const tinyxml2::XMLElement *e = cfg->FirstChildElement(); e; e = e->NextSiblingElement()) {
        const char * const text = e->GetText();
        if (!text)
            continue;
        const char * ename = e->Name();
        if (std::strcmp(ename,"Configuration")==0)
            configuration = text;
        else if (std::strcmp(ename,"Platform")==0) {
            platformStr = text;
            if (platformStr == "Win32")
                platform = Win32;
            else if (platformStr == "x64")
                platform = x64;
            else if (platformStr == "ARM64")
                platform = ARM64;
            else if (platformStr == "ARM")
                platform = ARM;
            else
                platform = Unknown;
        }
    }
}

void ImportProject::checkUnexpandedExpressions(const std::string &text, const char *context)
{
    // these are emulated so ignore them
    if (text == "$(VCTargetsPath)/Microsoft.Cpp.targets" ||
        text == "$(VCTargetsPath)/Microsoft.Cpp.props" ||
        text == "$(VCTargetsPath)/Microsoft.Cpp.Default.props")
        return;

    std::string::size_type pos = 0;
    while ((pos = text.find("$(", pos)) != std::string::npos) {
        const std::string::size_type end = text.find(')', pos + 2);
        if (end == std::string::npos)
            break;
        const std::string propName = text.substr(pos + 2, end - pos - 2);
        std::stringstream message;
        message << "unexpanded property $("
                << propName
                << ")"
                << (context ? " in " : "")
                << (context ? context : "")
                << ": " << text;
        debugs.emplace_back(message.str());
        pos = end + 1;
    }
    pos = 0;
    while ((pos = text.find("%(", pos)) != std::string::npos) {
        const std::string::size_type end = text.find(')', pos + 2);
        if (end == std::string::npos)
            break;
        std::stringstream message;
        message << "unexpanded metadata %("
                << text.substr(pos + 2, end - pos - 2)
                << ")"
                << (context ? " in " : "")
                << (context ? context : "")
                << ": " << text;
        debugs.emplace_back(message.str());
        pos = end + 1;
    }
}

// see https://learn.microsoft.com/en-us/visualstudio/msbuild/msbuild-conditions
class ImportProject::ConditionParser {
public:
    ConditionParser(ImportProject &project, const std::string &condition, const PropertiesMap &properties)
        : mProject(project), mCondition(condition), mVariables(properties) {}

    bool parse() {
        const std::string value = parseOr();

        skipWhitespace();

        if (mPos != mCondition.size()) {
            if (mCondition[mPos] == ')')
                throw std::runtime_error("unmatched ')' in condition " + mCondition);

            throw std::runtime_error("Invalid condition: '" + mCondition + "'");
        }

        if (value != "True" && value != "False")
            throw std::runtime_error("Invalid condition: '" + mCondition + "'");

        return value == "True";
    }

private:
    ImportProject &mProject;
    const std::string &mCondition;
    const PropertiesMap &mVariables;
    std::size_t mPos = 0;
    bool mEvaluate = true;  // false while parsing a short-circuited operand

    void skipWhitespace() {
        while (mPos < mCondition.size() && std::isspace(static_cast<unsigned char>(mCondition[mPos])))
            ++mPos;
    }

    bool match(const std::string &text) {
        skipWhitespace();
        if (mCondition.compare(mPos, text.size(), text) != 0)
            return false;
        mPos += text.size();
        return true;
    }

    bool matchWord(const std::string &word) {
        skipWhitespace();
        if (mCondition.size() - mPos < word.size())
            return false;
        if (caseInsensitiveStringCompare(mCondition.substr(mPos, word.size()), word) != 0)
            return false;

        const std::size_t end = mPos + word.size();
        if (end < mCondition.size() &&
            (std::isalnum(static_cast<unsigned char>(mCondition[end])) || mCondition[end] == '_'))
            return false;

        mPos = end;
        return true;
    }

    void expect(const std::string &text) {
        if (match(text))
            return;

        if (text == ")")
            throw std::runtime_error("'(' without closing ')'!");

        throw std::runtime_error("Expected '" + text + "' in condition '" + mCondition + "'");
    }

    std::string parseOr() {
        std::string lhs = parseAnd();
        while (matchWord("or")) {
            const bool savedEvaluate = mEvaluate;
            if (lhs == "True") mEvaluate = false;
            const std::string rhs = parseAnd();
            mEvaluate = savedEvaluate;
            if (lhs != "True")
                lhs = (rhs == "True") ? "True" : "False";
        }
        return lhs;
    }

    std::string parseAnd() {
        std::string lhs = parseUnary();
        while (matchWord("and")) {
            const bool savedEvaluate = mEvaluate;
            if (lhs == "False") mEvaluate = false;
            const std::string rhs = parseUnary();
            mEvaluate = savedEvaluate;
            if (lhs != "False")
                lhs = (rhs == "True") ? "True" : "False";
        }
        return lhs;
    }

    std::string parseUnary() {
        if (match("!") || matchWord("not")) {
            if (mPos == mCondition.size())
                throw std::runtime_error("Invalid condition: '" + mCondition + "'");
            return parseUnary() == "False" ? "True" : "False";
        }

        return parsePrimary();
    }

    std::string parsePrimary() {
        skipWhitespace();

        if (match("(")) {
            std::string value = parseOr();
            expect(")");
            return value;
        }

        if (matchWord("Exists"))
            return parseExists();

        if (matchWord("And") || matchWord("Or"))
            throw std::runtime_error("Invalid condition: '" + mCondition + "'");

        if (matchWord("HasTrailingSlash"))
            return parseHasTrailingSlash();

        return parseComparison();
    }

    std::string parseComparison() {
        const std::string lhs = parseValue();
        skipWhitespace();

        static constexpr const char *ops[] = { "==", "!=", "<=", ">=", "<", ">" };
        for (const char *op : ops) {
            if (match(op)) {
                const std::string rhs = parseValue();
                if (!mEvaluate)
                    return "False";
                return compare(lhs, op, rhs) ? "True" : "False";
            }
        }

        return lhs;
    }

    std::string parseValue() {
        skipWhitespace();

        if (mPos >= mCondition.size())
            throw std::runtime_error("Missing operator");

        if (matchWord("true"))
            return "True";

        if (matchWord("false"))
            return "False";

        if (mCondition[mPos] == '\'')
            return parseString();

        if (mCondition.compare(mPos, 2, "$(") == 0)
            return parsePropertyExpression();

        if (std::isdigit(static_cast<unsigned char>(mCondition[mPos])) ||
            (mCondition[mPos] == '-' &&
             mPos + 1 < mCondition.size() && std::isdigit(static_cast<unsigned char>(mCondition[mPos + 1])))) {
            const std::size_t begin = mPos++;

            while (mPos < mCondition.size() && std::isdigit(static_cast<unsigned char>(mCondition[mPos])))
                ++mPos;

            return mCondition.substr(begin, mPos - begin);
        }

        const std::size_t begin = mPos;
        while (mPos < mCondition.size()) {
            const auto c = static_cast<unsigned char>(mCondition[mPos]);
            if (!std::isalnum(c) && c != '_' && c != '-' && c != '.')
                break;
            ++mPos;
        }

        if (mPos != begin)
            return mCondition.substr(begin, mPos - begin);
        if (!mEvaluate)
            return std::string();
        throw std::runtime_error("Unknown/unhandled operator/operand '" + mCondition.substr(mPos) + "'");
    }

    std::string parseString() {
        ++mPos;
        std::string value;

        while (mPos < mCondition.size()) {
            const char c = mCondition[mPos++];

            if (c == '\'')
                return expandProperties(value);

            value += c;
        }

        if (!mEvaluate)
            return std::string();
        throw std::runtime_error("Can not tokenize condition");
    }

    static bool parseInteger(const std::string &s, long &value)
    {
        if (s.empty())
            return false;

        const char *begin = s.c_str();
        char *end = nullptr;
        int base = 10;

        if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            begin += 2;
            if (*begin == '\0')
                return false;
            base = 16;
        }

        value = std::strtol(begin, &end, base);
        return end != begin && *end == '\0';
    }

    std::string parseIdentifier() {
        skipWhitespace();
        std::string result;
        while (mPos < mCondition.size()) {
            if (mCondition.compare(mPos, 2, "$(") == 0) {
                result += parsePropertyExpression();
                continue;
            }
            const auto c = static_cast<unsigned char>(mCondition[mPos]);
            if (!std::isalnum(c) && c != '_' && c != '-')
                break;
            result += mCondition[mPos++];
        }
        if (result.empty())
            throw std::runtime_error("Expected identifier in condition '" + mCondition + "'");
        return result;
    }

    std::string parsePropertyExpression() {
        expect("$(");

        // $([ClassName]::Method(args)) — static property function
        if (mPos < mCondition.size() && mCondition[mPos] == '[') {
            ++mPos;  // skip '['
            std::string className;
            while (mPos < mCondition.size() && mCondition[mPos] != ']')
                className += mCondition[mPos++];
            if (mPos < mCondition.size()) ++mPos;  // skip ']'
            if (mPos + 1 < mCondition.size() && mCondition[mPos] == ':' && mCondition[mPos + 1] == ':')
                mPos += 2;  // skip '::'
            std::string member;
            while (mPos < mCondition.size()) {
                const auto c = static_cast<unsigned char>(mCondition[mPos]);
                if (!std::isalnum(c) && c != '_') break;
                member += mCondition[mPos++];
            }
            std::vector<std::string> args;
            skipWhitespace();  // allow space between method name and '('
            if (mPos < mCondition.size() && mCondition[mPos] == '(') {
                ++mPos;  // skip '('
                skipWhitespace();
                if (!match(")")) {
                    do {
                        args.push_back(parseValue());
                        skipWhitespace();
                    } while (match(","));
                    expect(")");
                }
            }
            expect(")");  // outer closing paren of $(...)
            return mEvaluate ? mProject.applyMSBuildStaticFunction(className, member, args) : std::string();
        }

        std::string value = getPropertyValue(parseIdentifier());

        while (true) {
            skipWhitespace();
            if (!match("."))
                break;

            const std::string method = parseIdentifier();
            if (!match("(")) {
                // Property access without parentheses (e.g. $(Foo.Length)).
                if (mEvaluate && caseInsensitiveStringCompare(method, "Length") == 0)
                    value = std::to_string(value.size());
                break;
            }
            std::vector<std::string> args;
            skipWhitespace();
            if (!match(")")) {
                do {
                    args.push_back(parseValue());
                } while (match(","));
                expect(")");
            }
            value = mEvaluate ? applyMethod(value, method, args) : std::string();
        }

        expect(")");
        return value;
    }

    std::string parseExists() {
        expect("(");
        const std::string filename = parseValue();
        expect(")");

        std::string path = filename;
        if (!Path::isAbsolute(path)) {
            auto it = mVariables.find("MSBuildThisFileDirectory");
            if (it == mVariables.end())
                it = mVariables.find("ProjectDir");
            if (it != mVariables.end())
                path = it->second + path;
        }

        return (Path::isFile(path) || Path::isDirectory(path)) ? "True" : "False";
    }

    std::string parseHasTrailingSlash() {
        expect("(");
        const std::string value = parseValue();
        expect(")");

        return (!value.empty() && (value.back() == '/' || value.back() == '\\'))
            ? "True"
            : "False";
    }

    std::string getPropertyValue(const std::string &name) const {
        const auto it = mVariables.find(name);
        if (it != mVariables.end())
            return it->second;

        const char *envValue = std::getenv(name.c_str());
        return envValue ? envValue : "";
    }

    std::string expandProperties(const std::string &input) const {
        // Delegate to PropertyValueExpander. In condition context unknown
        // variables must expand to "" (MSBuild semantics for quoted strings).
        PropertyValueExpander expander{mProject, mVariables, input};
        expander.mReplaceUnknown = true;
        return expander.expand();
    }

    static std::string applyMethod(std::string value,
                                   const std::string &method,
                                   const std::vector<std::string> &args) {
        return applyPropertyMethod(std::move(value), method, args);
    }

    static int compareVersions(const std::vector<int> &lhs,
                               const std::vector<int> &rhs) {
        const std::size_t count = std::max(lhs.size(), rhs.size());

        for (std::size_t i = 0; i < count; ++i) {
            // Missing trailing components are treated as -1 (not 0),
            // so '17' < '17.0' and '1.2.3' < '1.2.3.0'.
            const int l = (i < lhs.size()) ? lhs[i] : -1;
            const int r = (i < rhs.size()) ? rhs[i] : -1;
            if (l < r)
                return -1;
            if (l > r)
                return 1;
        }

        return 0;
    }

    static bool compareVersionResult(int result, const std::string &op) {
        if (op == "<")
            return result < 0;
        if (op == ">")
            return result > 0;
        if (op == "<=")
            return result <= 0;
        if (op == ">=")
            return result >= 0;
        return false;
    }

    static bool compare(const std::string &lhs, const std::string &op, const std::string &rhs) {
        const auto parseVersion = [](const std::string &s) -> std::vector<int> {
            if (s.empty())
                return {};

            std::size_t pos = (s[0] == 'v' || s[0] == 'V') ? 1 : 0;
            if (pos == s.size())
                return {};

            std::vector<int> parts;
            while (pos < s.size()) {
                const std::size_t dot = s.find('.', pos);
                const std::size_t end =
                    dot == std::string::npos ? s.size() : dot;

                if (end == pos)
                    return {};

                const std::string part = s.substr(pos, end - pos);
                char *endPtr = nullptr;
                const long value = std::strtol(part.c_str(), &endPtr, 10);

                if (endPtr != part.c_str() && *endPtr == '\0')
                    parts.push_back(static_cast<int>(value));
                else
                    return {};

                if (dot == std::string::npos)
                    break;

                pos = dot + 1;
            }

            if (parts.empty())
                return {};

            return parts;
        };

        if (op == "==" || op == "!=") {
            // MSBuild == / != is a plain case-insensitive string comparison.
            const bool strEqual = caseInsensitiveStringCompare(lhs, rhs) == 0;
            return (op == "==") ? strEqual : !strEqual;
        }

        if (caseInsensitiveStringCompare(lhs, "Current") == 0) {
            const auto rhsVersion = parseVersion(rhs);
            if (!rhsVersion.empty()) {
                const std::vector<int> currentVersion{ 18 };
                return compareVersionResult(compareVersions(currentVersion, rhsVersion), op);
            }
        }

        if (caseInsensitiveStringCompare(rhs, "Current") == 0) {
            const auto lhsVersion = parseVersion(lhs);
            if (!lhsVersion.empty()) {
                const std::vector<int> currentVersion{ 18 };
                return compareVersionResult(compareVersions(lhsVersion, currentVersion), op);
            }
        }

        long lhsInt = 0;
        long rhsInt = 0;
        if (parseInteger(lhs, lhsInt) && parseInteger(rhs, rhsInt)) {
            if (op == "<") return lhsInt < rhsInt;
            if (op == ">") return lhsInt > rhsInt;
            if (op == "<=") return lhsInt <= rhsInt;
            if (op == ">=") return lhsInt >= rhsInt;
        }

        const std::vector<int> lhsVersion = parseVersion(lhs);
        const std::vector<int> rhsVersion = parseVersion(rhs);

        if (!lhsVersion.empty() && !rhsVersion.empty())
            return compareVersionResult(compareVersions(lhsVersion, rhsVersion), op);

        throw std::runtime_error("Cannot compare '" + lhs + "' and '" + rhs + "'");
    }
};

bool ImportProject::evalCondition(const std::string &condition, const PropertiesMap &properties) {
    try {
        return ConditionParser(*this, condition, properties).parse();
    } catch (const std::exception &) {
        // malformed or unhandled condition syntax (e.g. property functions,
        // unknown methods, bare .Property access) — treat as false so import continues
        return false;
    }
}

bool ImportProject::conditionIsTrue(const tinyxml2::XMLElement *node,  const PropertiesMap &properties) {
    const char *condAttr = node->Attribute("Condition");
    if (!condAttr)
        return true;
    return evalCondition(condAttr, properties);
}

bool ImportProject::hasName(const tinyxml2::XMLElement *node, const char *nodeName, const PropertiesMap &properties) {
    const char *name = node->Name();
    if (!name || std::strcmp(nodeName, name) != 0)
        return false;
    return conditionIsTrue(node, properties);
}

bool ImportProject::hasNameAndAttribute(const tinyxml2::XMLElement *node, const char *nodeName, const char *attrName, const PropertiesMap &properties) {
    const char *name = node->Name();
    const char *attr = node->Attribute(attrName);
    if (!name || !attr || std::strcmp(nodeName, name) != 0)
        return false;
    return conditionIsTrue(node, properties);
}

bool ImportProject::hasNameAndLabel(const tinyxml2::XMLElement *node, const char *nodeName, const char *nodeAttr, const PropertiesMap &properties) {
    const char *name = node->Name();
    const char *label = node->Attribute("Label");
    if (!name || !label || std::strcmp(nodeName, name) != 0 || std::strcmp(label, nodeAttr) != 0)
        return false;
    return conditionIsTrue(node, properties);
}

bool ImportProject::hasNameAndNotLabel(const tinyxml2::XMLElement *node, const char *nodeName, const char *nodeAttr, const PropertiesMap &properties) {
    const char *name = node->Name();
    if (!name || std::strcmp(nodeName, name) != 0)
        return false;
    const char *label = node->Attribute("Label");
    if (label && std::strcmp(label, nodeAttr) == 0)
        return false;
    return conditionIsTrue(node, properties);
}

namespace {
    // Trim leading and trailing ASCII whitespace in-place.
    void trimWhitespace(std::string &s) {
        const auto first = s.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) { s.clear(); return; }
        s.erase(0, first);
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    }

    std::list<std::string> toStringList(const std::string &s)
    {
        std::list<std::string> ret;
        std::string::size_type pos1 = 0;
        std::string::size_type pos2;
        while ((pos2 = s.find(';',pos1)) != std::string::npos) {
            if (pos2 > pos1) {
                std::string piece = s.substr(pos1, pos2-pos1);
                trimWhitespace(piece);
                if (!piece.empty())
                    ret.push_back(std::move(piece));
            }
            pos1 = pos2 + 1;
            if (pos1 >= s.size())
                break;
        }
        if (pos1 < s.size()) {
            std::string piece = s.substr(pos1);
            trimWhitespace(piece);
            if (!piece.empty())
                ret.push_back(std::move(piece));
        }
        return ret;
    }

    /// Return the filename (basename) component of a forward-slash-normalised path.
    std::string fileBasename(const std::string &path) {
        const auto pos = path.rfind('/');
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

    /// Return the stem of \p filename (basename with its final extension removed).
    /// Strips only the tail extension so "foo.targets.props" → "foo.targets", not "foo".
    std::string fileStem(const std::string &filename) {
        const std::string ext = Path::getFilenameExtension(filename);
        if (ext.empty() || filename.size() <= ext.size())
            return filename;
        return filename.substr(0, filename.size() - ext.size());
    }

    /// Return \p path with its root component stripped.
    /// The path must already be normalised to forward slashes.
    /// UNC paths  "//server/share/foo/"  →  "foo/"
    /// Local paths "C:/foo/"             →  "foo/"
    /// The trailing separator (if any) is preserved unchanged.
    std::string stripPathRoot(const std::string &path) {
        if (path.size() >= 2 && path[0] == '/' && path[1] == '/') {
            // UNC: //server/share/... — skip server then share component.
            const auto serverEnd = path.find('/', 2);
            if (serverEnd == std::string::npos)
                return {};
            const auto shareEnd = path.find('/', serverEnd + 1);
            if (shareEnd == std::string::npos)
                return {};
            return path.substr(shareEnd + 1);
        }
        // Local absolute (C:/...) or relative: strip up to and including the first '/'.
        const auto pos = path.find('/');
        return (pos != std::string::npos) ? path.substr(pos + 1) : path;
    }

    struct MSBuildThis {
        PropertiesMap &propertiesMap;
        std::string thisFile;
        std::string thisFileName;
        std::string thisFileExtension;
        std::string thisFileDirectory;
        std::string thisFileDirectoryNoRoot;
        std::string thisFileFullPath;

        MSBuildThis(const std::string &filename, PropertiesMap &properties)
            : propertiesMap(properties)
            , thisFile(properties["MSBuildThisFile"])
            , thisFileName(properties["MSBuildThisFileName"])
            , thisFileExtension(properties["MSBuildThisFileExtension"])
            , thisFileDirectory(properties["MSBuildThisFileDirectory"])
            , thisFileDirectoryNoRoot(properties["MSBuildThisFileDirectoryNoRoot"])
            , thisFileFullPath(properties["MSBuildThisFileFullPath"]) {
            setMSBuildThis(filename, properties);
        }

        static void setMSBuildThis(const std::string &filename, PropertiesMap &properties) {
            // Normalize once so all subsequent path ops can assume '/' separators.
            const std::string nfilename = Path::simplifyPath(Path::fromNativeSeparators(filename));
            properties["MSBuildThisFileFullPath"] = nfilename;
            const std::string thisFile = fileBasename(nfilename);
            properties["MSBuildThisFile"] = thisFile;
            properties["MSBuildThisFileName"] = fileStem(thisFile);
            properties["MSBuildThisFileDirectory"] = Path::getPathFromFilename(nfilename);
            properties["MSBuildThisFileDirectoryNoRoot"] = stripPathRoot(Path::getPathFromFilename(nfilename));
            properties["MSBuildThisFileExtension"] = Path::getFilenameExtensionInLowerCase(nfilename);
        }

        ~MSBuildThis() {
            propertiesMap["MSBuildThisFile"] = thisFile;
            propertiesMap["MSBuildThisFileName"] = thisFileName;
            propertiesMap["MSBuildThisFileExtension"] = thisFileExtension;
            propertiesMap["MSBuildThisFileDirectory"] = thisFileDirectory;
            propertiesMap["MSBuildThisFileDirectoryNoRoot"] = thisFileDirectoryNoRoot;
            propertiesMap["MSBuildThisFileFullPath"] = thisFileFullPath;
        }
    };

    struct ImportStackGuard {
        std::unordered_set<std::string> &mStack;
        std::string mKey;

        ImportStackGuard(std::unordered_set<std::string> &stack, std::string key)
            : mStack(stack), mKey(std::move(key)) {}

        ~ImportStackGuard() {
            mStack.erase(mKey);
        }
    };
}

std::string ImportProject::toAbsolute(const std::string &path)
{
    std::string internal(Path::fromNativeSeparators(path));
    if (Path::isAbsolute(internal))
        return Path::simplifyPath(internal);
    return Path::simplifyPath(Path::getCurrentPath() + "/" + internal);
}

std::string ImportProject::toAbsolute(const std::string &filename, const std::string &baseDir, const PropertiesMap &properties)
{
    std::string resolved(Path::fromNativeSeparators(filename));
    if (!simplifyPathWithVariables(resolved, properties))
        return resolved;

    if (Path::isAbsolute(resolved))
        return Path::simplifyPath(resolved);
    return Path::simplifyPath(baseDir + resolved);
}

bool ImportProject::simplifyPathWithVariables(std::string &s, const PropertiesMap &properties)
{
    // Normalize native separators before expansion so the expander sees clean
    // paths and debug messages report '/' not '\\'.
    s = Path::fromNativeSeparators(std::move(s));
    expandMSBuildVariables(s, properties);
    checkUnexpandedExpressions(s, "path");
    if (s.find("$(") != std::string::npos)
        return false;
    // Property values substituted above may also carry native separators; normalize again.
    s = Path::fromNativeSeparators(std::move(s));
    s = Path::simplifyPath(std::move(s));
    return true;
}

void ImportProject::fsSetIncludePaths(FileSettings &fs, const std::string &basepath, const std::list<std::string> &in, const PropertiesMap &properties)
{
    std::set<std::string> found;
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    const std::list<std::string> copyIn(in);
    fs.includePaths.clear();
    for (const std::string &ipath : copyIn) {
        if (ipath.empty())
            continue;
        if (startsWith(ipath, "%("))
            continue;
        std::string s(Path::fromNativeSeparators(ipath));
        if (!found.insert(s).second)
            continue;
        if (s[0] == '/' || (s.size() > 1U && s.compare(1, 2, ":/") == 0)) {
            if (!endsWith(s, '/'))
                s += '/';
            fs.includePaths.push_back(std::move(s));
            continue;
        }

        if (endsWith(s, '/')) // this is a temporary hack, simplifyPath can crash if path ends with '/'
            s.pop_back();

        if (s.find("$(") == std::string::npos) {
            s = Path::simplifyPath(basepath + s);
        } else {
            if (!simplifyPathWithVariables(s, properties))
                continue;
        }
        if (s.empty())
            continue;
        fs.includePaths.push_back(s.back() == '/' ? s : (s + '/'));
    }
}

void ImportProject::addProperty(const tinyxml2::XMLElement *node, PropertiesMap &properties) {
    const char *eName = node->Name();
    if (!eName || !conditionIsTrue(node, properties))
        return;
    const char *eText = node->GetText();
    std::string text(eText ? eText : "");
    // Trim indentation/newlines from pretty-printed XML text content.
    trimWhitespace(text);
    // Normalize native path separators before expansion so property values are
    // stored with '/' and debug messages show normalized paths.
    text = Path::fromNativeSeparators(std::move(text));
    const std::string selfRef = "$(" + std::string(eName) + ")";
    // Pre-expand the prior value before embedding it so tokens already resolvable
    // in this context are expanded inside `original` rather than being re-evaluated
    // in the combined string.  This reduces the risk that step-3 cleanup erases
    // content that legitimately arrived through the accumulated value.
    std::string original = properties[eName];
    expandMSBuildVariables(original, properties);
    findAndReplace(original, selfRef, "");   // break any tainted self-ref in original
    findAndReplace(text, selfRef, original);
    expandMSBuildVariables(text, properties);
    // Safety net: erase self-references that survived (e.g. a token whose expansion
    // is not yet available), matching MSBuild's "undefined = empty string" rule.
    findAndReplace(text, selfRef, "");
    properties[eName] = text;
    checkUnexpandedExpressions(text, eName);
}

void ImportProject::addMetadata(const tinyxml2::XMLElement *node, const PropertiesMap &properties, MetadataMap &metadata) {
    const char *eName = node->Name();
    if (!eName || !conditionIsTrue(node, properties))
        return;
    const char *eText = node->GetText();
    std::string text(eText ? eText : "");
    trimWhitespace(text);
    text = Path::fromNativeSeparators(std::move(text));
    const std::string metaSelfRef = "%(" + std::string(eName) + ")";
    const std::string propSelfRef  = "$(" + std::string(eName) + ")";
    // Pre-expand the accumulated metadata value before embedding it: resolve %(other)
    // metadata refs and $(prop) refs inside `original` first, then break any tainted
    // self-references, so they don't propagate into the new value and risk step-3 erasure.
    std::string original = metadata[eName];
    findAndReplace(original, metaSelfRef, "");
    {
        std::string::size_type p = 0;
        while ((p = original.find("%(", p)) != std::string::npos) {
            const std::string::size_type e = original.find(')', p);
            if (e == std::string::npos) break;
            const std::string key = original.substr(p + 2, e - p - 2);
            const auto it = metadata.find(key);
            const std::string repl = (it != metadata.end()) ? it->second : std::string();
            original.replace(p, e - p + 1, repl);
            p += repl.size();
        }
    }
    expandMSBuildVariables(original, properties);
    findAndReplace(original, propSelfRef, "");

    findAndReplace(text, metaSelfRef, original);
    std::string::size_type pos = 0;
    while ((pos = text.find("%(", pos)) != std::string::npos) {
        const std::string::size_type end = text.find(')', pos);
        if (end == std::string::npos)
            break;
        const std::string key = text.substr(pos + 2, end - pos - 2);
        const auto it = metadata.find(key);
        const std::string replacement = (it != metadata.end()) ? it->second : std::string();
        text.replace(pos, end - pos + 1, replacement);
        pos += replacement.size();
    }
    expandMSBuildVariables(text, properties);
    // Handle $(eName) self-references: some props files use property-style
    // accumulation (e.g. <DisableSpecificWarnings>$(DisableSpecificWarnings);4100
    // </DisableSpecificWarnings>) in ItemDefinitionGroup blocks.
    findAndReplace(text, propSelfRef, original);
    findAndReplace(text, propSelfRef, "");
    metadata[eName] = text;
    checkUnexpandedExpressions(text, eName);
}

std::string ImportProject::getMetadata(const tinyxml2::XMLElement *node, const PropertiesMap &properties, const MetadataMap &metadata, const std::string &original) {
    const char *eName = node->Name();
    const char *eText = node->GetText();
    if (!eName || !eText || !conditionIsTrue(node, properties))
        return original;
    std::string text(eText);
    trimWhitespace(text);
    text = Path::fromNativeSeparators(std::move(text));
    const std::string metaSelfRef = "%(" + std::string(eName) + ")";
    const std::string propSelfRef  = "$(" + std::string(eName) + ")";
    // Pre-expand `original` (the prior per-item value) before embedding it,
    // matching the same strategy used in addMetadata and addProperty.
    std::string expandedOriginal = original;
    findAndReplace(expandedOriginal, metaSelfRef, "");
    {
        std::string::size_type p = 0;
        while ((p = expandedOriginal.find("%(", p)) != std::string::npos) {
            const std::string::size_type e = expandedOriginal.find(')', p);
            if (e == std::string::npos) break;
            const std::string key = expandedOriginal.substr(p + 2, e - p - 2);
            const auto it = metadata.find(key);
            const std::string repl = (it != metadata.end()) ? it->second : std::string();
            expandedOriginal.replace(p, e - p + 1, repl);
            p += repl.size();
        }
    }
    expandMSBuildVariables(expandedOriginal, properties);
    findAndReplace(expandedOriginal, propSelfRef, "");

    findAndReplace(text, metaSelfRef, expandedOriginal);
    {
        std::string::size_type pos = 0;
        while ((pos = text.find("%(", pos)) != std::string::npos) {
            const std::string::size_type end = text.find(')', pos);
            if (end == std::string::npos) break;
            const std::string key = text.substr(pos + 2, end - pos - 2);
            const auto it = metadata.find(key);
            const std::string replacement = (it != metadata.end()) ? it->second : std::string();
            text.replace(pos, end - pos + 1, replacement);
            pos += replacement.size();
        }
    }
    expandMSBuildVariables(text, properties);
    // Handle $(eName) self-references: same accumulation pattern as addMetadata.
    findAndReplace(text, propSelfRef, expandedOriginal);
    findAndReplace(text, propSelfRef, "");
    checkUnexpandedExpressions(text, eName);
    return text;
}

const std::string &ImportProject::importResultStr(ImportProject::ImportResult result) {
    static std::string ok("ok");
    static std::string notResolvable("Not Resolvable");
    static std::string notFound("Not Found");
    static std::string notValid("Not Valid");
    static std::string cycle("Cycle");
    static std::string unknown("Unknown");

    switch (result) {
    case ImportProject::ImportResult::Ok:
        return ok;
    case ImportProject::ImportResult::NotResolvable:
        return notResolvable;
    case ImportProject::ImportResult::NotFound:
        return notFound;
    case ImportProject::ImportResult::NotValid:
        return notValid;
    case ImportProject::ImportResult::Cycle:
        return cycle;
    }
    return unknown;
}

ImportProject::ImportResult ImportProject::importCompile(const tinyxml2::XMLElement *node,
                                                         const std::string &projectDir,
                                                         PropertiesMap &properties,
                                                         const MetadataMap &metadata,
                                                         std::list<ItemGroupClCompile> &compileList) {
    const char *include = node->Attribute("Include");
    if (!include)
        return ImportResult::NotFound;

    std::string toInclude = toAbsolute(include, projectDir, properties);
    if (!Path::acceptFile(toInclude))
        return ImportResult::NotFound;

    ItemGroupClCompile compile(toInclude);
    // a file with no override of its own inherits the ItemDefinitionGroup value outright
    compile.metadata = metadata;
    bool excludedFromBuild = false;

    for (const tinyxml2::XMLElement *e1 = node->FirstChildElement(); e1; e1 = e1->NextSiblingElement()) {
        const char *text = e1->GetText();
        if (!text)
            continue;

        if (hasName(e1, "ExcludedFromBuild", properties)) {
            std::string excludedText(text);
            expandMSBuildVariables(excludedText, properties);
            if (caseInsensitiveStringCompare(excludedText, "true") == 0) {
                excludedFromBuild = true;
                break;
            }
        } else if (hasName(e1, "AdditionalIncludeDirectories", properties)) {
            auto &v = compile.metadata["AdditionalIncludeDirectories"];
            v = getMetadata(e1, properties, compile.metadata, v);
        } else if (hasName(e1, "ForcedIncludeFiles", properties)) {
            auto &v = compile.metadata["ForcedIncludeFiles"];
            v = getMetadata(e1, properties, compile.metadata, v);
        } else if (hasName(e1, "PreprocessorDefinitions", properties)) {
            auto &v = compile.metadata["PreprocessorDefinitions"];
            v = getMetadata(e1, properties, compile.metadata, v);
        } else if (hasName(e1, "LanguageStandard", properties)) {
            auto &v = compile.metadata["LanguageStandard"];
            v = getMetadata(e1, properties, compile.metadata, v);
        } else if (hasName(e1, "AdditionalOptions", properties)) {
            auto &v = compile.metadata["AdditionalOptions"];
            v = getMetadata(e1, properties, compile.metadata, v);
        } else if (hasName(e1, "AdditionalUsingDirectories", properties)) {
            auto &v = compile.metadata["AdditionalUsingDirectories"];
            v = getMetadata(e1, properties, compile.metadata, v);
        }
    }

    if (!compile.metadata["AdditionalOptions"].empty()) {
        std::vector<std::string> args;
        std::string arg;
        bool quoted = false;
        const std::string &additionalOptions = compile.metadata["AdditionalOptions"];

        for (std::size_t i = 0; i < additionalOptions.size(); ++i) {
            const char c = additionalOptions[i];

            if (c == '"') {
                quoted = !quoted;
            } else if (std::isspace(static_cast<unsigned char>(c)) && !quoted) {
                if (!arg.empty()) {
                    args.emplace_back(std::move(arg));
                    arg.clear();
                }
            } else {
                arg += c;
            }
        }

        if (!arg.empty())
            args.emplace_back(std::move(arg));

        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string &option = args[i];

            if (option.size() >= 2 &&
                (option[0] == '/' || option[0] == '-') &&
                (option[1] == 'D' || option[1] == 'd')) {

                std::string define = option.substr(2);

                // /D NAME
                if (define.empty() && i + 1 < args.size())
                    define = args[++i];

                if (!define.empty()) {
                    if (!compile.metadata["PreprocessorDefinitions"].empty())
                        compile.metadata["PreprocessorDefinitions"] += ';';
                    compile.metadata["PreprocessorDefinitions"] += define;
                }

            } else if (option.size() >= 2 &&
                       (option[0] == '/' || option[0] == '-') &&
                       (option[1] == 'I' || option[1] == 'i')) {

                std::string path = option.substr(2);

                // /I path
                if (path.empty() && i + 1 < args.size())
                    path = args[++i];

                if (!path.empty()) {
                    if (!compile.metadata["AdditionalIncludeDirectories"].empty())
                        compile.metadata["AdditionalIncludeDirectories"] += ';';
                    compile.metadata["AdditionalIncludeDirectories"] += path;
                }
            } else if (option == "/std:c++11" || option == "-std=c++11") {
                compile.metadata["LanguageStandard"] = "stdcpp11";
            } else if (option == "/std:c++14" || option == "-std=c++14") {
                compile.metadata["LanguageStandard"] = "stdcpp14";
            } else if (option == "/std:c++17" || option == "-std=c++17") {
                compile.metadata["LanguageStandard"] = "stdcpp17";
            } else if (option == "/std:c++20" || option == "-std=c++20") {
                compile.metadata["LanguageStandard"] = "stdcpp20";
            } else if (option == "/std:c++23" || option == "-std=c++23") {
                compile.metadata["LanguageStandard"] = "stdcpp23";
            } else if (option == "/std:c++latest" || option == "-std=c++latest") {
                compile.metadata["LanguageStandard"] = "stdcpplatest";
            }
        }
    }

    if (!excludedFromBuild)
        compileList.emplace_back(compile);

    return ImportResult::Ok;
}

ImportProject::ImportResult ImportProject::importProject(const tinyxml2::XMLElement *node,
                                                         const std::string &projectDir,
                                                         PropertiesMap &properties,
                                                         MetadataMap &metadata,
                                                         std::list<ItemGroupClCompile> &compileList,
                                                         std::list<ProjectConfiguration> &projectConfigurationList,
                                                         std::unordered_set<std::string> &importStack,
                                                         EvalPhase phase) {
    const char *projectAttribute = node->Attribute("Project");
    if (!projectAttribute)
        return ImportResult::Ok;
    // During the discovery pass, re-run as a Properties-phase import but silently
    // discard any errors/debugs it generates — approximate properties cause many
    // spurious failures that would confuse the user.
    if (phase == EvalPhase::Discover) {
        const auto errSize = errors.size();
        const auto dbgSize = debugs.size();
        const ImportResult r = importProject(node, projectDir, properties, metadata, compileList,
                                             projectConfigurationList, importStack, EvalPhase::Properties);
        errors.resize(errSize);
        debugs.resize(dbgSize);
        return r;
    }
    std::string file = toAbsolute(projectAttribute, projectDir, properties);
    std::string extension = Path::getFilenameExtensionInLowerCase(file);
    if (extension == ".props" || extension == ".targets") {
        const char *sdk = node->Attribute("Sdk");
        if (sdk)
            return ImportResult::NotResolvable;

        // Identify well-known system files by their logical filename rather than a
        // substring search on the full path.  This avoids false positives when a
        // user file's path happens to contain "Microsoft.Cpp.props" etc.
        //
        // Works for both fully-resolved paths ("C:/…/Microsoft.Cpp.targets") and
        // paths that still carry an unresolved property reference
        // ("$(VCTargetsPath)Microsoft.Cpp.targets"): after the last '/' we strip the
        // trailing ')' of any property reference to reach the bare filename.
        const auto filenameIs = [&file](const char *name) {
            const std::string::size_type slash = file.rfind('/');
            const std::string part = (slash != std::string::npos) ? file.substr(slash + 1) : file;
            const std::string::size_type paren = part.rfind(')');
            const std::string namePart = (paren != std::string::npos) ? part.substr(paren + 1) : part;
            return caseInsensitiveStringCompare(namePart, name) == 0;
        };

        if (filenameIs("Microsoft.Cpp.targets")) {
            auto it = properties.find("ForceImportBeforeCppTargets");
            if (it != properties.end()) {
                ImportResult result = importPropsOrTargets(it->second, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + it->second + "\" - " + importResultStr(result));
                    return result;
                }
            }

            // Microsoft.Common.targets (imported by Microsoft.Cpp.targets) sets
            // OutputPath = $(OutDir) when OutputPath is not already defined.
            // Emulate this before importing Directory.Build.targets so that files
            // in that chain (e.g. bundle output paths) can expand $(OutputPath).
            if (phase == EvalPhase::Properties && properties.find("OutputPath") == properties.end()) {
                const auto outDirIt = properties.find("OutDir");
                if (outDirIt != properties.end())
                    properties["OutputPath"] = outDirIt->second;
            }

            if (file.find("$(") == std::string::npos) {
                // Path is fully resolved — import the actual file.  The real import chain
                // (Microsoft.Cpp.targets → Microsoft.Common.targets → Directory.Build.targets)
                // will handle Directory.Build.targets, so do NOT import it synthetically here.
                ImportResult result = importPropsOrTargets(file, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + file + "\" - " + importResultStr(result));
                    return result;
                }
            } else {
                // VCTargetsPath unresolved — the real import chain cannot run, so emulate its
                // key side-effect: Microsoft.Cpp.targets → Microsoft.Common.targets → Directory.Build.targets.
                std::string directoryBuildTargets = findFile(projectDir, "Directory.Build.targets");
                if (!directoryBuildTargets.empty()) {
                    ImportResult result = importPropsOrTargets(directoryBuildTargets, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                    if (result > ImportResult::NotResolvable)
                        debugs.emplace_back("Could not fully import \"" + directoryBuildTargets + "\" - " + importResultStr(result) + " (continuing)");
                }
            }

            it = properties.find("ForceImportAfterCppTargets");
            if (it != properties.end()) {
                ImportResult result = importPropsOrTargets(it->second, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + it->second + "\" - " + importResultStr(result));
                    return result;
                }
            }

            return ImportResult::Ok;
        }

        if (filenameIs("Microsoft.Cpp.Default.props")) {
            auto it = properties.find("ForceImportBeforeCppDefaultProps");
            if (it != properties.end()) {
                ImportResult result = importPropsOrTargets(it->second, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + it->second + "\" - " + importResultStr(result));
                    return result;
                }
            }

            if (file.find("$(") == std::string::npos) {
                // Path is fully resolved — import the actual file.  The real import chain
                // (Microsoft.Cpp.Default.props → Microsoft.Common.props → Directory.Build.props)
                // will handle Directory.Build.props, so do NOT import it synthetically here.
                ImportResult result = importPropsOrTargets(file, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + file + "\" - " + importResultStr(result));
                    return result;
                }
            } else if (phase == EvalPhase::Properties) {
                // VCTargetsPath unresolved — the real import chain cannot run, so emulate its
                // key side-effects here, including the Directory.Build.props import that
                // Microsoft.Common.props would normally perform.
                // Microsoft.Cpp.Default.props → Microsoft.Common.props → Directory.Build.props.
                {
                    std::string directoryBuildProps = findFile(projectDir, "Directory.Build.props");
                    if (!directoryBuildProps.empty()) {
                        ImportResult result = importPropsOrTargets(directoryBuildProps, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                        if (result > ImportResult::NotResolvable)
                            debugs.emplace_back("Could not fully import \"" + directoryBuildProps + "\" - " + importResultStr(result) + " (continuing)");
                    }
                }

                // Emulate key defaults set by Microsoft.Cpp.Default.props.
                // Derive DefaultPlatformToolset from VisualStudioVersion (already in properties from
                // the .sln header or the importVcxproj default).  The mapping is:
                //   VS 10 → v100, VS 11 → v110, VS 12 → v120,
                //   VS 14 → v140, VS 15 → v141, VS 16 → v142, VS 17 → v143, VS 18 → v144, …
                if (properties.find("DefaultPlatformToolset") == properties.end()) {
                    auto vsIt = properties.find("VisualStudioVersion");
                    if (vsIt != properties.end()) {
                        int major = 0;
                        try { major = std::stoi(vsIt->second); } catch (...) {}
                        std::string toolset;
                        if (major >= 14)
                            toolset = "v14" + std::to_string(major - 14);
                        else if (major == 12)
                            toolset = "v120";
                        else if (major == 11)
                            toolset = "v110";
                        else if (major == 10)
                            toolset = "v100";
                        if (!toolset.empty())
                            properties["DefaultPlatformToolset"] = toolset;
                    }
                }
            }

            it = properties.find("ForceImportAfterCppDefaultProps");
            if (it != properties.end()) {
                ImportResult result = importPropsOrTargets(it->second, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + it->second + "\" - " + importResultStr(result));
                    return result;
                }
            }

            return ImportResult::Ok;
        }

        if (filenameIs("Microsoft.Cpp.props")) {
            // ForceImportBeforeCppProps: honour any value set before Microsoft.Cpp.props
            // is processed (e.g. by the vcxproj itself or by Directory.Build.props, which
            // was already imported via the Microsoft.Cpp.Default.props handler above).
            auto it = properties.find("ForceImportBeforeCppProps");
            if (it != properties.end()) {
                ImportResult result = importPropsOrTargets(it->second, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + it->second + "\" - " + importResultStr(result));
                    return result;
                }
            }

            if (file.find("$(") != std::string::npos) {
                if (phase == EvalPhase::Properties) {
                    // VCTargetsPath unresolved — provide output-dir defaults that Cpp.props defines,
                    // but only when the vcxproj has not already set them in an earlier PropertyGroup.
                    std::string intDir = "$(Platform)/$(Configuration)/";
                    std::string outDir = "$(SolutionDir)$(Platform)/$(Configuration)/";
                    expandMSBuildVariables(intDir, properties);
                    expandMSBuildVariables(outDir, properties);
                    properties.emplace("IntDir", intDir);
                    properties.emplace("OutDir", outDir);
                    // GeneratedFilesDir defaults to "Generated Files\" (relative to the project
                    // directory) — that is what Microsoft.Cpp.Default.props provides, and it is
                    // the path CppWinRT projects use by convention (e.g. module.g.cpp).
                    // Do NOT prefix with IntDir here: IntDir is an absolute intermediate path
                    // specific to the build configuration, whereas GeneratedFilesDir is a
                    // project-relative folder that is often committed to source control.
                    properties.emplace("GeneratedFilesDir", "Generated Files/");
                }
            } else {
                ImportResult result = importPropsOrTargets(file, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + file + "\" - " + importResultStr(result));
                    return result;
                }
            }

            it = properties.find("ForceImportAfterCppProps");
            if (it != properties.end()) {
                ImportResult result = importPropsOrTargets(it->second, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                if (result > ImportResult::NotResolvable) {
                    errors.emplace_back("Could not import \"" + it->second + "\" - " + importResultStr(result));
                    return result;
                }
            }

            return ImportResult::Ok;
        }

        ImportResult result = importPropsOrTargets(file, properties, metadata, compileList, projectConfigurationList, importStack, phase);
        if (result > ImportResult::NotResolvable) {
            errors.emplace_back("Could not import \"" + file + "\" - " + importResultStr(result));
            return result;
        }
        if (result == ImportResult::NotResolvable) {
            debugs.emplace_back("Could not import \"" + file + "\" - " + importResultStr(result));
        }
    } else if (extension == ".vcxitems") {
        ImportResult result = importVcxitems(file, properties, metadata, compileList, projectConfigurationList, importStack, phase);
        if (result > ImportResult::NotResolvable)
            return result;
        if (result == ImportResult::NotResolvable) {
            debugs.emplace_back("Could not import \"" + file + "\" - " + importResultStr(result));
        }
    } else {
        debugs.emplace_back("Could not import \"" + file + "\" unsupported extension " + extension);
    }
    return ImportResult::Ok;
}

ImportProject::ImportResult ImportProject::importPropsOrTargets(const std::string &file,
                                                                PropertiesMap &properties,
                                                                MetadataMap &metadata,
                                                                std::list<ItemGroupClCompile> &compileList,
                                                                std::list<ProjectConfiguration> &projectConfigurationList,
                                                                std::unordered_set<std::string> &importStack,
                                                                EvalPhase phase)
{
    std::string filename(file);
    // properties can't be resolved
    if (!simplifyPathWithVariables(filename, properties))
        return ImportResult::NotResolvable;

    // prepend project dir (if it exists) to transform relative paths into absolute ones
    if (!Path::isAbsolute(filename) && properties.count("ProjectDir") > 0)
        filename = toAbsolute(filename, properties.at("ProjectDir"), properties);

    // detect circular property sheet imports (A imports B, B imports A, a file importing
    // itself, ...) instead of recursing until the stack overflows - mirrors MSBuild's own
    // import-cycle detection, which errors out rather than looping forever
    const std::string simplifiedFilename = Path::simplifyPath(filename);
    if (!importStack.insert(simplifiedFilename).second)
        return ImportResult::Cycle;

    ImportStackGuard guard(importStack, simplifiedFilename);  // erases on any exit from here

    tinyxml2::XMLDocument doc;
    if (doc.LoadFile(filename.c_str()) != tinyxml2::XML_SUCCESS)
        return ImportResult::NotFound;

    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr)
        return ImportResult::NotValid;

    MSBuildThis msBuildThis(filename, properties);
    std::string propsDir = Path::getPathFromFilename(filename);

    ImportResult ret = ImportResult::Ok;
    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (hasName(node, "ImportGroup", properties)) {
            // Accept any <ImportGroup> (PropertySheets, Shared, unlabeled) — .targets files
            // commonly use unlabeled or differently-labeled groups for transitive imports.
            const char* label = node->Attribute("Label");
            // MSBuild label matching is case-insensitive; use caseInsensitiveStringCompare
            // so that non-VS toolchains that emit lowercase labels are handled correctly.
            const bool isPropertySheets = (label == nullptr) ||
                                          (caseInsensitiveStringCompare(label, "PropertySheets") == 0) ||
                                          (caseInsensitiveStringCompare(label, "Shared") == 0) ||
                                          (caseInsensitiveStringCompare(label, "ExtensionSettings") == 0) ||
                                          (caseInsensitiveStringCompare(label, "ExtensionTargets") == 0);
            if (isPropertySheets) {
                for (const tinyxml2::XMLElement *importGroup = node->FirstChildElement(); importGroup; importGroup = importGroup->NextSiblingElement()) {
                    if (hasNameAndAttribute(importGroup, "Import", "Project", properties)) {
                        ImportResult result = importProject(importGroup, propsDir, properties, metadata, compileList, projectConfigurationList, importStack, phase);
                        // Non-fatal: log and continue so later elements (including
                        // <ItemGroup Label="ProjectConfigurations">) are still processed.
                        if (result > ImportResult::NotResolvable) {
                            if (phase != EvalPhase::Discover) {
                                const char *proj = importGroup->Attribute("Project");
                                debugs.emplace_back("Could not fully import \"" + std::string(proj ? proj : "") + "\" - " + importResultStr(result) + " (continuing)");
                            }
                            ret = std::max(result, ret);
                        }
                    }
                }
            }
        } else if (hasName(node, "PropertyGroup", properties)) {
            if (phase == EvalPhase::Properties || phase == EvalPhase::Discover) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement())
                    addProperty(e, properties);
            }
        } else if (hasName(node, "ItemDefinitionGroup", properties)) {
            if (phase == EvalPhase::ItemDefs) {
                for (const tinyxml2::XMLElement *e1 = node->FirstChildElement(); e1; e1 = e1->NextSiblingElement()) {
                    if (hasName(e1, "ClCompile", properties)) {
                        for (const tinyxml2::XMLElement *e2 = e1->FirstChildElement(); e2; e2 = e2->NextSiblingElement()) {
                            addMetadata(e2, properties, metadata);
                        }
                    }
                }
            }
        } else if (hasNameAndLabel(node, "ItemGroup", "ProjectConfigurations", properties)) {
            if (phase == EvalPhase::Properties || phase == EvalPhase::Discover) {
                for (const tinyxml2::XMLElement *pcNode = node->FirstChildElement("ProjectConfiguration"); pcNode; pcNode = pcNode->NextSiblingElement("ProjectConfiguration")) {
                    const ProjectConfiguration pc(pcNode);
                    if (!pc.configuration.empty()) {
                        // Deduplicate: the same config can arrive again when Directory.Build.props /
                        // Cpp.Build.props is re-imported inside the per-config loop.
                        const bool already = std::any_of(projectConfigurationList.cbegin(),
                                                         projectConfigurationList.cend(),
                                                         [&pc](const ProjectConfiguration &existing) {
                            return existing.name == pc.name;
                        });
                        if (!already) {
                            projectConfigurationList.emplace_back(pc);
                            mAllVSConfigs.insert(pc.configuration);
                        }
                    }
                }
            }
        } else if (hasNameAndAttribute(node, "Import", "Project", properties)) {
            ImportResult result = importProject(node, propsDir, properties, metadata, compileList, projectConfigurationList, importStack, phase);
            // Non-fatal: log and continue so later elements (including
            // <ItemGroup Label="ProjectConfigurations">) are still processed.
            if (result > ImportResult::NotResolvable && phase != EvalPhase::Discover) {
                const char *proj = node->Attribute("Project");
                debugs.emplace_back("Could not fully import \"" + std::string(proj ? proj : "") + "\" - " + importResultStr(result) + " (continuing)");
                ret = std::max(result, ret);
            }
        }
    }

    return ret;
}

ImportProject::ImportResult ImportProject::importVcxitems(const std::string &items,
                                                          PropertiesMap &properties,
                                                          MetadataMap &metadata,
                                                          std::list<ItemGroupClCompile> &compileList,
                                                          std::list<ProjectConfiguration> &projectConfigurationList,
                                                          std::unordered_set<std::string> &importStack,
                                                          EvalPhase phase)
{
    std::string filename(items);
    // properties can't be resolved
    if (!simplifyPathWithVariables(filename, properties))
        return ImportResult::NotResolvable;

    const std::string simplifiedFilename = Path::simplifyPath(filename);
    if (!importStack.insert(simplifiedFilename).second)
        return ImportResult::Cycle;

    ImportStackGuard guard(importStack, simplifiedFilename);  // erases on any exit from here

    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError error = doc.LoadFile(filename.c_str());
    if (error != tinyxml2::XML_SUCCESS)
        return ImportResult::NotFound;

    const tinyxml2::XMLElement *const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr)
        return ImportResult::NotValid;

    const std::string itemsDir = Path::simplifyPath(Path::getPathFromFilename(filename));
    MSBuildThis msBuildThis(filename, properties);

    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (hasName(node, "ItemGroup", properties)) {
            if (phase == EvalPhase::Items) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                    if (hasName(e, "ClCompile", properties)) {
                        importCompile(e, itemsDir, properties, metadata, compileList);
                    }
                }
            }
        } else if (hasName(node, "PropertyGroup", properties)) {
            if (phase == EvalPhase::Properties) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement())
                    addProperty(e, properties);
            }
        } else if (hasName(node, "ItemDefinitionGroup", properties)) {
            if (phase == EvalPhase::ItemDefs) {
                for (const tinyxml2::XMLElement *e1 = node->FirstChildElement(); e1; e1 = e1->NextSiblingElement()) {
                    if (hasName(e1, "ClCompile", properties)) {
                        for (const tinyxml2::XMLElement *e2 = e1->FirstChildElement(); e2; e2 = e2->NextSiblingElement())
                            addMetadata(e2, properties, metadata);
                    }
                }
            }
        } else if (hasNameAndAttribute(node, "Import", "Project", properties)) {
            const ImportResult result = importProject(node, itemsDir, properties, metadata, compileList, projectConfigurationList, importStack, phase);
            if (result > ImportResult::NotResolvable)
                return result;
        }
    }

    return ImportResult::Ok;
}

bool ImportProject::importVcxproj(const std::string &filename,
                                  PropertiesMap &properties,
                                  const std::vector<std::string> &fileFilters)
{
    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError error = doc.LoadFile(filename.c_str());
    if (error != tinyxml2::XML_SUCCESS) {
        errors.emplace_back(std::string("Visual Studio project file is not a valid XML - ") + tinyxml2::XMLDocument::ErrorIDToName(error));
        return false;
    }

    // Normalize separators once; callers typically pass toAbsolute() results
    // but normalize here as a safety net so all subsequent rfind('/') are correct.
    const std::string nfilename = Path::simplifyPath(Path::fromNativeSeparators(filename));

    properties.emplace("VisualStudioVersion", "17.0");

    // User-extensible MSVC properties that legitimately default to "" when not
    // set by the project.  In real MSBuild every undefined property expands to
    // the empty string; seed them here so that props files which append to them
    // (e.g. <ExtraWarningsToDisable>...;$(DisableSpecificWarnings)</ExtraWarningsToDisable>)
    // produce a clean resolved value instead of leaving $(X) unexpanded.
    properties.emplace("DisableSpecificWarnings", "");

    properties["ProjectPath"] = nfilename;
    const std::string projFileName = fileBasename(nfilename);
    properties["ProjectFileName"] = projFileName;
    const std::string projName = fileStem(projFileName);
    properties["ProjectName"] = projName;
    properties["ShortProjectName"] = projName.substr(0, std::min(projName.size(), size_t(16)));
    properties["ProjectExt"] = Path::getFilenameExtensionInLowerCase(nfilename);
    properties["ProjectDir"] = Path::getPathFromFilename(nfilename);

    // importVcxproj called directly
    if (properties.find("SolutionDir") == properties.end()) {
        debugs.clear();
        properties["SolutionDir"] = properties["ProjectDir"];
    }

    properties["MSBuildProjectName"] = properties["ProjectName"];
    properties["MSBuildProjectExtension"] = properties["ProjectExt"];
    properties["MSBuildProjectDirectory"] = properties["ProjectDir"];
    // remove file seperator on end of path
    if (!properties["MSBuildProjectDirectory"].empty() &&
        (properties["MSBuildProjectDirectory"].back() == '/' ||
         properties["MSBuildProjectDirectory"].back() == '\\')) {
        properties["MSBuildProjectDirectory"].pop_back();
    }
    properties["MSBuildProjectFile"] = properties["ProjectFileName"];
    properties["MSBuildProjectFullPath"] = properties["ProjectPath"];

    // MSBuildProjectDirectoryNoRoot: like MSBuildThisFileDirectoryNoRoot but for the
    // project itself.  ProjectDir has a trailing '/' which we strip to match the
    // MSBuildProjectDirectory (no trailing separator) convention.
    std::string noRoot = stripPathRoot(properties["ProjectDir"]);
    if (!noRoot.empty() && noRoot.back() == '/')
        noRoot.pop_back();
    properties["MSBuildProjectDirectoryNoRoot"] = noRoot;

    MSBuildThis::setMSBuildThis(nfilename, properties);

    std::string projectDir = properties["ProjectDir"];
    std::list<ProjectConfiguration> projectConfigurationList;
    std::list<ItemGroupClCompile> compileList;
    std::unordered_set<std::string> importStack;
    MetadataMap metadata;

    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr) {
        errors.emplace_back("Visual Studio project file has no XML root node");
        return false;
    }

    // Read MSBuildToolsVersion directly from <Project ToolsVersion="...">.
    // "Current" is the standard value for VS2019+ and is the correct fallback.
    const char *toolsVersion = rootnode->Attribute("ToolsVersion");
    properties["MSBuildToolsVersion"] = toolsVersion ? toolsVersion : "Current";

    // find all Visual Studio project configurations
    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        if (hasNameAndLabel(node, "ItemGroup", "ProjectConfigurations", properties)) {
            for (const tinyxml2::XMLElement *pcNode = node->FirstChildElement("ProjectConfiguration"); pcNode; pcNode = pcNode->NextSiblingElement("ProjectConfiguration")) {
                const ProjectConfiguration pc(pcNode);
                if (!pc.configuration.empty()) {  // only require a configuration name
                    projectConfigurationList.emplace_back(pc);
                    mAllVSConfigs.insert(pc.configuration);
                }
            }
        }
    }

    // Discovery pass: if no ProjectConfigurations were found inline in the vcxproj, walk
    // its <Import>/<ImportGroup> nodes through importProject so that every MSBuild import
    // mechanism (Directory.Build.props, ForceImportBeforeCppProps, etc.) is honoured
    // generically — no special-casing of individual property names required.
    // We also process <PropertyGroup> nodes so that properties needed to resolve import
    // paths are available.  Stop as soon as configurations are found.
    // Use isolated copies of properties, metadata and importStack so that side-effects
    // of the discovery imports (extra properties, pre-populated import stack, etc.) do
    // not bleed into the real per-configuration import pass that follows.
    if (projectConfigurationList.empty()) {
        PropertiesMap discoverProps = properties;
        // Seed properties that are unknown at discovery time so they don't generate
        // spurious unknown-property debug messages.  These only affect the isolated
        // discovery copy — the real per-config pass uses the unmodified properties map.
        discoverProps.emplace("Platform", "x64");
        discoverProps.emplace("Configuration", "Debug");
        // Name-mismatched env vars (same-name ones are auto-resolved by isKnown).
        auto discoverSeedEnv = [&](const char *prop, const char *envVar) {
            const char *val = std::getenv(envVar);
            discoverProps.emplace(prop, val ? val : "");
        };
        discoverSeedEnv("VsInstallRoot",         "VSINSTALLDIR");
        discoverSeedEnv("VsInstallDir",          "VSINSTALLDIR");
        discoverSeedEnv("VCInstallDir",          "VCINSTALLDIR");
        discoverSeedEnv("MSBuildProgramFiles32", "ProgramFiles(x86)");
        discoverSeedEnv("WindowsSdkDir_10",      "WindowsSdkDir");
        // MSBuild-internal properties with no env var equivalent.
        discoverProps.emplace("VC_LibraryPath_x64",         "");
        discoverProps.emplace("WindowsSDK_WindowsMetadata", "");
        discoverProps.emplace("WindowsSDK_LibraryPath_x64", "");
        MetadataMap discoverMeta;
        std::list<ItemGroupClCompile> discovertCompile;
        std::unordered_set<std::string> discoverStack;
        for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement();
             node && projectConfigurationList.empty();
             node = node->NextSiblingElement()) {
            if (hasName(node, "PropertyGroup", discoverProps)) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement())
                    addProperty(e, discoverProps);
            } else if (hasName(node, "ImportGroup", discoverProps)) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e && projectConfigurationList.empty(); e = e->NextSiblingElement()) {
                    if (hasNameAndAttribute(e, "Import", "Project", discoverProps))
                        importProject(e, projectDir, discoverProps, discoverMeta, discovertCompile, projectConfigurationList, discoverStack, EvalPhase::Discover);
                }
            } else if (hasNameAndAttribute(node, "Import", "Project", discoverProps)) {
                importProject(node, projectDir, discoverProps, discoverMeta, discovertCompile, projectConfigurationList, discoverStack, EvalPhase::Discover);
            }
        }
    }

    PropertiesMap originalVariables = properties;

    bool first = true;

    for (const ProjectConfiguration &pc : projectConfigurationList) {
        if (!first) {
            compileList.clear();
            properties = originalVariables;
            metadata.clear();
            importStack.clear();
        } else
            first = false;

        properties["Configuration"] = pc.configuration;
        properties["Platform"] = pc.platformStr;

        // Three-phase MSBuild evaluation mirrors the real MSBuild static-evaluation model:
        //   Phase 1 (Properties) — traverse the full import graph collecting PropertyGroups.
        //   Phase 2 (ItemDefs)   — traverse the full import graph collecting IDG metadata.
        //   Phase 3 (Items)      — traverse the full import graph collecting ClCompile items.
        // Each phase sees the fully-resolved output of all preceding phases, so that
        // properties set in late-imported .targets files (e.g. CppWinRT.targets setting
        // GeneratedFilesDir) are available when item paths are resolved in Phase 3.
        // — Phase 1 (Properties) —
        for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
            if (hasName(node, "PropertyGroup", properties)) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement())
                    addProperty(e, properties);
            } else if (hasName(node, "ImportGroup", properties)) {
                const char *labelAttribute = node->Attribute("Label");
                if (labelAttribute && caseInsensitiveStringCompare(labelAttribute, "PropertySheets") == 0) {
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            if (importProject(e, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Properties) > ImportResult::NotResolvable)
                                return false;
                        }
                    }
                } else if (labelAttribute && caseInsensitiveStringCompare(labelAttribute, "Shared") == 0) {
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            std::string file = toAbsolute(projectAttribute, projectDir, properties);
                            std::string extension = Path::getFilenameExtensionInLowerCase(file);
                            if (extension == ".vcxitems") {
                                ImportResult result = importVcxitems(file, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Properties);
                                if (result > ImportResult::NotResolvable) {
                                    errors.emplace_back("Could not import items \"" + file + "\" - " + importResultStr(result));
                                    return false;
                                }
                                if (result == ImportResult::NotResolvable) {
                                    debugs.emplace_back("Could not import items \"" + file + "\" - " + importResultStr(result));
                                }
                            } else {
                                debugs.emplace_back("Could not import \"" + file + "\" unsupported extension " + extension);
                            }
                        }
                    }
                } else {
                    // Unlabeled or other-labeled ImportGroup (e.g. ExtensionSettings,
                    // ExtensionTargets) — process <Import> children like PropertySheets.
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            if (importProject(e, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Properties) > ImportResult::NotResolvable)
                                return false;
                        }
                    }
                }
            } else if (hasNameAndAttribute(node, "Import", "Project", properties)) {
                if (importProject(node, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Properties) > ImportResult::NotResolvable)
                    return false;
            }
            // ItemDefinitionGroups and ItemGroups are deferred to later phases.
        }

        // Phase 2 (ItemDefs): traverse the full import graph collecting ItemDefinitionGroup
        // metadata.  All properties are now fully resolved from Phase 1, so metadata values
        // that expand property references (e.g. $(Configuration)) are correct.
        importStack.clear();
        for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
            if (hasName(node, "ItemDefinitionGroup", properties)) {
                for (const tinyxml2::XMLElement *e1 = node->FirstChildElement(); e1; e1 = e1->NextSiblingElement()) {
                    if (hasName(e1, "ClCompile", properties)) {
                        for (const tinyxml2::XMLElement *e2 = e1->FirstChildElement(); e2; e2 = e2->NextSiblingElement())
                            addMetadata(e2, properties, metadata);
                    }
                }
            } else if (hasName(node, "ImportGroup", properties)) {
                const char *labelAttribute = node->Attribute("Label");
                if (labelAttribute && caseInsensitiveStringCompare(labelAttribute, "Shared") == 0) {
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            std::string file = toAbsolute(projectAttribute, projectDir, properties);
                            if (Path::getFilenameExtensionInLowerCase(file) == ".vcxitems") {
                                ImportResult result = importVcxitems(file, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::ItemDefs);
                                if (result > ImportResult::NotResolvable) {
                                    errors.emplace_back("Could not import items \"" + file + "\" - " + importResultStr(result));
                                    return false;
                                }
                            }
                        }
                    }
                } else {
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            if (importProject(e, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::ItemDefs) > ImportResult::NotResolvable)
                                return false;
                        }
                    }
                }
            } else if (hasNameAndAttribute(node, "Import", "Project", properties)) {
                if (importProject(node, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::ItemDefs) > ImportResult::NotResolvable)
                    return false;
            }
        }

        // Phase 3 (Items): traverse the full import graph collecting ClCompile items.
        // By now all properties and metadata (from Phases 1 and 2) are fully resolved,
        // so item paths, AdditionalIncludeDirectories etc. expand correctly.
        importStack.clear();
        for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
            if (hasNameAndNotLabel(node, "ItemGroup", "ProjectConfigurations", properties)) {
                for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                    if (hasNameAndAttribute(e, "ClCompile", "Include", properties))
                        importCompile(e, projectDir, properties, metadata, compileList);
                }
            } else if (hasName(node, "ImportGroup", properties)) {
                const char *labelAttribute = node->Attribute("Label");
                if (labelAttribute && caseInsensitiveStringCompare(labelAttribute, "Shared") == 0) {
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            std::string file = toAbsolute(projectAttribute, projectDir, properties);
                            if (Path::getFilenameExtensionInLowerCase(file) == ".vcxitems") {
                                ImportResult result = importVcxitems(file, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Items);
                                if (result > ImportResult::NotResolvable) {
                                    errors.emplace_back("Could not import items \"" + file + "\" - " + importResultStr(result));
                                    return false;
                                }
                                if (result == ImportResult::NotResolvable) {
                                    debugs.emplace_back("Could not import items \"" + file + "\" - " + importResultStr(result));
                                }
                            }
                        }
                    }
                } else {
                    for (const tinyxml2::XMLElement *e = node->FirstChildElement(); e; e = e->NextSiblingElement()) {
                        if (hasName(e, "Import", properties)) {
                            const char *projectAttribute = e->Attribute("Project");
                            if (!projectAttribute)
                                continue;
                            if (importProject(e, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Items) > ImportResult::NotResolvable)
                                return false;
                        }
                    }
                }
            } else if (hasNameAndAttribute(node, "Import", "Project", properties)) {
                if (importProject(node, projectDir, properties, metadata, compileList, projectConfigurationList, importStack, EvalPhase::Items) > ImportResult::NotResolvable)
                    return false;
            }
        }

        // # TODO: support signedness of char via /J (and potential XML option for it)?
        // we can only set it globally but in this context it needs to be treated per file

        // Project files
        PathMatch filtermatcher(fileFilters, Path::getCurrentPath());
        for (const ItemGroupClCompile &compile : compileList) {
            if (!fileFilters.empty() && !filtermatcher.match(compile.filename))
                continue;

            if (!guiProject.checkVsConfigs.empty()) {
                const bool doChecking = std::any_of(guiProject.checkVsConfigs.cbegin(), guiProject.checkVsConfigs.cend(), [&](const std::string &c) {
                    return c == pc.configuration;
                });
                if (!doChecking)
                    continue;
            }

            FileSettings fs{ compile.filename, Standards::Language::None, 0 }; // file will be identified later on
            fs.cfg = pc.name;
            // TODO: detect actual MSC version
            fs.msc = true;
            fs.defines = "_WIN32=1";
            if (pc.platform == ProjectConfiguration::Win32)
                fs.platformType = Platform::Type::Win32W;
            else if (pc.platform == ProjectConfiguration::x64) {
                fs.platformType = Platform::Type::Win64;
                fs.defines += ";_WIN64=1";
            } else if (pc.platform == ProjectConfiguration::ARM64) {
                fs.platformType = Platform::Type::WinARM64;
                fs.defines += ";_M_ARM64=1";
            } else if (pc.platform == ProjectConfiguration::ARM) {
                fs.platformType = Platform::Type::WinARM;
                fs.defines += ";_M_ARM=1";
            }

            Standards::cppstd_t cppstd = Standards::CPPLatest;
            const std::string &languageStandard = compile.get("LanguageStandard");
            if (languageStandard == "stdcpp11")
                cppstd = Standards::CPP11;
            else if (languageStandard == "stdcpp14")
                cppstd = Standards::CPP14;
            else if (languageStandard == "stdcpp17")
                cppstd = Standards::CPP17;
            else if (languageStandard == "stdcpp20")
                cppstd = Standards::CPP20;
            else if (languageStandard == "stdcpp23")
                cppstd = Standards::CPP23;
            else if (languageStandard == "stdcpplatest")
                cppstd = Standards::CPPLatest;
            fs.standard = Standards::getCPP(cppstd);

            std::string enableEnhancedInstructionSet = compile.get("EnableEnhancedInstructionSet");
            if (enableEnhancedInstructionSet == "StreamingSIMDExtensions")
                fs.defines += ";__SSE__";
            else if (enableEnhancedInstructionSet == "StreamingSIMDExtensions2")
                fs.defines += ";__SSE2__";
            else if (enableEnhancedInstructionSet == "AdvancedVectorExtensions")
                fs.defines += ";__AVX__";
            else if (enableEnhancedInstructionSet == "AdvancedVectorExtensions2")
                fs.defines += ";__AVX2__";
            else if (enableEnhancedInstructionSet == "AdvancedVectorExtensions512")
                fs.defines += ";__AVX512F__";

            const auto charSetIt = properties.find("CharacterSet");
            const std::string charSet = (charSetIt != properties.end()) ? charSetIt->second : std::string();

            const auto useOfMfcIt = properties.find("UseOfMfc");
            fs.useMfc = useOfMfcIt != properties.end() && !useOfMfcIt->second.empty() &&
                        caseInsensitiveStringCompare(useOfMfcIt->second, "false") != 0;

            if (charSet == "Unicode") {
                fs.defines += ";UNICODE=1;_UNICODE=1";
            } else if (charSet == "MultiByte") {
                fs.defines += ";_MBCS=1";
            }

            std::string defines = fs.defines;
            if (!compile.get("PreprocessorDefinitions").empty())
                defines += (";" + compile.get("PreprocessorDefinitions"));
            fsSetDefines(fs, defines);
            {
                const auto includePathIt = properties.find("IncludePath");
                fsSetIncludePaths(fs, projectDir, toStringList(includePathIt != properties.end() ? includePathIt->second : std::string()), properties);
            }
            fs.systemIncludePaths = std::move(fs.includePaths);
            fsSetIncludePaths(fs, projectDir, toStringList(compile.get("AdditionalIncludeDirectories")), properties);
            fs.forcedIncludes = toStringList(compile.get("ForcedIncludeFiles"));
            for (auto &forcedInclude : fs.forcedIncludes)
                forcedInclude = toAbsolute(forcedInclude, projectDir, properties);

            fileSettings.push_back(std::move(fs));
        }
    }

    return true;
}

bool ImportProject::importBcb6Prj(const std::string &projectFilename)
{
    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError error = doc.LoadFile(projectFilename.c_str());
    if (error != tinyxml2::XML_SUCCESS) {
        errors.emplace_back(std::string("Borland project file is not a valid XML - ") + tinyxml2::XMLDocument::ErrorIDToName(error));
        return false;
    }
    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr) {
        errors.emplace_back("Borland project file has no XML root node");
        return false;
    }

    const std::string& projectDir = Path::simplifyPath(Path::getPathFromFilename(projectFilename));

    std::list<std::string> compileList;
    std::string includePath;
    std::string userdefines;
    std::string sysdefines;
    std::string cflag1;

    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        const char* name = node->Name();
        if (std::strcmp(name, "FILELIST") == 0) {
            for (const tinyxml2::XMLElement *f = node->FirstChildElement(); f; f = f->NextSiblingElement()) {
                if (std::strcmp(f->Name(), "FILE") == 0) {
                    const char *filename = f->Attribute("FILENAME");
                    if (filename && Path::acceptFile(filename))
                        compileList.emplace_back(filename);
                }
            }
        } else if (std::strcmp(name, "MACROS") == 0) {
            for (const tinyxml2::XMLElement *m = node->FirstChildElement(); m; m = m->NextSiblingElement()) {
                const char* mname = m->Name();
                if (std::strcmp(mname, "INCLUDEPATH") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        includePath = v;
                } else if (std::strcmp(mname, "USERDEFINES") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        userdefines = v;
                } else if (std::strcmp(mname, "SYSDEFINES") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        sysdefines = v;
                }
            }
        } else if (std::strcmp(name, "OPTIONS") == 0) {
            for (const tinyxml2::XMLElement *m = node->FirstChildElement(); m; m = m->NextSiblingElement()) {
                if (std::strcmp(m->Name(), "CFLAG1") == 0) {
                    const char *v = m->Attribute("value");
                    if (v)
                        cflag1 = v;
                }
            }
        }
    }

    std::set<std::string> cflags;

    // parse cflag1 and fill the cflags set
    {
        std::string arg;

        for (const char i : cflag1) {
            if (i == ' ' && !arg.empty()) {
                cflags.insert(arg);
                arg.clear();
                continue;
            }
            arg += i;
        }

        if (!arg.empty()) {
            cflags.insert(std::move(arg));
        }

        // cleanup: -t is "An alternate name for the -Wxxx switches; there is no difference"
        // -> Remove every known -txxx argument and replace it with its -Wxxx counterpart.
        //    This way, we know what we have to check for later on.
        static const std::map<std::string, std::string> synonyms = {
            { "-tC","-WC" },
            { "-tCDR","-WCDR" },
            { "-tCDV","-WCDV" },
            { "-tW","-W" },
            { "-tWC","-WC" },
            { "-tWCDR","-WCDR" },
            { "-tWCDV","-WCDV" },
            { "-tWD","-WD" },
            { "-tWDR","-WDR" },
            { "-tWDV","-WDV" },
            { "-tWM","-WM" },
            { "-tWP","-WP" },
            { "-tWR","-WR" },
            { "-tWU","-WU" },
            { "-tWV","-WV" }
        };

        for (auto i = synonyms.cbegin(); i != synonyms.cend(); ++i) {
            if (cflags.erase(i->first) > 0) {
                cflags.insert(i->second);
            }
        }
    }

    std::string predefines;
    std::string cppPredefines;

    // Collecting predefines. See BCB6 help topic "Predefined macros"
    {
        cppPredefines +=
            // Defined if you've selected C++ compilation; will increase in later releases.
            // value 0x0560 (but 0x0564 for our BCB6 SP4)
            // @see http://docwiki.embarcadero.com/RADStudio/Tokyo/en/Predefined_Macros#C.2B.2B_Compiler_Versions_in_Predefined_Macros
            ";__BCPLUSPLUS__=0x0560"

            // Defined if in C++ mode; otherwise, undefined.
            ";__cplusplus=1"

            // Defined as 1 for C++ files(meaning that templates are supported); otherwise, it is undefined.
            ";__TEMPLATES__=1"

            // Defined only for C++ programs to indicate that wchar_t is an intrinsically defined data type.
            ";_WCHAR_T"

            // Defined only for C++ programs to indicate that wchar_t is an intrinsically defined data type.
            ";_WCHAR_T_DEFINED"

            // Defined in any compiler that has an optimizer.
            ";__BCOPT__=1"

            // Version number.
            // BCB6 is 0x056X (SP4 is 0x0564)
            // @see http://docwiki.embarcadero.com/RADStudio/Tokyo/en/Predefined_Macros#C.2B.2B_Compiler_Versions_in_Predefined_Macros
            ";__BORLANDC__=0x0560"
            ";__TCPLUSPLUS__=0x0560"
            ";__TURBOC__=0x0560";

        // Defined if Calling Convention is set to cdecl; otherwise undefined.
        const bool useCdecl = (cflags.find("-p") == cflags.end()
                               && cflags.find("-pm") == cflags.end()
                               && cflags.find("-pr") == cflags.end()
                               && cflags.find("-ps") == cflags.end());
        if (useCdecl)
            predefines += ";__CDECL=1";

        // Defined by default indicating that the default char is unsigned char. Use the -K compiler option to undefine this macro.
        const bool treatCharAsUnsignedChar = (cflags.find("-K") != cflags.end());
        if (treatCharAsUnsignedChar)
            predefines += ";_CHAR_UNSIGNED=1";

        // Defined whenever one of the CodeGuard compiler options is used; otherwise it is undefined.
        const bool codeguardUsed = (cflags.find("-vGd") != cflags.end()
                                    || cflags.find("-vGt") != cflags.end()
                                    || cflags.find("-vGc") != cflags.end());
        if (codeguardUsed)
            predefines += ";__CODEGUARD__";

        // When defined, the macro indicates that the program is a console application.
        const bool isConsoleApp = (cflags.find("-WC") != cflags.end());
        if (isConsoleApp)
            predefines += ";__CONSOLE__=1";

        // Enable stack unwinding. This is true by default; use -xd- to disable.
        const bool enableStackUnwinding = (cflags.find("-xd-") == cflags.end());
        if (enableStackUnwinding)
            predefines += ";_CPPUNWIND=1";

        // Defined whenever the -WD compiler option is used; otherwise it is undefined.
        const bool isDLL = (cflags.find("-WD") != cflags.end());
        if (isDLL)
            predefines += ";__DLL__=1";

        // Defined when compiling in 32-bit flat memory model.
        // TODO: not sure how to switch to another memory model or how to read configuration from project file
        predefines += ";__FLAT__=1";

        // Always defined. The default value is 300. You can change the value to 400 or 500 by using the /4 or /5 compiler options.
        if (cflags.find("-6") != cflags.end())
            predefines += ";_M_IX86=600";
        else if (cflags.find("-5") != cflags.end())
            predefines += ";_M_IX86=500";
        else if (cflags.find("-4") != cflags.end())
            predefines += ";_M_IX86=400";
        else
            predefines += ";_M_IX86=300";

        // Defined only if the -WM option is used. It specifies that the multithread library is to be linked.
        const bool linkMtLib = (cflags.find("-WM") != cflags.end());
        if (linkMtLib)
            predefines += ";__MT__=1";

        // Defined if Calling Convention is set to Pascal; otherwise undefined.
        const bool usePascalCallingConvention = (cflags.find("-p") != cflags.end());
        if (usePascalCallingConvention)
            predefines += ";__PASCAL__=1";

        // Defined if you compile with the -A compiler option; otherwise, it is undefined.
        const bool useAnsiKeywordExtensions = (cflags.find("-A") != cflags.end());
        if (useAnsiKeywordExtensions)
            predefines += ";__STDC__=1";

        // Thread Local Storage. Always true in C++Builder.
        predefines += ";__TLC__=1";

        // Defined for Windows-only code.
        const bool isWindowsTarget = (cflags.find("-WC") != cflags.end()
                                      || cflags.find("-WCDR") != cflags.end()
                                      || cflags.find("-WCDV") != cflags.end()
                                      || cflags.find("-WD") != cflags.end()
                                      || cflags.find("-WDR") != cflags.end()
                                      || cflags.find("-WDV") != cflags.end()
                                      || cflags.find("-WM") != cflags.end()
                                      || cflags.find("-WP") != cflags.end()
                                      || cflags.find("-WR") != cflags.end()
                                      || cflags.find("-WU") != cflags.end()
                                      || cflags.find("-WV") != cflags.end());
        if (isWindowsTarget)
            predefines += ";_Windows";

        // Defined for console and GUI applications.
        // TODO: I'm not sure about the difference to define "_Windows".
        //       From description, I would assume __WIN32__ is only defined for
        //       executables, while _Windows would also be defined for DLLs, etc.
        //       However, in a newly created DLL project, both __WIN32__ and
        //       _Windows are defined. -> treating them the same for now.
        //       Also boost uses __WIN32__ for OS identification.
        const bool isConsoleOrGuiApp = isWindowsTarget;
        if (isConsoleOrGuiApp)
            predefines += ";__WIN32__=1";
    }

    // Include paths may contain properties like "$(BCB)\include" or "$(BCB)\include\vcl".
    // Those get resolved by ImportProject::FileSettings::setIncludePaths by
    // 1. checking the provided properties map ("BCB" => "C:\\Program Files (x86)\\Borland\\CBuilder6")
    // 2. checking env properties as a fallback
    // Setting env is always possible. Configuring the properties via cli might be an addition.
    // Reading the BCB6 install location from registry in windows environments would also be possible,
    // but I didn't see any such functionality around the source. Not in favor of adding it only
    // for the BCB6 project loading.
    PropertiesMap properties;
    const std::string defines = predefines + ";" + sysdefines + ";" + userdefines;
    const std::string cppDefines  = cppPredefines + ";" + defines;
    const bool forceCppMode = (cflags.find("-P") != cflags.end());

    for (const std::string &c : compileList) {
        // C++ compilation is selected by file extension by default, so these
        // defines have to be configured on a per-file base.
        //
        // > Files with the .CPP extension compile as C++ files. Files with a .C
        // > extension, with no extension, or with extensions other than .CPP,
        // > .OBJ, .LIB, or .ASM compile as C files.
        // (http://docwiki.embarcadero.com/RADStudio/Tokyo/en/BCC32.EXE,_the_C%2B%2B_32-bit_Command-Line_Compiler)
        //
        // We can also force C++ compilation for all files using the -P command line switch.
        const bool cppMode = forceCppMode || Path::getFilenameExtensionInLowerCase(c) == ".cpp";
        // TODO: needs to set language and ignore later identification and language enforcement
        FileSettings fs{Path::simplifyPath(Path::isAbsolute(c) ? c : projectDir + c), Standards::Language::None, 0}; // file will be identified later on
        fsSetIncludePaths(fs, projectDir, toStringList(includePath), properties);
        fsSetDefines(fs, cppMode ? cppDefines : defines);
        fileSettings.push_back(std::move(fs));
    }

    return true;
}

static std::string joinRelativePath(const std::string &path1, const std::string &path2)
{
    if (!path1.empty() && !Path::isAbsolute(path2))
        return path1 + path2;
    return path2;
}

static std::list<std::string> readXmlStringList(const tinyxml2::XMLElement *node, const std::string &path, const char name[], const char attribute[])
{
    std::list<std::string> ret;
    for (const tinyxml2::XMLElement *child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (strcmp(child->Name(), name) != 0)
            continue;
        const char *attr = attribute ? child->Attribute(attribute) : child->GetText();
        if (attr)
            ret.emplace_back(joinRelativePath(path, attr));
    }
    return ret;
}

static std::list<std::string> readXmlPathMatchList(const tinyxml2::XMLElement *node, const std::string &path, const char name[], const char attribute[])
{
    std::list<std::string> ret;
    for (const tinyxml2::XMLElement *child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
        if (strcmp(child->Name(), name) != 0)
            continue;
        const char *attr = attribute ? child->Attribute(attribute) : child->GetText();
        if (attr)
            ret.emplace_back(PathMatch::joinRelativePattern(path, attr));
    }
    return ret;
}

static std::string join(const std::list<std::string> &strlist, const char *sep)
{
    std::string ret;
    for (const std::string &s : strlist) {
        ret += (ret.empty() ? "" : sep) + s;
    }
    return ret;
}

static std::string istream_to_string(std::istream &istr)
{
    std::istreambuf_iterator<char> eos;
    return std::string(std::istreambuf_iterator<char>(istr), eos);
}

bool ImportProject::importCppcheckGuiProject(std::istream &istr, Settings &settings, Suppressions &supprs)
{
    tinyxml2::XMLDocument doc;
    const std::string xmldata = istream_to_string(istr);
    const tinyxml2::XMLError error = doc.Parse(xmldata.data(), xmldata.size());
    if (error != tinyxml2::XML_SUCCESS) {
        errors.emplace_back(std::string("Cppcheck GUI project file is not a valid XML - ") + tinyxml2::XMLDocument::ErrorIDToName(error));
        return false;
    }
    const tinyxml2::XMLElement * const rootnode = doc.FirstChildElement();
    if (rootnode == nullptr || strcmp(rootnode->Name(), CppcheckXml::ProjectElementName) != 0) {
        errors.emplace_back("Cppcheck GUI project file has no XML root node");
        return false;
    }

    const std::string &path = mPath;

    std::list<std::string> paths;
    std::list<SuppressionList::Suppression> suppressions;
    Settings temp;

    // default to --check-level=normal for import for now
    temp.setCheckLevel(Settings::CheckLevel::normal);

    // TODO: this should support all available command-line options
    for (const tinyxml2::XMLElement *node = rootnode->FirstChildElement(); node; node = node->NextSiblingElement()) {
        const char* name = node->Name();
        if (strcmp(name, CppcheckXml::RootPathName) == 0) {
            const char* attr = node->Attribute(CppcheckXml::RootPathNameAttrib);
            if (attr) {
                temp.basePaths.push_back(Path::fromNativeSeparators(joinRelativePath(path, attr)));
                temp.relativePaths = true;
            }
        } else if (strcmp(name, CppcheckXml::BuildDirElementName) == 0)
            temp.buildDir = joinRelativePath(path, empty_if_null(node->GetText()));
        else if (strcmp(name, CppcheckXml::IncludeDirElementName) == 0)
            temp.includePaths = readXmlStringList(node, path, CppcheckXml::DirElementName, CppcheckXml::DirNameAttrib); // TODO: append instead of overwrite
        else if (strcmp(name, CppcheckXml::DefinesElementName) == 0)
            temp.userDefines = join(readXmlStringList(node, "", CppcheckXml::DefineName, CppcheckXml::DefineNameAttrib), ";"); // TODO: append instead of overwrite
        else if (strcmp(name, CppcheckXml::UndefinesElementName) == 0) {
            for (const std::string &u : readXmlStringList(node, "", CppcheckXml::UndefineName, nullptr))
                temp.userUndefs.insert(u);
        } else if (strcmp(name, CppcheckXml::UserIncludeElementName) == 0) {
            const char* i = node->GetText();
            if (i)
                temp.userIncludes.emplace_back(i);
        } else if (strcmp(name, CppcheckXml::ImportProjectElementName) == 0) {
            const std::string t_str = empty_if_null(node->GetText());
            if (!t_str.empty())
                guiProject.projectFile = path + t_str;
        }
        else if (strcmp(name, CppcheckXml::PathsElementName) == 0)
            paths = readXmlStringList(node, path, CppcheckXml::PathName, CppcheckXml::PathNameAttrib);
        else if (strcmp(name, CppcheckXml::ExcludeElementName) == 0)
            guiProject.excludedPaths = readXmlPathMatchList(node, path, CppcheckXml::ExcludePathName, CppcheckXml::ExcludePathNameAttrib); // TODO: append instead of overwrite
        else if (strcmp(name, CppcheckXml::FunctionContracts) == 0)
            ;
        else if (strcmp(name, CppcheckXml::VariableContractsElementName) == 0)
            ;
        else if (strcmp(name, CppcheckXml::IgnoreElementName) == 0)
            guiProject.excludedPaths = readXmlPathMatchList(node, path, CppcheckXml::IgnorePathName, CppcheckXml::IgnorePathNameAttrib); // TODO: append instead of overwrite
        else if (strcmp(name, CppcheckXml::LibrariesElementName) == 0)
            guiProject.libraries = readXmlStringList(node, "", CppcheckXml::LibraryElementName, nullptr); // TODO: append instead of overwrite
        else if (strcmp(name, CppcheckXml::SuppressionsElementName) == 0) {
            for (const tinyxml2::XMLElement *child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
                if (strcmp(child->Name(), CppcheckXml::SuppressionElementName) != 0)
                    continue;
                SuppressionList::Suppression s;
                s.errorId = empty_if_null(child->GetText());
                s.fileName = empty_if_null(child->Attribute("fileName"));
                if (!s.fileName.empty())
                    s.fileName = joinRelativePath(path, s.fileName);
                s.lineNumber = child->IntAttribute("lineNumber", SuppressionList::Suppression::NO_LINE); // TODO: should not depend on Suppression
                s.symbolName = empty_if_null(child->Attribute("symbolName"));
                s.hash = strToInt<std::size_t>(default_if_null(child->Attribute("hash"), "0"));
                suppressions.push_back(std::move(s));
            }
        } else if (strcmp(name, CppcheckXml::VSConfigurationElementName) == 0)
            guiProject.checkVsConfigs = readXmlStringList(node, "", CppcheckXml::VSConfigurationName, nullptr);
        else if (strcmp(name, CppcheckXml::PlatformElementName) == 0)
            guiProject.platform = empty_if_null(node->GetText());
        else if (strcmp(name, CppcheckXml::AnalyzeAllVsConfigsElementName) == 0)
            temp.analyzeAllVsConfigs = std::string(empty_if_null(node->GetText())) != "false";
        else if (strcmp(name, CppcheckXml::Parser) == 0)
            temp.clang = true;
        else if (strcmp(name, CppcheckXml::AddonsElementName) == 0) {
            const auto& addons = readXmlStringList(node, "", CppcheckXml::AddonElementName, nullptr);
            temp.addons.insert(addons.cbegin(), addons.cend());
            if (settings.premium) {
                auto it = temp.addons.find("misra");
                if (it != temp.addons.end()) {
                    temp.addons.erase(it);
                    temp.premiumArgs += " --misra-c-2012";
                }
            }
        }
        else if (strcmp(name, CppcheckXml::TagsElementName) == 0)
            node->Attribute(CppcheckXml::TagElementName); // FIXME: Write some warning
        else if (strcmp(name, CppcheckXml::ToolsElementName) == 0) {
            const std::list<std::string> toolList = readXmlStringList(node, "", CppcheckXml::ToolElementName, nullptr);
            for (const std::string &toolName : toolList) {
                if (toolName == CppcheckXml::ClangTidy)
                    temp.clangTidy = true;
            }
        } else if (strcmp(name, CppcheckXml::CheckHeadersElementName) == 0)
            temp.checkHeaders = (strcmp(default_if_null(node->GetText(), ""), "true") == 0);
        else if (strcmp(name, CppcheckXml::CheckLevelReducedElementName) == 0)
            temp.setCheckLevel(Settings::CheckLevel::reduced);
        else if (strcmp(name, CppcheckXml::CheckLevelNormalElementName) == 0)
            temp.setCheckLevel(Settings::CheckLevel::normal);
        else if (strcmp(name, CppcheckXml::CheckLevelExhaustiveElementName) == 0)
            temp.setCheckLevel(Settings::CheckLevel::exhaustive);
        else if (strcmp(name, CppcheckXml::CheckUnusedTemplatesElementName) == 0)
            temp.checkUnusedTemplates = (strcmp(default_if_null(node->GetText(), ""), "true") == 0);
        else if (strcmp(name, CppcheckXml::InlineSuppression) == 0)
            temp.inlineSuppressions = (strcmp(default_if_null(node->GetText(), ""), "true") == 0);
        else if (strcmp(name, CppcheckXml::MaxCtuDepthElementName) == 0)
            temp.maxCtuDepth = strToInt<int>(default_if_null(node->GetText(), "2")); // TODO: bail out when missing?
        else if (strcmp(name, CppcheckXml::MaxTemplateRecursionElementName) == 0)
            temp.maxTemplateRecursion = strToInt<int>(default_if_null(node->GetText(), "100")); // TODO: bail out when missing?
        else if (strcmp(name, CppcheckXml::CheckUnknownFunctionReturn) == 0)
            ; // TODO
        else if (strcmp(name, Settings::SafeChecks::XmlRootName) == 0) {
            for (const tinyxml2::XMLElement *child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
                const char* childname = child->Name();
                if (strcmp(childname, Settings::SafeChecks::XmlClasses) == 0)
                    temp.safeChecks.classes = true;
                else if (strcmp(childname, Settings::SafeChecks::XmlExternalFunctions) == 0)
                    temp.safeChecks.externalFunctions = true;
                else if (strcmp(childname, Settings::SafeChecks::XmlInternalFunctions) == 0)
                    temp.safeChecks.internalFunctions = true;
                else if (strcmp(childname, Settings::SafeChecks::XmlExternalVariables) == 0)
                    temp.safeChecks.externalVariables = true;
                else {
                    errors.emplace_back("Unknown '" + std::string(Settings::SafeChecks::XmlRootName) + "' element '" + childname + "' in Cppcheck GUI project file");
                    return false;
                }
            }
        } else if (strcmp(name, CppcheckXml::TagWarningsElementName) == 0)
            ; // TODO
        // Cppcheck Premium features
        else if (strcmp(name, CppcheckXml::BughuntingElementName) == 0)
            temp.premiumArgs += " --bughunting";
        else if (strcmp(name, CppcheckXml::CertIntPrecisionElementName) == 0)
            temp.premiumArgs += std::string(" --cert-c-int-precision=") + default_if_null(node->GetText(), "0");
        else if (strcmp(name, CppcheckXml::CodingStandardsElementName) == 0) {
            for (const tinyxml2::XMLElement *child = node->FirstChildElement(); child; child = child->NextSiblingElement()) {
                if (strcmp(child->Name(), CppcheckXml::CodingStandardElementName) == 0) {
                    const char* text = child->GetText();
                    if (text)
                        temp.premiumArgs += std::string(" --") + text;
                }
            }
        }
        else if (strcmp(name, CppcheckXml::ProjectNameElementName) == 0)
            ; // no-op
        else {
            errors.emplace_back("Unknown element '" + std::string(name) + "' in Cppcheck GUI project file");
            return false;
        }
    }
    settings.basePaths = temp.basePaths; // TODO: append instead of overwrite
    settings.relativePaths |= temp.relativePaths;
    settings.buildDir = temp.buildDir;
    settings.includePaths = temp.includePaths; // TODO: append instead of overwrite
    settings.userDefines = temp.userDefines; // TODO: append instead of overwrite
    settings.userUndefs = temp.userUndefs; // TODO: append instead of overwrite
    settings.userIncludes = temp.userIncludes; // TODO: append instead of overwrite
    for (const std::string &addon : temp.addons)
        settings.addons.emplace(addon);
    settings.clang = temp.clang;
    settings.clangTidy = temp.clangTidy;
    settings.analyzeAllVsConfigs = temp.analyzeAllVsConfigs;

    if (!settings.premiumArgs.empty())
        settings.premiumArgs += temp.premiumArgs;
    else if (!temp.premiumArgs.empty())
        settings.premiumArgs = temp.premiumArgs.substr(1);

    for (const std::string &p : paths)
        guiProject.pathNames.push_back(Path::fromNativeSeparators(p));

    bool ok = true;
    for (const auto &suppression : suppressions) {
        const std::string addError = supprs.nomsg.addSuppression(suppression);
        if (!addError.empty()) {
            errors.emplace_back(addError);
            ok = false;
        }
    }
    if (!ok)
        return false;

    settings.checkHeaders = temp.checkHeaders;
    settings.checkUnusedTemplates = temp.checkUnusedTemplates;
    settings.maxCtuDepth = temp.maxCtuDepth;
    settings.maxTemplateRecursion = temp.maxTemplateRecursion;
    settings.inlineSuppressions |= temp.inlineSuppressions;
    settings.safeChecks = temp.safeChecks;
    settings.setCheckLevel(temp.checkLevel);

    return true;
}

void ImportProject::selectOneVsConfig(Platform::Type platform)
{
    std::set<std::string> filenames;
    for (auto it = fileSettings.cbegin(); it != fileSettings.cend();) {
        if (it->cfg.empty()) {
            ++it;
            continue;
        }
        const FileSettings &fs = *it;
        bool remove = false;
        const std::string cfgName = fs.cfg.substr(0, fs.cfg.find('|'));
        if (cfgName.size() < 5 || caseInsensitiveStringCompare(cfgName.substr(0, 5), "Debug") != 0)
            remove = true;

        if (platform == Platform::Type::Win64 && fs.platformType != Platform::Type::Win64)
            remove = true;
        else if (platform == Platform::Type::WinARM64 && fs.platformType != Platform::Type::WinARM64)
            remove = true;
        else if (platform == Platform::Type::WinARM && fs.platformType != Platform::Type::WinARM)
            remove = true;
        else if ((platform == Platform::Type::Win32A || platform == Platform::Type::Win32W) &&
                 (fs.platformType == Platform::Type::Win64 ||
                  fs.platformType == Platform::Type::WinARM64 ||
                  fs.platformType == Platform::Type::WinARM))
            remove = true;
        else if (filenames.find(fs.filename()) != filenames.end())
            remove = true;
        if (remove) {
            it = fileSettings.erase(it);
        } else {
            filenames.insert(fs.filename());
            ++it;
        }
    }
}

void ImportProject::selectVsConfigurations(Platform::Type platform, const std::vector<std::string> &configurations)
{
    for (auto it = fileSettings.cbegin(); it != fileSettings.cend();) {
        if (it->cfg.empty()) {
            ++it;
            continue;
        }
        const FileSettings &fs = *it;
        const auto config = fs.cfg.substr(0, fs.cfg.find('|'));
        bool remove = false;
        if (std::find(configurations.begin(), configurations.end(), config) == configurations.end())
            remove = true;
        if (platform == Platform::Type::Win64 && fs.platformType != Platform::Type::Win64)
            remove = true;
        else if (platform == Platform::Type::WinARM64 && fs.platformType != Platform::Type::WinARM64)
            remove = true;
        else if (platform == Platform::Type::WinARM && fs.platformType != Platform::Type::WinARM)
            remove = true;
        else if ((platform == Platform::Type::Win32A || platform == Platform::Type::Win32W) &&
                 (fs.platformType == Platform::Type::Win64 ||
                  fs.platformType == Platform::Type::WinARM64 ||
                  fs.platformType == Platform::Type::WinARM))
            remove = true;
        if (remove) {
            it = fileSettings.erase(it);
        } else {
            ++it;
        }
    }
}

std::list<std::string> ImportProject::getVSConfigs()
{
    return std::list<std::string>(mAllVSConfigs.cbegin(), mAllVSConfigs.cend());
}

void ImportProject::setRelativePaths(const std::string &filename)
{
    if (Path::isAbsolute(filename))
        return;
    const std::vector<std::string> basePaths{Path::fromNativeSeparators(Path::getCurrentPath())};
    for (auto &fs: fileSettings) {
        fs.file.setPath(Path::getRelativePath(fs.filename(), basePaths));
        for (auto &includePath: fs.includePaths) {
            const std::string rel = Path::getRelativePath(includePath, basePaths);
            includePath = rel.empty() ? "." : rel;
        }
        for (auto &includePath: fs.systemIncludePaths) {
            const std::string rel = Path::getRelativePath(includePath, basePaths);
            includePath = rel.empty() ? "." : rel;
        }
        for (auto &forcedInclude: fs.forcedIncludes)
            forcedInclude = Path::getRelativePath(forcedInclude, basePaths);
    }
}

// only used by tests (testimportproject.cpp::testVcxprojConditions):
// cppcheck-suppress unusedFunction
bool cppcheck::testing::evaluateVcxprojCondition(const std::string& condition,
                                                 const std::string& configuration,
                                                 const std::string& platform)
{
    ImportProject project;
    PropertiesMap properties;
    properties["Platform"] = platform;
    properties["Configuration"] = configuration;
    // Use ConditionParser directly so exceptions propagate to the caller;
    // evalCondition swallows them (by design for production use).
    ImportProject::ConditionParser parser(project, condition, properties);
    return parser.parse();
}

// cppcheck-suppress unusedFunction
std::string cppcheck::testing::expandMSBuildExpression(const std::string& expr)
{
    ImportProject project;
    PropertiesMap properties;
    std::string s = expr;
    project.expandMSBuildVariables(s, properties);
    return s;
}

// cppcheck-suppress unusedFunction
std::string cppcheck::testing::expandMSBuildProperties(const std::string& expr,
                                                       const std::string& configuration,
                                                       const std::string& platform)
{
    ImportProject project;
    PropertiesMap properties;
    properties["Configuration"] = configuration;
    properties["Platform"] = platform;
    std::string s = expr;
    project.expandMSBuildVariables(s, properties);
    return s;
}
