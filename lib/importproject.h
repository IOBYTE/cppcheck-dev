/* -*- C++ -*-
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

//---------------------------------------------------------------------------
#ifndef importprojectH
#define importprojectH
//---------------------------------------------------------------------------

#include "config.h"
#include "filesettings.h"
#include "platform.h"
#include "utils.h"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <list>
#include <map>
#include <set>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

class Settings;
struct Suppressions;

namespace tinyxml2 {
    class XMLElement;
}

/// @addtogroup Core
/// @{

namespace cppcheck {
    struct stricmp {
        bool operator()(const std::string &lhs, const std::string &rhs) const {
            return caseInsensitiveStringCompare(lhs,rhs) < 0;
        }
    };

    namespace testing
    {
        CPPCHECKLIB bool evaluateVcxprojCondition(const std::string& condition, const std::string& configuration, const std::string& platform);
        CPPCHECKLIB std::string expandMSBuildExpression(const std::string& expr);
        CPPCHECKLIB std::string expandMSBuildProperties(const std::string& expr, const std::string& configuration, const std::string& platform);
        CPPCHECKLIB std::string resolveVcxitemsFilename(const std::string& items, const std::string& projectDir);
    }
}

using PropertiesMap = std::map<std::string, std::string, cppcheck::stricmp>;
using MetadataMap = std::map<std::string, std::string, cppcheck::stricmp>;

/**
 * @brief Importing project settings.
 */
class CPPCHECKLIB WARN_UNUSED ImportProject {
public:
    friend CPPCHECKLIB bool cppcheck::testing::evaluateVcxprojCondition(const std::string &condition, const std::string &configuration, const std::string &platform);
    friend CPPCHECKLIB std::string cppcheck::testing::expandMSBuildExpression(const std::string &expr);
    friend CPPCHECKLIB std::string cppcheck::testing::expandMSBuildProperties(const std::string &expr, const std::string &configuration, const std::string &platform);
    friend CPPCHECKLIB std::string cppcheck::testing::resolveVcxitemsFilename(const std::string &items, const std::string &projectDir);

    enum class Type : std::uint8_t {
        NONE,
        UNKNOWN,
        MISSING,
        FAILURE,
        COMPILE_DB,
        VS_SLN,
        VS_SLNX,
        VS_VCXPROJ,
        BORLAND,
        CPPCHECK_GUI
    };
    enum class ImportResult : std::uint8_t {
        Ok,
        Cycle,         // MSBuild silently ignores circular imports; treat as Ok-level
        NotResolvable,
        NotFound,
        NotValid,
    };

    /// Controls which element types are processed during a given evaluation pass.
    /// MSBuild evaluates in three ordered phases across the full import graph:
    ///   Phase 1 (Properties) -- all PropertyGroups
    ///   Phase 2 (ItemDefs)   -- all ItemDefinitionGroups
    ///   Phase 3 (Items)      -- all ItemGroups (ClCompile)
    /// Separating the phases ensures each phase sees the fully-resolved output
    /// of all preceding phases, matching real MSBuild evaluation semantics.
    enum class EvalPhase : std::uint8_t {
        Properties, ///< Pass 1: collect/expand PropertyGroup elements only
        ItemDefs,   ///< Pass 2: collect ItemDefinitionGroup metadata only
        Items,      ///< Pass 3: collect ClCompile items only
        Discover,   ///< Like Properties but silences debug/error noise from unresolvable imports
    };

protected:
    static void fsSetDefines(FileSettings& fs, std::string defs);
    void fsSetIncludePaths(FileSettings& fs, const std::string &basepath, const std::list<std::string> &in, const PropertiesMap &properties);
    /** Set the project path prefix used to resolve relative paths in the project file.
     *  Normally set automatically by import(); exposed here so unit tests can exercise
     *  path-joining without needing a real file on disk. */
    // cppcheck-suppress unusedFunction
    void setProjectPath(const std::string& p) {
        mPath = p;
    }

public:
    std::list<FileSettings> fileSettings;
    std::vector<std::string> errors;
    std::vector<std::string> debugs;

