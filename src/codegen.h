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
class Statement;
class Expression;

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
    // Dynamic third-dimension bounds (runtime values in __lo3 / __stride2 allocas)
    bool hasDynStride2 = false;
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

    // NAME parameters: name -> (slot pointer, value type). The slot is a local
    // shadow that keeps existing NAME code (getVarPtr, TEXT ops) working. The
    // actual by-name re-evaluation goes through nameThunks below.
    std::map<std::string, std::pair<llvm::Value*, llvm::Type*>> nameParams;
    // Full call-by-name: name -> pointer to a {env, getfn, setfn} thunk struct.
    // Reads call getfn(env) (re-evaluating the actual); assignments call
    // setfn(env, v) (propagating to the actual's lvalue). This is what makes
    // Jensen's device work: an expression actual is re-evaluated per access.
    std::map<std::string, llvm::Value*> nameThunks;
    llvm::StructType* nameThunkTy = nullptr;
    int nameThunkCounter = 0;
    llvm::AllocaInst* returnValueAlloca = nullptr;

    // For each procedure name, which parameter indices are NAME (pass-by-reference).
    // Call sites use this to pass the address of the caller's variable instead of
    // the value, even for ptr-typed variables (where the type alone is ambiguous).
    std::map<std::string, std::set<int>> nameParamIndices;
    // For each procedure with NAME params, the formal's declared LLVM type per
    // index. Thunks are built by the CALLER, which must convert between the
    // actual's type and the formal's type (e.g. INTEGER actual, REAL formal) —
    // the thunk ABI's i64 carrier always holds a formal-typed value.
    std::map<std::string, std::map<int, llvm::Type*>> nameParamFormalTypes;

    // For each procedure name, which parameter indices are LABEL (non-local goto
    // targets). Call sites pass a {jmp_buf*, id} record; callees longjmp to it.
    std::map<std::string, std::set<int>> labelParamIndices;

    // Non-local GOTO state for the function currently being compiled:
    llvm::Value* currentJmpBuf = nullptr;            // ptr to this function's jmp_buf
    std::map<std::string, int> nonLocalLabelIds;     // local label name -> setjmp id
    // Labels of ENCLOSING functions reachable by direct GOTO from nested
    // procedures: label -> (module global holding the owner's live jmp_buf
    // pointer, dispatch id). Static scoping: the owner is active whenever the
    // nested procedure runs, so the global is valid at jump time.
    std::map<std::string, std::pair<llvm::GlobalVariable*, int>> outerNlLabels;
    std::set<std::string> labelParamNames;           // names of LABEL params in scope
    std::set<std::string> procParamNames;            // names of formal PROCEDURE params in scope
    std::map<std::string, int> procParamRetTypes;    // typed formal PROCEDURE result types (VarDeclaration::Type)
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

    // Block nesting depth (a Block bumps it) and the set of names declared in the
    // current block. Together these give correct Algol/Simula block scoping: only
    // the outermost main block (depth 1) makes globals; a nested block re-declaring
    // a name gets fresh shadowing storage; a name is a genuine same-block duplicate
    // only if it is already in blockDeclared.
    int blockDepth = 0;
    std::set<std::string> blockDeclared;

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
    llvm::Function* coroCallFunc = nullptr;
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

    // Scan `body` for labels passed as call arguments (non-local GOTO targets) and,
    // if any exist, emit a setjmp dispatch at the current point so a longjmp from a
    // callee resumes at the right label. Positions the builder at the post-setjmp
    // "body" path. Used for procedure bodies and the outermost program block.
    void emitNonLocalGotoSetup(llvm::Function* func, Statement* body);

    // Checked-mode runtime guards. Each splits the current basic block, emits a
    // failing path that calls the matching runtime abort (printing a
    // source-located diagnostic), and leaves the builder positioned in the
    // success continuation so codegen proceeds normally.
    void emitDivZeroCheck(llvm::Value* divisor, int line);
    void emitNilCheck(llvm::Value* refPtr, int line);
    // Bounds-check one array subscript: idx must lie in [lo, lo+size-1], where
    // size is that dimension's element count (a constant for constant-bound
    // arrays, a runtime value for dynamic ones). Applied per dimension so an
    // out-of-range subscript is caught even when it would wrap within the
    // allocation (e.g. G(2,5) in G(1:3,1:3)).
    void emitDimCheck(llvm::Value* idx, llvm::Value* lo, llvm::Value* size, int line);
    // First-dimension element count for bounds checking: a constant for
    // constant-bound arrays, the runtime __n1 alloca for dynamic ones. Returns
    // null when unknown (e.g. a closure-captured dynamic array) so the caller
    // can skip the check rather than emit a wrong one.
    llvm::Value* arrayFirstDimSize(const std::string& name, const ArrayInfo& info);

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
    // Static qualification (REF class) of any object expression: identifier, THIS,
    // QUA, NEW, a REF-returning proc/method call, or a chained member access
    // (obj.field / obj.method / obj.proc.attr). "" if it can't be determined.
    std::string resolveObjectRefClass(Expression* e);
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

    // Call-by-name (NAME parameter) thunk support.
    llvm::StructType* getNameThunkType();
    // Build a {env,get,set} thunk for `actual` in the current scope; returns a
    // pointer to a stack-allocated thunk struct to pass as the NAME argument.
    // formalTy is the callee formal's declared type: get() converts the actual's
    // value to it, set() converts from it back to the actual's type.
    llvm::Value* buildNameThunk(Expression* actual, llvm::Type* formalTy = nullptr);
    // Emit a call to the thunk's getfn, returning the actual's current value as `type`.
    llvm::Value* emitNameThunkGet(const std::string& name, llvm::Type* type);
    // Emit a call to the thunk's setfn, propagating `val` to the actual's lvalue.
    void emitNameThunkSet(const std::string& name, llvm::Value* val);
    // Re-evaluate a NAME TEXT formal's actual into its shadow slot (call-by-name
    // for descriptor accesses that go through getVarPtr rather than Identifier).
    void refreshNameText(const std::string& name);
    // Compute the address of an array element a(i[,j]) using current bounds.
    llvm::Value* emitArrayElemAddr(const ArrayInfo& ainfo, const std::string& name,
                                   const std::vector<std::unique_ptr<Expression>>& idxArgs);

    // Closure support: captured variables from outer scope
    // Maps function name -> list of captured variable names
    std::map<std::string, std::vector<std::string>> capturedVars;
    // Maps function name -> list of captured variable LLVM types
    std::map<std::string, std::vector<llvm::Type*>> capturedTypes;

    // Set up TEXT position tracking for class fields of type TEXT
    void setupTextFieldTracking(llvm::Function* func);
};

#endif // ESIMC_CODEGEN_H
