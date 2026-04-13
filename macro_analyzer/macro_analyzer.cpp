// macro_analyzer.cpp - Complete analysis of macro definitions and references (including usage locations)
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Lexer.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Decl.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <iostream>
#include <map>
#include <set>
#include <vector>
#include <filesystem>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

static std::string g_compileDir;

struct SymbolInfo {
    std::string kind;
    SourceLocation defLocation;
    SourceRange defRange;
};

// Information about usage locations
struct UsageLocation {
    std::string name;
    std::string kind;
    SourceLocation useLoc;        // Usage location
    SourceLocation defLoc;        // Definition location
    std::string useLocStr;        // Stringified usage location
    std::string defLocStr;        // Stringified definition location
    unsigned startLine;
    unsigned endLine;
    bool resolved = false;
};

struct MacroSymbolInfo {
    std::string name;
    std::string kind;
    SourceLocation defLocation;
    SourceRange defRange;
    std::vector<UsageLocation> uses;
    std::vector<std::string> parameters;
};

class MacroCollector {
public:
    SourceManager *SM;
    Preprocessor *PP;
    std::map<std::string, MacroSymbolInfo> macros;
    std::map<std::string, SymbolInfo> symbols;
    std::set<std::pair<std::string, std::string>> resolvedTokens;
    
    explicit MacroCollector(SourceManager *SM, Preprocessor *PP) 
        : SM(SM), PP(PP) {}
    
    bool isSystemLocation(SourceLocation Loc) {
        if (Loc.isInvalid()) return true;
        return SM->isInSystemHeader(Loc);
    }
    
    // std::string getAbsolutePath(StringRef filename) {
    //     std::filesystem::path p(filename.str());
    //     std::error_code ec;
    //     if (!p.is_absolute()) {
    //         p = std::filesystem::absolute(p, ec);
    //     }
    //     return p.lexically_normal().string();
    // }

    std::string getAbsolutePath(StringRef filename) {
        // added: return virtual paths such as <built-in> and <command line> as-is
        std::string name = filename.str();
        if (!name.empty() && name[0] == '<') {
            if (!g_compileDir.empty()) {
                return g_compileDir + "/" + name;
            }
            return name;
        }
        // ended
        std::filesystem::path p(name);
        std::error_code ec;
        if (!p.is_absolute()) {
            p = std::filesystem::absolute(p, ec);
        }
        return p.lexically_normal().string();
    }
    
    std::string getLocationString(SourceLocation Loc) {
        if (Loc.isInvalid()) return "unknown";
        
        PresumedLoc PLoc = SM->getPresumedLoc(Loc);
        if (PLoc.isInvalid()) return "unknown";
        
        std::string absPath = getAbsolutePath(PLoc.getFilename());
        std::string Result;
        llvm::raw_string_ostream OS(Result);
        OS << absPath << ":" << PLoc.getLine() << ":" << PLoc.getColumn();
        return OS.str();
    }
    
    std::pair<unsigned, unsigned> getLineRange(SourceRange Range) {
        if (Range.isInvalid()) return {0, 0};
        
        PresumedLoc StartLoc = SM->getPresumedLoc(Range.getBegin());
        PresumedLoc EndLoc = SM->getPresumedLoc(Range.getEnd());
        
        if (StartLoc.isInvalid() || EndLoc.isInvalid()) return {0, 0};
        
        return {StartLoc.getLine(), EndLoc.getLine()};
    }
    
