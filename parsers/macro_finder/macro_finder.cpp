// macro_finder.cpp
// grep -E "IdentifierInfo \*Ident_" /usr/lib/llvm-19/include/clang/Lex/Preprocessor.h
#include <filesystem> 
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/MacroInfo.h"
#include "clang/Lex/Lexer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <map>
#include <unordered_map>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

static std::string g_compileDir;



class MacroCallbacks : public PPCallbacks {
public:
  // Struct to hold information about #if-family directives
  struct IfDirectiveInfo {
    SourceLocation loc;
    std::string type;  // "IF", "IFDEF", "IFNDEF"
    std::string info;  // Macro name or condition expression
    unsigned end_line;
    unsigned end_column;
  };

  // Hold all directives within an #if block
  struct IfBlockInfo {
    IfDirectiveInfo ifDirective;  // Original #if/#ifdef/#ifndef
    std::vector<IfDirectiveInfo> elifDirectives;  // List of #elif
    IfDirectiveInfo elseDirective;  // #else (if present)
    bool hasElse = false;
  };

  explicit MacroCallbacks(SourceManager &SM, Preprocessor &PP)
      : SM(SM), PP(PP) {}

  // Detect macro definitions
  void MacroDefined(const Token &MacroNameTok,
                  const MacroDirective *MD) override {
    SourceLocation Loc = MacroNameTok.getLocation();
    std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
    
    SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
      MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
    
    const MacroInfo *MI = MD->getMacroInfo();
    
    // Get the macro definition body directly from the source text.
    // Using Lexer::getSourceText preserves the original spelling, including
    // line continuations, escaped characters in string literals, and _Pragma.
    std::string MacroBody = getMacroBodyFromSource(MI);


    // // ★ Get the macro definition body
    // std::string MacroBody;
    // if (MI && MI->getNumTokens() > 0) {
    //   // Reconstruct definition body from token sequence
    //   for (unsigned i = 0; i < MI->getNumTokens(); ++i) {
    //     const Token &Tok = MI->getReplacementToken(i);
        
    //     // Handle spaces between tokens
    //     if (i > 0 && Tok.hasLeadingSpace()) {
    //       MacroBody += " ";
    //     }
        
    //     // Get the text of the token
    //     if (Tok.isLiteral()) {
    //       MacroBody += StringRef(Tok.getLiteralData(), Tok.getLength()).str();
    //     } else if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
    //       MacroBody += Tok.getIdentifierInfo()->getName().str();
    //     } else {
    //       // Other tokens (operators, etc.)
    //       MacroBody += PP.getSpelling(Tok);
    //     }
    //   }
    // }
    
    if (MI && MI->isFunctionLike()) {
      std::string details;
      
      // Get signature directly from source text
      bool Invalid = false;
      const char *Start = SM.getCharacterData(MI->getDefinitionLoc(), &Invalid);
      if (!Invalid) {
          const char *Ptr = Start;
          while (*Ptr && *Ptr != '(' && *Ptr != '\n') Ptr++;
          if (*Ptr == '(') {
              int Depth = 1;
              Ptr++;
              while (*Ptr && Depth > 0) {
                  if (*Ptr == '(') Depth++;
                  else if (*Ptr == ')') Depth--;
                  Ptr++;
              }
              details = std::string(Start, Ptr - Start);
          } else {
              details = MacroName + "()";
          }
      } else {
          details = MacroName + "()";
      }

    // if (MI && MI->isFunctionLike()) {
    //   std::string details = MacroName + "(";
      
    //   for (unsigned i = 0; i < MI->getNumParams(); ++i) {
    //     if (i > 0) details += ", ";
    //     details += MI->params()[i]->getName().str();
    //   }
      
    //   if (MI->isVariadic()) {
    //     if (MI->getNumParams() > 0) details += ", ";
    //     details += "...";
    //   }
      
    //   details += ")";
      
      // Also output the definition body
      if (!MacroBody.empty()) {
        details += " -> " + MacroBody;
      }
      
      printLocationWithEnd("DEFINED_FUNC", Loc, MacroEnd, details);
    } else {
      // Also output the definition body for object-like macros
      std::string info = MacroName;
      if (!MacroBody.empty()) {
        info += " -> " + MacroBody;
      }
      printLocationWithEnd("DEFINED", Loc, MacroEnd, info);
    }
  }

  // Detect macro definitions (within skipped blocks)
  void SkippedMacroDefined(const Token &MacroNameTok) override {
    SourceLocation Loc = MacroNameTok.getLocation();
    if (MacroNameTok.getIdentifierInfo()) {  // null check
      // Added null check
      SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
        MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
      
      printLocationWithEnd("DEFINED (skipped)", Loc, MacroEnd, 
                          MacroNameTok.getIdentifierInfo()->getName());
    }
  }

  void MacroUndefined(const Token &MacroNameTok,
    const MacroDefinition &MD,
    const MacroDirective *Undef) override {
    SourceLocation Loc = MacroNameTok.getLocation();
    
    // Get the end position of the macro name
    SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
      MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
    
    printLocationWithEnd("UNDEFINED", Loc, MacroEnd, 
                        MacroNameTok.getIdentifierInfo()->getName());
  }

  void SkippedMacroUndefined(const Token &MacroNameTok) override {
    SourceLocation Loc = MacroNameTok.getLocation();
    if (MacroNameTok.getIdentifierInfo()) {
      // Get the end position of the macro name
      SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
        MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
      
      printLocationWithEnd("UNDEFINED (skipped)", Loc, MacroEnd, 
                          MacroNameTok.getIdentifierInfo()->getName());
    }
  }

  // Detect #ifdef
  void Ifdef(SourceLocation Loc, const Token &MacroNameTok,
            const MacroDefinition &MD) override {
    std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
    std::string DefLoc = getDefinitionLocation(MD);
    
    // Get the end position of the macro names
    SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
      MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
    
    // Get MacroInfo and check if it's a function-like macro
    const MacroInfo *MI = MD.getMacroInfo();
    //std::string MacroType = "IFDEF";

    // added
    bool evaluated = MD ? true : false;
    std::string MacroType = evaluated ? "IFDEF_TRUE" : "IFDEF_FALSE";
    //std::string MacroType = "IFDEF_TRUE";  // Always evaluated
    // ended

    if (MI && MI->isFunctionLike()) {
      MacroType = evaluated ? "IFDEF_FUNC_TRUE" : "IFDEF_FUNC_FALSE";
      
      // Get parameter information from source text
      //MacroName += getSignatureFromSource(MI);
    }


    printLocationWithDefAndEnd(MacroType, Loc, MacroEnd, MacroName, DefLoc);
    
    // Record the correspondence
    PresumedLoc EndPLoc = SM.getPresumedLoc(MacroEnd);
    //ifStack.push_back({Loc, MacroType, MacroName, EndPLoc.getLine(), EndPLoc.getColumn()});
    IfBlockInfo block;
    block.ifDirective = {Loc, MacroType, MacroName, EndPLoc.getLine(), EndPLoc.getColumn()};
    ifBlockStack.push_back(block);
  }

