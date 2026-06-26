#ifndef ESIMC_CODEGEN_H
#define ESIMC_CODEGEN_H

#include <map>
#include <string>
#include <vector>
#include <memory>
#include <set>
#include <iosfwd>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

class Program;
class VarDeclaration;
class ClassDecl;

struct ClassInfo {
    std::string name;
    std::string parentName;
    int classId;
    llvm::StructType* structType;
    struct FieldInfo {
        std::string name;
        int type; // VarDeclaration::Type or -1 for REF
        std::string refClassName;
        int structIndex;
    };
    std::vector<FieldInfo> fields;
    std::map<std::string, llvm::Function*> methods;
    llvm::Function* bodyFunc;
    int coroFieldIndex;
    std::map<std::string, std::pair<long long, long long>> arrayMeta;

    // Virtual dispatch
    llvm::StructType* vtableType = nullptr;
    llvm::GlobalVariable* vtableGlobal = nullptr;
    std::vector<std::string> vtableMethodOrder; // method names in vtable order
    std::map<std::string, int> vtableIndex; // method name -> index in vtable (0=classId)

    // Declared return type for VIRTUAL methods that have no implementation in
    // this class yet. Lets call sites build the right function type when the
    // concrete override is compiled later. VarDeclaration::Type; -1 void, -2 REF.
    std::map<std::string, int> virtualReturnTypes;

    // Index of the first field that belongs to THIS class (not inherited from parent).
    // Constructor args are stored starting at this index. Set during declareSkeleton.
    int firstOwnFieldIndex = 2;

    // AST node, so subclasses can re-emit the prefix chain's body statements
    // around their own (INNER semantics). Set during declareSkeleton.
    ClassDecl* decl = nullptr;
};

struct ArrayInfo {
    llvm::Value* basePtr;    // pointer to start of array data (alloca or loaded ptr)
    llvm::Type* elementType;
    long long lowerBound;
    long long size;
    bool isStackArray;       // true: basePtr is alloca of [N x T]; false: basePtr is ptr to T
    // 2D array support (stride == 0 means 1D array)
    long long lowerBound2 = 0;
    long long stride = 0;    // size of dim 2 (hi2-lo2+1); 0 = 1D
    // 3D array support (stride2 == 0 means <=2D); constant bounds only
    long long lowerBound3 = 0;
    long long stride2 = 0;   // size of dim 3 (hi3-lo3+1); 0 = not 3D
    // Dynamic second-dimension lower bound (runtime value stored in __lo2 alloca)
    bool hasDynLo2 = false;
    bool hasDynStride = false;
    bool isTextElem = false;  // element type is TEXT (descriptor): := copies in place
};

class CodeGenContext {
public:
    std::unique_ptr<llvm::LLVMContext> llvmContext;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Scope: variable name -> alloca
    std::map<std::string, llvm::AllocaInst*> locals;

    // Array info
    std::map<std::string, ArrayInfo> arrays;

    // Label -> basic block (within current function)
    std::map<std::string, llvm::BasicBlock*> labelBlocks;

    // Class registry
    std::map<std::string, ClassInfo> classes;
    int nextClassId = 1;

    // Current context for class body / procedure codegen
    llvm::Value* currentThis = nullptr;
    std::string currentClassName;
    std::string currentProcName;
    bool insideMethod = false; // true when compiling a method body (nested procs aren't methods)

    // The outermost "this" of the current method (preserved through INSPECT)
    llvm::Value* methodThis = nullptr;
    std::string methodThisClassName;

    // NAME parameters: name -> (pointer, value type) for pass-by-reference
    std::map<std::string, std::pair<llvm::Value*, llvm::Type*>> nameParams;
    llvm::AllocaInst* returnValueAlloca = nullptr;

    // For each procedure name, which parameter indices are NAME (pass-by-reference).
    // Call sites use this to pass the address of the caller's variable instead of
    // the value, even for ptr-typed variables (where the type alone is ambiguous).
    std::map<std::string, std::set<int>> nameParamIndices;

    // For each procedure name, which parameter indices are LABEL (non-local goto
    // targets). Call sites pass a {jmp_buf*, id} record; callees longjmp to it.
    std::map<std::string, std::set<int>> labelParamIndices;

    // Non-local GOTO state for the function currently being compiled:
    llvm::Value* currentJmpBuf = nullptr;            // ptr to this function's jmp_buf
    std::map<std::string, int> nonLocalLabelIds;     // local label name -> setjmp id
    std::set<std::string> labelParamNames;           // names of LABEL params in scope
    llvm::StructType* labelRecordType = nullptr;     // { ptr jmpbuf, i64 id }
    llvm::Function* setjmpFunc = nullptr;
    llvm::Function* longjmpFunc = nullptr;

    // SWITCH declarations: name -> ordered list of label names
    std::map<std::string, std::vector<std::string>> switches;

    // Active INSPECT connections enclosing the current code (outermost first).
    // Identifier lookup falls back to these so nested INSPECT bodies still see
    // the outer connected object's attributes.
    std::vector<std::pair<llvm::Value*, std::string>> inspectStack;

