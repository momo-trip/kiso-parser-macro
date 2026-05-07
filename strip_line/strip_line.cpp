#include "clang/Lex/Lexer.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/FileSystem.h"
#include <fstream>
#include <vector>
#include <string>

using namespace clang;
using namespace llvm;

static cl::opt<std::string> InputFile(
    cl::Positional, cl::desc("<input file>"), cl::Required);

static cl::opt<bool> InPlace(
    "i", cl::desc("Edit file in place"), cl::init(false));

static cl::opt<std::string> OutputFile(
    "o", cl::desc("Output file (default: stdout)"), cl::init(""));

int main(int argc, const char **argv) {
  cl::ParseCommandLineOptions(argc, argv, "Strip #line directives from C source\n");

  // Read the input file
  auto BufOrErr = MemoryBuffer::getFile(InputFile);
  if (!BufOrErr) {
    errs() << "Error: cannot open " << InputFile << ": "
           << BufOrErr.getError().message() << "\n";
    return 1;
  }
  std::unique_ptr<MemoryBuffer> Buf = std::move(*BufOrErr);
  StringRef Src = Buf->getBuffer();

  // Set up a minimal SourceManager
  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts(new DiagnosticOptions());
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());
  DiagnosticsEngine Diags(DiagID, DiagOpts.get());

  IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS = llvm::vfs::getRealFileSystem();
  FileManager FileMgr({}, VFS);
  SourceManager SM(Diags, FileMgr);

  // Register the buffer with the SourceManager
  std::unique_ptr<MemoryBuffer> BufCopy =
      MemoryBuffer::getMemBufferCopy(Src, InputFile);
  FileID FID = SM.createFileID(std::move(BufCopy));
  SM.setMainFileID(FID);

  LangOptions LangOpts;
  LangOpts.C99 = 1;  // Enable C99 features

  // Create a Lexer in raw mode
  Lexer L(FID, SM.getBufferOrFake(FID), SM, LangOpts);
  L.SetCommentRetentionState(false);

  // Collect [start, end) byte offsets to remove
  std::vector<std::pair<size_t, size_t>> ranges;

  Token Tok;
  // When the inner loop reads one token past the directive (the first token
  // of the next line), we must not discard it. This flag tells the outer
  // loop to reuse the current Tok instead of lexing a new one.
  bool reuseTok = false;

  while (true) {
    if (!reuseTok) {
      L.LexFromRawLexer(Tok);
    }
    reuseTok = false;
    if (Tok.is(tok::eof)) break;

    // Look for '#' at the start of a line
    if (Tok.is(tok::hash) && Tok.isAtStartOfLine()) {
      size_t hashOffset = SM.getFileOffset(Tok.getLocation());

      // Peek the next token
      Token Next;
      L.LexFromRawLexer(Next);

      bool isLineDirective = false;
      if (Next.is(tok::raw_identifier) &&
          Next.getRawIdentifier() == "line") {
        // Standard form: #line 158 "file"
        isLineDirective = true;
      } else if (Next.is(tok::numeric_constant)) {
        // GCC short form: # 158 "file"
        isLineDirective = true;
      }

      if (isLineDirective) {
        // Track the byte offset just after the last token of this directive
        size_t directiveEnd = SM.getFileOffset(Next.getLocation()) + Next.getLength();

        while (true) {
            L.LexFromRawLexer(Tok);
            if (Tok.is(tok::eof)) break;
            if (Tok.isAtStartOfLine()) {
                // Tok belongs to the next line, not to this directive.
                // Hand it back to the outer loop so a possible '#' there
                // (e.g. consecutive #line directives) is not skipped.
                reuseTok = true;
                break;
            }
            directiveEnd = SM.getFileOffset(Tok.getLocation()) + Tok.getLength();
        }

        // Remove only the directive text itself.
        // The leading whitespace (if any) and the trailing newline are preserved,
        // so line numbers of all other lines stay the same.
        ranges.emplace_back(hashOffset, directiveEnd);
        continue;
      } else {
        // '#' was followed by something other than 'line' or a number
        // (e.g. #define, #include). If Next already sits at the start of
        // a new line, reuse it so we don't lose it.
        if (Next.isAtStartOfLine()) {
          Tok = Next;
          reuseTok = true;
        }
      }
    }
  }

  // Apply removals in reverse order
  std::string result = Src.str();
  for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
    result.erase(it->first, it->second - it->first);
  }

  // Write output
  if (InPlace) {
    std::ofstream ofs(InputFile.c_str());
    if (!ofs) {
      errs() << "Error: cannot write " << InputFile << "\n";
      return 1;
    }
    ofs << result;
  } else if (!OutputFile.empty()) {
    std::ofstream ofs(OutputFile.c_str());
    if (!ofs) {
      errs() << "Error: cannot write " << OutputFile << "\n";
      return 1;
    }
    ofs << result;
  } else {
    outs() << result;
  }

  errs() << "Removed " << ranges.size() << " #line directives from "
         << InputFile << "\n";
  return 0;
}