  // void SkippedIfdef(SourceLocation Loc, const Token &MacroNameTok) override {
  //   if (MacroNameTok.getIdentifierInfo()) {
  //     std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
  //     printLocationWithDef("IFDEF (skipped)", Loc, MacroName, "unknown");
  //   }
  // }

  void SkippedIfdef(SourceLocation Loc, const Token &MacroNameTok) override {
    if (MacroNameTok.getIdentifierInfo()) {
      std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
      
      // Get the end position of the macro name
      SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
        MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
      
      // Get MacroDefinition ourselves
      IdentifierInfo *II = MacroNameTok.getIdentifierInfo();
      MacroDefinition MD = PP.getMacroDefinition(II);
      
      std::string MacroType = "IFDEF (skipped)";
      std::string DefLoc = "unknown";
      
      if (MD) {
        DefLoc = getDefinitionLocation(MD);
        
        const MacroInfo *MI = MD.getMacroInfo();
        // if (MI && MI->isFunctionLike()) {
        //   MacroType = "IFDEF_FUNC (skipped)";
          
        //   // Add parameter information
        //   MacroName += "(";
        //   for (unsigned i = 0; i < MI->getNumParams(); ++i) {
        //     if (i > 0) MacroName += ", ";
        //     MacroName += MI->params()[i]->getName().str();
        //   }
        //   if (MI->isVariadic()) {
        //     if (MI->getNumParams() > 0) MacroName += ", ";
        //     MacroName += "...";
        //   }
        //   MacroName += ")";
        // }
        if (MI && MI->isFunctionLike()) {
          MacroType = "IFDEF_FUNC (skipped)";
          
          // Get parameter information from source text
          //MacroName += getSignatureFromSource(MI);
        }
        
      }
      
      printLocationWithDefAndEnd(MacroType, Loc, MacroEnd, MacroName, DefLoc);
      
      // ★ Record the correspondence
      PresumedLoc EndPLoc = SM.getPresumedLoc(MacroEnd);
      //ifStack.push_back({Loc, MacroType, MacroName, EndPLoc.getLine(), EndPLoc.getColumn()});
      IfBlockInfo block;
      block.ifDirective = {Loc, MacroType, MacroName, EndPLoc.getLine(), EndPLoc.getColumn()};
      ifBlockStack.push_back(block);

    }
  }

  // Detect #ifnded
  void Ifndef(SourceLocation Loc, const Token &MacroNameTok,
              const MacroDefinition &MD) override {
    std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
    std::string DefLoc = getDefinitionLocation(MD);
    
    // Get the end position of the macro name
    SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
      MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
    
    // added
    bool evaluated = MD ? false : true;
    std::string MacroType = evaluated ? "IFNDEF_TRUE" : "IFNDEF_FALSE";
    //std::string MacroType = "IFNDEF_TRUE";  // Always evaluated
    
    //printLocationWithDefAndEnd("IFNDEF", Loc, MacroEnd, MacroName, DefLoc);
    printLocationWithDefAndEnd(MacroType, Loc, MacroEnd, MacroName, DefLoc);
    // ended

    // ★ Record the correspondence
    PresumedLoc EndPLoc = SM.getPresumedLoc(MacroEnd);
    // added
    //ifStack.push_back({Loc, MacroType, MacroName, EndPLoc.getLine(), EndPLoc.getColumn()});
    //ifStack.push_back({Loc, "IFNDEF", MacroName, EndPLoc.getLine(), EndPLoc.getColumn()});
    IfBlockInfo block;
    block.ifDirective = {Loc, MacroType, MacroName, EndPLoc.getLine(), EndPLoc.getColumn()};
    ifBlockStack.push_back(block);
    // ended
  }
  
  void SkippedIfndef(SourceLocation Loc, const Token &MacroNameTok) override {
    if (MacroNameTok.getIdentifierInfo()) {
      std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
      
      SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
        MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
      
      printLocationWithDefAndEnd("IFNDEF (skipped)", Loc, MacroEnd, MacroName, "unknown");
      
      // Added: Push to ifStack
      PresumedLoc EndPLoc = SM.getPresumedLoc(MacroEnd);
      //ifStack.push_back({Loc, "IFNDEF (skipped)", MacroName, EndPLoc.getLine(), EndPLoc.getColumn()});
      IfBlockInfo block;
      block.ifDirective = {Loc, "IFNDEF (skipped)", MacroName, EndPLoc.getLine(), EndPLoc.getColumn()};
      ifBlockStack.push_back(block);

    }
  }

