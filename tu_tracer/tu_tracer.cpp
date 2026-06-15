// tu_tracer.cpp
// Records the preprocessing trace (include order) of each translation unit.
// Output format: JSONL, one line per TU.
//   {"tu":"<src.c> | <dir> || <file> || <args> || <out>",
//    "edges":[{"from":"...","to":"...","order":0,"inclusion":"<parent>:<line>:<col>"}, ...]}
//
// The "tu" field is built the SAME way as macro_finder.cpp's
// buildTUIdFromCompileCommand(), prefixed with the absolute source path and
// " | ", so the two tools emit identical TU identifiers. ClangTool is run once
// per compile_commands.json entry (a synchronous Tool.run per entry), with
// g_currentTUId fixed for that entry, exactly mirroring macro_finder.

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
// and writes are serialized (each entry is run synchronously, but keep the
// lock to stay robust).
static std::string g_outputPath;
static std::mutex g_outputMutex;

// Set before each ClangTool.run(), mirroring macro_finder.cpp. The TU id is
// fixed for the duration of one compile_commands.json entry.
static std::string g_currentTUId;

// Compilation database directory. Used to rewrite virtual paths like
// <built-in> / <command line> into "<g_compileDir>/<name>", matching
// macro_finder.cpp's getAbsolutePath so both tools emit identical paths.
static std::string g_compileDir;

// Build a unique TU id from a CompileCommand entry by concatenating its four
// fields verbatim with " || ". IDENTICAL to macro_finder.cpp; keep in sync.
static std::string buildTUIdFromCompileCommand(const CompileCommand &cmd) {
  std::string argsJoined;
  for (size_t i = 0; i < cmd.CommandLine.size(); ++i) {
    if (i > 0) argsJoined += " ";
    argsJoined += cmd.CommandLine[i];
  }
  std::string id;
  id.append(cmd.Directory);
  id.append(" || ");
  id.append(cmd.Filename);
  id.append(" || ");
  id.append(argsJoined);
  id.append(" || ");
  id.append(cmd.Output);
  while (!id.empty() && id.back() == ' ') id.pop_back();
  return id;
}

// PPCallback that records every file entry during preprocessing.
// FileChanged(EnterFile) fires whenever the preprocessor enters a new file.
// Guard-skipped files do not trigger FileChanged; FileSkipped handles them.
class TUTraceCallback : public PPCallbacks {
public:
  TUTraceCallback(SourceManager &SM, std::string mainFile,
                  const std::string &tuId)
      : SM(SM), mainFile(std::move(mainFile)) {
    // TU id format matches macro_finder: "<abs source path> | <tu id>".
    TUPath = this->mainFile + " | " + tuId;
  }

  // Records the #include site (parent file : line : col) for the NEXT file
  // the preprocessor enters or skips. Consumed by FileChanged / FileSkipped.
  void InclusionDirective(SourceLocation HashLoc, const Token &,
                          StringRef, bool, CharSourceRange,
                          OptionalFileEntryRef File, StringRef, StringRef,
                          const Module *, bool,
                          SrcMgr::CharacteristicKind) override {
    PresumedLoc PLoc = SM.getPresumedLoc(HashLoc);
    if (PLoc.isInvalid()) {
      pendingInclusionLoc.clear();
      return;
    }
    std::string parent = resolveFilename(PLoc.getFilename());
    std::string s;
    llvm::raw_string_ostream os(s);
    os << parent << ":" << PLoc.getLine() << ":" << PLoc.getColumn();
    pendingInclusionLoc = os.str();
  }

