// tu_tracer.cpp
// Records the preprocessing trace (include order) of each translation unit.
// Output format: JSONL, one line per TU.
//   {"tu":"/abs/path/to/main.c","trace":["/abs/path/to/main.c","/abs/path/to/header.h",...]}

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

using namespace clang;
using namespace clang::tooling;

// Global output path and mutex. The output file is shared across all TUs,
// and ClangTool may process TUs in parallel, so writes must be serialized.
static std::string g_outputPath;
static std::mutex g_outputMutex;

// PPCallback that records every file entry during preprocessing.
// FileChanged(EnterFile) fires whenever the preprocessor enters a new file.
// Skipped files (e.g. due to include guards) do not trigger FileChanged,
// which is the desired behavior: the trace reflects what the preprocessor
// actually read.

// class TUTraceCallback : public PPCallbacks {
// public:
//   TUTraceCallback(SourceManager &SM, std::string mainFile)
//       : SM(SM), mainFile(std::move(mainFile)) {}

//   void FileChanged(SourceLocation Loc, FileChangeReason Reason,
//                    SrcMgr::CharacteristicKind, FileID) override {
//     if (Reason == EnterFile) {
//       std::string path = resolvePath(Loc);
//       if (path.empty()) return;
//       if (!stack.empty()) {
//         edges.push_back({stack.back(), path, static_cast<int>(edges.size())});
//       }
//       stack.push_back(path);
//     } else if (Reason == ExitFile) {
//       if (!stack.empty()) stack.pop_back();
//     }
//   }


class TUTraceCallback : public PPCallbacks {
public:
  TUTraceCallback(SourceManager &SM, std::string mainFile)
      : SM(SM), mainFile(std::move(mainFile)) {}

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID) override {
    if (Reason == EnterFile) {
      std::string path = resolvePath(Loc);
      if (path.empty()) return;
      if (!stack.empty()) {
        addEdge(stack.back(), path);
      }
      stack.push_back(path);
      // Mark that we are inside `path` for the first time in this TU;
      // children will be appended to firstChildren[path].
      if (firstChildren.find(path) == firstChildren.end()) {
        firstChildren[path] = {};
        currentlyRecording.push_back(path);
      }
    } else if (Reason == ExitFile) {
      if (!stack.empty()) {
        std::string leaving = stack.back();
        stack.pop_back();
        if (!currentlyRecording.empty() &&
            currentlyRecording.back() == leaving) {
          currentlyRecording.pop_back();
        }
      }
    }
  }

  // Fires when an #include is skipped due to include guards or #pragma once.
  // The skipped file's children are not visited by the preprocessor, so we
  // re-emit them from the first-entry recording.
  void FileSkipped(const FileEntryRef &SkippedFile, const Token &,
                   SrcMgr::CharacteristicKind) override {
    if (stack.empty()) return;
    std::filesystem::path p(SkippedFile.getName().str());
    std::error_code EC;
    auto c = std::filesystem::canonical(p, EC);
    std::string included = EC ? p.lexically_normal().string() : c.string();
    if (included.empty()) return;

    addEdge(stack.back(), included);
    replaySubtree(included);
  }

  void EndOfMainFile() override {
    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::ofstream ofs(g_outputPath, std::ios::app);
    if (!ofs) return;

    ofs << "{\"tu\":\"" << escape(mainFile) << "\",\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
      if (i > 0) ofs << ",";
      ofs << "{\"from\":\"" << escape(edges[i].from)
          << "\",\"to\":\"" << escape(edges[i].to)
          << "\",\"order\":" << edges[i].order << "}";
    }
    ofs << "]}\n";
  }

private:
  struct Edge {
    std::string from;
    std::string to;
    int order;
  };

  SourceManager &SM;
  std::string mainFile;
  std::vector<std::string> stack;
  std::vector<Edge> edges;

  // First-entry child recording: for each file, the list of immediate
  // children seen when that file was first entered in this TU.
  std::unordered_map<std::string, std::vector<std::string>> firstChildren;
  // Files currently being recorded (i.e., on the stack and not yet exited).
  std::vector<std::string> currentlyRecording;

  void addEdge(const std::string &from, const std::string &to) {
    edges.push_back({from, to, static_cast<int>(edges.size())});
    if (!currentlyRecording.empty() &&
        currentlyRecording.back() == from) {
      firstChildren[from].push_back(to);
    }
  }

  // Re-emit the subtree of `root` as if we had entered it. Used when a
  // file is skipped due to an include guard: the preprocessor will not
  // visit its children, but logically those edges still exist in this
  // include chain instance.
  void replaySubtree(const std::string &root) {
    std::unordered_set<std::string> visited;
    replaySubtreeImpl(root, visited);
  }

  void replaySubtreeImpl(const std::string &root,
                        std::unordered_set<std::string> &visited) {
    if (!visited.insert(root).second) return;  // already visited
    auto it = firstChildren.find(root);
    if (it == firstChildren.end()) return;
    for (const std::string &child : it->second) {
      edges.push_back({root, child, static_cast<int>(edges.size())});
      replaySubtreeImpl(child, visited);
    }
  }

  std::string resolvePath(SourceLocation Loc) {
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

  static std::string escape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      if (c == '\\' || c == '"') out.push_back('\\');
      out.push_back(c);
    }
    return out;
  }
};

// class TUTraceCallback : public PPCallbacks {
// public:
//   TUTraceCallback(SourceManager &SM, std::string mainFile)
//       : SM(SM), mainFile(std::move(mainFile)) {}