  void If(SourceLocation Loc, SourceRange ConditionRange,
    ConditionValueKind ConditionValue,
    ArrayRef<Token> UnexpandedTokens) override {
    
    // Get the end position of the condition expression
    SourceLocation ConditionEnd = Lexer::getLocForEndOfToken(
      ConditionRange.getEnd(), 0, SM, PP.getLangOpts());
    
    std::set<std::string> Macros;
    for (const Token &Tok : UnexpandedTokens) {
      if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
        StringRef Name;
        if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
          Name = Tok.getIdentifierInfo()->getName();
        } else if (Tok.is(tok::raw_identifier)) {
          Name = Tok.getRawIdentifier();
        } else {
          continue;
        }
        
        // if (Name != "defined") {
        //   Macros.insert(Name.str());
        // }
        if (!isPreprocessorOperator(Name)) { 
          Macros.insert(Name.str());
        }
      }
    }

    // added
    std::string TypeStr = (ConditionValue == CVK_True) ? "IF_TRUE" : "IF_FALSE";
    // std::string TypeStr;
    // if (ConditionValue == CVK_NotEvaluated) {
    //   TypeStr = "IF_FALSE";  // Was not evaluated
    // } else {
    //   TypeStr = "IF_TRUE";   // Was evaluated (whether CVK_True or CVK_False)
    // }

    // ended

    // std::string CondText = getConditionText(ConditionRange);
    std::string CondText = getConditionFromTokens(UnexpandedTokens);

    
    // added
    //printLocationWithMacrosAndEnd("IF", Loc, ConditionEnd, CondText, Macros);
    printLocationWithMacrosAndEnd(TypeStr, Loc, ConditionEnd, CondText, Macros);
    // ended

    // ★ Record the correspondence
    PresumedLoc EndPLoc = SM.getPresumedLoc(ConditionEnd);

    // added
    //ifStack.push_back({Loc, "IF", CondText, EndPLoc.getLine(), EndPLoc.getColumn()});
    //ifStack.push_back({Loc, TypeStr, CondText, EndPLoc.getLine(), EndPLoc.getColumn()});
    IfBlockInfo block;
    block.ifDirective = {Loc, TypeStr, CondText, EndPLoc.getLine(), EndPLoc.getColumn()};
    ifBlockStack.push_back(block);
    // ended

  }

  void SkippedIf(SourceLocation Loc, SourceRange ConditionRange,
               ArrayRef<Token> UnexpandedTokens) override {
    // Get the end position of the condition expression
    SourceLocation ConditionEnd = Lexer::getLocForEndOfToken(
      ConditionRange.getEnd(), 0, SM, PP.getLangOpts());
    
    std::set<std::string> Macros;
    for (const Token &Tok : UnexpandedTokens) {
      if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
        StringRef Name;
        if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
          Name = Tok.getIdentifierInfo()->getName();
        } else if (Tok.is(tok::raw_identifier)) {
          Name = Tok.getRawIdentifier();
        } else {
          continue;
        }
        
        // if (Name != "defined") {
        //   Macros.insert(Name.str());
        // }
        if (!isPreprocessorOperator(Name)) { 
          Macros.insert(Name.str());
        }
      }
    }
    
    // std::string CondText = getConditionText(ConditionRange);
    std::string CondText = getConditionFromTokens(UnexpandedTokens);

    printLocationWithMacrosAndEnd("IF (skipped)", Loc, ConditionEnd, CondText, Macros);
    
    PresumedLoc EndPLoc = SM.getPresumedLoc(ConditionEnd);
    // ifStack.push_back({Loc, "IF (skipped)", CondText, EndPLoc.getLine(), EndPLoc.getColumn()});
    IfBlockInfo block;
    block.ifDirective = {Loc, "IF (skipped)", CondText, EndPLoc.getLine(), EndPLoc.getColumn()};
    ifBlockStack.push_back(block);

  }

  void SkippedElif(SourceLocation Loc, SourceRange ConditionRange, 
                  SourceLocation IfLoc, ArrayRef<Token> UnexpandedTokens) override {
    // Get the end position of the condition expression
    SourceLocation ConditionEnd = Lexer::getLocForEndOfToken(
      ConditionRange.getEnd(), 0, SM, PP.getLangOpts());
    
    std::set<std::string> Macros;
    for (const Token &Tok : UnexpandedTokens) {
      if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
        StringRef Name;
        if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
          Name = Tok.getIdentifierInfo()->getName();
        } else if (Tok.is(tok::raw_identifier)) {
          Name = Tok.getRawIdentifier();
        } else {
          continue;
        }
        
        // if (Name != "defined") {
        //   Macros.insert(Name.str());
        // }
        if (!isPreprocessorOperator(Name)) { 
          Macros.insert(Name.str());
        }
      }
    }
    
    // std::string CondText = getConditionText(ConditionRange);
    std::string CondText = getConditionFromTokens(UnexpandedTokens);

    printLocationWithMacrosAndEnd("ELIF (skipped)", Loc, ConditionEnd, CondText, Macros);

    // added
    if (!ifBlockStack.empty()) {
      PresumedLoc EndPLoc = SM.getPresumedLoc(ConditionEnd);
      ifBlockStack.back().elifDirectives.push_back(
          {Loc, "ELIF (skipped)", CondText, EndPLoc.getLine(), EndPLoc.getColumn()}
      );
    }
    // ended
  }

  // // Detect #elif (using UnexpandedTokens)
  // void Elif(SourceLocation Loc, SourceRange ConditionRange,
  //     ConditionValueKind ConditionValue, SourceLocation IfLoc,
  //     ArrayRef<Token> UnexpandedTokens) override { 

  //   // Extract macros directly from tokens
  //   std::set<std::string> Macros;
  //   for (const Token &Tok : UnexpandedTokens) {
  //     if (Tok.is(tok::identifier)) {
  //       StringRef Name = Tok.getRawIdentifier();
  //       if (Name != "defined") {
  //         Macros.insert(Name.str());
  //       }
  //     }
  //   }
  //   std::string CondText = getConditionText(ConditionRange);
  //   printLocationWithMacros("ELIF", Loc, CondText, Macros);
  // }

  void Elif(SourceLocation Loc, SourceRange ConditionRange,
    ConditionValueKind ConditionValue, SourceLocation IfLoc,
    ArrayRef<Token> UnexpandedTokens) override {

    // Get the end position of the condition expression
    SourceLocation ConditionEnd = Lexer::getLocForEndOfToken(
      ConditionRange.getEnd(), 0, SM, PP.getLangOpts());
    
    std::set<std::string> Macros;
    for (const Token &Tok : UnexpandedTokens) {
      if (Tok.is(tok::identifier) || Tok.is(tok::raw_identifier)) {
        StringRef Name;
        if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
          Name = Tok.getIdentifierInfo()->getName();
        } else if (Tok.is(tok::raw_identifier)) {
          Name = Tok.getRawIdentifier();
        } else {
          continue;
        }
        
        // if (Name != "defined") {
        //   Macros.insert(Name.str());
        // }
        if (!isPreprocessorOperator(Name)) { 
          Macros.insert(Name.str());
        }
      }
    }

    // added
    std::string TypeStr;
    switch (ConditionValue) {
      case CVK_True:
        TypeStr = "ELIF_TRUE";
        break;
      case CVK_False:
        TypeStr = "ELIF_FALSE";
        break;
      case CVK_NotEvaluated:
        TypeStr = "ELIF_NOT_EVALUATED";
        break;
    }
    // std::string TypeStr;
    // if (ConditionValue == CVK_NotEvaluated) {
    //   TypeStr = "ELIF_FALSE";  // Was not evaluated
    // } else {
    //   TypeStr = "ELIF_TRUE";   // Was evaluated
    // }
    // ended
    
    // std::string CondText = getConditionText(ConditionRange);
    std::string CondText = getConditionFromTokens(UnexpandedTokens);

    // added
    //printLocationWithMacrosAndEnd("ELIF", Loc, ConditionEnd, CondText, Macros);
    printLocationWithMacrosAndEnd(TypeStr, Loc, ConditionEnd, CondText, Macros);
    // ended

    // added
    if (!ifBlockStack.empty()) {
      PresumedLoc EndPLoc = SM.getPresumedLoc(ConditionEnd);
      ifBlockStack.back().elifDirectives.push_back(
          {Loc, TypeStr, CondText, EndPLoc.getLine(), EndPLoc.getColumn()}
      );
    }
    // ended

  }

  // void SkippedElif(SourceLocation Loc, SourceRange ConditionRange, SourceLocation IfLoc) override {
  //   printLocation("ELIF (skipped)", Loc, "");
  // }

  // Detect #else
  void Else(SourceLocation Loc, SourceLocation IfLoc) override {
    // #else is a single keyword, so start position = end position
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    
    if (PLoc.isValid()) {
      std::string absPath = getAbsolutePath(PLoc.getFilename());
      
      // llvm::outs() << "[ELSE] " 
      //              << absPath 
      //              << ":" << PLoc.getLine() 
      //              << ":" << PLoc.getColumn()
      //              << ":" << PLoc.getLine()     // Same line
      //              << ":" << PLoc.getColumn()   // Same column
      //              << "\n";

      // #else is executed = all previous #if/#elif were False
      llvm::outs() << "[ELSE_TRUE] "
                    << absPath 
                    << ":" << PLoc.getLine() 
                    << ":" << PLoc.getColumn()
                    << ":" << PLoc.getLine()
                    << ":" << PLoc.getColumn()
                    << "\n";
    }

    // Add to ifStack (for correspondence with #endif)
    PresumedLoc EndPLoc = SM.getPresumedLoc(Loc);
    //ifStack.push_back({Loc, "ELSE_TRUE", "", EndPLoc.getLine(), EndPLoc.getColumn()});

    if (!ifBlockStack.empty()) {
      PresumedLoc PLoc = SM.getPresumedLoc(Loc);
      ifBlockStack.back().elseDirective = {Loc, "ELSE_TRUE", "", PLoc.getLine(), PLoc.getColumn()};
      ifBlockStack.back().hasElse = true;
    }
  }

  void SkippedElse(SourceLocation Loc, SourceLocation IfLoc) override {
    // #else is a single keyword, so start position = end position
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    
    if (PLoc.isValid()) {
      std::string absPath = getAbsolutePath(PLoc.getFilename());
      
      // Skipped #else = evaluated=False
      llvm::outs() << "[ELSE_FALSE] "
                    << absPath 
                    << ":" << PLoc.getLine() 
                    << ":" << PLoc.getColumn()
                    << ":" << PLoc.getLine()
                    << ":" << PLoc.getColumn()
                    << "\n";

      // llvm::outs() << "[ELSE (skipped)] " 
      //              << absPath 
      //              << ":" << PLoc.getLine() 
      //              << ":" << PLoc.getColumn()
      //              << ":" << PLoc.getLine()     // Same line
      //              << ":" << PLoc.getColumn()   // Same column
      //              << "\n";
    }

    if (!ifBlockStack.empty()) {
      PresumedLoc PLoc = SM.getPresumedLoc(Loc);
      ifBlockStack.back().elseDirective = {Loc, "ELSE_FALSE", "", PLoc.getLine(), PLoc.getColumn()};
      ifBlockStack.back().hasElse = true;
    }
    
  }

  // Detect #endif
  void Endif(SourceLocation Loc, SourceLocation IfLoc) override {
    // #endif is a single keyword, so start position = end position
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    
    if (PLoc.isValid()) {
      std::string absPath = getAbsolutePath(PLoc.getFilename());
      
      llvm::outs() << "[ENDIF] " 
                   << absPath 
                   << ":" << PLoc.getLine() 
                   << ":" << PLoc.getColumn()
                   << ":" << PLoc.getLine()     // Same line
                   << ":" << PLoc.getColumn()   // Same column
                   << "\n";
    }
    
    // Search for the corresponding #if and output
    if (!ifBlockStack.empty()) {
      auto &block = ifBlockStack.back();
      PresumedLoc EndifPLoc = SM.getPresumedLoc(Loc);
      
      // Output => Closes for #if
      {
          auto &ifInfo = block.ifDirective;
          PresumedLoc IfPLoc = SM.getPresumedLoc(ifInfo.loc);
          if (EndifPLoc.isValid() && IfPLoc.isValid()) {
              std::string ifPath = getAbsolutePath(IfPLoc.getFilename());
              llvm::outs() << "  => Closes [" << ifInfo.type << "] at "
                           << ifPath << ":" << IfPLoc.getLine() 
                           << ":" << IfPLoc.getColumn()
                           << ":" << ifInfo.end_line
                           << ":" << ifInfo.end_column;
              if (!ifInfo.info.empty()) {
                  llvm::outs() << " (" << ifInfo.info << ")";
              }

              llvm::outs() << "\n";
          }
      }
      
      // Output => Closes for #elif
      for (auto &elifInfo : block.elifDirectives) {
          PresumedLoc ElifPLoc = SM.getPresumedLoc(elifInfo.loc);
          if (EndifPLoc.isValid() && ElifPLoc.isValid()) {
              std::string elifPath = getAbsolutePath(ElifPLoc.getFilename());
              llvm::outs() << "  => Closes [" << elifInfo.type << "] at "
                           << elifPath << ":" << ElifPLoc.getLine() 
                           << ":" << ElifPLoc.getColumn()
                           << ":" << elifInfo.end_line
                           << ":" << elifInfo.end_column;
              if (!elifInfo.info.empty()) {
                  llvm::outs() << " (" << elifInfo.info << ")";
              }
              llvm::outs() << "\n";
          }
      }
      
      // Output => Closes for #else
      if (block.hasElse) {
          auto &elseInfo = block.elseDirective;
          PresumedLoc ElsePLoc = SM.getPresumedLoc(elseInfo.loc);
          if (EndifPLoc.isValid() && ElsePLoc.isValid()) {
              std::string elsePath = getAbsolutePath(ElsePLoc.getFilename());
              llvm::outs() << "  => Closes [" << elseInfo.type << "] at "
                           << elsePath << ":" << ElsePLoc.getLine() 
                           << ":" << ElsePLoc.getColumn()
                           << ":" << elseInfo.end_line
                           << ":" << elseInfo.end_column
                           << "\n";
          }
      }
      
      ifBlockStack.pop_back();
    }
    // ended
    // if (!ifStack.empty()) {
    //   auto &ifInfo = ifStack.back();
      
    //   PresumedLoc EndifPLoc = SM.getPresumedLoc(Loc);
    //   PresumedLoc IfPLoc = SM.getPresumedLoc(ifInfo.loc);
      
    //   if (EndifPLoc.isValid() && IfPLoc.isValid()) {
    //     std::string ifPath = getAbsolutePath(IfPLoc.getFilename());
    //     llvm::outs() << "  => Closes [" << ifInfo.type << "] at "
    //                  << ifPath << ":" << IfPLoc.getLine() 
    //                  << ":" << IfPLoc.getColumn()
    //                  << ":" << ifInfo.end_line      
    //                  << ":" << ifInfo.end_column;   
    //     if (!ifInfo.info.empty()) {
    //       llvm::outs() << " (" << ifInfo.info << ")";
    //     }
    //     llvm::outs() << "\n";
        
    //     // Save correspondence to map (can be used later)
    //     endifToIfMap[Loc.getRawEncoding()] = ifInfo;
    //   }
      
    //   ifStack.pop_back();
    // }
  }

  // void SkippedEndif(SourceLocation Loc, SourceLocation IfLoc) override {
  //   printLocation("ENDIF (skipped)", Loc, "");
  // }
  

  void SkippedEndif(SourceLocation Loc, SourceLocation IfLoc) override {
    // #endif is a single keyword, so start position = end position
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    
    if (PLoc.isValid()) {
      std::string absPath = getAbsolutePath(PLoc.getFilename());
      
      llvm::outs() << "[ENDIF (skipped)] " 
                   << absPath 
                   << ":" << PLoc.getLine() 
                   << ":" << PLoc.getColumn()
                   << ":" << PLoc.getLine()     // Same line
                   << ":" << PLoc.getColumn()   // Same column
                   << "\n";
    }
    
    // Search for the corresponding #if and output (same logic as Endif)
    // if (!ifStack.empty()) {
    //   auto &ifInfo = ifStack.back();
      
    //   PresumedLoc EndifPLoc = SM.getPresumedLoc(Loc);
    //   PresumedLoc IfPLoc = SM.getPresumedLoc(ifInfo.loc);
      
    //   if (EndifPLoc.isValid() && IfPLoc.isValid()) {
    //     std::string ifPath = getAbsolutePath(IfPLoc.getFilename());
    //     llvm::outs() << "  => Closes [" << ifInfo.type << "] at "
    //                  << ifPath << ":" << IfPLoc.getLine() 
    //                  << ":" << IfPLoc.getColumn()
    //                  << ":" << ifInfo.end_line      
    //                  << ":" << ifInfo.end_column;   
    //     if (!ifInfo.info.empty()) {
    //       llvm::outs() << " (" << ifInfo.info << ")";
    //     }
    //     llvm::outs() << "\n";
        
    //     // Save correspondence to map
    //     endifToIfMap[Loc.getRawEncoding()] = ifInfo;
    //   }
      
    //   ifStack.pop_back();
    // }

    // added
    if (!ifBlockStack.empty()) {
      auto &block = ifBlockStack.back();
      PresumedLoc EndifPLoc = SM.getPresumedLoc(Loc);
      
      // Output => Closes for #if
      {
          auto &ifInfo = block.ifDirective;
          PresumedLoc IfPLoc = SM.getPresumedLoc(ifInfo.loc);
          if (EndifPLoc.isValid() && IfPLoc.isValid()) {
              std::string ifPath = getAbsolutePath(IfPLoc.getFilename());
              llvm::outs() << "  => Closes [" << ifInfo.type << "] at "
                          << ifPath << ":" << IfPLoc.getLine() 
                          << ":" << IfPLoc.getColumn()
                          << ":" << ifInfo.end_line
                          << ":" << ifInfo.end_column;
              if (!ifInfo.info.empty()) {
                  llvm::outs() << " (" << ifInfo.info << ")";
              }

              llvm::outs() << "\n";
          }
      }
      
      // Output => Closes for #elif
      for (auto &elifInfo : block.elifDirectives) {
          PresumedLoc ElifPLoc = SM.getPresumedLoc(elifInfo.loc);
          if (EndifPLoc.isValid() && ElifPLoc.isValid()) {
              std::string elifPath = getAbsolutePath(ElifPLoc.getFilename());
              llvm::outs() << "  => Closes [" << elifInfo.type << "] at "
                          << elifPath << ":" << ElifPLoc.getLine() 
                          << ":" << ElifPLoc.getColumn()
                          << ":" << elifInfo.end_line
                          << ":" << elifInfo.end_column;
              if (!elifInfo.info.empty()) {
                  llvm::outs() << " (" << elifInfo.info << ")";
              }

              llvm::outs() << "\n";
          }
      }
      
      // Output => Closes for #else
      if (block.hasElse) {
          auto &elseInfo = block.elseDirective;
          PresumedLoc ElsePLoc = SM.getPresumedLoc(elseInfo.loc);
          if (EndifPLoc.isValid() && ElsePLoc.isValid()) {
              std::string elsePath = getAbsolutePath(ElsePLoc.getFilename());
              llvm::outs() << "  => Closes [" << elseInfo.type << "] at "
                          << elsePath << ":" << ElsePLoc.getLine() 
                          << ":" << ElsePLoc.getColumn()
                          << ":" << elseInfo.end_line
                          << ":" << elseInfo.end_column
                          << "\n";
          }
      }
      
      ifBlockStack.pop_back();
    }
    // ended
  }

  // Method to get correspondence (accessible from outside)
  const std::map<unsigned, IfDirectiveInfo>& getEndifToIfMap() const {
    return endifToIfMap;
  }

