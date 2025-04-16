include(CheckCXXSourceCompiles)
include(CheckIncludeFileCXX)
include(CheckSymbolExists)

# Set required compile flags for Clang tests
set(CMAKE_REQUIRED_FLAGS "${LLVM_DEFINITIONS}")
set(CMAKE_REQUIRED_INCLUDES "${LLVM_INCLUDE_DIRS}" "${CLANG_INCLUDE_DIRS}")
set(CMAKE_REQUIRED_LIBRARIES "-L${LLVM_LIBRARY_DIRS}" "${CLANG_LIBRARIES}" "${LLVM_LIBRARIES}")

# Function to perform all Clang feature checks
function(check_clang_features)
  # Get Clang prefix
  execute_process(
    COMMAND ${LLVM_CONFIG_EXEC} --prefix
    OUTPUT_VARIABLE CLANG_PREFIX_VALUE
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE LLVM_CONFIG_RESULT
  )
  if(LLVM_CONFIG_RESULT EQUAL 0 AND CLANG_PREFIX_VALUE)
    set(CLANG_PREFIX "${CLANG_PREFIX_VALUE}" PARENT_SCOPE)
  else()
    message(FATAL_ERROR "Failed to get CLANG_PREFIX from ${LLVM_CONFIG_EXEC}")
  endif()

  # Test for existence of Clang headers
  check_include_file_cxx("clang/Basic/SourceLocation.h" HAVE_CLANG_BASIC_SOURCELOCATION_H)

  # Check for llvm/TargetParser/Host.h
  check_include_file_cxx("llvm/TargetParser/Host.h" HAVE_TARGETPARSER_HOST_H)
  set(HAVE_TARGETPARSER_HOST_H ${HAVE_TARGETPARSER_HOST_H} PARENT_SCOPE)
  if(HAVE_TARGETPARSER_HOST_H)
    # If we have the header, no need to define getDefaultTargetTriple
  else()
    # Check if we have getDefaultTargetTriple in llvm/Support/Host.h
    check_cxx_source_compiles("
      #include <llvm/Support/Host.h>
      int main() {
        llvm::sys::getDefaultTargetTriple();
        return 0;
      }
    " HAVE_GETDEFAULTTARGETTRIPLE)
    if(NOT HAVE_GETDEFAULTTARGETTRIPLE)
      set(getDefaultTargetTriple "getHostTriple" PARENT_SCOPE)
    endif()
  endif()

  # Check for expansion vs instantiation methods
  check_cxx_source_compiles("
    #include <clang/Tooling/Tooling.h>
    #include <clang/Basic/SourceLocation.h>
    int main() {
      using namespace clang;
      SourceManager* SM;
      SourceLocation* Loc;
 
      SourceLocation ExpansionLoc = SM->getExpansionLoc(*Loc);
      auto result = SM->getExpansionLineNumber(ExpansionLoc);
      return 0;
    }
  " HAVE_GETEXPANSIONLINENUMBER)
  if(NOT HAVE_GETEXPANSIONLINENUMBER)
    set(getExpansionLineNumber "getInstantiationLineNumber" PARENT_SCOPE)
  endif()

  check_cxx_source_compiles("
    #include <clang/Tooling/Tooling.h>
    #include <clang/Basic/SourceLocation.h>
    int main() {
      using namespace clang;
      SourceManager* SM;
      SourceLocation* Loc;
 
      SourceLocation ExpansionLoc = SM->getExpansionLoc(*Loc);
      auto result = SM->getExpansionColumnNumber(ExpansionLoc);
      return 0;
    }
  " HAVE_GETEXPANSIONCOLUMNNUMBER)
  if(NOT HAVE_GETEXPANSIONCOLUMNNUMBER)
    set(getExpansionColumnNumber "getInstantiationColumnNumber" PARENT_SCOPE)
  endif()

  check_cxx_source_compiles("
    #include <clang/Basic/SourceManager.h>
    int main() {
      clang::SourceManager* sm;
      sm->getExpansionLoc(clang::SourceLocation());
      return 0;
    }
  " HAVE_GETEXPANSIONLOC)
  if(NOT HAVE_GETEXPANSIONLOC)
    set(getExpansionLoc "getInstantiationLoc" PARENT_SCOPE)
  endif()

  # Check for DiagnosticConsumer vs DiagnosticClient
  check_cxx_source_compiles("
    #include <clang/Basic/Diagnostic.h>
    int main() {
      clang::DiagnosticConsumer* consumer;
      return 0;
    }
  " HAVE_DIAGNOSTIC_CONSUMER)
  if(NOT HAVE_DIAGNOSTIC_CONSUMER)
    set(DiagnosticConsumer "DiagnosticClient" PARENT_SCOPE)
  endif()

  # Check for DiagnosticsEngine vs Diagnostic
  check_cxx_source_compiles("
    #include <clang/Basic/Diagnostic.h>
    int main() {
      clang::DiagnosticsEngine* engine;
      return 0;
    }
  " HAVE_DIAGNOSTICS_ENGINE)
  if(HAVE_DIAGNOSTICS_ENGINE)
    set(DiagnosticInfo "Diagnostic" PARENT_SCOPE)
  else()
    set(DiagnosticsEngine "Diagnostic" PARENT_SCOPE)
  endif()

  # Check for getLocWithOffset vs getFileLocWithOffset
  check_cxx_source_compiles("
    #include <clang/Basic/SourceLocation.h>
    int main() {
      clang::SourceLocation().getLocWithOffset(0);
      return 0;
    }
  " HAVE_GETLOCWITHOFFSET)
  if(NOT HAVE_GETLOCWITHOFFSET)
    set(getLocWithOffset "getFileLocWithOffset" PARENT_SCOPE)
  endif()

  # Test for ArrayRef in Driver::BuildCompilation
  check_cxx_source_compiles("
    #include <clang/Driver/Driver.h>
    int main() {
      using namespace clang;
      driver::Driver* drv;
      llvm::ArrayRef<const char*> args;
      drv->BuildCompilation(args);
      return 0;
    }
  " USE_ARRAYREF)
  set(USE_ARRAYREF ${USE_ARRAYREF} PARENT_SCOPE)

  # Test for DecayedType
  check_include_file_cxx("clang/AST/Type.h" HAVE_TYPE_H)
  if(HAVE_TYPE_H)
    check_cxx_source_compiles("
      #include <clang/AST/Type.h>
      int main() {
        clang::DecayedType* type;
        return 0;
      }
    " HAVE_DECAYEDTYPE)
    set(HAVE_DECAYEDTYPE ${HAVE_DECAYEDTYPE} PARENT_SCOPE)
  endif()

  # Check for HeaderSearchOptions::AddPath taking 4 arguments
  check_include_file_cxx("clang/Lex/HeaderSearchOptions.h" HAVE_LEX_HEADERSEARCHOPTIONS_H)
  set(HAVE_LEX_HEADERSEARCHOPTIONS_H ${HAVE_LEX_HEADERSEARCHOPTIONS_H} PARENT_SCOPE)
  if(HAVE_LEX_HEADERSEARCHOPTIONS_H)
    check_cxx_source_compiles("
      #include <clang/Lex/HeaderSearchOptions.h>
      int main() {
        using namespace clang;
        HeaderSearchOptions HSO;
        HSO.AddPath(\"\", frontend::Angled, false, false);
        return 0;
      }
    " ADDPATH_TAKES_4_ARGUMENTS)
    set(ADDPATH_TAKES_4_ARGUMENTS ${ADDPATH_TAKES_4_ARGUMENTS} PARENT_SCOPE)
  endif()

  # Check for createDiagnostics taking different arguments
  check_cxx_source_compiles("
    #include <clang/Frontend/CompilerInstance.h>
    int main() {
      using namespace clang;
      DiagnosticConsumer *client;
      CompilerInstance *Clang;
      Clang->createDiagnostics(client);
      return 0;
    }
  " HAVE_CREATEDIAGNOSTICS_TAKES_CLIENT)
  if(NOT HAVE_CREATEDIAGNOSTICS_TAKES_CLIENT)
    set(CREATEDIAGNOSTICS_TAKES_ARG 1 PARENT_SCOPE)
  endif()

  # Check for createPreprocessor taking TranslationUnitKind
  check_cxx_source_compiles("
    #include <clang/Frontend/CompilerInstance.h>
    int main() {
      using namespace clang;
      CompilerInstance *Clang;
      Clang->createPreprocessor(TU_Complete);
      return 0;
    }
  " CREATEPREPROCESSOR_TAKES_TUKIND)
  set(CREATEPREPROCESSOR_TAKES_TUKIND ${CREATEPREPROCESSOR_TAKES_TUKIND} PARENT_SCOPE)

  # Check for TargetInfo::CreateTargetInfo taking pointer or shared_ptr
  check_cxx_source_compiles("
    #include <clang/Basic/TargetInfo.h>
    int main() {
      using namespace clang;
      TargetOptions *TO;
      DiagnosticsEngine *Diags;
      TargetInfo::CreateTargetInfo(*Diags, TO);
      return 0;
    }
  " CREATETARGETINFO_TAKES_POINTER)
  set(CREATETARGETINFO_TAKES_POINTER ${CREATETARGETINFO_TAKES_POINTER} PARENT_SCOPE)

  check_cxx_source_compiles("
    #include <clang/Basic/TargetInfo.h>
    #include <memory>
    int main() {
      using namespace clang;
      std::shared_ptr<TargetOptions> TO;
      DiagnosticsEngine *Diags;
      TargetInfo::CreateTargetInfo(*Diags, TO);
      return 0;
    }
  " CREATETARGETINFO_TAKES_SHARED_PTR)
  set(CREATETARGETINFO_TAKES_SHARED_PTR ${CREATETARGETINFO_TAKES_SHARED_PTR} PARENT_SCOPE)

  # Check for CompilerInvocation::CreateFromArgs taking ArrayRef
  check_cxx_source_compiles("
    #include <clang/Frontend/CompilerInvocation.h>
    int main() {
      using namespace clang;
      llvm::ArrayRef<const char*> Args;
      clang::DiagnosticsEngine* DE;
      CompilerInvocation* CI;
      CompilerInvocation::CreateFromArgs(*CI, Args, *DE);
      return 0;
    }
  " CREATE_FROM_ARGS_TAKES_ARRAYREF)
  set(CREATE_FROM_ARGS_TAKES_ARRAYREF ${CREATE_FROM_ARGS_TAKES_ARRAYREF} PARENT_SCOPE)

  # Check if Driver constructor takes default image name
  check_cxx_source_compiles("
    #include <clang/Driver/Driver.h>
    int main() {
      using namespace clang;
      DiagnosticsEngine *Diags;
      new driver::Driver(\"\", \"\", \"\", *Diags);
      return 0;
    }
  " DRIVER_CTOR_TAKES_DEFAULTIMAGENAME)
  set(DRIVER_CTOR_TAKES_DEFAULTIMAGENAME ${DRIVER_CTOR_TAKES_DEFAULTIMAGENAME} PARENT_SCOPE)

  # Check for CXXIsProduction argument in Driver constructor
  check_cxx_source_compiles("
    #include <clang/Driver/Driver.h>
    int main() {
      using namespace clang;
      DiagnosticsEngine *Diags;
      new driver::Driver(\"\", \"\", *Diags, true);
      return 0;
    }
  " HAVE_CXXISPRODUCTION)
  set(HAVE_CXXISPRODUCTION ${HAVE_CXXISPRODUCTION} PARENT_SCOPE)

  # Check for IsProduction argument in Driver constructor
  check_cxx_source_compiles("
    #include <clang/Driver/Driver.h>
    int main() {
      using namespace clang;
      DiagnosticsEngine *Diags;
      new driver::Driver(\"\", \"\", *Diags, false);
      return 0;
    }
  " HAVE_ISPRODUCTION)
  set(HAVE_ISPRODUCTION ${HAVE_ISPRODUCTION} PARENT_SCOPE)

  # Check for getTypeInfo returning TypeInfo object
  check_cxx_source_compiles("
    #include <clang/AST/ASTContext.h>
    int main() {
      clang::ASTContext* context;
      clang::QualType type;
      clang::TypeInfo ti = context->getTypeInfo(type);
      return 0;
    }
  " GETTYPEINFORETURNSTYPEINFO)
  set(GETTYPEINFORETURNSTYPEINFO ${GETTYPEINFORETURNSTYPEINFO} PARENT_SCOPE)

  # Check for various headers
  check_include_file_cxx("llvm/ADT/OwningPtr.h" HAVE_ADT_OWNINGPTR_H)
  set(HAVE_ADT_OWNINGPTR_H ${HAVE_ADT_OWNINGPTR_H} PARENT_SCOPE)

  check_include_file_cxx("clang/Basic/DiagnosticOptions.h" HAVE_BASIC_DIAGNOSTICOPTIONS_H)
  set(HAVE_BASIC_DIAGNOSTICOPTIONS_H ${HAVE_BASIC_DIAGNOSTICOPTIONS_H} PARENT_SCOPE)

  check_include_file_cxx("clang/Lex/PreprocessorOptions.h" HAVE_LEX_PREPROCESSOROPTIONS_H)
  set(HAVE_LEX_PREPROCESSOROPTIONS_H ${HAVE_LEX_PREPROCESSOROPTIONS_H} PARENT_SCOPE)

  # Check for getBeginLoc and getEndLoc methods
  check_cxx_source_compiles("
    #include <clang/AST/Decl.h>
    int main() {
      clang::FunctionDecl *fd;
      fd->getBeginLoc();
      fd->getEndLoc();
      return 0;
    }
  " HAVE_BEGIN_END_LOC)
  set(HAVE_BEGIN_END_LOC ${HAVE_BEGIN_END_LOC} PARENT_SCOPE)

  check_include_file_cxx("clang/Basic/LangStandard.h" HAVE_CLANG_BASIC_LANGSTANDARD_H)
  set(HAVE_CLANG_BASIC_LANGSTANDARD_H ${HAVE_CLANG_BASIC_LANGSTANDARD_H} PARENT_SCOPE)

  # Check for findLocationAfterToken method
  check_cxx_source_compiles("
    #include <clang/Lex/Lexer.h>
    int main() {
      clang::SourceManager* sm;
      clang::SourceLocation loc;
      clang::tok::TokenKind kind;
      bool invalid;
      clang::Lexer::findLocationAfterToken(loc, kind, *sm, clang::LangOptions(), false);
      return 0;
    }
  " HAVE_FINDLOCATIONAFTERTOKEN)
  set(HAVE_FINDLOCATIONAFTERTOKEN ${HAVE_FINDLOCATIONAFTERTOKEN} PARENT_SCOPE)

  # Check for llvm/Option/Arg.h
  check_include_file_cxx("llvm/Option/Arg.h" HAVE_LLVM_OPTION_ARG_H)
  set(HAVE_LLVM_OPTION_ARG_H ${HAVE_LLVM_OPTION_ARG_H} PARENT_SCOPE)

  # Check for setMainFileID method
  check_cxx_source_compiles("
    #include <clang/Basic/SourceManager.h>
    int main() {
      clang::SourceManager* sm;
      clang::FileID id;
      sm->setMainFileID(id);
      return 0;
    }
  " HAVE_SETMAINFILEID)
  set(HAVE_SETMAINFILEID ${HAVE_SETMAINFILEID} PARENT_SCOPE)

  # Check for setDiagnosticGroupWarningAsError method
  check_cxx_source_compiles("
    #include <clang/Basic/Diagnostic.h>
    int main() {
      clang::DiagnosticsEngine* engine;
      engine->setDiagnosticGroupWarningAsError(\"\", true);
      return 0;
    }
  " HAVE_SET_DIAGNOSTIC_GROUP_WARNING_AS_ERROR)
  set(HAVE_SET_DIAGNOSTIC_GROUP_WARNING_AS_ERROR ${HAVE_SET_DIAGNOSTIC_GROUP_WARNING_AS_ERROR} PARENT_SCOPE)

  # Check for StmtRange class
  check_cxx_source_compiles("
    #include <clang/AST/StmtIterator.h>
    int main() {
      clang::StmtRange range;
      return 0;
    }
  " HAVE_STMTRANGE)
  set(HAVE_STMTRANGE ${HAVE_STMTRANGE} PARENT_SCOPE)

  # Check for translateLineCol method
  check_cxx_source_compiles("
    #include <clang/Basic/SourceManager.h>
    int main() {
      clang::SourceManager* sm;
      clang::FileID id;
      unsigned line, col;
      sm->translateLineCol(id, line, col);
      return 0;
    }
  " HAVE_TRANSLATELINECOL)
  set(HAVE_TRANSLATELINECOL ${HAVE_TRANSLATELINECOL} PARENT_SCOPE)

  # Check for standard system headers
  check_include_file_cxx("dlfcn.h" HAVE_DLFCN_H)
  set(HAVE_DLFCN_H ${HAVE_DLFCN_H} PARENT_SCOPE)
  
  check_include_file_cxx("inttypes.h" HAVE_INTTYPES_H)
  set(HAVE_INTTYPES_H ${HAVE_INTTYPES_H} PARENT_SCOPE)
  
  check_include_file_cxx("stdint.h" HAVE_STDINT_H)
  set(HAVE_STDINT_H ${HAVE_STDINT_H} PARENT_SCOPE)
  
  check_include_file_cxx("stdio.h" HAVE_STDIO_H)
  set(HAVE_STDIO_H ${HAVE_STDIO_H} PARENT_SCOPE)
  
  check_include_file_cxx("stdlib.h" HAVE_STDLIB_H)
  set(HAVE_STDLIB_H ${HAVE_STDLIB_H} PARENT_SCOPE)

  check_include_file_cxx("strings.h" HAVE_STRINGS_H)
  set(HAVE_STRINGS_H ${HAVE_STRINGS_H} PARENT_SCOPE)
  
  check_include_file_cxx("string.h" HAVE_STRING_H)
  set(HAVE_STRING_H ${HAVE_STRING_H} PARENT_SCOPE)
  
  check_include_file_cxx("sys/stat.h" HAVE_SYS_STAT_H)
  set(HAVE_SYS_STAT_H ${HAVE_SYS_STAT_H} PARENT_SCOPE)
  
  check_include_file_cxx("sys/types.h" HAVE_SYS_TYPES_H)
  set(HAVE_SYS_TYPES_H ${HAVE_SYS_TYPES_H} PARENT_SCOPE)

  check_include_file_cxx("unistd.h" HAVE_UNISTD_H)
  set(HAVE_UNISTD_H ${HAVE_UNISTD_H} PARENT_SCOPE)

  # Check HandleTopLevelDeclReturn and HandleTopLevelDeclContinue
  # Use a more accurate test that checks the actual method signature
  check_cxx_source_compiles("
    #include <clang/AST/ASTConsumer.h>
    int main() {
      // Try to call HandleTopLevelDecl with void return
      // This compiles if HandleTopLevelDecl returns void, fails otherwise
      clang::ASTConsumer* consumer = nullptr;
      clang::DeclGroupRef ref;
      void (*test_func)(clang::ASTConsumer*, clang::DeclGroupRef) = 
          [](clang::ASTConsumer* c, clang::DeclGroupRef r) { 
              c->HandleTopLevelDecl(r); 
          };
      return 0;
    }
  " HANDLE_TOP_LEVEL_DECL_RETURNS_VOID)
  
  if(HANDLE_TOP_LEVEL_DECL_RETURNS_VOID)
    set(HandleTopLevelDeclReturn "void" PARENT_SCOPE)
    set(HandleTopLevelDeclContinue "" PARENT_SCOPE) # Empty for void return type
  else()
    set(HandleTopLevelDeclReturn "bool" PARENT_SCOPE)
    set(HandleTopLevelDeclContinue "true" PARENT_SCOPE)
  endif()

  # Check IK_C definition
  check_cxx_source_compiles("
    #include <clang/Frontend/FrontendOptions.h>
    int main() {
      auto lang = clang::InputKind::C;
      return 0;
    }
  " HAS_INPUTKIND_LANGUAGE)

  if(HAS_INPUTKIND_LANGUAGE)
    set(IK_C "InputKind::C")
  else()
    check_cxx_source_compiles("
      #include <clang/Basic/LangStandard.h>
      int main() {
        auto lang = clang::Language::C;
        return 0;
      }
    " HAS_LANGUAGE_C)
    
    if(HAS_LANGUAGE_C)
      set(IK_C "Language::C")
    else()
      set(IK_C "InputKind::C")
    endif()
  endif()

  # Check for PragmaIntroducer
  check_cxx_source_compiles("
    #include <clang/Lex/Pragma.h>
    int main() {
      clang::PragmaIntroducer intro;
      return 0;
    }
  " HAS_PRAGMA_INTRODUCER)
  
  if(NOT HAS_PRAGMA_INTRODUCER)
    set(PragmaIntroducer "PragmaIntroducerKind" PARENT_SCOPE)
  endif()

  # Check for SETLANGDEFAULTS definition
  check_cxx_source_compiles("
    #include <clang/Basic/LangOptions.h>
    int main() {
      using namespace clang;
      LangOptions* Opts;
      llvm::Triple* T;
      std::vector<std::string>* I;
      LangOptions::setLangDefaults(*Opts, ${IK_C}, *T, *I);
      return 0;
    }
  " LANGOPT_HAS_SETLANGDEFAULTS)
  
  if(LANGOPT_HAS_SETLANGDEFAULTS)
    set(SETLANGDEFAULTS "LangOptions")
  else()
    set(SETLANGDEFAULTS "CompilerInvocation")
  endif()

  check_cxx_source_compiles("
    #include <clang/Basic/TargetOptions.h>
    #include <clang/Lex/PreprocessorOptions.h>
    #include <clang/Frontend/CompilerInstance.h>
    #include <clang/Basic/LangStandard.h>

    int main() {
      using namespace clang;
      CompilerInstance *Clang;
      llvm::Triple *T;
      std::vector<std::string>* I;
      ${SETLANGDEFAULTS}::setLangDefaults(Clang->getLangOpts(),
                                   ${IK_C},
                                   *T,
                                   *I,
                                   LangStandard::lang_unspecified);
      return 0;
    }
  " SETLANGDEFAULTS_TAKES_5_ARGUMENTS)
  set(SETLANGDEFAULTS_TAKES_5_ARGUMENTS ${SETLANGDEFAULTS_TAKES_5_ARGUMENTS} PARENT_SCOPE)

  set(SETLANGDEFAULTS "${SETLANGDEFAULTS}" PARENT_SCOPE)
  set(IK_C "${IK_C}" PARENT_SCOPE)

  # Check if CompilerInstance::setInvocation takes a shared_ptr
  check_cxx_source_compiles("
    #include <clang/Frontend/CompilerInstance.h>
    #include <clang/Frontend/CompilerInvocation.h>
    #include <memory>
    int main() {
      using namespace clang;
      CompilerInvocation *invocation;
      CompilerInstance *Clang;
      Clang->setInvocation(std::make_shared<CompilerInvocation>(*invocation));
      return 0;
    }
  " SETINVOCATION_TAKES_SHARED_PTR)
  set(SETINVOCATION_TAKES_SHARED_PTR ${SETINVOCATION_TAKES_SHARED_PTR} PARENT_SCOPE)

  # Check standard C headers
  if(HAVE_STDLIB_H AND HAVE_STDINT_H AND HAVE_STRING_H AND HAVE_STDIO_H)
    set(STDC_HEADERS 1 PARENT_SCOPE)
  endif()

  # Check TypedefNameDecl vs TypedefDecl
  check_cxx_source_compiles("
    #include <clang/AST/Type.h>
    int main() {
      clang::TypedefNameDecl* decl;
      return 0;
    }
  " HAS_TYPEDEF_NAME_DECL)
  
  if(NOT HAS_TYPEDEF_NAME_DECL)
    set(TypedefNameDecl "TypedefDecl" PARENT_SCOPE)
    
    # If we don't have TypedefNameDecl, we likely need to check for getTypedefForAnonDecl
    check_cxx_source_compiles("
      #include <clang/AST/Type.h>
      int main() {
        clang::RecordType *rt;
        rt->getTypedefNameForAnonDecl();
        return 0;
      }
    " HAS_GET_TYPEDEF_NAME_FOR_ANON_DECL)
    
    if(NOT HAS_GET_TYPEDEF_NAME_FOR_ANON_DECL)
      set(getTypedefNameForAnonDecl "getTypedefForAnonDecl" PARENT_SCOPE)
    endif()
  endif()

  # Check if ArraySizeModifier is nested
  check_cxx_source_compiles("
    #include <clang/AST/Type.h>
    int main() {
      clang::ArrayType::ArraySizeModifier mod;
      return 0;
    }
  " USE_NESTED_ARRAY_SIZE_MODIFIER)
  set(USE_NESTED_ARRAY_SIZE_MODIFIER ${USE_NESTED_ARRAY_SIZE_MODIFIER} PARENT_SCOPE)

  # Check ext_implicit_function_decl_c99
  check_cxx_source_compiles("
    #include <clang/Basic/DiagnosticCategories.h>
    int main() {
      unsigned diag = clang::diag::ext_implicit_function_decl_c99;
      return 0;
    }
  " HAS_EXT_IMPLICIT_FUNCTION_DECL_C99)
  
  if(NOT HAS_EXT_IMPLICIT_FUNCTION_DECL_C99)
    set(ext_implicit_function_decl_c99 "ext_implicit_function_decl" PARENT_SCOPE)
  endif()

  # Check getReturnType vs getResultType
  check_cxx_source_compiles("
    #include <clang/AST/Decl.h>
    int main() {
      clang::FunctionDecl *fd;
      fd->getReturnType();
      return 0;
    }
  " HAS_GET_RETURN_TYPE)
  
  if(NOT HAS_GET_RETURN_TYPE)
    set(getReturnType "getResultType" PARENT_SCOPE)
  endif()

  # Check for initializeBuiltins
  check_cxx_source_compiles("
    #include <clang/Basic/Builtins.h>
    int main() {
      using namespace clang;
      Builtin::Context context;
      IdentifierTable* IT;
      LangOptions* LO;
      context.initializeBuiltins(*IT, *LO);
      return 0;
    }
  " HAS_INITIALIZE_BUILTINS)
  
  if(NOT HAS_INITIALIZE_BUILTINS)
    set(initializeBuiltins "InitializeBuiltins" PARENT_SCOPE)
  endif()
endfunction()
