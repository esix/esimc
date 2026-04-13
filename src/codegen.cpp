#include "codegen.h"
#include "ast.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <algorithm>

// ============================================================
// CodeGenContext implementation
// ============================================================

CodeGenContext::CodeGenContext() {
    llvmContext = std::make_unique<llvm::LLVMContext>();
    module = std::make_unique<llvm::Module>("esimc", *llvmContext);
    builder = std::make_unique<llvm::IRBuilder<>>(*llvmContext);
}

llvm::Type* CodeGenContext::getLLVMType(int varDeclType) {
    switch (varDeclType) {
        case VarDeclaration::INTEGER: return llvm::Type::getInt64Ty(*llvmContext);
        case VarDeclaration::REAL:    return llvm::Type::getDoubleTy(*llvmContext);
        case VarDeclaration::BOOLEAN: return llvm::Type::getInt1Ty(*llvmContext);
        case VarDeclaration::TEXT:    return llvm::PointerType::getUnqual(*llvmContext);
        default:                      return llvm::Type::getInt64Ty(*llvmContext);
    }
}

llvm::Type* CodeGenContext::getRefType() {
    return llvm::PointerType::getUnqual(*llvmContext);
}

std::map<std::string, llvm::AllocaInst*> CodeGenContext::saveScope() {
    return locals;
}

void CodeGenContext::restoreScope(std::map<std::string, llvm::AllocaInst*>& saved) {
    locals = saved;
}

int CodeGenContext::getFieldIndex(const std::string& className, const std::string& fieldName) {
    auto it = classes.find(className);
    if (it == classes.end()) return -1;
    for (auto& f : it->second.fields) {
        if (f.name == fieldName) return f.structIndex;
    }
    return -1;
}

llvm::Type* CodeGenContext::getFieldLLVMType(const std::string& className, const std::string& fieldName) {
    auto it = classes.find(className);
    if (it == classes.end()) return nullptr;
    for (auto& f : it->second.fields) {
        if (f.name == fieldName) {
            if (f.type == -1) return getRefType(); // REF field
            return getLLVMType(f.type);
        }
    }
    return nullptr;
}

std::string CodeGenContext::resolveRefType(const std::string& varName) {
    auto it = refTypes.find(varName);
    if (it != refTypes.end()) return it->second;
    return "";
}

std::set<int> CodeGenContext::getClassIdSet(const std::string& className) {
    std::set<int> ids;
    auto it = classes.find(className);
    while (it != classes.end()) {
        ids.insert(it->second.classId);
        if (it->second.parentName.empty()) break;
        it = classes.find(it->second.parentName);
    }
    return ids;
}

void CodeGenContext::declareRuntimeFunctions() {
    auto ptrTy = llvm::PointerType::getUnqual(*llvmContext);
    auto i32Ty = llvm::Type::getInt32Ty(*llvmContext);
    auto i64Ty = llvm::Type::getInt64Ty(*llvmContext);
    auto voidTy = llvm::Type::getVoidTy(*llvmContext);

    // printf
    printfFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, {ptrTy}, true),
        llvm::Function::ExternalLinkage, "printf", module.get());

    // puts
    putsFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "puts", module.get());

    // simula_alloc(i64) -> ptr
    allocFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {i64Ty}, false),
        llvm::Function::ExternalLinkage, "simula_alloc", module.get());

    // simula_coro_create() -> ptr
    coroCreateFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, false),
        llvm::Function::ExternalLinkage, "simula_coro_create", module.get());

    // simula_coro_start(ptr coro, ptr func, ptr arg)
    coroStartFunc = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_coro_start", module.get());

    // simula_coro_detach(ptr coro)
    coroDetachFunc = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_coro_detach", module.get());

    // simula_coro_resume(ptr coro)
    coroResumeFunc = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_coro_resume", module.get());
}

llvm::AllocaInst* CodeGenContext::createEntryBlockAlloca(
    llvm::Function* func, const std::string& name, llvm::Type* type) {
    llvm::IRBuilder<> tmpBuilder(&func->getEntryBlock(),
                                  func->getEntryBlock().begin());
    return tmpBuilder.CreateAlloca(type, nullptr, name);
}

void CodeGenContext::generateCode(Program& program) {
    declareRuntimeFunctions();

    // Create main function
    auto mainType = llvm::FunctionType::get(
        llvm::Type::getInt32Ty(*llvmContext), false);
    auto mainFunc = llvm::Function::Create(
        mainType, llvm::Function::ExternalLinkage, "main", module.get());

    auto entry = llvm::BasicBlock::Create(*llvmContext, "entry", mainFunc);
    builder->SetInsertPoint(entry);

    program.codegen(*this);

    builder->CreateRet(llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(*llvmContext), 0));

    if (llvm::verifyFunction(*mainFunc, &llvm::errs())) {
        std::cerr << "Error: generated main function is invalid\n";
    }
    if (llvm::verifyModule(*module, &llvm::errs())) {
        std::cerr << "Error: generated module is invalid\n";
    }
}