private:
  SourceManager &SM;
  Preprocessor &PP;

  // Stack for #if-family directives (supports nesting)
  //std::vector<IfDirectiveInfo> ifStack;
  std::vector<IfBlockInfo> ifBlockStack;  // added
  
  // Map of #endif to corresponding #if
  std::map<unsigned, IfDirectiveInfo> endifToIfMap;

  void printLocation(StringRef Type, SourceLocation Loc, StringRef Info) {
    if (Loc.isInvalid() || !Loc.isFileID()) return;
    
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    if (PLoc.isInvalid()) return;

    std::string absPath = getAbsolutePath(PLoc.getFilename());
    llvm::outs() << "[" << Type << "] " 
                 << absPath  // << PLoc.getFilename() 
                 << ":" << PLoc.getLine() 
                 << ":" << PLoc.getColumn();
    if (!Info.empty())
      llvm::outs() << " - " << Info;

    llvm::outs() << "\n";
  }

  // New helper function: output with end position
  void printLocationWithEnd(StringRef Type, SourceLocation Loc, 
                           SourceLocation EndLoc, StringRef Info) {
    if (Loc.isInvalid() || !Loc.isFileID()) return;
    
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    PresumedLoc EndPLoc = SM.getPresumedLoc(EndLoc);
    if (PLoc.isInvalid() || EndPLoc.isInvalid()) return;

    std::string absPath = getAbsolutePath(PLoc.getFilename());
    llvm::outs() << "[" << Type << "] " 
                 << absPath 
                 << ":" << PLoc.getLine() 
                 << ":" << PLoc.getColumn()
                 << ":" << EndPLoc.getLine()     // End line
                 << ":" << EndPLoc.getColumn();  // End column
    if (!Info.empty())
      llvm::outs() << " - " << Info;

    llvm::outs() << "\n";
  }

  // Output usage location and definition location
  void printLocationWithDef(StringRef Type, SourceLocation Loc, 
                            StringRef MacroName, StringRef DefLoc) {
    if (Loc.isInvalid() || !Loc.isFileID()) return;
    
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    if (PLoc.isInvalid()) return;

    std::string absPath = getAbsolutePath(PLoc.getFilename());
    llvm::outs() << "[" << Type << "] " 
                 << absPath 
                 // << PLoc.getFilename() 
                 << ":" << PLoc.getLine() 
                 << ":" << PLoc.getColumn()
                 << " - " << MacroName;
    if (!DefLoc.empty())
      llvm::outs() << " (defined at: " << DefLoc << ")";

    llvm::outs() << "\n";
  }

  // New helper function: output with definition location and end position
  void printLocationWithDefAndEnd(StringRef Type, SourceLocation Loc, 
                                 SourceLocation EndLoc,
                                 StringRef MacroName, StringRef DefLoc) {
    if (Loc.isInvalid() || !Loc.isFileID()) return;
    
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    PresumedLoc EndPLoc = SM.getPresumedLoc(EndLoc);
    if (PLoc.isInvalid() || EndPLoc.isInvalid()) return;

    std::string absPath = getAbsolutePath(PLoc.getFilename());
    llvm::outs() << "[" << Type << "] " 
                 << absPath 
                 << ":" << PLoc.getLine() 
                 << ":" << PLoc.getColumn()
                 << ":" << EndPLoc.getLine()     // End line
                 << ":" << EndPLoc.getColumn()   // End column
                 << " - " << MacroName;
    if (!DefLoc.empty())
      llvm::outs() << " (defined at: " << DefLoc << ")";

    llvm::outs() << "\n";
  }

  // Output the condition expression and macro list for #if
  void printLocationWithMacros(StringRef Type, SourceLocation Loc,
                               StringRef CondText, const std::set<std::string> &Macros) {
    if (Loc.isInvalid() || !Loc.isFileID()) return;
    
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    if (PLoc.isInvalid()) return;

    std::string absPath = getAbsolutePath(PLoc.getFilename());
    llvm::outs() << "[" << Type << "] " 
                 // << PLoc.getFilename() 
                 << absPath 
                 << ":" << PLoc.getLine() 
                 << ":" << PLoc.getColumn()
                 << " - " << CondText;
    
    if (!Macros.empty()) {
      llvm::outs() << " [";
      bool first = true;
      for (const auto &M : Macros) {
        if (!first) llvm::outs() << "; ";
        llvm::outs() << M;
        
        // Get the definition location of each macro
        IdentifierInfo *II = PP.getIdentifierInfo(M);
        if (II && II->hasMacroDefinition()) {
          MacroDefinition MD = PP.getMacroDefinition(II);
          std::string DefLoc = getDefinitionLocation(MD);
          llvm::outs() << " defined at: " << DefLoc;
        } else {
          llvm::outs() << " defined at: undefined";
        }
        first = false;
      }
      llvm::outs() << "]";
    }

    llvm::outs() << "\n";
  }

  // New helper function: output with condition expression, macro list, and end position
  void printLocationWithMacrosAndEnd(StringRef Type, SourceLocation Loc,
                                    SourceLocation EndLoc,
                                    StringRef CondText, 
                                    const std::set<std::string> &Macros) {
    if (Loc.isInvalid() || !Loc.isFileID()) return;
    
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    PresumedLoc EndPLoc = SM.getPresumedLoc(EndLoc);
    if (PLoc.isInvalid() || EndPLoc.isInvalid()) return;

    std::string absPath = getAbsolutePath(PLoc.getFilename());
    llvm::outs() << "[" << Type << "] " 
                 << absPath 
                 << ":" << PLoc.getLine() 
                 << ":" << PLoc.getColumn()
                 << ":" << EndPLoc.getLine()     // End line
                 << ":" << EndPLoc.getColumn()   // End column
                 << " - " << CondText;
    
    if (!Macros.empty()) {
      llvm::outs() << " [";
      bool first = true;
      for (const auto &M : Macros) {
        if (!first) llvm::outs() << "; ";
        llvm::outs() << M;
        
        // Get the definition location of each macro
        IdentifierInfo *II = PP.getIdentifierInfo(M);
        if (II && II->hasMacroDefinition()) {
          MacroDefinition MD = PP.getMacroDefinition(II);
          std::string DefLoc = getDefinitionLocation(MD);
          llvm::outs() << " defined at: " << DefLoc;
        } else {
          //llvm::outs() << " defined at: undefined";
          llvm::outs() << " defined at: undefined"; 
        }
        first = false;
      }
      llvm::outs() << "]";
    }

    llvm::outs() << "\n";
  }

  // Get the position of the macro definition
  std::string getDefinitionLocation(const MacroDefinition &MD) {
    if (!MD) return "undefined";
    
    const MacroInfo *MI = MD.getMacroInfo();
    if (!MI) return "undefined";
    
    SourceLocation DefLoc = MI->getDefinitionLoc();
    if (DefLoc.isInvalid()) return "unknown";
    
    PresumedLoc PLoc = SM.getPresumedLoc(DefLoc);
    if (PLoc.isInvalid()) return "unknown";
    
    std::string absPath = getAbsolutePath(PLoc.getFilename());
    std::string Result;
    llvm::raw_string_ostream OS(Result);
    // OS << PLoc.getFilename() << ":" << PLoc.getLine() << ":" << PLoc.getColumn();
    OS << absPath << ":" << PLoc.getLine() << ":" << PLoc.getColumn();
    return OS.str();
  }

  // // Extract macros from condition expression using Lexer
  // std::set<std::string> extractMacrosUsingLexer(SourceRange ConditionRange) {
  //   std::set<std::string> Macros;
    
  //   if (ConditionRange.isInvalid()) return Macros;
    
  //   SourceLocation Begin = ConditionRange.getBegin();
  //   SourceLocation End = ConditionRange.getEnd();
    
  //   // Convert macro expansion location to actual file location
  //   if (Begin.isMacroID()) Begin = SM.getExpansionLoc(Begin);
  //   if (End.isMacroID()) End = SM.getExpansionLoc(End);
    
  //   bool Invalid = false;
  //   const char *BeginPtr = SM.getCharacterData(Begin, &Invalid);
  //   if (Invalid) return Macros;
    
  //   const char *EndPtr = SM.getCharacterData(End, &Invalid);
  //   if (Invalid) return Macros;
    
  //   // Get token sequence using Lexer
  //   Lexer RawLexer(Begin, PP.getLangOpts(), BeginPtr, BeginPtr, EndPtr);
    
  //   Token Tok;
  //   while (!RawLexer.LexFromRawLexer(Tok)) {
  //     // Extract only identifier tokens
  //     if (Tok.is(tok::identifier)) {
  //       StringRef Name = Tok.getRawIdentifier();
  //       std::string MacroName = Name.str();
        
  //       // Exclude "defined" keyword
  //       if (MacroName != "defined") {
  //         Macros.insert(MacroName);
  //       }
  //     }
      
  //     // End check
  //     if (Tok.getLocation() >= End) break;
  //   }
    
  //   return Macros;
  // }

  // // Get absolute path
  // std::string getAbsolutePath(StringRef filename) {
  //   std::filesystem::path p(filename.str());
  //   if (p.is_absolute()) {
  //     return filename.str();
  //   }
  //   return std::filesystem::absolute(p).string();
  // }
  // Get absolute path (resolving ..)

  // std::string getAbsolutePath(StringRef filename) {
  //   std::filesystem::path p(filename.str());
    
  //   // Normalize with canonical() (resolve .. + resolve symbolic links)
  //   std::error_code EC;
  //   auto result = std::filesystem::canonical(p, EC);
  //   if (!EC) {
  //     return result.string();
  //   }
    
  //   // Fallback: normalize manually
  //   if (!p.is_absolute()) {
  //     p = std::filesystem::absolute(p);
  //   }
    
  //   std::filesystem::path normalized;
  //   for (const auto& part : p) {
  //     if (part == "..") {
  //       if (normalized.has_parent_path() && normalized.filename() != "..") {
  //         normalized = normalized.parent_path();
  //       }
  //     } else if (part != ".") {
  //       normalized /= part;
  //     }
  //   }
    
  //   return normalized.string();
  // }

  // Cache for getAbsolutePath results to avoid repeated filesystem calls.
  std::unordered_map<std::string, std::string> pathCache;

  std::string getAbsolutePath(StringRef filename) {
    std::string name = filename.str();

    // Check cache first
    auto it = pathCache.find(name);
    if (it != pathCache.end()) {
      return it->second;
    }

    std::string result;

    // added: Return virtual paths like <built-in>, <command line> as-is
    if (!name.empty() && name[0] == '<') {
      if (!g_compileDir.empty()) {
        result = g_compileDir + "/" + name;
      } else {
        result = name;
      }
      pathCache.emplace(name, result);
      return result;
    }
    // ended

    std::filesystem::path p(name);

    std::error_code EC;
    auto canonical = std::filesystem::canonical(p, EC);
    if (!EC) {
      result = canonical.string();
      pathCache.emplace(name, result);
      return result;
    }

    if (!p.is_absolute()) {
      p = std::filesystem::absolute(p);
    }

    std::filesystem::path normalized;
    for (const auto& part : p) {
      if (part == "..") {
        if (normalized.has_parent_path() && normalized.filename() != "..") {
          normalized = normalized.parent_path();
        }
      } else if (part != ".") {
        normalized /= part;
      }
    }

    result = normalized.string();
    pathCache.emplace(name, result);
    return result;
  }


  // Added near getAbsolutePath
  std::string getSignatureFromSource(const MacroInfo *MI) {
    if (!MI || !MI->isFunctionLike()) return "";
    
    bool Invalid = false;
    const char *Start = SM.getCharacterData(MI->getDefinitionLoc(), &Invalid);
    if (Invalid) return "";
    
    const char *Ptr = Start;
    while (*Ptr && *Ptr != '(' && *Ptr != '\n') Ptr++;
    if (*Ptr != '(') return "";
    
    int Depth = 1;
    Ptr++;
    while (*Ptr && Depth > 0) {
        if (*Ptr == '(') Depth++;
        else if (*Ptr == ')') Depth--;
        Ptr++;
    }
    // Return only the parameter part excluding the name part (the "(s, ...)" part)
    const char *ParenStart = Start;
    while (ParenStart < Ptr && *ParenStart != '(') ParenStart++;
    return std::string(ParenStart, Ptr - ParenStart);
  }

  // Extract the macro replacement body as it appears in the source.
  // Returns an empty string for macros with no replacement tokens.
  std::string getMacroBodyFromSource(const MacroInfo *MI) {
    if (!MI || MI->getNumTokens() == 0) return "";
    
    // Start of the body: location of the first replacement token.
    SourceLocation BodyBegin = MI->getReplacementToken(0).getLocation();
    
    // End of the body: end of the last replacement token.
    SourceLocation BodyEnd = Lexer::getLocForEndOfToken(
        MI->getDefinitionEndLoc(), 0, SM, PP.getLangOpts());
    
    if (BodyBegin.isInvalid() || BodyEnd.isInvalid()) return "";
    
    CharSourceRange Range = CharSourceRange::getCharRange(BodyBegin, BodyEnd);
    bool Invalid = false;
    StringRef Text = Lexer::getSourceText(Range, SM, PP.getLangOpts(), &Invalid);
    
    if (Invalid) return "";
    return Text.str();
  }


  // Get text from ConditionRange
  std::string getConditionText0(SourceRange ConditionRange) {
    if (ConditionRange.isInvalid()) return "";
    
    CharSourceRange CharRange = CharSourceRange::getTokenRange(ConditionRange);
    bool Invalid = false;
    StringRef Text = Lexer::getSourceText(CharRange, SM, PP.getLangOpts(), &Invalid);
    
    if (Invalid) return "";
    return Text.str();
  }

  // added
  std::string getConditionText(SourceRange ConditionRange) {
    if (ConditionRange.isInvalid()) return "";
    
    SourceLocation Begin = ConditionRange.getBegin();
    SourceLocation End = ConditionRange.getEnd();
    
    if (Begin.isMacroID())
      Begin = SM.getExpansionLoc(Begin);
    if (End.isMacroID())
      End = SM.getExpansionLoc(End);
    
    CharSourceRange CharRange = CharSourceRange::getTokenRange(SourceRange(Begin, End));
    bool Invalid = false;
    StringRef Text = Lexer::getSourceText(CharRange, SM, PP.getLangOpts(), &Invalid);
    
    if (Invalid) return "";
    return Text.str();
  }
  // ended

  // modified
  std::string getConditionFromTokens(ArrayRef<Token> UnexpandedTokens) {
    std::string Result;
    for (const Token &Tok : UnexpandedTokens) {
        if (!Result.empty() && Tok.hasLeadingSpace()) {
            Result += ' ';
        }
        Result += PP.getSpelling(Tok);
    }
    return Result;
  }

  // std::string getConditionText(SourceRange ConditionRange) { // Is this one more correct? Reverted for now.
  //   if (ConditionRange.isInvalid()) return "";
    
  //   // Required: Extend the end position
  //   SourceLocation End = Lexer::getLocForEndOfToken(
  //     ConditionRange.getEnd(), 0, SM, PP.getLangOpts());
    
  //   CharSourceRange CharRange = CharSourceRange::getCharRange(
  //     ConditionRange.getBegin(), End);
    
  //   bool Invalid = false;
  //   StringRef Text = Lexer::getSourceText(CharRange, SM, PP.getLangOpts(), &Invalid);
    
  //   if (Invalid) return "";
  //   return Text.str();
  // }

  static bool isPreprocessorOperator(llvm::StringRef name) {
    static const std::set<llvm::StringRef> ops = {
      "defined",
      "__has_feature", "__has_extension",
      "__has_builtin", "__has_constexpr_builtin",
      "__has_attribute", "__has_embed",
      "__has_include", "__has_include_next",
      "__has_warning", "__is_identifier",
      "__building_module",
      "__has_cpp_attribute", "__has_c_attribute",
      "__has_declspec",
      "__is_target_arch", "__is_target_vendor",
      "__is_target_os", "__is_target_environment",
      "__is_target_variant_os", "__is_target_variant_environment",
    };
    return ops.count(name) > 0;
  }
};