    ImportProject() = default;
    virtual ~ImportProject() = default;
    ImportProject(const ImportProject&) = default;
    ImportProject& operator=(const ImportProject&) & = default;

    void selectOneVsConfig(Platform::Type platform);
    void selectVsConfigurations(Platform::Type platform, const std::vector<std::string> &configurations);

    std::list<std::string> getVSConfigs();

    // Cppcheck GUI output
    struct {
        std::vector<std::string> pathNames;
        std::list<std::string> libraries;
        std::list<std::string> excludedPaths;
        std::list<std::string> checkVsConfigs;
        std::string projectFile;
        std::string platform;
    } guiProject;

    void ignorePaths(const std::vector<std::string> &ipaths, bool debug = false);
    void ignoreOtherConfigs(const std::string &cfg);

    Type import(const std::string &filename, Settings *settings=nullptr, Suppressions *supprs=nullptr);

    static const std::string &importResultStr(ImportResult result);

protected:
    bool importCompileCommands(std::istream &istr);
    bool importCppcheckGuiProject(std::istream &istr, Settings &settings, Suppressions &supprs);
    static std::string collectArgs(const std::string &cmd, std::vector<std::string> &args);
    void setRelativePaths(const std::string &filename);

private:
    struct PropertyValueExpander;
    class ConditionParser;

    static void parseArgs(FileSettings &fs, const std::vector<std::string> &args);

    bool importBcb6Prj(const std::string &projectFilename);

    struct ProjectConfiguration {
        explicit ProjectConfiguration(const tinyxml2::XMLElement *cfg);

        std::string name;
        std::string configuration;
        enum : std::uint8_t { Win32, x64, ARM64, ARM64EC, ARM, Unknown } platform = Unknown;
        std::string platformStr;
    };

    struct ItemGroupClCompile {
        explicit ItemGroupClCompile(std::string filename) : filename(std::move(filename)) {}
        std::string filename;
        MetadataMap metadata;
        const std::string &get(const std::string &key) const {
            static const std::string empty;
            const auto it = metadata.find(key);
            return (it != metadata.end()) ? it->second : empty;
        }
    };

    bool importSln(std::istream &istr, const std::string &filename, const std::vector<std::string> &fileFilters);
    bool importSlnx(const std::string& filename, const std::vector<std::string>& fileFilters);
    bool importVcxproj(const std::string &filename, PropertiesMap &properties, const std::vector<std::string> &fileFilters);