    void collectMacroUses(const MacroInfo *MI, const std::string &ownerName) {
        if (!MI) return;
        
        std::set<std::string> paramNames;
        if (MI->isFunctionLike()) {
            for (const IdentifierInfo *Param : MI->params()) {
                if (Param) paramNames.insert(Param->getName().str());
            }
        }
        
        for (const Token &Tok : MI->tokens()) {
            if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
                StringRef Name;
                if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
                    Name = Tok.getIdentifierInfo()->getName();
                } else if (Tok.is(tok::raw_identifier)) {
                    Name = Tok.getRawIdentifier();
                } else {
                    continue;
                }
                
                std::string tokenName = Name.str();
                if (paramNames.count(tokenName)) continue;
                
                UsageLocation usage;
                usage.name = tokenName;
                usage.useLoc = Tok.getLocation();
                
                // Attempt immediate resolution (case 1)
                IdentifierInfo *II = PP->getIdentifierInfo(tokenName);
                if (II && II->hasMacroDefinition()) {
                    MacroDefinition MD = PP->getMacroDefinition(II);
                    if (MD && MD.getMacroInfo()) {
                        const MacroInfo *RefMI = MD.getMacroInfo();
                        usage.kind = RefMI->isFunctionLike()
                            ? "macro_function" : "macro";
                        usage.defLoc = RefMI->getDefinitionLoc();
                        PresumedLoc sl = SM->getPresumedLoc(RefMI->getDefinitionLoc());
                        PresumedLoc el = SM->getPresumedLoc(RefMI->getDefinitionEndLoc());
                        usage.startLine = sl.isValid() ? sl.getLine() : 0;
                        usage.endLine = el.isValid() ? el.getLine() : 0;
                        usage.resolved = true;
                        resolvedTokens.insert({ownerName, tokenName});
                        macros[ownerName].uses.push_back(usage);
                        continue;
                    }
                }
                
                // Unresolved
                usage.kind = "unknown";
                usage.resolved = false;
                usage.startLine = 0;
                usage.endLine = 0;
                macros[ownerName].uses.push_back(usage);
            }
        }
    }

    // void collectMacroUses(const MacroInfo *MI, const std::string &ownerName) {
    //     if (!MI) return;
    //     std::set<std::string> paramNames;
    //     if (MI->isFunctionLike()) {
    //         for (const IdentifierInfo *Param : MI->params()) {
    //             if (Param) paramNames.insert(Param->getName().str());
    //         }
    //     }
        
    //     for (const Token &Tok : MI->tokens()) {
    //         if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
    //             StringRef Name;
    //             if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
    //                 Name = Tok.getIdentifierInfo()->getName();
    //             } else if (Tok.is(tok::raw_identifier)) {
    //                 Name = Tok.getRawIdentifier();
    //             } else {
    //                 continue;
    //             }
                
    //             std::string tokenName = Name.str();
                
    //             if (paramNames.count(tokenName)) continue;
                
    //             UsageLocation usage;
    //             usage.name = tokenName;
    //             usage.useLoc = Tok.getLocation();
    //             usage.kind = "unknown";
    //             usage.startLine = 0;
    //             usage.endLine = 0;
    //             macros[ownerName].uses.push_back(usage);
    //         }
    //     }
    // }

    // void collectMacroUses(const MacroInfo *MI, const std::string &ownerName) {
    //     if (!MI) return;
    
    //     for (const Token &Tok : MI->tokens()) {
    //         if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
    //             StringRef Name;
    //             if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
    //                 Name = Tok.getIdentifierInfo()->getName();
    //             } else if (Tok.is(tok::raw_identifier)) {
    //                 Name = Tok.getRawIdentifier();
    //             } else {
    //                 continue;
    //             }
                
    //             std::string tokenName = Name.str();
    //             SourceLocation useLoc = Tok.getLocation();
                
    //             UsageLocation usage;
    //             usage.name = tokenName;
    //             usage.useLoc = useLoc;
                
    //             if (macros.count(tokenName)) {
    //                 const auto &usedMacro = macros[tokenName];
    //                 usage.kind = usedMacro.kind;
    //                 usage.defLoc = usedMacro.defLocation;
    //                 auto [sl, el] = getLineRange(usedMacro.defRange);
    //                 usage.startLine = sl;
    //                 usage.endLine = el;
    //             } else if (symbols.count(tokenName)) {
    //                 const auto &usedSymbol = symbols[tokenName];
    //                 usage.kind = usedSymbol.kind;
    //                 usage.defLoc = usedSymbol.defLocation;
    //                 auto [sl, el] = getLineRange(usedSymbol.defRange);
    //                 usage.startLine = sl;
    //                 usage.endLine = el;
    //             } else {
    //                 usage.kind = "unknown";
    //                 usage.defLoc = SourceLocation();
    //                 usage.startLine = 0;
    //                 usage.endLine = 0;
    //             }
                
    //             macros[ownerName].uses.push_back(usage);
    //         }
    //     }
    // }
};

class SymbolCollector : public RecursiveASTVisitor<SymbolCollector> {
private:
    MacroCollector &Collector;
    
public:
    explicit SymbolCollector(MacroCollector &Collector) 
        : Collector(Collector) {}
    