void CodeGenContext::writeIR(const std::string& filename) {
    std::error_code ec;
    llvm::raw_fd_ostream out(filename, ec);
    if (ec) {
        std::cerr << "Error opening output file: " << ec.message() << "\n";
        return;
    }
    module->print(out, nullptr);
}

// ============================================================
// Expression codegen
// ============================================================

llvm::Value* IntegerLiteral::codegen(CodeGenContext& ctx) {
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), value);
}

llvm::Value* RealLiteral::codegen(CodeGenContext& ctx) {
    return llvm::ConstantFP::get(llvm::Type::getDoubleTy(*ctx.llvmContext), value);
}

llvm::Value* TextLiteral::codegen(CodeGenContext& ctx) {
    return ctx.builder->CreateGlobalString(value, "str");
}

llvm::Value* BooleanLiteral::codegen(CodeGenContext& ctx) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(*ctx.llvmContext), value ? 1 : 0);
}

llvm::Value* NoneLiteral::codegen(CodeGenContext& ctx) {
    return llvm::ConstantPointerNull::get(
        llvm::PointerType::getUnqual(*ctx.llvmContext));
}

llvm::Value* Identifier::codegen(CodeGenContext& ctx) {
    // Check if this is the return variable of a typed procedure
    if (name == ctx.currentProcName && ctx.returnValueAlloca) {
        return ctx.builder->CreateLoad(ctx.returnValueAlloca->getAllocatedType(),
                                        ctx.returnValueAlloca, name);
    }

    auto it = ctx.locals.find(name);
    if (it != ctx.locals.end()) {
        return ctx.builder->CreateLoad(it->second->getAllocatedType(),
                                        it->second, name);
    }

    // Check if it's a class field accessed from within the class body
    if (ctx.currentThis && !ctx.currentClassName.empty()) {
        int idx = ctx.getFieldIndex(ctx.currentClassName, name);
        if (idx >= 0) {
            auto& ci = ctx.classes[ctx.currentClassName];
            auto gep = ctx.builder->CreateStructGEP(ci.structType, ctx.currentThis, idx, name);
            auto fieldTy = ci.structType->getElementType(idx);
            return ctx.builder->CreateLoad(fieldTy, gep, name);
        }
    }

    std::cerr << "Error: unknown variable '" << name << "'\n";
    return nullptr;
}

llvm::Value* BinaryOp::codegen(CodeGenContext& ctx) {
    auto L = lhs->codegen(ctx);
    auto R = rhs->codegen(ctx);
    if (!L || !R) return nullptr;

    bool isReal = L->getType()->isDoubleTy() || R->getType()->isDoubleTy();

    // Promote to double if mixed
    if (isReal) {
        if (!L->getType()->isDoubleTy())
            L = ctx.builder->CreateSIToFP(L, llvm::Type::getDoubleTy(*ctx.llvmContext), "tofp");
        if (!R->getType()->isDoubleTy())
            R = ctx.builder->CreateSIToFP(R, llvm::Type::getDoubleTy(*ctx.llvmContext), "tofp");
    }

    if (isReal) {
        switch (op) {
            case ADD: return ctx.builder->CreateFAdd(L, R, "addtmp");
            case SUB: return ctx.builder->CreateFSub(L, R, "subtmp");
            case MUL: return ctx.builder->CreateFMul(L, R, "multmp");
            case DIV: return ctx.builder->CreateFDiv(L, R, "divtmp");
            case IDIV: {
                auto d = ctx.builder->CreateFDiv(L, R, "divtmp");
                return ctx.builder->CreateFPToSI(d, llvm::Type::getInt64Ty(*ctx.llvmContext), "idivtmp");
            }
            case EQ:  return ctx.builder->CreateFCmpOEQ(L, R, "eqtmp");
            case NE:  return ctx.builder->CreateFCmpONE(L, R, "netmp");
            case LT:  return ctx.builder->CreateFCmpOLT(L, R, "lttmp");
            case LE:  return ctx.builder->CreateFCmpOLE(L, R, "letmp");
            case GT:  return ctx.builder->CreateFCmpOGT(L, R, "gttmp");
            case GE:  return ctx.builder->CreateFCmpOGE(L, R, "getmp");
            case AND: case OR: break; // shouldn't happen with reals
        }
    }

    switch (op) {
        case ADD: return ctx.builder->CreateAdd(L, R, "addtmp");
        case SUB: return ctx.builder->CreateSub(L, R, "subtmp");
        case MUL: return ctx.builder->CreateMul(L, R, "multmp");
        case DIV: return ctx.builder->CreateSDiv(L, R, "divtmp");
        case IDIV: return ctx.builder->CreateSDiv(L, R, "idivtmp");
        case EQ:  return ctx.builder->CreateICmpEQ(L, R, "eqtmp");
        case NE:  return ctx.builder->CreateICmpNE(L, R, "netmp");
        case LT:  return ctx.builder->CreateICmpSLT(L, R, "lttmp");
        case LE:  return ctx.builder->CreateICmpSLE(L, R, "letmp");
        case GT:  return ctx.builder->CreateICmpSGT(L, R, "gttmp");
        case GE:  return ctx.builder->CreateICmpSGE(L, R, "getmp");
        case AND: return ctx.builder->CreateAnd(L, R, "andtmp");
        case OR:  return ctx.builder->CreateOr(L, R, "ortmp");
    }
    return nullptr;
}

