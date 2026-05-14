// pragma_finder.cpp
// Locates every #pragma once (and _Pragma("once")) directive that the
// preprocessor actually evaluates as active. Comments, strings, and
// disabled #if branches are filtered out automatically by Clang.
//
// Output format: JSONL, one line per occurrence.
//   {"tu":"/abs/path/main.c","file":"/abs/path/header.h","line":3}
//
// Note: the same header may be reported multiple times if it is reached
// from multiple translation units. Deduplicate on (file, line) downstream
// if a unique list is needed.

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Pragma.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

// Shared output path. ClangTool may process TUs in parallel, so writes
// from the pragma handler must be serialized.
static std::string g_outputPath;
static std::mutex g_outputMutex;

static std::string escapeJson(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '\\' || c == '"') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

static std::string resolvePath(SourceManager &SM, SourceLocation Loc) {
  FileID FID = SM.getFileID(Loc);
  if (auto FE = SM.getFileEntryRefForID(FID)) {
    std::filesystem::path p(FE->getName().str());
    std::error_code EC;
    auto c = std::filesystem::canonical(p, EC);
    if (!EC) return c.string();
    if (!p.is_absolute()) p = std::filesystem::absolute(p);
    return p.lexically_normal().string();
  }
  PresumedLoc PLoc = SM.getPresumedLoc(Loc);
  return PLoc.isValid() ? std::string(PLoc.getFilename()) : "";
}

// Registered as a handler for the literal pragma name "once".
// HandlePragma fires only when Clang determines the directive is active,
// so we get the same judgment the compiler itself uses. _Pragma("once")
// is routed through the same handler internally.
class OncePragmaHandler : public PragmaHandler {
public:
  OncePragmaHandler(SourceManager &SM, std::string tu)
      : PragmaHandler("once"), SM(SM), tu(std::move(tu)) {}

  void HandlePragma(Preprocessor &PP, PragmaIntroducer Intro,
                    Token &FirstTok) override {
    std::string path = resolvePath(SM, Intro.Loc);
    if (path.empty()) return;
    unsigned line = SM.getSpellingLineNumber(Intro.Loc);

    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::ofstream ofs(g_outputPath, std::ios::app);
    if (!ofs) return;
    ofs << "{\"tu\":\"" << escapeJson(tu)
        << "\",\"file\":\"" << escapeJson(path)
        << "\",\"line\":" << line << "}\n";
  }

private:
  SourceManager &SM;
  std::string tu;
};

class PragmaFinderAction : public PreprocessOnlyAction {
protected:
  void ExecuteAction() override {
    auto &CI = getCompilerInstance();
    std::string tu = std::filesystem::absolute(getCurrentFile().str())
                         .lexically_normal()
                         .string();
    // The preprocessor owns the handler and releases it at end of TU.
    CI.getPreprocessor().AddPragmaHandler(
        new OncePragmaHandler(CI.getSourceManager(), tu));
    PreprocessOnlyAction::ExecuteAction();
  }
};

// Mirror macro_finder / tu_tracer flag configuration so headers resolve
// the same way across tools. Keep in sync with those files.
static void addCustomIncludePaths(ClangTool &Tool) {
  Tool.appendArgumentsAdjuster(
      [](const CommandLineArguments &Args,
         llvm::StringRef Filename) -> CommandLineArguments {
        CommandLineArguments NewArgs = Args;

        std::vector<std::string> CommonArgs = {
            "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
            "-w",
            "-Wno-incompatible-function-pointer-types",
            "-Wno-incompatible-pointer-types",
            "-fno-strict-aliasing",
            "-isystem/usr/lib/llvm-19/lib/clang/19/include",
        };

        bool isCxx = Filename.ends_with(".cxx") ||
                     Filename.ends_with(".cpp") ||
                     Filename.ends_with(".cc")  ||
                     Filename.ends_with(".C")   ||
                     Filename.ends_with(".hpp") ||
                     Filename.ends_with(".hxx");

        if (isCxx) {
          std::vector<std::string> CxxArgs = {
              "-isystem/usr/include/c++/11",
              "-isystem/usr/include/aarch64-linux-gnu/c++/11",
              "-isystem/usr/include/c++/11/backward",
          };
          CommonArgs.insert(CommonArgs.end(), CxxArgs.begin(), CxxArgs.end());
        } else {
          CommonArgs.push_back("-nostdinc++");
        }

        CommonArgs.push_back("-isystem/usr/include/aarch64-linux-gnu");
        CommonArgs.push_back("-isystem/usr/include");

        NewArgs.insert(NewArgs.begin() + 1, CommonArgs.begin(),
                       CommonArgs.end());
        return NewArgs;
      });
}

static llvm::cl::OptionCategory ToolCategory("pragma-finder options");

int main(int argc, const char **argv) {
  std::string dbPath;
  bool hasDBPath = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-p" && i + 1 < argc) {
      dbPath = argv[i + 1];
      hasDBPath = true;
    } else if (arg == "-o" && i + 1 < argc) {
      g_outputPath = argv[i + 1];
    }
  }

  if (g_outputPath.empty()) {
    llvm::errs() << "Error: -o <output_jsonl_path> is required\n";
    return 1;
  }

  // Truncate at start so reruns do not append to stale data.
  {
    std::ofstream ofs(g_outputPath, std::ios::trunc);
    if (!ofs) {
      llvm::errs() << "Error: cannot open output file: " << g_outputPath
                   << "\n";
      return 1;
    }
  }

  if (hasDBPath) {
    std::filesystem::path absDbPath =
        std::filesystem::absolute(dbPath).lexically_normal();

    std::string errMsg;
    auto DB = CompilationDatabase::loadFromDirectory(absDbPath.string(),
                                                     errMsg);
    if (!DB) {
      llvm::errs() << "Error loading compilation database from '" << dbPath
                   << "': " << errMsg << "\n";
      return 1;
    }

    std::vector<std::string> AllFiles = DB->getAllFiles();
    if (AllFiles.empty()) {
      llvm::errs() << "No files found in compilation database at '" << dbPath
                   << "'\n";
      return 1;
    }

    llvm::errs() << "Processing " << AllFiles.size()
                 << " files from compilation database\n";

    ClangTool Tool(*DB, AllFiles);
    addCustomIncludePaths(Tool);
    return Tool.run(newFrontendActionFactory<PragmaFinderAction>().get());
  }

  // Fallback: standard CommonOptionsParser path.
  auto ExpectedParser =
      CommonOptionsParser::create(argc, argv, ToolCategory);
  if (!ExpectedParser) {
    llvm::errs() << "Error: failed to parse command line\n";
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());
  addCustomIncludePaths(Tool);
  return Tool.run(newFrontendActionFactory<PragmaFinderAction>().get());
}