    bool VisitFunctionDecl(FunctionDecl *FD) {
        if (Collector.isSystemLocation(FD->getLocation())) {
            return true;
        }
        
        if (FD->isThisDeclarationADefinition()) {
            SymbolInfo info;
            info.kind = "function";
            info.defLocation = FD->getLocation();
            info.defRange = FD->getSourceRange();
            Collector.symbols[FD->getNameAsString()] = info;
        }
        return true;
    }
    
    bool VisitVarDecl(VarDecl *VD) {
        if (Collector.isSystemLocation(VD->getLocation())) {
            return true;
        }
        
        if (VD->hasGlobalStorage() && !VD->isStaticLocal()) {
            SymbolInfo info;
            info.kind = "global_var";
            info.defLocation = VD->getLocation();
            info.defRange = VD->getSourceRange();
            Collector.symbols[VD->getNameAsString()] = info;
        }
        return true;
    }
    
    bool VisitTypedefDecl(TypedefDecl *TD) {
        if (Collector.isSystemLocation(TD->getLocation())) {
            return true;
        }
        
        SymbolInfo info;
        info.kind = "typedef";
        info.defLocation = TD->getLocation();
        info.defRange = TD->getSourceRange();
        Collector.symbols[TD->getNameAsString()] = info;
        return true;
    }
    
    bool VisitRecordDecl(RecordDecl *RD) {
        if (Collector.isSystemLocation(RD->getLocation())) {
            return true;
        }
        
        if (RD->isThisDeclarationADefinition() && RD->getIdentifier()) {
            SymbolInfo info;
            info.kind = RD->isStruct() ? "struct" : "union";
            info.defLocation = RD->getLocation();
            info.defRange = RD->getSourceRange();
            Collector.symbols[RD->getNameAsString()] = info;
        }
        return true;
    }
    
    bool VisitEnumDecl(EnumDecl *ED) {
        if (Collector.isSystemLocation(ED->getLocation())) {
            return true;
        }
        
        if (ED->isThisDeclarationADefinition() && ED->getIdentifier()) {
            SymbolInfo info;
            info.kind = "enum";
            info.defLocation = ED->getLocation();
            info.defRange = ED->getSourceRange();
            Collector.symbols[ED->getNameAsString()] = info;
        }
        return true;
    }
    
    bool VisitEnumConstantDecl(EnumConstantDecl *ECD) {
        if (Collector.isSystemLocation(ECD->getLocation())) {
            return true;
        }
        
        SymbolInfo info;
        info.kind = "enum_constant";
        info.defLocation = ECD->getLocation();
        info.defRange = ECD->getSourceRange();
        Collector.symbols[ECD->getNameAsString()] = info;
        return true;
    }
};

class MacroCallbacks : public PPCallbacks {
private:
    MacroCollector &Collector;

public:
    explicit MacroCallbacks(MacroCollector &Collector) 
        : Collector(Collector) {}

    void MacroDefined(const Token &MacroNameTok, const MacroDirective *MD) override {
        SourceLocation Loc = MacroNameTok.getLocation();
        
        std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
        const MacroInfo *MI = MD->getMacroInfo();
        
        MacroSymbolInfo &info = Collector.macros[MacroName];
        info.name = MacroName;
        info.defLocation = Loc;
        
        if (MI) {
            SourceLocation endLoc = MI->getDefinitionEndLoc();
            SourceLocation tokenEnd = Lexer::getLocForEndOfToken(
                endLoc, 0, *Collector.SM, Collector.PP->getLangOpts()
            );

            info.defRange = SourceRange(
                MI->getDefinitionLoc(),
                tokenEnd
            );
            
            if (MI->isFunctionLike()) {
                info.kind = "macro_function";
                
                for (unsigned i = 0; i < MI->getNumParams(); ++i) {
                    info.parameters.push_back(MI->params()[i]->getName().str());
                }
                
                if (MI->isVariadic()) {
                    info.parameters.push_back("...");
                }
            } else {
                info.kind = "macro"; //_object";
            }
            
            Collector.collectMacroUses(MI, MacroName);
        } else {
            info.kind = "macro"; //_object";
            info.defRange = SourceRange(Loc, Loc);
        }
    }