class MacroFinderAction : public PreprocessOnlyAction {
protected:
  void ExecuteAction() override {
    // added
    CompilerInstance &CI = getCompilerInstance();
    Preprocessor &PP = CI.getPreprocessor();
    PP.addPPCallbacks(std::make_unique<MacroCallbacks>(
        CI.getSourceManager(), PP));
    PreprocessOnlyAction::ExecuteAction();
    // ended
  }
//   void ExecuteAction() override {
//     Preprocessor &PP = getCompilerInstance().getPreprocessor();

//     // PP.setRecordCondDirectiveLocs(true);

//     PP.addPPCallbacks(std::make_unique<MacroCallbacks>(
//         getCompilerInstance().getSourceManager(), PP));
//     PreprocessOnlyAction::ExecuteAction();
//   }
};


static cl::OptionCategory MyToolCategory("macro-finder options");

// Unified include path addition (C/C++ switching)
static const std::vector<std::string> CUSTOM_INCLUDE_PATHS = {
  // Specify Clang's resource directory
  "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
  // Disable all warnings
  "-w",
  "-Wno-incompatible-function-pointer-types", 
  "-Wno-incompatible-pointer-types",

  // C system headers (after C++ - so they can be found by #include_next)
  "-isystem/usr/include/aarch64-linux-gnu",
  "-isystem/usr/include",
  
  // Compiler settings
  // "-std=gnu11",
  // "-std=gnu++11",
  "-fno-strict-aliasing",
};