//   void FileChanged(SourceLocation Loc, FileChangeReason Reason,
//                    SrcMgr::CharacteristicKind, FileID) override {
//     if (Reason == EnterFile) {
//       std::string path = resolvePath(Loc);
//       if (path.empty()) return;
//       if (!stack.empty()) {
//         edges.push_back({stack.back(), path, static_cast<int>(edges.size())});
//       }
//       stack.push_back(path);
//     } else if (Reason == ExitFile) {
//       if (!stack.empty()) stack.pop_back();
//     }
//   }

//   // Fires for every #include directive, including those whose target file
//   // is skipped due to include guards (#pragma once or #ifndef-guarded headers).
//   // FileChanged(EnterFile) only fires when the file is actually entered, so
//   // guard-skipped includes would otherwise be invisible to the trace.

//   // void InclusionDirective(SourceLocation HashLoc, const Token &,
//   //                         StringRef, bool, CharSourceRange,
//   //                         OptionalFileEntryRef File,
//   //                         StringRef, StringRef, const Module *,
//   //                         SrcMgr::CharacteristicKind) override {
//   //   if (!File) return;
//   //   if (stack.empty()) return;

//   //   std::filesystem::path p(File->getName().str());
//   //   std::error_code EC;
//   //   auto c = std::filesystem::canonical(p, EC);
//   //   std::string included = EC ? p.lexically_normal().string() : c.string();
//   //   if (included.empty()) return;

//   //   // Skip if FileChanged will (or did) report the same edge; we only want
//   //   // to add edges that FileChanged misses (guard-skipped includes).
//   //   pendingIncludes.push_back({stack.back(), included});
//   // }

//   void FileSkipped(const FileEntryRef &SkippedFile, const Token &,
//                    SrcMgr::CharacteristicKind) override {
//     if (stack.empty()) return;
//     std::filesystem::path p(SkippedFile.getName().str());
//     std::error_code EC;
//     auto c = std::filesystem::canonical(p, EC);
//     std::string included = EC ? p.lexically_normal().string() : c.string();
//     if (included.empty()) return;
//     edges.push_back({stack.back(), included, static_cast<int>(edges.size())});
//   }

//   void EndOfMainFile() override {
//     std::lock_guard<std::mutex> lock(g_outputMutex);
//     std::ofstream ofs(g_outputPath, std::ios::app);
//     if (!ofs) return;

//     ofs << "{\"tu\":\"" << escape(mainFile) << "\",\"edges\":[";
//     for (size_t i = 0; i < edges.size(); ++i) {
//       if (i > 0) ofs << ",";
//       ofs << "{\"from\":\"" << escape(edges[i].from)
//           << "\",\"to\":\"" << escape(edges[i].to)
//           << "\",\"order\":" << edges[i].order << "}";
//     }
//     ofs << "]}\n";
//   }

// private:
//   struct Edge {
//     std::string from;
//     std::string to;
//     int order;
//   };

//   SourceManager &SM;
//   std::string mainFile;
//   std::vector<std::string> stack;
//   std::vector<Edge> edges;

//   std::string resolvePath(SourceLocation Loc) {
//     FileID FID = SM.getFileID(Loc);
//     if (auto FE = SM.getFileEntryRefForID(FID)) {
//       std::filesystem::path p(FE->getName().str());
//       std::error_code EC;
//       auto c = std::filesystem::canonical(p, EC);
//       if (!EC) return c.string();
//       if (!p.is_absolute()) p = std::filesystem::absolute(p);
//       return p.lexically_normal().string();
//     }
//     PresumedLoc PLoc = SM.getPresumedLoc(Loc);
//     return PLoc.isValid() ? std::string(PLoc.getFilename()) : "";
//   }

//   static std::string escape(const std::string &s) {
//     std::string out;
//     out.reserve(s.size());
//     for (char c : s) {
//       if (c == '\\' || c == '"') out.push_back('\\');
//       out.push_back(c);
//     }
//     return out;
//   }
// };

class TUTraceAction : public PreprocessOnlyAction {
protected:
  void ExecuteAction() override {
    auto &CI = getCompilerInstance();
    std::string mainFile = std::filesystem::absolute(
                               getCurrentFile().str())
                               .lexically_normal()
                               .string();
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<TUTraceCallback>(CI.getSourceManager(), mainFile));
    PreprocessOnlyAction::ExecuteAction();
  }
};

// Inject the same include path and flag configuration as macro_finder.
// Keep this in sync with macro_finder.cpp's addCustomIncludePaths.
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
          // For C files, block C++ system includes entirely.
          CommonArgs.push_back("-nostdinc++");
        }

        CommonArgs.push_back("-isystem/usr/include/aarch64-linux-gnu");
        CommonArgs.push_back("-isystem/usr/include");

        NewArgs.insert(NewArgs.begin() + 1, CommonArgs.begin(),
                       CommonArgs.end());
        return NewArgs;
      });
}

static llvm::cl::OptionCategory ToolCategory("tu-tracer options");

int main(int argc, const char **argv) {
  // Manually parse -p (compilation database dir) and -o (output path)
  // to mirror the macro_finder invocation convention.
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

  // Truncate the output file at start so reruns do not append to old data.
  {
    std::ofstream ofs(g_outputPath, std::ios::trunc);
    if (!ofs) {
      llvm::errs() << "Error: cannot open output file: " << g_outputPath
                   << "\n";
      return 1;
    }
  }

  if (hasDBPath) {
    std::filesystem::path absDbPath = std::filesystem::absolute(dbPath)
                                          .lexically_normal();

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
    return Tool.run(newFrontendActionFactory<TUTraceAction>().get());
  }

  // Fallback path: use CommonOptionsParser.
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
  return Tool.run(newFrontendActionFactory<TUTraceAction>().get());
}