    void SkippedMacroDefined(const Token &MacroNameTok) override {
        // SourceLocation Loc = MacroNameTok.getLocation();
        
        // if (!MacroNameTok.getIdentifierInfo()) return;
        
        // std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
        
        // MacroSymbolInfo &info = Collector.macros[MacroName];
        // info.name = MacroName;
        // info.kind = "macro"; // _object";
        // info.defLocation = Loc;
        // info.defRange = SourceRange(Loc, Loc);
    }

    void MacroExpands(const Token &MacroNameTok,
                  const MacroDefinition &MD,
                  SourceRange Range,
                  const MacroArgs *Args) override {
    const MacroInfo *MI = MD.getMacroInfo();
    if (!MI) return;
    
    std::string macroName =
        MacroNameTok.getIdentifierInfo()->getName().str();
    
    // Skip if this macro is not in the macros map
    if (!Collector.macros.count(macroName)) return;
    
    for (const Token &Tok : MI->tokens()) {
        if (!Tok.is(tok::identifier)) continue;
        if (!Tok.getIdentifierInfo()) continue;
        
        std::string tokenName =
            Tok.getIdentifierInfo()->getName().str();
        
        // Skip if already resolved
        auto key = std::make_pair(macroName, tokenName);
        if (Collector.resolvedTokens.count(key)) continue;
        
        IdentifierInfo *II = Collector.PP->getIdentifierInfo(tokenName);
        if (II && II->hasMacroDefinition()) {
            MacroDefinition RefMD = Collector.PP->getMacroDefinition(II);
            if (RefMD && RefMD.getMacroInfo()) {
                const MacroInfo *RefMI = RefMD.getMacroInfo();
                
                for (auto &u : Collector.macros[macroName].uses) {
                    if (u.name == tokenName && !u.resolved) {
                        u.kind = RefMI->isFunctionLike()
                            ? "macro_function" : "macro";
                        u.defLoc = RefMI->getDefinitionLoc();
                        PresumedLoc sl = Collector.SM->getPresumedLoc(
                            RefMI->getDefinitionLoc());
                        PresumedLoc el = Collector.SM->getPresumedLoc(
                            RefMI->getDefinitionEndLoc());
                        u.startLine = sl.isValid() ? sl.getLine() : 0;
                        u.endLine = el.isValid() ? el.getLine() : 0;
                        u.resolved = true;
                        break;
                    }
                }
                
                Collector.resolvedTokens.insert(key);
            }
        }
    }
}
};

class CombinedASTConsumer : public ASTConsumer {
private:
    MacroCollector &Collector;
    SymbolCollector SymVisitor;
    
public:
    explicit CombinedASTConsumer(MacroCollector &Collector)
        : Collector(Collector), SymVisitor(Collector) {}
    
    void HandleTranslationUnit(ASTContext &Context) override {
        SymVisitor.TraverseDecl(Context.getTranslationUnitDecl());
    }
};


// === Globals for batch mode ===
struct ResolvedMacro {
    std::string defLocStr;
    std::string name;
    std::string kind;
    std::vector<std::string> parameters;
    unsigned startLine;
    unsigned endLine;
    std::vector<UsageLocation> uses;
};
static std::vector<ResolvedMacro> g_allMacros;
static bool g_batchMode = false;