llvm::Value* UnaryOp::codegen(CodeGenContext& ctx) {
    auto V = operand->codegen(ctx);
    if (!V) return nullptr;
    switch (op) {
        case NEG:
            if (V->getType()->isDoubleTy())
                return ctx.builder->CreateFNeg(V, "negtmp");
            return ctx.builder->CreateNeg(V, "negtmp");
        case NOT: return ctx.builder->CreateNot(V, "nottmp");
    }
    return nullptr;
}

llvm::Value* ProcedureCall::codegen(CodeGenContext& ctx) {
    // Check if it's a method call on THIS (within a class)
    if (ctx.currentThis && !ctx.currentClassName.empty()) {
        auto& ci = ctx.classes[ctx.currentClassName];
        auto mit = ci.methods.find(name);
        if (mit != ci.methods.end()) {
            std::vector<llvm::Value*> argsV;
            argsV.push_back(ctx.currentThis);
            for (auto& arg : args) {
                auto v = arg->codegen(ctx);
                if (!v) return nullptr;
                argsV.push_back(v);
            }
            return ctx.builder->CreateCall(mit->second, argsV);
        }
    }

    auto func = ctx.module->getFunction(name);
    if (!func) {
        std::cerr << "Error: unknown function '" << name << "'\n";
        return nullptr;
    }
    std::vector<llvm::Value*> argsV;
    for (auto& arg : args) {
        auto v = arg->codegen(ctx);
        if (!v) return nullptr;
        argsV.push_back(v);
    }
    return ctx.builder->CreateCall(func, argsV);
}

llvm::Value* NewExpression::codegen(CodeGenContext& ctx) {
    auto it = ctx.classes.find(className);
    if (it == ctx.classes.end()) {
        std::cerr << "Error: unknown class '" << className << "'\n";
        return nullptr;
    }
    auto& ci = it->second;

    // Allocate the object
    auto sizeofStruct = ctx.module->getDataLayout().getTypeAllocSize(ci.structType);
    auto obj = ctx.builder->CreateCall(ctx.allocFunc,
        {llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), sizeofStruct)});

    // Store class ID at index 0
    auto classIdPtr = ctx.builder->CreateStructGEP(ci.structType, obj, 0, "classid_ptr");
    ctx.builder->CreateStore(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), ci.classId),
        classIdPtr);

    // Create coroutine context and store at index 1
    auto coro = ctx.builder->CreateCall(ctx.coroCreateFunc);
    auto coroPtr = ctx.builder->CreateStructGEP(ci.structType, obj, 1, "coro_ptr");
    ctx.builder->CreateStore(coro, coroPtr);

    // Store constructor arguments into fields
    // Constructor params start at field index 2
    size_t argIdx = 0;
    for (auto& param : ci.fields) {
        if (param.structIndex < 2) continue; // skip classId and coro
        if (argIdx < args.size()) {
            auto val = args[argIdx]->codegen(ctx);
            if (val) {
                auto fieldPtr = ctx.builder->CreateStructGEP(
                    ci.structType, obj, param.structIndex, param.name + "_ptr");
                // Type convert if needed
                auto destTy = ci.structType->getElementType(param.structIndex);
                if (val->getType() != destTy) {
                    if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
                        val = ctx.builder->CreateSIToFP(val, destTy);
                    else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                        val = ctx.builder->CreateFPToSI(val, destTy);
                }
                ctx.builder->CreateStore(val, fieldPtr);
            }
            argIdx++;
        }
    }

    // Start the class body as a coroutine
    if (ci.bodyFunc) {
        ctx.builder->CreateCall(ctx.coroStartFunc, {coro, ci.bodyFunc, obj});
    }

    return obj;
}