  void FileChanged(SourceLocation Loc, FileChangeReason Reason,
                   SrcMgr::CharacteristicKind, FileID) override {
    if (Reason == EnterFile) {
      std::string path = resolvePath(Loc);
      if (path.empty()) return;
      // The #include site for this entry (empty for the main file / built-in).
      std::string incLoc = pendingInclusionLoc;
      pendingInclusionLoc.clear();
      if (!stack.empty()) {
        addEdge(stack.back(), path, incLoc);
      }

      // added
      events.push_back({
          "enter_file",
          path,
          stack.empty() ? "" : stack.back(),
          path,
          static_cast<int>(events.size()),
          incLoc
      });
      // ended

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

        // added
        events.push_back({
            "exit_file",
            leaving,
            leaving,
            "",
            static_cast<int>(events.size()),
            ""
        });
        // ended

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
  // re-emit them (with their recorded inclusion sites) from the first-entry
  // recording.
  void FileSkipped(const FileEntryRef &SkippedFile, const Token &,
                   SrcMgr::CharacteristicKind) override {
    if (stack.empty()) return;
    std::string included = resolveFileEntry(SkippedFile.getName().str());
    if (included.empty()) return;

    // The #include site recorded by InclusionDirective applies to this skip.
    std::string incLoc = pendingInclusionLoc;
    pendingInclusionLoc.clear();

    // added
    events.push_back({
        "skip_file",
        included,
        stack.back(),
        included,
        static_cast<int>(events.size()),
        incLoc
    });
    // ended

    addEdge(stack.back(), included, incLoc);
    replaySubtree(included);
  }

  void EndOfMainFile() override {
    // added
    while (!stack.empty()) {
      std::string leaving = stack.back();

      events.push_back({
          "exit_file",
          leaving,
          leaving,
          "",
          static_cast<int>(events.size()),
          ""
      });

      stack.pop_back();
      if (!currentlyRecording.empty() &&
          currentlyRecording.back() == leaving) {
        currentlyRecording.pop_back();
      }
    }
    // ended

    std::lock_guard<std::mutex> lock(g_outputMutex);
    std::ofstream ofs(g_outputPath, std::ios::app);
    if (!ofs) return;

    ofs << "{\"tu\":\"" << escape(TUPath) << "\",\"edges\":[";
    for (size_t i = 0; i < edges.size(); ++i) {
      if (i > 0) ofs << ",";
      ofs << "{\"from\":\"" << escape(edges[i].from)
          << "\",\"to\":\"" << escape(edges[i].to)
          << "\",\"order\":" << edges[i].order
          << ",\"inclusion\":\"" << escape(edges[i].inclusion) << "\"}";
    }
    // added
    ofs << "],\"events\":[";

    for (size_t i = 0; i < events.size(); ++i) {
      if (i > 0) ofs << ",";

      ofs << "{\"kind\":\"" << escape(events[i].kind)
          << "\",\"file\":\"" << escape(events[i].file)
          << "\",\"from\":\"" << escape(events[i].from)
          << "\",\"to\":\"" << escape(events[i].to)
          << "\",\"order\":" << events[i].order
          << ",\"inclusion\":\"" << escape(events[i].inclusion)
          << "\"}";
    }
    // ended

    ofs << "]}\n";
  }

private:
  struct Edge {
    std::string from;
    std::string to;
    int order;
    std::string inclusion;  // "<parent file>:<line>:<col>" of the #include
  };

  struct ChildEdge {
    std::string child;
    std::string inclusion;
  };

  struct Event {
    std::string kind;
    std::string file;
    std::string from;
    std::string to;
    int order;
    std::string inclusion;
  };

  SourceManager &SM;
  std::string mainFile;
  std::string TUPath;
  std::vector<std::string> stack;
  std::vector<Edge> edges;
  std::vector<Event> events;

  // Set by InclusionDirective, consumed by the next FileChanged/FileSkipped.
  std::string pendingInclusionLoc;

  // First-entry child recording: for each file, the list of immediate
  // children (and their inclusion sites) seen when that file was first
  // entered in this TU.
  std::unordered_map<std::string, std::vector<ChildEdge>> firstChildren;
  // Files currently being recorded (on the stack, not yet exited).
  std::vector<std::string> currentlyRecording;

  void addEdge(const std::string &from, const std::string &to,
               const std::string &inclusion) {
    edges.push_back({from, to, static_cast<int>(edges.size()), inclusion});
    if (!currentlyRecording.empty() &&
        currentlyRecording.back() == from) {
      firstChildren[from].push_back({to, inclusion});
    }
  }

  // Re-emit the subtree of `root` as if we had entered it, preserving the
  // recorded inclusion sites. Used when a file is skipped due to an include
  // guard: the preprocessor will not visit its children, but those edges
  // still exist logically in this include chain instance.
  void replaySubtree(const std::string &root) {
    std::unordered_set<std::string> visited;
    replaySubtreeImpl(root, visited);
  }

  void replaySubtreeImpl(const std::string &root,
                         std::unordered_set<std::string> &visited) {
    if (!visited.insert(root).second) return;  // already visited
    auto it = firstChildren.find(root);
    if (it == firstChildren.end()) return;
    for (const ChildEdge &ce : it->second) {
      edges.push_back(
          {root, ce.child, static_cast<int>(edges.size()), ce.inclusion});
      replaySubtreeImpl(ce.child, visited);
    }
  }

  std::string resolvePath(SourceLocation Loc) {
    FileID FID = SM.getFileID(Loc);
    if (auto FE = SM.getFileEntryRefForID(FID)) {
      return resolveFileEntry(FE->getName().str());
    }
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    return PLoc.isValid() ? resolveFilename(PLoc.getFilename()) : "";
  }

  static std::string resolveFileEntry(const std::string &name) {
    // Match macro_finder: return virtual paths like <built-in>, <command line>
    // as "<g_compileDir>/<name>" (or as-is if g_compileDir is empty).
    if (!name.empty() && name[0] == '<') {
      if (!g_compileDir.empty()) return g_compileDir + "/" + name;
      return name;
    }
    std::filesystem::path p(name);
    std::error_code EC;
    auto c = std::filesystem::canonical(p, EC);
    if (!EC) return c.string();
    if (!p.is_absolute()) p = std::filesystem::absolute(p);
    return p.lexically_normal().string();
  }

  static std::string resolveFilename(const std::string &name) {
    return resolveFileEntry(name);
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

class TUTraceAction : public PreprocessOnlyAction {
protected:
  void ExecuteAction() override {
    auto &CI = getCompilerInstance();
    std::string mainFile = std::filesystem::absolute(
                               getCurrentFile().str())
                               .lexically_normal()
                               .string();
    CI.getPreprocessor().addPPCallbacks(
        std::make_unique<TUTraceCallback>(CI.getSourceManager(), mainFile,
                                          g_currentTUId));
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
    g_compileDir = absDbPath.string();

    std::string errMsg;
    auto DB = CompilationDatabase::loadFromDirectory(absDbPath.string(),
                                                     errMsg);
    if (!DB) {
      llvm::errs() << "Error loading compilation database from '" << dbPath
                   << "': " << errMsg << "\n";
      return 1;
    }

    // Run ClangTool once per compile command entry, mirroring macro_finder.
    // The unique tu value is buildTUIdFromCompileCommand(cmd), fixed per
    // iteration. Tool.run is synchronous, so the trace inside always belongs
    // to this cmd. This also avoids the last-write-wins collision that a
    // per-file map would suffer when the same source appears in multiple
    // entries with different flags.
    auto AllCommands = DB->getAllCompileCommands();
    if (AllCommands.empty()) {
      llvm::errs() << "No compile commands found in compilation database at '"
                   << dbPath << "'\n";
      return 1;
    }

    llvm::errs() << "Processing " << AllCommands.size()
                 << " compile commands from compilation database\n";

    int finalRet = 0;
    for (const auto &cmd : AllCommands) {
      g_currentTUId = buildTUIdFromCompileCommand(cmd);

      // Build a single-entry DB. Drop argv[0] (compiler path) and the input
      // source file; FixedCompilationDatabase appends the source file (passed
      // via ClangTool's source list) itself, so leaving it in would duplicate.
      std::vector<std::string> args;
      for (size_t i = 1; i < cmd.CommandLine.size(); ++i) {
        const std::string &a = cmd.CommandLine[i];
        if (a == "-o" && i + 1 < cmd.CommandLine.size()) {
          args.push_back(a);
          args.push_back(cmd.CommandLine[++i]);
          continue;
        }
        if (!a.empty() && a[0] != '-') {
          std::filesystem::path ap(a), fp(cmd.Filename);
          auto ext = ap.extension();
          bool isSource = ext == ".c"  || ext == ".cc" || ext == ".cpp" ||
                          ext == ".cxx" || ext == ".C";
          if (isSource &&
              (ap.lexically_normal() == fp.lexically_normal() ||
               ap.filename() == fp.filename())) {
            continue;
          }
        }
        args.push_back(a);
      }

      FixedCompilationDatabase singleDB(cmd.Directory, args);
      ClangTool Tool(singleDB, {cmd.Filename});
      addCustomIncludePaths(Tool);

      int ret = Tool.run(newFrontendActionFactory<TUTraceAction>().get());
      if (ret != 0) finalRet = ret;
    }

    return finalRet;
  }

  // Fallback path: use CommonOptionsParser. No per-entry tu id available here,
  // so g_currentTUId stays empty (TUPath degrades to "<file> | ").
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