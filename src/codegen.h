#ifndef ESIMC_CODEGEN_H
#define ESIMC_CODEGEN_H

#include <map>
#include <string>
#include <vector>
#include <memory>
#include <set>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

class Program;
class VarDeclaration;

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
    // Array metadata: name -> {lowerBound, size}
    std::map<std::string, std::pair<long long, long long>> arrayMeta;
};

struct ArrayInfo {
    llvm::Value* basePtr;    // pointer to start of array data (alloca or loaded ptr)
    llvm::Type* elementType;
    long long lowerBound;
    long long size;
    bool isStackArray;       // true: basePtr is alloca of [N x T]; false: basePtr is ptr to T
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
    llvm::AllocaInst* returnValueAlloca = nullptr;

    // REF type info: variable name -> class name
    std::map<std::string, std::string> refTypes;

    // TEXT variables (have an associated __pos alloca)
    std::set<std::string> textVars;

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
    llvm::Function* textSubFunc = nullptr;
    llvm::Function* textEqFunc = nullptr;

    CodeGenContext();

    void generateCode(Program& program);
    void writeIR(const std::string& filename);
    void declareRuntimeFunctions();

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

    // Label helpers
    llvm::BasicBlock* getOrCreateLabel(const std::string& name);

    // Variable access: returns a pointer to the variable (alloca or GEP for class fields)
    // and the LLVM type of the stored value. Returns {nullptr, nullptr} if not found.
    std::pair<llvm::Value*, llvm::Type*> getVarPtr(const std::string& name);
};

#endif // ESIMC_CODEGEN_H