// void addCustomIncludePaths(ClangTool &Tool) {
//   Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
//       CUSTOM_INCLUDE_PATHS,
//       ArgumentInsertPosition::BEGIN));
// }


void addCustomIncludePaths(ClangTool &Tool) {
  Tool.appendArgumentsAdjuster(
    [](const CommandLineArguments &Args, StringRef Filename) -> CommandLineArguments {
      CommandLineArguments NewArgs = Args;
      
      // Common arguments
      std::vector<std::string> CommonArgs = {
        "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
        "-w",
        "-Wno-incompatible-function-pointer-types",
        "-Wno-incompatible-pointer-types",
        "-fno-strict-aliasing",
        "-isystem/usr/lib/llvm-19/lib/clang/19/include",
      };

      bool isCxx = Filename.ends_with(".cxx") || Filename.ends_with(".cpp") || 
                   Filename.ends_with(".cc")  || Filename.ends_with(".C")   ||
                   Filename.ends_with(".hpp") || Filename.ends_with(".hxx");

      if (isCxx) {
        // For C++ files: add C++ headers
        std::vector<std::string> CxxArgs = {
          "-isystem/usr/include/c++/11",
          //"-isystem/usr/include/aarch64-linux-gnu/c++/11",
          "-isystem/usr/include/aarch64-linux-gnu/c++/11",
          "-isystem/usr/include/c++/11/backward",
        };
        CommonArgs.insert(CommonArgs.end(), CxxArgs.begin(), CxxArgs.end());
      } else {
        // ★ For C files: completely block C++ system includes
        CommonArgs.push_back("-nostdinc++");
      }

      // System headers (common)
      //CommonArgs.push_back("-isystem/usr/include/aarch64-linux-gnu");
      CommonArgs.push_back("-isystem/usr/include/aarch64-linux-gnu");
      CommonArgs.push_back("-isystem/usr/include");

      NewArgs.insert(NewArgs.begin() + 1, CommonArgs.begin(), CommonArgs.end());

      return NewArgs;
    }
  );
}