static std::string escapeJSON(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

static void outputAllJSON() {
    std::cout << "{\n  \"macros\": [\n";
    bool first = true;
    for (const auto &m : g_allMacros) {
        if (!first) std::cout << ",\n";
        first = false;
        std::cout << "    {\n";
        std::cout << "      \"kind\": \"" << escapeJSON(m.kind) << "\",\n";
        std::cout << "      \"name\": \"" << escapeJSON(m.name) << "\",\n";
        if (!m.parameters.empty()) {
            std::cout << "      \"parameters\": [";
            bool fp = true;
            for (const auto &p : m.parameters) {
                if (!fp) std::cout << ", ";
                std::cout << "\"" << escapeJSON(p) << "\"";
                fp = false;
            }
            std::cout << "],\n";
        }
        std::cout << "      \"definition\": \"" << escapeJSON(m.defLocStr) << "\",\n";
        std::cout << "      \"start_line\": " << m.startLine << ",\n";
        std::cout << "      \"end_line\": " << m.endLine << ",\n";
        std::cout << "      \"uses\": []\n";
        // std::cout << "      \"uses\": [\n";
        // bool fd = true;
        // for (const auto &u : m.uses) {
        //     if (!fd) std::cout << ",\n";
        //     fd = false;
        //     std::cout << "        {\n";
        //     std::cout << "          \"kind\": \"" << escapeJSON(u.kind) << "\",\n";
        //     std::cout << "          \"name\": \"" << escapeJSON(u.name) << "\",\n";
        //     std::cout << "          \"usage_location\": \"" << escapeJSON(u.useLocStr) << "\",\n";
        //     std::cout << "          \"definition\": \"" << escapeJSON(u.defLocStr) << "\",\n";
        //     std::cout << "          \"start_line\": " << u.startLine << ",\n";
        //     std::cout << "          \"end_line\": " << u.endLine << "\n";
        //     std::cout << "        }";
        // }
        // std::cout << "\n      ]\n";
        std::cout << "    }";
    }
    std::cout << "\n  ]\n}\n";
}


class MacroAnalyzerAction : public ASTFrontendAction {
private:
    MacroCollector *Collector;

protected:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(
        CompilerInstance &CI, StringRef file) override {
        
        Preprocessor &PP = CI.getPreprocessor();
        SourceManager &SM = CI.getSourceManager();
        
        Collector = new MacroCollector(&SM, &PP);
        
        PP.addPPCallbacks(std::make_unique<MacroCallbacks>(*Collector));
        
        return std::make_unique<CombinedASTConsumer>(*Collector);
    }
    
    // void EndSourceFileAction() override {
    //     outputJSON();
    //     delete Collector;
    // }
    
    void EndSourceFileAction() override {
        // Case 3: Resolve unresolved items using fallback
        for (auto &[name, info] : Collector->macros) {
            for (auto &u : info.uses) {
                if (u.resolved) continue;
                
                if (Collector->macros.count(u.name)) {
                    const auto &ref = Collector->macros[u.name];
                    u.kind = ref.kind;
                    u.defLoc = ref.defLocation;
                    auto [sl, el] = Collector->getLineRange(ref.defRange);
                    u.startLine = sl;
                    u.endLine = el;
                    u.resolved = true;
                } else if (Collector->symbols.count(u.name)) {
                    const auto &ref = Collector->symbols[u.name];
                    u.kind = ref.kind;
                    u.defLoc = ref.defLocation;
                    auto [sl, el] = Collector->getLineRange(ref.defRange);
                    u.startLine = sl;
                    u.endLine = el;
                    u.resolved = true;
                }
            }
        }
        
        // Convert to string (must be done before destroying Collector)
        if (g_batchMode) {
            for (auto &[name, info] : Collector->macros) {
                ResolvedMacro rm;
                rm.defLocStr = Collector->getLocationString(info.defLocation);
                rm.name = info.name;
                rm.kind = info.kind;
                rm.parameters = info.parameters;
                auto [sl, el] = Collector->getLineRange(info.defRange);
                rm.startLine = sl;
                rm.endLine = el;
                for (auto &u : info.uses) {
                    u.useLocStr = Collector->getLocationString(u.useLoc);
                    u.defLocStr = Collector->getLocationString(u.defLoc);
                }
                rm.uses = info.uses;
                g_allMacros.push_back(std::move(rm));
            }
        } else {
            outputJSON();
        }
        
        delete Collector;
    }

    // void EndSourceFileAction() override {
    //     for (auto &[name, info] : Collector->macros) {
    //         for (auto &u : info.uses) {
    //             if (Collector->macros.count(u.name)) {
    //                 const auto &ref = Collector->macros[u.name];
    //                 u.kind = ref.kind;
    //                 u.defLoc = ref.defLocation;
    //                 auto [sl, el] = Collector->getLineRange(ref.defRange);
    //                 u.startLine = sl;
    //                 u.endLine = el;
    //             } else if (Collector->symbols.count(u.name)) {
    //                 const auto &ref = Collector->symbols[u.name];
    //                 u.kind = ref.kind;
    //                 u.defLoc = ref.defLocation;
    //                 auto [sl, el] = Collector->getLineRange(ref.defRange);
    //                 u.startLine = sl;
    //                 u.endLine = el;
    //             } else {
    //                 u.kind = "unknown";
    //             }
    //         }
    //     }
    
    //     if (g_batchMode) {
    //         for (auto &[name, info] : Collector->macros) {
    //             ResolvedMacro rm;
    //             rm.defLocStr = Collector->getLocationString(info.defLocation);
    //             rm.name = info.name;
    //             rm.kind = info.kind;
    //             rm.parameters = info.parameters;
    //             auto [sl, el] = Collector->getLineRange(info.defRange);
    //             rm.startLine = sl;
    //             rm.endLine = el;
    //             for (auto &u : info.uses) {
    //                 u.useLocStr = Collector->getLocationString(u.useLoc);
    //                 u.defLocStr = Collector->getLocationString(u.defLoc);
    //             }
    //             rm.uses = info.uses;
    //             g_allMacros.push_back(std::move(rm));
    //         }
    //     } else {
    //         outputJSON();
    //     }
    //     delete Collector;
    // }

    // void EndSourceFileAction0() override {
    //     if (g_batchMode) {
    //         for (auto &[name, info] : Collector->macros) {
    //             ResolvedMacro rm;
    //             rm.defLocStr = Collector->getLocationString(info.defLocation);
    //             rm.name = info.name;
    //             rm.kind = info.kind;
    //             rm.parameters = info.parameters;
    //             auto [sl, el] = Collector->getLineRange(info.defRange);
    //             rm.startLine = sl;
    //             rm.endLine = el;
    //             for (auto &u : info.uses) {
    //                 u.useLocStr = Collector->getLocationString(u.useLoc);
    //                 u.defLocStr = Collector->getLocationString(u.defLoc);
    //             }
    //             rm.uses = info.uses;
    //             g_allMacros.push_back(std::move(rm));
    //         }
    //     } else {
    //         outputJSON();
    //     }
    //     delete Collector;
    // }
    
    void outputJSON() {
        std::cout << "{\n";
        std::cout << "  \"macros\": [\n";
        
        bool first = true;
        for (const auto &[name, info] : Collector->macros) {
            if (!first) {
                std::cout << ",\n";
            }
            first = false;
            
            std::cout << "    {\n";
            std::cout << "      \"kind\": \"" << info.kind << "\",\n";
            std::cout << "      \"name\": \"" << info.name << "\",\n";
            
            if (!info.parameters.empty()) {
                std::cout << "      \"parameters\": [";
                bool firstParam = true;
                for (const auto &param : info.parameters) {
                    if (!firstParam) std::cout << ", ";
                    std::cout << "\"" << param << "\"";
                    firstParam = false;
                }
                std::cout << "],\n";
            }
            
            std::cout << "      \"definition\": \"" 
                      << Collector->getLocationString(info.defLocation) << "\",\n";
            
            auto [startLine, endLine] = Collector->getLineRange(info.defRange);
            std::cout << "      \"start_line\": " << startLine << ",\n";
            std::cout << "      \"end_line\": " << endLine << ",\n";
            
            std::cout << "      \"uses\": [\n";
            
            bool firstDep = true;
            for (const auto &usage : info.uses) {
                if (!firstDep) {
                    std::cout << ",\n";
                }
                firstDep = false;
                
                std::cout << "        {\n";
                std::cout << "          \"kind\": \"" << usage.kind << "\",\n";
                std::cout << "          \"name\": \"" << usage.name << "\",\n";
                std::cout << "          \"usage_location\": \"" 
                          << Collector->getLocationString(usage.useLoc) << "\",\n";
                std::cout << "          \"definition\": \"" 
                          << Collector->getLocationString(usage.defLoc) << "\",\n";
                std::cout << "          \"start_line\": " << usage.startLine << ",\n";
                std::cout << "          \"end_line\": " << usage.endLine << "\n";
                std::cout << "        }";
            }
            
            std::cout << "\n      ]\n";
            std::cout << "    }";
        }
        
        std::cout << "\n  ]\n";
        std::cout << "}\n";
    }
};

static llvm::cl::OptionCategory MyToolCategory("macro-analyzer options");

// === Define include paths in one place ===
static const std::vector<std::string> CUSTOM_INCLUDE_PATHS = {
    // Specify Clang resource directory
    //"-resource-dir=/root/SmartC2Rust/macro/llvm-custom/lib/clang/19",
    "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
    // Disable all warnings
    "-w",
    "-Wno-incompatible-function-pointer-types", 
    "-Wno-incompatible-pointer-types",
    // "-Wno-incompatible-pointer-types-discards-qualifiers",
    //"-Wno-error", 
    //"-Wno-everything",
    
    // Prioritize Clang built-in headers
    //"-isystem/root/SmartC2Rust/macro/llvm-custom/lib/clang/19/include",
    // ★★★ Add OpenMP headers ★★★
    //"-isystem/usr/lib/llvm-14/lib/clang/14.0.0/include",
    //"-isystem/usr/lib/llvm-19/lib/clang/19/include",
    // C++ headers (before C headers)
    // "-isystem/usr/include/c++/11",
    // "-isystem/usr/include/x86_64-linux-gnu/c++/11",
    // "-isystem/usr/include/c++/11/backward",
    
    // C system headers (after C++ so they can be found via #include_next)
    "-isystem/usr/include/x86_64-linux-gnu",
    "-isystem/usr/include",
    
    // Compiler settings
    // "-std=gnu11",
    // "-std=gnu++11",
    "-fno-strict-aliasing",
};


// static const std::vector<std::string> CUSTOM_INCLUDE_PATHS = {
//     "-resource-dir=/root/SmartC2Rust/macro/llvm-custom/lib/clang/19",
//     "-w",
//     "-isystem/root/SmartC2Rust/macro/llvm-custom/lib/clang/19/include",
//     "-isystem/usr/include/c++/11",
//     "-isystem/usr/include/x86_64-linux-gnu/c++/11",
//     "-isystem/usr/include/c++/11/backward",
//     "-isystem/usr/lib/gcc/x86_64-linux-gnu/11/include",
//     "-isystem/usr/local/include",
//     "-isystem/usr/include/x86_64-linux-gnu",
//     "-isystem/usr/include",
//     "-std=gnu11",
//     "-fno-strict-aliasing",
//     // "-Wno-error",
//     // "-w"
//     "-Wno-error=incompatible-function-pointer-types",
//     "-Wno-error=atomic-alignment",
//     // "-Wno-error=atomic-alignment",
//     // "-Wno-error"
// };

// static const std::vector<std::string> CUSTOM_INCLUDE_PATHS = {
//     "-isystem/root/SmartC2Rust/macro/llvm-custom/lib/clang/19/include",
//     "-isystem/usr/include",
//     "-isystem/usr/include/x86_64-linux-gnu",
//     "-isystem/usr/lib/gcc/x86_64-linux-gnu/11/include",
//     "-isystem/usr/include/c++/11",
//     "-isystem/usr/include/x86_64-linux-gnu/c++/11",
//     "-isystem/usr/include/c++/11/backward"
// };




void addCustomIncludePaths(ClangTool &Tool) {
    Tool.appendArgumentsAdjuster(
        [](const CommandLineArguments &Args, StringRef Filename) -> CommandLineArguments {
        CommandLineArguments NewArgs = Args;
        
        std::vector<std::string> CommonArgs = {
            "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
            "-w",
            "-Wno-incompatible-function-pointer-types",
            "-Wno-incompatible-pointer-types",
            "-fno-strict-aliasing",
            "-isystem/usr/lib/llvm-19/lib/clang/19/include",
        };

        if (Filename.ends_with(".cxx") || Filename.ends_with(".cpp") || 
            Filename.ends_with(".cc") || Filename.ends_with(".C")) {
            CommonArgs.push_back("-isystem/usr/include/c++/11");
            CommonArgs.push_back("-isystem/usr/include/x86_64-linux-gnu/c++/11");
            CommonArgs.push_back("-isystem/usr/include/c++/11/backward");
        }

        CommonArgs.push_back("-isystem/usr/include/x86_64-linux-gnu");
        CommonArgs.push_back("-isystem/usr/include");

        NewArgs.insert(NewArgs.begin() + 1, CommonArgs.begin(), CommonArgs.end());
        return NewArgs;
        }
    );
}


void addCustomIncludePaths0(ClangTool &Tool) {
    Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
        CUSTOM_INCLUDE_PATHS,
        ArgumentInsertPosition::BEGIN));
}