llvm::Value* MemberAccess::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    // Determine the class name from the object
    std::string clsName;

    // Check if the object expression is an Identifier and look up its ref type
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && !ctx.currentClassName.empty()) {
        // Could be THIS
        if (dynamic_cast<ThisExpression*>(object.get())) {
            clsName = ctx.currentClassName;
        }
    }

    if (clsName.empty()) {
        std::cerr << "Error: cannot determine class type for member access '." << member << "'\n";
        return nullptr;
    }

    int idx = ctx.getFieldIndex(clsName, member);
    if (idx < 0) {
        std::cerr << "Error: class '" << clsName << "' has no field '" << member << "'\n";
        return nullptr;
    }

    auto& ci = ctx.classes[clsName];
    auto gep = ctx.builder->CreateStructGEP(ci.structType, obj, idx, member + "_ptr");
    auto fieldTy = ci.structType->getElementType(idx);
    return ctx.builder->CreateLoad(fieldTy, gep, member);
}

llvm::Value* MethodCall::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    std::string clsName;
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }

    if (clsName.empty()) {
        std::cerr << "Error: cannot determine class type for method call '." << method << "'\n";
        return nullptr;
    }

    // Look up the method in the class hierarchy
    std::string searchClass = clsName;
    llvm::Function* methodFunc = nullptr;
    while (!searchClass.empty()) {
        auto cit = ctx.classes.find(searchClass);
        if (cit == ctx.classes.end()) break;
        auto mit = cit->second.methods.find(method);
        if (mit != cit->second.methods.end()) {
            methodFunc = mit->second;
            break;
        }
        searchClass = cit->second.parentName;
    }

    if (!methodFunc) {
        std::cerr << "Error: class '" << clsName << "' has no method '" << method << "'\n";
        return nullptr;
    }

    std::vector<llvm::Value*> argsV;
    argsV.push_back(obj); // this pointer
    for (auto& arg : args) {
        auto v = arg->codegen(ctx);
        if (!v) return nullptr;
        argsV.push_back(v);
    }
    return ctx.builder->CreateCall(methodFunc, argsV);
}

llvm::Value* ThisExpression::codegen(CodeGenContext& ctx) {
    if (!ctx.currentThis) {
        std::cerr << "Error: THIS used outside of a class body\n";
        return nullptr;
    }
    return ctx.currentThis;
}

llvm::Value* IsExpression::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto cit = ctx.classes.find(className);
    if (cit == ctx.classes.end()) {
        std::cerr << "Error: unknown class '" << className << "' in IS expression\n";
        return nullptr;
    }

    // Load class ID from object (field index 0)
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);

    // Use a generic two-field struct to access the class ID
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {i64Ty, ptrTy});
    auto idPtr = ctx.builder->CreateStructGEP(baseTy, obj, 0, "classid_ptr");
    auto classId = ctx.builder->CreateLoad(i64Ty, idPtr, "classid");

    return ctx.builder->CreateICmpEQ(classId,
        llvm::ConstantInt::get(i64Ty, cit->second.classId), "is_check");
}

llvm::Value* InExpression::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto ids = ctx.getClassIdSet(className);
    if (ids.empty()) {
        std::cerr << "Error: unknown class '" << className << "' in IN expression\n";
        return nullptr;
    }

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {i64Ty, ptrTy});
    auto idPtr = ctx.builder->CreateStructGEP(baseTy, obj, 0, "classid_ptr");
    auto classId = ctx.builder->CreateLoad(i64Ty, idPtr, "classid");

    // OR together checks for each class ID in the hierarchy
    llvm::Value* result = ctx.builder->getFalse();
    for (int id : ids) {
        auto cmp = ctx.builder->CreateICmpEQ(classId,
            llvm::ConstantInt::get(i64Ty, id), "in_cmp");
        result = ctx.builder->CreateOr(result, cmp, "in_or");
    }
    return result;
}

// ============================================================
// Statement codegen
// ============================================================

llvm::Value* Block::codegen(CodeGenContext& ctx) {
    auto saved = ctx.saveScope();
    auto savedRefTypes = ctx.refTypes;
    llvm::Value* last = nullptr;
    for (auto& stmt : statements) {
        last = stmt->codegen(ctx);
    }
    ctx.restoreScope(saved);
    ctx.refTypes = savedRefTypes;
    return last;
}

llvm::Value* VarDeclaration::codegen(CodeGenContext& ctx) {
    auto ty = ctx.getLLVMType(type);
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, name, ty);
    ctx.locals[name] = alloca;

    if (init) {
        auto val = init->codegen(ctx);
        if (val) ctx.builder->CreateStore(val, alloca);
    } else {
        ctx.builder->CreateStore(llvm::Constant::getNullValue(ty), alloca);
    }
    return alloca;
}

llvm::Value* RefDeclaration::codegen(CodeGenContext& ctx) {
    auto ty = ctx.getRefType();
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, varName, ty);
    ctx.locals[varName] = alloca;
    ctx.refTypes[varName] = className;

    // Initialize to null
    ctx.builder->CreateStore(
        llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx.llvmContext)),
        alloca);
    return alloca;
}