int main(int argc, const char **argv) {
  // Enable buffering on llvm::outs() to reduce I/O overhead.
  // The buffer is flushed automatically on normal program exit.
  llvm::outs().SetBufferSize(65536);


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
    std::filesystem::path absDbPath = std::filesystem::absolute(dbPath);
    absDbPath = absDbPath.lexically_normal();
    g_compileDir = absDbPath.string();

    std::string errMsg;
    auto DB = CompilationDatabase::loadFromDirectory(absDbPath.string(), errMsg);
    if (!DB) {
      llvm::errs() << "Error loading compilation database from '"
                   << dbPath << "': " << errMsg << "\n";
      return 1;
    }

    // added
    // Run ClangTool once per compile command entry. The per-command
    // identifier is fixed per iteration. Tool.run is synchronous,
    // so the analysis inside always belongs to this command.
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

      int ret = Tool.run(newFrontendActionFactory<MacroFinderAction>().get());
      if (ret != 0) finalRet = ret;
    }

    llvm::outs().flush();
    return finalRet;
    // ended
  }

  // Legacy path: via CommonOptionsParser
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
  if (!ExpectedParser) {
    // llvm::errs() << ExpectedParser.takeError();
    return 1;
  }
  CommonOptionsParser &OptionsParser = ExpectedParser.get();
  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());

  addCustomIncludePaths(Tool);

  return Tool.run(newFrontendActionFactory<MacroFinderAction>().get());
}