// Additional include (to be added at the top of the file)
// #include "clang/Tooling/CompilationDatabase.h"  // ← It may already be included via Tooling.h. If not, add it.

int main(int argc, const char **argv) {
    // Check whether the -p option is provided
    std::string dbPath;
    bool hasDBPath = false;
    
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-p" && i + 1 < argc) {
            dbPath = argv[i + 1];
            hasDBPath = true;
            break;
        }
    }
    
    if (hasDBPath) {
        // -p path: automatically obtain the file list from the compilation database
        // Convert relative path to absolute path
        std::filesystem::path absDbPath = std::filesystem::absolute(dbPath);
        absDbPath = absDbPath.lexically_normal();
        g_compileDir = absDbPath.string();  // added
        
        std::string errMsg;
        auto DB = CompilationDatabase::loadFromDirectory(absDbPath.string(), errMsg);
        if (!DB) {
            llvm::errs() << "Error loading compilation database from '" 
                         << dbPath << "': " << errMsg << "\n";
            return 1;
        }
        
        std::vector<std::string> AllFiles = DB->getAllFiles();
        if (AllFiles.empty()) {
            llvm::errs() << "No files found in compilation database at '" 
                         << dbPath << "'\n";
            return 1;
        }
        
        llvm::errs() << "Processing " << AllFiles.size() 
                      << " files from compilation database\n";
        
        // ClangTool Tool(*DB, AllFiles);
        // addCustomIncludePaths(Tool);
        
        // return Tool.run(newFrontendActionFactory<MacroAnalyzerAction>().get());
        g_batchMode = true;
        
        ClangTool Tool(*DB, AllFiles);
        addCustomIncludePaths(Tool);
        
        int ret = Tool.run(newFrontendActionFactory<MacroAnalyzerAction>().get());
        outputAllJSON();
        return ret;

    }
    
    // Single-file mode: macro_analyzer <source_file> [-- <compile_flags>]
    if (argc >= 2) {
        std::vector<std::string> SourcePaths;
        SourcePaths.push_back(argv[1]);
        
        std::vector<std::string> CompileCommands;
        bool afterDashes = false;
        for (int i = 2; i < argc; ++i) {
            if (std::string(argv[i]) == "--") {
                afterDashes = true;
                continue;
            }
            if (afterDashes) {
                CompileCommands.push_back(argv[i]);
            }
        }
        
        auto Compilations = std::make_unique<FixedCompilationDatabase>(
            ".", CompileCommands);
        
        ClangTool Tool(*Compilations, SourcePaths);
        addCustomIncludePaths(Tool);
        
        return Tool.run(newFrontendActionFactory<MacroAnalyzerAction>().get());
    }
    
    llvm::errs() << "Usage:\n"
                 << "  macro_analyzer <source_file> [-- <compile_flags>]\n"
                 << "  macro_analyzer -p <compile_commands_dir>\n";
    return 1;
}