    ImportResult importPropsOrTargets(const std::string &file,
                                      PropertiesMap &properties,
                                      MetadataMap &metadata,
                                      std::list<ItemGroupClCompile> &compileList,
                                      std::list<ProjectConfiguration> &projectConfigurationList,
                                      std::unordered_set<std::string> &importStack,
                                      EvalPhase phase = EvalPhase::Properties);
    ImportResult importProject(const tinyxml2::XMLElement *node,
                               const std::string &projectDir,
                               PropertiesMap &properties,
                               MetadataMap &metadata,
                               std::list<ItemGroupClCompile> &compileList,
                               std::list<ProjectConfiguration> &projectConfigurationList,
                               std::unordered_set<std::string> &importStack,
                               EvalPhase phase = EvalPhase::Properties);
    ImportResult importImportGroup(const tinyxml2::XMLElement *node,
                                   const std::string &baseDir,
                                   PropertiesMap &properties,
                                   MetadataMap &metadata,
                                   std::list<ItemGroupClCompile> &compileList,
                                   std::list<ProjectConfiguration> &projectConfigurationList,
                                   std::unordered_set<std::string> &importStack,
                                   EvalPhase phase);
    ImportResult importCompile(const tinyxml2::XMLElement *node,
                               const std::string &projectDir,
                               const PropertiesMap &properties,
                               const MetadataMap &metadata,
                               std::list<ItemGroupClCompile> &compileList);
    // Returns (original-segment, absolute-path) pairs.  The original segment is
    // the spec after property expansion but before toAbsolute(), preserving the
    // relative form needed to compute %(RelativeDir) in importCompile().
    std::vector<std::pair<std::string, std::string>> expandItemSpec(const std::string &spec,
                                                                    const std::string &projectDir,
                                                                    const PropertiesMap &properties);
    std::vector<std::string> expandItemSpecFiles(const std::string &spec,
                                                 const std::string &projectDir,
                                                 const PropertiesMap &properties);
    std::string applyMSBuildStaticFunction(const std::string &className,
                                           const std::string &member,
                                           const std::vector<std::string> &args,
                                           const PropertiesMap *properties = nullptr);
    void applyClCompileChild(const tinyxml2::XMLElement *e1,
                             const PropertiesMap &properties,
                             MetadataMap &metadata);
    void applyClCompileUpdate(const tinyxml2::XMLElement *node,
                              const std::string &dir,
                              const PropertiesMap &properties,
                              std::list<ItemGroupClCompile> &compileList);
    void applyClCompileRemove(const tinyxml2::XMLElement *node,
                              const std::string &dir,
                              const PropertiesMap &properties,
                              std::list<ItemGroupClCompile> &compileList);
    void expandMSBuildVariables(std::string &s, const PropertiesMap &properties);
    bool evalCondition(const std::string &condition, const PropertiesMap &properties);
    bool conditionIsTrue(const tinyxml2::XMLElement *node, const PropertiesMap &properties);
    bool hasName(const tinyxml2::XMLElement *node, const char *nodeName, const PropertiesMap &properties);
    bool hasNameAndAttribute(const tinyxml2::XMLElement *node, const char *nodeName, const char *attrName, const PropertiesMap &properties);
    bool hasNameAndLabel(const tinyxml2::XMLElement *node, const char *nodeName, const char *nodeAttr, const PropertiesMap &properties);
    bool hasNameAndNotLabel(const tinyxml2::XMLElement * node, const char *nodeName, const char *nodeAttr, const PropertiesMap & properties);
    // Decide whether an <Import> / <ImportGroup> element is taken.
    // MSBuild resolves the import graph exactly once, while evaluating properties; the
    // item-definition and item passes then walk that same resolved graph without
    // re-evaluating Import conditions.  (Otherwise an import guard such as
    //   <Import Project="x.props" Condition="'$(XImported)' != 'true'"/>
    // would be taken in the property pass and skipped in every later pass.)
    // During the Properties phase this evaluates the Condition and records the outcome
    // in mImportGraph; during the ItemDefs/Items phases it replays the recorded outcome.
    // When mImportGraph is not active (outside importVcxproj's per-configuration loop)
    // it simply evaluates the Condition.
    bool importTaken(const tinyxml2::XMLElement *node, const char *nodeName, const char *attrName, const PropertiesMap &properties);
    void checkUnexpandedExpressions(const std::string &text, const char *context);
    bool simplifyPathWithVariables(std::string &s, const PropertiesMap &properties);
    void addProperty(const tinyxml2::XMLElement *node, PropertiesMap &properties);
    void addMetadata(const tinyxml2::XMLElement *node, const PropertiesMap &properties, MetadataMap &metadata);
    std::string getMetadata(const tinyxml2::XMLElement *node, const PropertiesMap &properties, const MetadataMap &metadata, const std::string &original);
    std::string toAbsolute(const std::string &filename, const std::string &baseDir, const PropertiesMap &properties);
    static std::string toAbsolute(const std::string &path);
    static void setSolution(const std::string &filename, PropertiesMap &properties);

    /// The import graph resolved during the Properties phase of one project
    /// configuration, replayed during the ItemDefs and Items phases (see importTaken()).
    struct ImportGraph {
        /// file key (lower-cased MSBuildThisFileFullPath) -> outcome of each
        /// <Import>/<ImportGroup> Condition in that file, in document order
        std::map<std::string, std::vector<bool>> decisions;
        /// replay position per file key; reset at the start of every phase
        std::map<std::string, std::size_t> cursor;
        /// files already imported during the current phase.  MSBuild imports a file at
        /// most once per evaluation and ignores a repeated <Import> of it (MSB4011).
        std::unordered_set<std::string> imported;
        bool active = false;  ///< true only inside importVcxproj's per-configuration loop
        bool replay = false;  ///< false while recording (Properties), true while replaying
    };