llvm::Value* Assignment::codegen(CodeGenContext& ctx) {
    // Check if assigning to procedure return variable
    if (name == ctx.currentProcName && ctx.returnValueAlloca) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
        ctx.builder->CreateStore(val, ctx.returnValueAlloca);
        return val;
    }

    auto it = ctx.locals.find(name);
    if (it != ctx.locals.end()) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
        // Type convert if needed
        auto destTy = it->second->getAllocatedType();
        if (val->getType() != destTy) {
            if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
                val = ctx.builder->CreateSIToFP(val, destTy);
            else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                val = ctx.builder->CreateFPToSI(val, destTy);
        }
        ctx.builder->CreateStore(val, it->second);
        return val;
    }

    // Check class field
    if (ctx.currentThis && !ctx.currentClassName.empty()) {
        int idx = ctx.getFieldIndex(ctx.currentClassName, name);
        if (idx >= 0) {
            auto val = value->codegen(ctx);
            if (!val) return nullptr;
            auto& ci = ctx.classes[ctx.currentClassName];
            auto gep = ctx.builder->CreateStructGEP(ci.structType, ctx.currentThis, idx, name + "_ptr");
            auto destTy = ci.structType->getElementType(idx);
            if (val->getType() != destTy) {
                if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
                    val = ctx.builder->CreateSIToFP(val, destTy);
                else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                    val = ctx.builder->CreateFPToSI(val, destTy);
            }
            ctx.builder->CreateStore(val, gep);
            return val;
        }
    }

    std::cerr << "Error: unknown variable '" << name << "'\n";
    return nullptr;
}

llvm::Value* MemberAssignment::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    std::string clsName;
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }
    if (clsName.empty()) {
        std::cerr << "Error: cannot determine class for member assignment '." << member << "'\n";
        return nullptr;
    }

    int idx = ctx.getFieldIndex(clsName, member);
    if (idx < 0) {
        std::cerr << "Error: class '" << clsName << "' has no field '" << member << "'\n";
        return nullptr;
    }

    auto val = value->codegen(ctx);
    if (!val) return nullptr;

    auto& ci = ctx.classes[clsName];
    auto gep = ctx.builder->CreateStructGEP(ci.structType, obj, idx, member + "_ptr");
    auto destTy = ci.structType->getElementType(idx);
    if (val->getType() != destTy) {
        if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, destTy);
        else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
            val = ctx.builder->CreateFPToSI(val, destTy);
    }
    ctx.builder->CreateStore(val, gep);
    return val;
}

llvm::Value* RefAssignment::codegen(CodeGenContext& ctx) {
    auto it = ctx.locals.find(name);
    if (it == ctx.locals.end()) {
        std::cerr << "Error: unknown REF variable '" << name << "'\n";
        return nullptr;
    }
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    ctx.builder->CreateStore(val, it->second);
    return val;
}

llvm::Value* ExprStatement::codegen(CodeGenContext& ctx) {
    return expr->codegen(ctx);
}

llvm::Value* IfStatement::codegen(CodeGenContext& ctx) {
    auto condV = condition->codegen(ctx);
    if (!condV) return nullptr;

    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(*ctx.llvmContext, "then", func);
    auto elseBB = llvm::BasicBlock::Create(*ctx.llvmContext, "else");
    auto mergeBB = llvm::BasicBlock::Create(*ctx.llvmContext, "ifcont");

    if (elseBranch) {
        ctx.builder->CreateCondBr(condV, thenBB, elseBB);
    } else {
        ctx.builder->CreateCondBr(condV, thenBB, mergeBB);
    }

    ctx.builder->SetInsertPoint(thenBB);
    thenBranch->codegen(ctx);
    ctx.builder->CreateBr(mergeBB);

    if (elseBranch) {
        func->insert(func->end(), elseBB);
        ctx.builder->SetInsertPoint(elseBB);
        elseBranch->codegen(ctx);
        ctx.builder->CreateBr(mergeBB);
    }

    func->insert(func->end(), mergeBB);
    ctx.builder->SetInsertPoint(mergeBB);
    return nullptr;
}

llvm::Value* WhileStatement::codegen(CodeGenContext& ctx) {
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto condBB = llvm::BasicBlock::Create(*ctx.llvmContext, "whcond", func);
    auto bodyBB = llvm::BasicBlock::Create(*ctx.llvmContext, "whbody");
    auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "whend");

    ctx.builder->CreateBr(condBB);
    ctx.builder->SetInsertPoint(condBB);

    auto condV = condition->codegen(ctx);
    ctx.builder->CreateCondBr(condV, bodyBB, afterBB);

    func->insert(func->end(), bodyBB);
    ctx.builder->SetInsertPoint(bodyBB);
    body->codegen(ctx);
    ctx.builder->CreateBr(condBB);

    func->insert(func->end(), afterBB);
    ctx.builder->SetInsertPoint(afterBB);
    return nullptr;
}

