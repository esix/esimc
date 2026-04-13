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
    // All fields in order: (name, type_enum, fieldIndex_in_struct)
    struct FieldInfo {
        std::string name;
        int type; // VarDeclaration::Type or -1 for REF
        std::string refClassName; // if type == -1
        int structIndex; // index in the LLVM struct
    };
    std::vector<FieldInfo> fields;
    std::map<std::string, llvm::Function*> methods;
    llvm::Function* bodyFunc; // class body function for coroutine
    int coroFieldIndex; // index of coroutine ptr in struct (always 1)
};

class CodeGenContext {
public:
    std::unique_ptr<llvm::LLVMContext> llvmContext;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::IRBuilder<>> builder;

    // Scope: variable name -> alloca
    std::map<std::string, llvm::AllocaInst*> locals;

    // Class registry
    std::map<std::string, ClassInfo> classes;
    int nextClassId = 1;

    // Current context for class body / procedure codegen
    llvm::Value* currentThis = nullptr;
    std::string currentClassName;
    std::string currentProcName; // for return-by-name assignment
    llvm::AllocaInst* returnValueAlloca = nullptr;

    // REF type info: variable name -> class name
    std::map<std::string, std::string> refTypes;

    // Runtime functions (C library)
    llvm::Function* printfFunc = nullptr;
    llvm::Function* putsFunc = nullptr;
    llvm::Function* sprintfFunc = nullptr;

    // Simula runtime functions
    llvm::Function* allocFunc = nullptr;
    llvm::Function* coroCreateFunc = nullptr;
    llvm::Function* coroStartFunc = nullptr;
    llvm::Function* coroDetachFunc = nullptr;
    llvm::Function* coroResumeFunc = nullptr;

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

    // Collect all class IDs for a class and its parents (for IN operator)
    std::set<int> getClassIdSet(const std::string& className);
};

#endif // ESIMC_CODEGEN_H
