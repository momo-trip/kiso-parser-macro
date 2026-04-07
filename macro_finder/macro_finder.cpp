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

  explicit MacroCallbacks(SourceManager &SM, Preprocessor &PP) : SM(SM), PP(PP) {}

  // // Detect macro definitions
  // void MacroDefined(const Token &MacroNameTok,
  //                   const MacroDirective *MD) override {
  //   SourceLocation Loc = MacroNameTok.getLocation();
  //   printLocation("DEFINED", Loc, MacroNameTok.getIdentifierInfo()->getName());
  // }

  // Detect macro definitions
  void MacroDefined(const Token &MacroNameTok,
                  const MacroDirective *MD) override {
    SourceLocation Loc = MacroNameTok.getLocation();
    std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
    
    SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
      MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
    
    const MacroInfo *MI = MD->getMacroInfo();
    
    // ★ Get the macro definition body
    std::string MacroBody;
    if (MI && MI->getNumTokens() > 0) {
      // Reconstruct definition body from token sequence
      for (unsigned i = 0; i < MI->getNumTokens(); ++i) {
        const Token &Tok = MI->getReplacementToken(i);
        
        // Handle spaces between tokens
        if (i > 0 && Tok.hasLeadingSpace()) {
          MacroBody += " ";
        }
        
        // Get the text of the token
        if (Tok.isLiteral()) {
          MacroBody += StringRef(Tok.getLiteralData(), Tok.getLength()).str();
        } else if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
          MacroBody += Tok.getIdentifierInfo()->getName().str();
        } else {
          // Other tokens (operators, etc.)
          MacroBody += PP.getSpelling(Tok);
        }
      }
    }
    
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

  // void MacroDefined(const Token &MacroNameTok,
  //                   const MacroDirective *MD) override {
  //   SourceLocation Loc = MacroNameTok.getLocation();
  //   std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
    
  //   // Get the end position of the macro name
  //   SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
  //     MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
    
  //   // Get MacroInfo and check if it's function-like
  //   const MacroInfo *MI = MD->getMacroInfo();
    
  //   if (MI && MI->isFunctionLike()) {
  //     // For function-like macros: with detailed information
  //     std::string details = MacroName + "(";
      
  //     // Get parameter names
  //     for (unsigned i = 0; i < MI->getNumParams(); ++i) {
  //       if (i > 0) details += ", ";
  //       details += MI->params()[i]->getName().str();
  //     }
      
  //     // For variadic arguments
  //     if (MI->isVariadic()) {
  //       if (MI->getNumParams() > 0) details += ", ";
  //       details += "...";
  //     }
      
  //     details += ")";
      
  //     printLocationWithEnd("DEFINED_FUNC", Loc, MacroEnd, details);
  //   } else {
  //     // For macro variables (as before)
  //     printLocationWithEnd("DEFINED", Loc, MacroEnd, MacroName);
  //   }
  // }

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

  // // Detect #ifdef
  // void Ifdef(SourceLocation Loc, const Token &MacroNameTok,
  //            const MacroDefinition &MD) override {
  //   std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
  //   std::string DefLoc = getDefinitionLocation(MD);
  //   printLocationWithDef("IFDEF", Loc, MacroName, DefLoc);
    
  //   // Record the correspondence
  //   ifStack.push_back({Loc, "IFDEF", MacroName});
  // }

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

    // if (MI && MI->isFunctionLike()) {
    //   //MacroType = "IFDEF_FUNC";

    //   // added
    //   MacroType = evaluated ? "IFDEF_FUNC_TRUE" : "IFDEF_FUNC_FALSE";
    //   //MacroType = "IFDEF_FUNC_TRUE";  // Function-like macros are also always evaluated
    //   // ended
      
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
  // void SkippedIfndef(SourceLocation Loc, const Token &MacroNameTok) override {
  //   if (MacroNameTok.getIdentifierInfo()) {
  //     std::string MacroName = MacroNameTok.getIdentifierInfo()->getName().str();
      
  //     // Get the end position of the macro name
  //     SourceLocation MacroEnd = Lexer::getLocForEndOfToken(
  //       MacroNameTok.getLocation(), 0, SM, PP.getLangOpts());
      
  //     printLocationWithDefAndEnd("IFNDEF (skipped)", Loc, MacroEnd, MacroName, "unknown");
  //   }
  // }

  // Detect #if (extract macros precisely using Lexer)
  // void If(SourceLocation Loc, SourceRange ConditionRange,
  //         ConditionValueKind ConditionValue) override {
  //   std::set<std::string> Macros = extractMacrosUsingLexer(ConditionRange);
  //   std::string CondText = getConditionText(ConditionRange);
  //   printLocationWithMacros("IF", Loc, CondText, Macros);
  // }

  // // Detect #elif (extract macros precisely using Lexer)
  // void Elif(SourceLocation Loc, SourceRange ConditionRange,
  //           ConditionValueKind ConditionValue, SourceLocation IfLoc) override {
  //   std::set<std::string> Macros = extractMacrosUsingLexer(ConditionRange);
  //   std::string CondText = getConditionText(ConditionRange);
  //   printLocationWithMacros("ELIF", Loc, CondText, Macros);
  // }

  // // Detect #if (using UnexpandedTokens)
  // void If(SourceLocation Loc, SourceRange ConditionRange,
  //   ConditionValueKind ConditionValue,
  //   ArrayRef<Token> UnexpandedTokens) override { 

  //   // Extract macros directly from tokens (no Lexer needed!)
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
  //   printLocationWithMacros("IF", Loc, CondText, Macros);
  // }

  void If(SourceLocation Loc, SourceRange ConditionRange,
    ConditionValueKind ConditionValue,
    ArrayRef<Token> UnexpandedTokens) override {

    // llvm::errs() << "DEBUG: Collected " << UnexpandedTokens.size() << " tokens:\n";
    // for (const Token &Tok : UnexpandedTokens) {
    //   llvm::errs() << "  Kind: " << Tok.getName() << ", ";
    //   if (Tok.is(tok::identifier) && Tok.getIdentifierInfo()) {
    //     llvm::errs() << "Identifier: " << Tok.getIdentifierInfo()->getName();
    //   } else if (Tok.is(tok::raw_identifier)) {
    //     llvm::errs() << "RawIdentifier: " << Tok.getRawIdentifier();
    //   } else if (Tok.is(tok::numeric_constant)) {
    //     llvm::errs() << "Numeric";
    //   }
    //   llvm::errs() << "\n";
    // }
    //
    
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
        
        if (Name != "defined") {
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
        
        if (Name != "defined") {
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
        
        if (Name != "defined") {
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
        
        if (Name != "defined") {
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

      // added
      // #else is executed = all previous #if/#elif were False
      llvm::outs() << "[ELSE_TRUE] "
                    << absPath 
                    << ":" << PLoc.getLine() 
                    << ":" << PLoc.getColumn()
                    << ":" << PLoc.getLine()
                    << ":" << PLoc.getColumn()
                    << "\n";
      // ended
    }

    // added
    // Add to ifStack (for correspondence with #endif)
    PresumedLoc EndPLoc = SM.getPresumedLoc(Loc);
    //ifStack.push_back({Loc, "ELSE_TRUE", "", EndPLoc.getLine(), EndPLoc.getColumn()});
    // ended
    
    // ★ Record the correspondence (optional)
    // When updating the last element of ifStack
    // if (!ifStack.empty()) {
    //   ifStack.back().loc = Loc;  // Update to the position of #else
    // }

    // added
    if (!ifBlockStack.empty()) {
      PresumedLoc PLoc = SM.getPresumedLoc(Loc);
      ifBlockStack.back().elseDirective = {Loc, "ELSE_TRUE", "", PLoc.getLine(), PLoc.getColumn()};
      ifBlockStack.back().hasElse = true;
    }
    // ended

  }

  void SkippedElse(SourceLocation Loc, SourceLocation IfLoc) override {
    // #else is a single keyword, so start position = end position
    PresumedLoc PLoc = SM.getPresumedLoc(Loc);
    
    if (PLoc.isValid()) {
      std::string absPath = getAbsolutePath(PLoc.getFilename());
      
      // added
      // Skipped #else = evaluated=False
      llvm::outs() << "[ELSE_FALSE] "
                    << absPath 
                    << ":" << PLoc.getLine() 
                    << ":" << PLoc.getColumn()
                    << ":" << PLoc.getLine()
                    << ":" << PLoc.getColumn()
                    << "\n";
      // ended
      
      // llvm::outs() << "[ELSE (skipped)] " 
      //              << absPath 
      //              << ":" << PLoc.getLine() 
      //              << ":" << PLoc.getColumn()
      //              << ":" << PLoc.getLine()     // Same line
      //              << ":" << PLoc.getColumn()   // Same column
      //              << "\n";
    }

    // // added
    // // Add to ifStack
    // PresumedLoc EndPLoc = SM.getPresumedLoc(Loc);
    // ifStack.push_back({Loc, "ELSE_FALSE", "", EndPLoc.getLine(), EndPLoc.getColumn()});
    // // ended

    // added
    if (!ifBlockStack.empty()) {
      PresumedLoc PLoc = SM.getPresumedLoc(Loc);
      ifBlockStack.back().elseDirective = {Loc, "ELSE_FALSE", "", PLoc.getLine(), PLoc.getColumn()};
      ifBlockStack.back().hasElse = true;
    }
    // ended
    
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

  std::string getAbsolutePath(StringRef filename) {
    // added: Return virtual paths like <built-in>, <command line> as-is
    std::string name = filename.str();
    if (!name.empty() && name[0] == '<') {
      if (!g_compileDir.empty()) {
        return g_compileDir + "/" + name;
      }
      return name;
    }
    // ended
    std::filesystem::path p(name);
    
    std::error_code EC;
    auto result = std::filesystem::canonical(p, EC);
    if (!EC) {
      return result.string();
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
    
    return normalized.string();
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
};

class MacroFinderAction : public PreprocessOnlyAction {
protected:
  void ExecuteAction() override {
    Preprocessor &PP = getCompilerInstance().getPreprocessor();

    // PP.setRecordCondDirectiveLocs(true);

    PP.addPPCallbacks(std::make_unique<MacroCallbacks>(
        getCompilerInstance().getSourceManager(), PP));
    PreprocessOnlyAction::ExecuteAction();
  }
};


static cl::OptionCategory MyToolCategory("macro-finder options");

// Unified include path addition (C/C++ switching)
static const std::vector<std::string> CUSTOM_INCLUDE_PATHS = {
  // Specify Clang's resource directory
  //"-resource-dir=/home/ubuntu/macrust/llvm-custom/lib/clang/19",
  "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
  // Disable all warnings
  "-w",
  "-Wno-incompatible-function-pointer-types", 
  "-Wno-incompatible-pointer-types",
  // "-Wno-incompatible-pointer-types-discards-qualifiers",
  //"-Wno-error", 
  //"-Wno-everything",
  
  // Prioritize Clang's built-in headers
  //"-isystem/home/ubuntu/macrust/llvm-custom/lib/clang/19/include",
  // ★★★ Add OpenMP headers ★★★
  //"-isystem/usr/lib/llvm-14/lib/clang/14.0.0/include",
  //"-isystem/usr/lib/llvm-19/lib/clang/19/include",
  // C++ headers (before C headers)
  // "-isystem/usr/include/c++/11",
  // "-isystem/usr/include/x86_64-linux-gnu/c++/11",
  // "-isystem/usr/include/c++/11/backward",
  
  // C system headers (after C++ - so they can be found by #include_next)
  "-isystem/usr/include/x86_64-linux-gnu",
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

      if (Filename.ends_with(".cxx") || Filename.ends_with(".cpp") || 
          Filename.ends_with(".cc") || Filename.ends_with(".C")) {
        // For C++ files: add C++ headers
        std::vector<std::string> CxxArgs = {
          "-isystem/usr/include/c++/11",
          "-isystem/usr/include/x86_64-linux-gnu/c++/11",
          "-isystem/usr/include/c++/11/backward",
        };
        CommonArgs.insert(CommonArgs.end(), CxxArgs.begin(), CxxArgs.end());
      }

      // System headers (common)
      CommonArgs.push_back("-isystem/usr/include/x86_64-linux-gnu");
      CommonArgs.push_back("-isystem/usr/include");

      NewArgs.insert(NewArgs.begin() + 1, CommonArgs.begin(), CommonArgs.end());
      return NewArgs;
    }
  );
}

int main(int argc, const char **argv) {
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
    
    ClangTool Tool(*DB, AllFiles);
    addCustomIncludePaths(Tool);
    
    return Tool.run(newFrontendActionFactory<MacroFinderAction>().get());
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

// static cl::OptionCategory MyToolCategory("macro-finder options");

// int main(int argc, const char **argv) {
//   auto ExpectedParser = CommonOptionsParser::create(argc, argv, MyToolCategory);
//   if (!ExpectedParser) {
//     // llvm::errs() << ExpectedParser.takeError();
//     return 1;
//   }
//   CommonOptionsParser &OptionsParser = ExpectedParser.get();
//   ClangTool Tool(OptionsParser.getCompilations(),
//                  OptionsParser.getSourcePathList());
  
//   // Add include paths for custom-built Clang
//   // Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
//   //   {// Specify Clang's resource directory
//   //   //"-resource-dir=/home/ubuntu/macrust/llvm-custom/lib/clang/19",
//   //   "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
//   //   // Disable all warnings
//   //   "-w",
//   //   // Prioritize Clang's built-in headers
//   //   //"-isystem/home/ubuntu/macrust/llvm-custom/lib/clang/19/include",
//   //   // ★★★ Add OpenMP headers ★★★
//   //   //"-isystem/usr/lib/llvm-14/lib/clang/14.0.0/include",
//   //   "-isystem/usr/lib/llvm-19/lib/clang/19/include",
//   //   // C++ headers (before C headers)
//   //   // "-isystem/usr/include/c++/11",
//   //   // "-isystem/usr/include/x86_64-linux-gnu/c++/11",
//   //   // "-isystem/usr/include/c++/11/backward",
    
//   //   // C system headers (after C++ - so they can be found by #include_next)
//   //   "-isystem/usr/include/x86_64-linux-gnu",
//   //   "-isystem/usr/include"},
//   //   ArgumentInsertPosition::BEGIN));

//   Tool.appendArgumentsAdjuster(
//     [](const CommandLineArguments &Args, StringRef Filename) -> CommandLineArguments {
//       CommandLineArguments NewArgs = Args;
      
//       if (Filename.ends_with(".c")) {
//         std::vector<std::string> CArgs = {
//           "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
//           "-w",
//           "-isystem/usr/lib/llvm-19/lib/clang/19/include",
//           "-isystem/usr/include/x86_64-linux-gnu",
//           "-isystem/usr/include",
//         };
//         NewArgs.insert(NewArgs.begin() + 1, CArgs.begin(), CArgs.end());
//       } else {
//         std::vector<std::string> CxxArgs = {
//           "-resource-dir=/usr/lib/llvm-19/lib/clang/19",
//           "-w",
//           "-isystem/usr/lib/llvm-19/lib/clang/19/include",
//           "-isystem/usr/include/c++/11",
//           "-isystem/usr/include/x86_64-linux-gnu/c++/11",
//           "-isystem/usr/include/c++/11/backward",
//           "-isystem/usr/include/x86_64-linux-gnu",
//           "-isystem/usr/include",
//         };
//         NewArgs.insert(NewArgs.end(), CxxArgs.begin(), CxxArgs.end());
//       }
      
//       return NewArgs;
//     }
//   );

//   return Tool.run(newFrontendActionFactory<MacroFinderAction>().get());
// }


// Tool.appendArgumentsAdjuster(getInsertArgumentAdjuster(
//   {"-isystem/home/ubuntu/macrust/llvm-custom/lib/clang/19/include",
//    "-isystem/usr/include",
//    "-isystem/usr/include/x86_64-linux-gnu",
//   "-isystem/usr/lib/gcc/x86_64-linux-gnu/11/include"},
//   ArgumentInsertPosition::BEGIN));