llvm::Value* ForStatement::codegen(CodeGenContext& ctx) {
    auto func = ctx.builder->GetInsertBlock()->getParent();

    auto startV = start->codegen(ctx);
    auto it = ctx.locals.find(var);
    if (it == ctx.locals.end()) {
        std::cerr << "Error: FOR variable '" << var << "' not declared\n";
        return nullptr;
    }
    ctx.builder->CreateStore(startV, it->second);

    auto condBB = llvm::BasicBlock::Create(*ctx.llvmContext, "forcond", func);
    auto bodyBB = llvm::BasicBlock::Create(*ctx.llvmContext, "forbody");
    auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "forend");

    ctx.builder->CreateBr(condBB);
    ctx.builder->SetInsertPoint(condBB);

    auto curVal = ctx.builder->CreateLoad(it->second->getAllocatedType(),
                                           it->second, var);
    auto limitV = limit->codegen(ctx);
    auto condV = ctx.builder->CreateICmpSLE(curVal, limitV, "forcmp");
    ctx.builder->CreateCondBr(condV, bodyBB, afterBB);

    func->insert(func->end(), bodyBB);
    ctx.builder->SetInsertPoint(bodyBB);
    body->codegen(ctx);

    auto curVal2 = ctx.builder->CreateLoad(it->second->getAllocatedType(),
                                            it->second, var);
    auto stepV = step->codegen(ctx);
    auto nextVal = ctx.builder->CreateAdd(curVal2, stepV, "forstep");
    ctx.builder->CreateStore(nextVal, it->second);
    ctx.builder->CreateBr(condBB);

    func->insert(func->end(), afterBB);
    ctx.builder->SetInsertPoint(afterBB);
    return nullptr;
}

// ---- Procedure declaration ----

llvm::Value* ProcedureDecl::codegen(CodeGenContext& ctx) {
    // Determine return type
    llvm::Type* retTy;
    if (hasReturnType) {
        retTy = ctx.getLLVMType(returnType);
    } else {
        retTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    }

    // Build parameter types
    std::vector<llvm::Type*> paramTypes;
    // If inside a class, first param is 'this' pointer
    bool isMethod = !ctx.currentClassName.empty();
    if (isMethod) {
        paramTypes.push_back(ctx.getRefType());
    }
    for (auto& p : params) {
        paramTypes.push_back(ctx.getLLVMType(p.type));
    }

    auto funcType = llvm::FunctionType::get(retTy, paramTypes, false);
    std::string funcName = isMethod ? (ctx.currentClassName + "_" + name) : name;
    auto func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                        funcName, ctx.module.get());

    // Register as method if inside a class
    if (isMethod) {
        ctx.classes[ctx.currentClassName].methods[name] = func;
    }

    // Save state
    auto savedBlock = ctx.builder->GetInsertBlock();
    auto savedLocals = ctx.saveScope();
    auto savedRefTypes = ctx.refTypes;
    auto savedProcName = ctx.currentProcName;
    auto savedRetAlloca = ctx.returnValueAlloca;
    auto savedThis = ctx.currentThis;
    auto savedClassName = ctx.currentClassName;

    // Create entry block
    auto entry = llvm::BasicBlock::Create(*ctx.llvmContext, "entry", func);
    ctx.builder->SetInsertPoint(entry);
    ctx.locals.clear();

    // Set up parameters
    auto argIt = func->arg_begin();
    if (isMethod) {
        ctx.currentThis = &*argIt;
        ctx.currentThis->setName("this");
        ++argIt;
    } else {
        ctx.currentThis = nullptr;
        ctx.currentClassName = "";
    }

    for (auto& p : params) {
        auto ty = ctx.getLLVMType(p.type);
        auto alloca = ctx.createEntryBlockAlloca(func, p.name, ty);
        ctx.builder->CreateStore(&*argIt, alloca);
        ctx.locals[p.name] = alloca;
        ++argIt;
    }

    // Set up return variable for typed procedures
    if (hasReturnType) {
        ctx.currentProcName = name;
        ctx.returnValueAlloca = ctx.createEntryBlockAlloca(func, name, retTy);
        ctx.builder->CreateStore(llvm::Constant::getNullValue(retTy), ctx.returnValueAlloca);
    } else {
        ctx.currentProcName = "";
        ctx.returnValueAlloca = nullptr;
    }

    // Generate body
    body->codegen(ctx);

    // Return
    if (hasReturnType) {
        auto retVal = ctx.builder->CreateLoad(retTy, ctx.returnValueAlloca, "retval");
        ctx.builder->CreateRet(retVal);
    } else {
        ctx.builder->CreateRetVoid();
    }

    llvm::verifyFunction(*func, &llvm::errs());

    // Restore state
    ctx.builder->SetInsertPoint(savedBlock);
    ctx.restoreScope(savedLocals);
    ctx.refTypes = savedRefTypes;
    ctx.currentProcName = savedProcName;
    ctx.returnValueAlloca = savedRetAlloca;
    ctx.currentThis = savedThis;
    ctx.currentClassName = savedClassName;

    return func;
}