    std::string mPath;
    std::set<std::string> mAllVSConfigs;
    ImportGraph mImportGraph;
};


namespace CppcheckXml {
    static constexpr char ProjectElementName[] = "project";
    static constexpr char ProjectVersionAttrib[] = "version";
    static constexpr char ProjectFileVersion[] = "1";
    static constexpr char BuildDirElementName[] = "builddir";
    static constexpr char ImportProjectElementName[] = "importproject";
    static constexpr char AnalyzeAllVsConfigsElementName[] = "analyze-all-vs-configs";
    static constexpr char Parser[] = "parser";
    static constexpr char IncludeDirElementName[] = "includedir";
    static constexpr char DirElementName[] = "dir";
    static constexpr char DirNameAttrib[] = "name";
    static constexpr char DefinesElementName[] = "defines";
    static constexpr char DefineName[] = "define";
    static constexpr char DefineNameAttrib[] = "name";
    static constexpr char UndefinesElementName[] = "undefines";
    static constexpr char UndefineName[] = "undefine";
    static constexpr char UserIncludeElementName[] = "user-include";
    static constexpr char PathsElementName[] = "paths";
    static constexpr char PathName[] = "dir";
    static constexpr char PathNameAttrib[] = "name";
    static constexpr char RootPathName[] = "root";
    static constexpr char RootPathNameAttrib[] = "name";
    static constexpr char IgnoreElementName[] = "ignore";
    static constexpr char IgnorePathName[] = "path";
    static constexpr char IgnorePathNameAttrib[] = "name";
    static constexpr char ExcludeElementName[] = "exclude";
    static constexpr char ExcludePathName[] = "path";
    static constexpr char ExcludePathNameAttrib[] = "name";
    static constexpr char FunctionContracts[] = "function-contracts";
    static constexpr char VariableContractsElementName[] = "variable-contracts";
    static constexpr char LibrariesElementName[] = "libraries";
    static constexpr char LibraryElementName[] = "library";
    static constexpr char PlatformElementName[] = "platform";
    static constexpr char SuppressionsElementName[] = "suppressions";
    static constexpr char SuppressionElementName[] = "suppression";
    static constexpr char AddonElementName[] = "addon";
    static constexpr char AddonsElementName[] = "addons";
    static constexpr char ToolElementName[] = "tool";
    static constexpr char ToolsElementName[] = "tools";
    static constexpr char TagsElementName[] = "tags";
    static constexpr char TagElementName[] = "tag";
    static constexpr char TagWarningsElementName[] = "tag-warnings";
    static constexpr char TagAttributeName[] = "tag";
    static constexpr char WarningElementName[] = "warning";
    static constexpr char HashAttributeName[] = "hash";
    static constexpr char CheckLevelExhaustiveElementName[] = "check-level-exhaustive";
    static constexpr char CheckLevelNormalElementName[] = "check-level-normal";
    static constexpr char CheckLevelReducedElementName[] = "check-level-reduced";
    static constexpr char CheckHeadersElementName[] = "check-headers";
    static constexpr char CheckUnusedTemplatesElementName[] = "check-unused-templates";
    static constexpr char MaxCtuDepthElementName[] = "max-ctu-depth";
    static constexpr char MaxTemplateRecursionElementName[] = "max-template-recursion";
    static constexpr char CheckUnknownFunctionReturn[] = "check-unknown-function-return-values";
    static constexpr char InlineSuppression[] = "inline-suppression";
    static constexpr char ClangTidy[] = "clang-tidy";
    static constexpr char Name[] = "name";
    static constexpr char VSConfigurationElementName[] = "vs-configurations";
    static constexpr char VSConfigurationName[] = "config";
    // Cppcheck Premium
    static constexpr char BughuntingElementName[] = "bug-hunting";
    static constexpr char CodingStandardsElementName[] = "coding-standards";
    static constexpr char CodingStandardElementName[] = "coding-standard";
    static constexpr char CertIntPrecisionElementName[] = "cert-c-int-precision";
    static constexpr char ProjectNameElementName[] = "project-name";
}

/// @}
//---------------------------------------------------------------------------
#endif // importprojectH