// int main(int argc, const char **argv) {
//     if (argc >= 2 && std::string(argv[1]) != "-p") {
//         std::vector<std::string> SourcePaths;
//         SourcePaths.push_back(argv[1]);
        
//         std::vector<std::string> CompileCommands;
//         bool afterDashes = false;
//         for (int i = 2; i < argc; ++i) {
//             if (std::string(argv[i]) == "--") {
//                 afterDashes = true;
//                 continue;
//             }
//             if (afterDashes) {
//                 CompileCommands.push_back(argv[i]);
//             }
//         }
        
//         auto Compilations = std::make_unique<FixedCompilationDatabase>(
//             ".", CompileCommands);
        
//         ClangTool Tool(*Compilations, SourcePaths);
//         addCustomIncludePaths(Tool);
        
//         return Tool.run(newFrontendActionFactory<MacroAnalyzerAction>().get());
//     }
    
//     auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
//     if (!ExpectedParser) {
//         llvm::errs() << ExpectedParser.takeError();
//         return 1;
//     }
//     CommonOptionsParser &OptionsParser = ExpectedParser.get();
//     ClangTool Tool(OptionsParser.getCompilations(),
//                    OptionsParser.getSourcePathList());
    
//     addCustomIncludePaths(Tool);
    
//     return Tool.run(newFrontendActionFactory<MacroAnalyzerAction>().get());
// }