// ---- Class declaration ----

llvm::Value* ClassDecl::codegen(CodeGenContext& ctx) {
    ClassInfo ci;
    ci.name = name;
    ci.parentName = parentName;
    ci.classId = ctx.nextClassId++;
    ci.coroFieldIndex = 1;

    // Build struct fields: [classId (i64), coroPtr (ptr), inherited fields..., own fields...]
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext)); // classId
    fieldTypes.push_back(ctx.getRefType()); // coro context ptr

    // Inherit parent fields
    if (!parentName.empty()) {
        auto pit = ctx.classes.find(parentName);
        if (pit != ctx.classes.end()) {
            for (auto& pf : pit->second.fields) {
                if (pf.structIndex < 2) continue; // skip classId and coro
                ci.fields.push_back(pf);
                ci.fields.back().structIndex = (int)fieldTypes.size();
                fieldTypes.push_back(pit->second.structType->getElementType(pf.structIndex));
            }
            // Inherit methods
            ci.methods = pit->second.methods;
        }
    }

    // Add constructor parameter fields
    for (auto& p : params) {
        ClassInfo::FieldInfo fi;
        fi.name = p.name;
        fi.type = p.type;
        fi.structIndex = (int)fieldTypes.size();
        ci.fields.push_back(fi);
        fieldTypes.push_back(ctx.getLLVMType(p.type));
    }

    // Create struct type
    ci.structType = llvm::StructType::create(*ctx.llvmContext, fieldTypes, name);

    // Register class before processing body (so body can reference the class)
    ctx.classes[name] = ci;

    // Body VarDeclarations/RefDeclarations are treated as local variables
    // in the body function (they live on the coroutine stack and persist
    // across DETACH/RESUME). They are NOT struct fields.

    // Save state
    auto savedBlock = ctx.builder->GetInsertBlock();
    auto savedThis = ctx.currentThis;
    auto savedClassName = ctx.currentClassName;
    auto savedLocals = ctx.saveScope();
    auto savedRefTypes = ctx.refTypes;
    auto savedProcName = ctx.currentProcName;
    auto savedRetAlloca = ctx.returnValueAlloca;

    ctx.currentClassName = name;
    ctx.currentProcName = "";
    ctx.returnValueAlloca = nullptr;

    // Second pass: compile procedure declarations as methods
    for (auto& stmt : bodyStmts) {
        if (dynamic_cast<ProcedureDecl*>(stmt.get())) {
            // Temporarily set class context for method compilation
            stmt->codegen(ctx);
        }
    }

    // Create class body function: void @ClassName_body(ptr %this)
    auto bodyFuncType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(*ctx.llvmContext), {ctx.getRefType()}, false);
    auto bodyFunc = llvm::Function::Create(
        bodyFuncType, llvm::Function::ExternalLinkage,
        name + "_body", ctx.module.get());
    ctx.classes[name].bodyFunc = bodyFunc;

    // Generate body function
    auto bodyEntry = llvm::BasicBlock::Create(*ctx.llvmContext, "entry", bodyFunc);
    ctx.builder->SetInsertPoint(bodyEntry);
    ctx.currentThis = bodyFunc->arg_begin();
    ctx.currentThis->setName("this");
    ctx.locals.clear();
    ctx.refTypes.clear();

    // Set up ref types for REF fields
    for (auto& fi : ctx.classes[name].fields) {
        if (fi.type == -1) {
            ctx.refTypes[fi.name] = fi.refClassName;
        }
    }

    // Execute body statements (VarDecls create local allocas on the coroutine stack,
    // ProcedureDecls were already compiled above)
    for (auto& stmt : bodyStmts) {
        if (dynamic_cast<ProcedureDecl*>(stmt.get())) continue;
        stmt->codegen(ctx);
    }

    ctx.builder->CreateRetVoid();
    llvm::verifyFunction(*bodyFunc, &llvm::errs());

    // Restore state
    ctx.builder->SetInsertPoint(savedBlock);
    ctx.currentThis = savedThis;
    ctx.currentClassName = savedClassName;
    ctx.restoreScope(savedLocals);
    ctx.refTypes = savedRefTypes;
    ctx.currentProcName = savedProcName;
    ctx.returnValueAlloca = savedRetAlloca;

    return nullptr;
}