    // REF type info: variable name -> class name
    std::map<std::string, std::string> refTypes;

    // Global variables (from outermost block, accessible by all procedures)
    std::map<std::string, llvm::GlobalVariable*> globals;
    bool inMainBlock = false; // true when generating the outermost block

    // Set whenever a codegen diagnostic is emitted; main exits 1 if set so a
    // dropped statement can never become a silently-wrong binary.
    bool hadError = false;

    // Emit an error diagnostic, prefixed with the source line when known
    // (line > 0). Returns std::cerr so the caller can stream the message.
    std::ostream& errorAt(int line);

    // TEXT variables (named TEXT locals/params/fields in scope)
    std::set<std::string> textVars;

    // Procedure/method names whose declared return type is TEXT, so TEXT-typed
    // call results are recognized for == identity and member dispatch.
    std::set<std::string> textReturningProcs;

    // Runtime functions (C library)
    llvm::Function* printfFunc = nullptr;
    llvm::Function* putsFunc = nullptr;
    llvm::Function* scanfFunc = nullptr;
    llvm::Function* getcharFunc = nullptr;

    // Simula runtime functions
    llvm::Function* allocFunc = nullptr;
    llvm::Function* coroCreateFunc = nullptr;
    llvm::Function* coroStartFunc = nullptr;
    llvm::Function* coroDetachFunc = nullptr;
    llvm::Function* coroResumeFunc = nullptr;
    llvm::Function* blanksFunc = nullptr;
    llvm::Function* textCopyFunc = nullptr;
    llvm::Function* textConcatFunc = nullptr;
    llvm::Function* textLengthFunc = nullptr;
    llvm::Function* textStripFunc = nullptr;
    llvm::Function* inopenFunc = nullptr;
    llvm::Function* inreadlineFunc = nullptr;
    llvm::Function* incloseFunc = nullptr;
    llvm::Function* textSubFunc = nullptr;
    llvm::Function* textEqFunc = nullptr;
    llvm::Function* lastitemFunc = nullptr;

    CodeGenContext();

    // When true, run an LLVM -O2 module pipeline before emitting IR/objects.
    // Disabled by the driver's -O0 flag.
    bool optimize = true;

    void generateCode(Program& program);
    void writeIR(const std::string& filename);
    // Run the default -O2 module optimization pipeline in place (mem2reg, SROA,
    // instcombine, GVN, simplifycfg, inlining, ...). No-op if optimize is false.
    void optimizeModule();
    // Emit a native object file for the host target. Returns false on failure
    // (and sets hadError). Used by the one-step compile-and-link driver.
    bool emitObject(const std::string& filename);
    void declareRuntimeFunctions();

    // Checked-mode runtime guards. Each splits the current basic block, emits a
    // failing path that calls the matching runtime abort (printing a
    // source-located diagnostic), and leaves the builder positioned in the
    // success continuation so codegen proceeds normally.
    void emitDivZeroCheck(llvm::Value* divisor, int line);
    void emitBoundsCheck(llvm::Value* idx, llvm::Value* lo, llvm::Value* hi, int line);
    void emitNilCheck(llvm::Value* refPtr, int line);

    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              const std::string& name,
                                              llvm::Type* type);

    llvm::Type* getLLVMType(int varDeclType);
    llvm::Type* getRefType();

    // Scope management
    std::map<std::string, llvm::AllocaInst*> saveScope();
    void restoreScope(std::map<std::string, llvm::AllocaInst*>& saved);

    // Class helpers
    int getFieldIndex(const std::string& className, const std::string& fieldName);
    llvm::Type* getFieldLLVMType(const std::string& className, const std::string& fieldName);
    std::string resolveRefType(const std::string& varName);
    std::set<int> getClassIdSet(const std::string& className);
    // IDs of className and every class derived from it (for IN / WHEN matching)
    std::set<int> getDescendantIdSet(const std::string& className);
    // True if `method` is declared VIRTUAL in className or any prefix ancestor
    // (so unqualified calls must dispatch dynamically through the vtable).
    bool isVirtualMethod(const std::string& className, const std::string& method);

    // Label helpers
    llvm::BasicBlock* getOrCreateLabel(const std::string& name);
    llvm::Value* makeLabelArg(const std::string& labelName);

    llvm::Value* loadClassId(llvm::Value* obj);

    // Build/rebuild vtables for all classes (called after all ClassDecls)
    void buildAllVtables();

    // Variable access: returns a pointer to the variable (alloca or GEP for class fields)
    // and the LLVM type of the stored value. Returns {nullptr, nullptr} if not found.
    std::pair<llvm::Value*, llvm::Type*> getVarPtr(const std::string& name);

    // Closure support: captured variables from outer scope
    // Maps function name -> list of captured variable names
    std::map<std::string, std::vector<std::string>> capturedVars;
    // Maps function name -> list of captured variable LLVM types
    std::map<std::string, std::vector<llvm::Type*>> capturedTypes;

    // Set up TEXT position tracking for class fields of type TEXT
    void setupTextFieldTracking(llvm::Function* func);
};

#endif // ESIMC_CODEGEN_H