// ---- INSPECT/WHEN ----

llvm::Value* InspectStatement::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);

    // Load class ID
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {i64Ty, ptrTy});
    auto idPtr = ctx.builder->CreateStructGEP(baseTy, obj, 0, "classid_ptr");
    auto classId = ctx.builder->CreateLoad(i64Ty, idPtr, "classid");

    auto mergeBB = llvm::BasicBlock::Create(*ctx.llvmContext, "inspect_end");

    for (size_t i = 0; i < whenClauses.size(); i++) {
        auto& wc = whenClauses[i];
        auto cit = ctx.classes.find(wc.className);
        if (cit == ctx.classes.end()) {
            std::cerr << "Error: unknown class '" << wc.className << "' in WHEN clause\n";
            continue;
        }

        auto cmpV = ctx.builder->CreateICmpEQ(classId,
            llvm::ConstantInt::get(i64Ty, cit->second.classId), "when_cmp");

        auto whenBB = llvm::BasicBlock::Create(*ctx.llvmContext, "when_" + wc.className, func);
        auto nextBB = llvm::BasicBlock::Create(*ctx.llvmContext, "when_next");

        ctx.builder->CreateCondBr(cmpV, whenBB, nextBB);

        ctx.builder->SetInsertPoint(whenBB);
        wc.body->codegen(ctx);
        ctx.builder->CreateBr(mergeBB);

        func->insert(func->end(), nextBB);
        ctx.builder->SetInsertPoint(nextBB);
    }

    // OTHERWISE
    if (otherwiseBody) {
        otherwiseBody->codegen(ctx);
    }
    ctx.builder->CreateBr(mergeBB);

    func->insert(func->end(), mergeBB);
    ctx.builder->SetInsertPoint(mergeBB);
    return nullptr;
}

// ---- Coroutines ----

llvm::Value* DetachStatement::codegen(CodeGenContext& ctx) {
    if (!ctx.currentThis) {
        std::cerr << "Error: DETACH used outside of a class body\n";
        return nullptr;
    }

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {i64Ty, ptrTy});

    auto coroSlot = ctx.builder->CreateStructGEP(baseTy, ctx.currentThis, 1, "coro_slot");
    auto coro = ctx.builder->CreateLoad(ptrTy, coroSlot, "coro");

    return ctx.builder->CreateCall(ctx.coroDetachFunc, {coro});
}

llvm::Value* ResumeStatement::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {i64Ty, ptrTy});

    auto coroSlot = ctx.builder->CreateStructGEP(baseTy, obj, 1, "coro_slot");
    auto coro = ctx.builder->CreateLoad(ptrTy, coroSlot, "coro");

    return ctx.builder->CreateCall(ctx.coroResumeFunc, {coro});
}

llvm::Value* CallStatement::codegen(CodeGenContext& ctx) {
    // CALL is treated as RESUME for now
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {i64Ty, ptrTy});

    auto coroSlot = ctx.builder->CreateStructGEP(baseTy, obj, 1, "coro_slot");
    auto coro = ctx.builder->CreateLoad(ptrTy, coroSlot, "coro");

    return ctx.builder->CreateCall(ctx.coroResumeFunc, {coro});
}

// ---- I/O ----

llvm::Value* OutIntStatement::codegen(CodeGenContext& ctx) {
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    auto fmt = ctx.builder->CreateGlobalString("%lld", "intfmt");
    return ctx.builder->CreateCall(ctx.printfFunc, {fmt, val});
}

llvm::Value* OutRealStatement::codegen(CodeGenContext& ctx) {
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    auto dec = decimals->codegen(ctx);
    if (!dec) return nullptr;
    // Use a fixed format for now: %.*f
    auto fmt = ctx.builder->CreateGlobalString("%.*f", "realfmt");
    // dec is the precision
    auto decI32 = ctx.builder->CreateTrunc(dec, llvm::Type::getInt32Ty(*ctx.llvmContext), "dec32");
    return ctx.builder->CreateCall(ctx.printfFunc, {fmt, decI32, val});
}

llvm::Value* OutTextStatement::codegen(CodeGenContext& ctx) {
    auto val = text->codegen(ctx);
    if (!val) return nullptr;
    auto fmt = ctx.builder->CreateGlobalString("%s", "strfmt");
    return ctx.builder->CreateCall(ctx.printfFunc, {fmt, val});
}

llvm::Value* OutImageStatement::codegen(CodeGenContext& ctx) {
    auto nl = ctx.builder->CreateGlobalString("", "newline");
    return ctx.builder->CreateCall(ctx.putsFunc, {nl});
}

// ---- Program ----

llvm::Value* Program::codegen(CodeGenContext& ctx) {
    return block->codegen(ctx);
}
