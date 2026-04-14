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
        case VarDeclaration::INTEGER:   return llvm::Type::getInt64Ty(*llvmContext);
        case VarDeclaration::REAL:      return llvm::Type::getDoubleTy(*llvmContext);
        case VarDeclaration::BOOLEAN:   return llvm::Type::getInt1Ty(*llvmContext);
        case VarDeclaration::TEXT:      return llvm::PointerType::getUnqual(*llvmContext);
        case VarDeclaration::CHARACTER: return llvm::Type::getInt8Ty(*llvmContext);
        default:                        return llvm::Type::getInt64Ty(*llvmContext);
    }
}

llvm::Type* CodeGenContext::getRefType() {
    return llvm::PointerType::getUnqual(*llvmContext);
}

llvm::BasicBlock* CodeGenContext::getOrCreateLabel(const std::string& name) {
    auto it = labelBlocks.find(name);
    if (it != labelBlocks.end()) return it->second;
    auto bb = llvm::BasicBlock::Create(*llvmContext, "label_" + name);
    // Don't add to function yet - LabeledStatement will do that
    labelBlocks[name] = bb;
    return bb;
}

std::pair<llvm::Value*, llvm::Type*> CodeGenContext::getVarPtr(const std::string& name) {
    // Check local variables first
    auto it = locals.find(name);
    if (it != locals.end()) {
        return {it->second, it->second->getAllocatedType()};
    }
    // Check class fields (accessed through currentThis)
    if (currentThis && !currentClassName.empty()) {
        int idx = getFieldIndex(currentClassName, name);
        if (idx >= 0) {
            auto& ci = classes[currentClassName];
            auto gep = builder->CreateStructGEP(ci.structType, currentThis, idx, name + "_fptr");
            auto fieldTy = ci.structType->getElementType(idx);
            return {gep, fieldTy};
        }
    }
    return {nullptr, nullptr};
}

void CodeGenContext::setupTextFieldTracking(llvm::Function* func) {
    if (currentClassName.empty() || !currentThis) return;
    auto cit = classes.find(currentClassName);
    if (cit == classes.end()) return;
    for (auto& fi : cit->second.fields) {
        if (fi.type == VarDeclaration::TEXT && fi.structIndex >= 2) {
            // The __pos companion field is in the struct — find it and create
            // a local "proxy" alloca that reads/writes through the struct GEP.
            // But actually, the TEXT dispatch uses locals[name+"__pos"] as an AllocaInst*.
            // We need to provide that. Use a local alloca that loads from struct on entry
            // and stores back on... well, every write. That's complex.
            //
            // Simpler: just make locals[name+"__pos"] point to the struct field directly
            // via getVarPtr. But locals expects AllocaInst*, not Value*.
            //
            // Simplest: just register in textVars. The TEXT dispatch will use getVarPtr
            // for the __pos field too.
            textVars.insert(fi.name);
        }
    }
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
            if (f.type == -1) return getRefType();
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

    // printf (varargs)
    printfFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, {ptrTy}, true),
        llvm::Function::ExternalLinkage, "printf", module.get());

    // puts
    putsFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "puts", module.get());

    // scanf (varargs)
    scanfFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, {ptrTy}, true),
        llvm::Function::ExternalLinkage, "scanf", module.get());

    // getchar
    getcharFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, false),
        llvm::Function::ExternalLinkage, "getchar", module.get());

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

    // simula_blanks(i64) -> ptr
    blanksFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {i64Ty}, false),
        llvm::Function::ExternalLinkage, "simula_blanks", module.get());

    // simula_text_copy(ptr) -> ptr
    textCopyFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_text_copy", module.get());

    // simula_text_concat(ptr, ptr) -> ptr
    textConcatFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_text_concat", module.get());

    // simula_text_length(ptr) -> i64
    textLengthFunc = llvm::Function::Create(
        llvm::FunctionType::get(i64Ty, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_text_length", module.get());

    // simula_text_strip(ptr) -> ptr
    textStripFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_text_strip", module.get());

    // simula_text_sub(ptr, i64, i64) -> ptr
    textSubFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty, i64Ty}, false),
        llvm::Function::ExternalLinkage, "simula_text_sub", module.get());

    // simula_text_eq(ptr, ptr) -> i64
    textEqFunc = llvm::Function::Create(
        llvm::FunctionType::get(i64Ty, {ptrTy, ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_text_eq", module.get());
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

llvm::Value* CharLiteral::codegen(CodeGenContext& ctx) {
    return llvm::ConstantInt::get(llvm::Type::getInt8Ty(*ctx.llvmContext), (uint8_t)value);
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

    // CONCAT: call simula_text_concat
    if (op == CONCAT) {
        return ctx.builder->CreateCall(ctx.textConcatFunc, {L, R}, "concat");
    }

    // Check for character comparisons (both i8)
    bool isChar = L->getType()->isIntegerTy(8) && R->getType()->isIntegerTy(8);
    if (isChar) {
        switch (op) {
            case EQ:  return ctx.builder->CreateICmpEQ(L, R, "eqtmp");
            case NE:  return ctx.builder->CreateICmpNE(L, R, "netmp");
            case LT:  return ctx.builder->CreateICmpULT(L, R, "lttmp");
            case LE:  return ctx.builder->CreateICmpULE(L, R, "letmp");
            case GT:  return ctx.builder->CreateICmpUGT(L, R, "gttmp");
            case GE:  return ctx.builder->CreateICmpUGE(L, R, "getmp");
            default: break;
        }
    }

    bool isReal = L->getType()->isDoubleTy() || R->getType()->isDoubleTy();

    // Promote integer widths: i1/i8 -> i64 when mixed with i64
    if (!isReal && L->getType() != R->getType()) {
        auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
        if (L->getType()->isIntegerTy() && R->getType()->isIntegerTy()) {
            if (L->getType()->getIntegerBitWidth() < R->getType()->getIntegerBitWidth())
                L = ctx.builder->CreateZExt(L, R->getType(), "zext");
            else
                R = ctx.builder->CreateZExt(R, L->getType(), "zext");
        }
        (void)i64Ty;
    }

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
            case AND: case OR: case CONCAT: break;
        }
    }

    // Pointer (TEXT) comparison: use runtime string comparison
    bool isPtr = L->getType()->isPointerTy() && R->getType()->isPointerTy();
    if (isPtr && (op == EQ || op == NE)) {
        auto eqVal = ctx.builder->CreateCall(ctx.textEqFunc, {L, R}, "texteq");
        auto cmp = ctx.builder->CreateICmpNE(eqVal,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), 0), "txtcmp");
        if (op == NE) cmp = ctx.builder->CreateNot(cmp, "txtne");
        return cmp;
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
        case CONCAT: break; // handled above
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
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto i8Ty = llvm::Type::getInt8Ty(*ctx.llvmContext);
    auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);

    // Check if it's an array access
    auto ait = ctx.arrays.find(name);
    if (ait != ctx.arrays.end()) {
        auto& info = ait->second;
        if (args.empty()) {
            std::cerr << "Error: array '" << name << "' access requires an index\n";
            return nullptr;
        }
        auto idxVal = args[0]->codegen(ctx);
        if (!idxVal) return nullptr;
        // Compute adjusted index: index - lowerBound
        llvm::Value* loBound;
        auto loIt = ctx.locals.find(name + "__lo");
        if (loIt != ctx.locals.end()) {
            loBound = ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
        } else {
            loBound = llvm::ConstantInt::get(i64Ty, info.lowerBound);
        }
        auto adjusted = ctx.builder->CreateSub(idxVal, loBound, "adj_idx");
        llvm::Value* gep;
        if (info.isStackArray) {
            auto arrTy = llvm::ArrayType::get(info.elementType, info.size);
            gep = ctx.builder->CreateGEP(arrTy, info.basePtr,
                {llvm::ConstantInt::get(i64Ty, 0), adjusted}, "arr_elem");
        } else {
            gep = ctx.builder->CreateGEP(info.elementType, info.basePtr,
                adjusted, "arr_elem");
        }
        return ctx.builder->CreateLoad(info.elementType, gep, "arr_val");
    }

    // Check built-in functions (identifiers are already lowered by the lexer)
    if (name == "abs") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isDoubleTy()) {
            // fabs via select: val < 0 ? -val : val
            auto neg = ctx.builder->CreateFNeg(val, "neg");
            auto zero = llvm::ConstantFP::get(doubleTy, 0.0);
            auto cmp = ctx.builder->CreateFCmpOLT(val, zero, "cmp");
            return ctx.builder->CreateSelect(cmp, neg, val, "abs");
        } else {
            // integer abs: val < 0 ? -val : val
            auto neg = ctx.builder->CreateNeg(val, "neg");
            auto zero = llvm::ConstantInt::get(i64Ty, 0);
            auto cmp = ctx.builder->CreateICmpSLT(val, zero, "cmp");
            return ctx.builder->CreateSelect(cmp, neg, val, "abs");
        }
    }

    if (name == "mod") {
        if (args.size() < 2) return nullptr;
        auto a = args[0]->codegen(ctx);
        auto b = args[1]->codegen(ctx);
        if (!a || !b) return nullptr;
        return ctx.builder->CreateSRem(a, b, "mod");
    }

    if (name == "entier") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        return ctx.builder->CreateFPToSI(val, i64Ty, "entier");
    }

    if (name == "sign") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        // return -1, 0, or 1
        auto zero = llvm::ConstantInt::get(i64Ty, 0);
        auto one = llvm::ConstantInt::get(i64Ty, 1);
        auto neg1 = llvm::ConstantInt::getSigned(i64Ty, -1);
        auto isNeg = ctx.builder->CreateICmpSLT(val, zero, "isneg");
        auto isPos = ctx.builder->CreateICmpSGT(val, zero, "ispos");
        auto sel1 = ctx.builder->CreateSelect(isPos, one, zero, "sel1");
        return ctx.builder->CreateSelect(isNeg, neg1, sel1, "sign");
    }

    if (name == "char") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        return ctx.builder->CreateTrunc(val, i8Ty, "char");
    }

    if (name == "rank") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        return ctx.builder->CreateZExt(val, i64Ty, "rank");
    }

    if (name == "blanks") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        return ctx.builder->CreateCall(ctx.blanksFunc, {val}, "blanks");
    }

    if (name == "copy") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        return ctx.builder->CreateCall(ctx.textCopyFunc, {val}, "copy");
    }

    if (name == "length") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        return ctx.builder->CreateCall(ctx.textLengthFunc, {val}, "length");
    }

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

    // Look up in module functions
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
        if (param.structIndex < 2) continue;
        if (argIdx < args.size()) {
            auto val = args[argIdx]->codegen(ctx);
            if (val) {
                auto fieldPtr = ctx.builder->CreateStructGEP(
                    ci.structType, obj, param.structIndex, param.name + "_ptr");
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
    // Check for TEXT variable member access first
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        if (ctx.textVars.count(ident->name)) {
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto i8Ty = llvm::Type::getInt8Ty(*ctx.llvmContext);
            auto [varPtr, varTy] = ctx.getVarPtr(ident->name);
            auto [posPtr, posTy] = ctx.getVarPtr(ident->name + "__pos");
            if (!varPtr) {
                std::cerr << "Error: TEXT variable '" << ident->name << "' not accessible\n";
                return nullptr;
            }
            // If no __pos field found, create a temp (for non-class TEXT vars without tracking)
            llvm::Value* posStorage = posPtr;
            if (!posStorage) {
                auto it2 = ctx.locals.find(ident->name + "__pos");
                if (it2 != ctx.locals.end()) posStorage = it2->second;
            }
            auto dataPtr = ctx.builder->CreateLoad(ptrTy, varPtr, "txtdata");

            if (member == "more") {
                if (!posStorage) return ctx.builder->getFalse();
                auto pos = ctx.builder->CreateLoad(i64Ty, posStorage, "pos");
                auto len = ctx.builder->CreateCall(ctx.textLengthFunc, {dataPtr}, "len");
                return ctx.builder->CreateICmpSLT(pos, len, "more");
            }
            if (member == "length") {
                return ctx.builder->CreateCall(ctx.textLengthFunc, {dataPtr}, "len");
            }
            if (member == "pos") {
                if (!posStorage) return llvm::ConstantInt::get(i64Ty, 1);
                auto pos = ctx.builder->CreateLoad(i64Ty, posStorage, "pos0");
                return ctx.builder->CreateAdd(pos,
                    llvm::ConstantInt::get(i64Ty, 1), "pos1");
            }
            if (member == "getchar") {
                if (!posStorage) return llvm::ConstantInt::get(i8Ty, 0);
                auto pos = ctx.builder->CreateLoad(i64Ty, posStorage, "pos");
                auto charPtr = ctx.builder->CreateGEP(i8Ty, dataPtr, pos, "chptr");
                auto ch = ctx.builder->CreateLoad(i8Ty, charPtr, "ch");
                auto newPos = ctx.builder->CreateAdd(pos,
                    llvm::ConstantInt::get(i64Ty, 1), "newpos");
                ctx.builder->CreateStore(newPos, posStorage);
                return ch;
            }
            if (member == "strip") {
                return ctx.builder->CreateCall(ctx.textStripFunc, {dataPtr}, "stripped");
            }
            if (member == "main") {
                return dataPtr;
            }
            std::cerr << "Error: unknown TEXT member '." << member << "'\n";
            return nullptr;
        }
    }

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
    // Check for TEXT variable method call first
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        if (ctx.textVars.count(ident->name)) {
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto i8Ty = llvm::Type::getInt8Ty(*ctx.llvmContext);
            auto [varPtr, varTy] = ctx.getVarPtr(ident->name);
            auto [posPtr, posTy] = ctx.getVarPtr(ident->name + "__pos");
            if (!varPtr) {
                std::cerr << "Error: TEXT variable '" << ident->name << "' not accessible\n";
                return nullptr;
            }
            llvm::Value* posStorage = posPtr;
            if (!posStorage) {
                auto it2 = ctx.locals.find(ident->name + "__pos");
                if (it2 != ctx.locals.end()) posStorage = it2->second;
            }
            auto dataPtr = ctx.builder->CreateLoad(ptrTy, varPtr, "txtdata");

            if (method == "setpos") {
                if (!posStorage) return llvm::ConstantInt::get(i64Ty, 0);
                auto posArg = args[0]->codegen(ctx);
                auto pos0 = ctx.builder->CreateSub(posArg,
                    llvm::ConstantInt::get(i64Ty, 1), "pos0");
                ctx.builder->CreateStore(pos0, posStorage);
                return llvm::ConstantInt::get(i64Ty, 0);
            }
            if (method == "getchar") {
                if (!posStorage) return llvm::ConstantInt::get(i8Ty, 0);
                auto pos = ctx.builder->CreateLoad(i64Ty, posStorage, "pos");
                auto charPtr = ctx.builder->CreateGEP(i8Ty, dataPtr, pos, "chptr");
                auto ch = ctx.builder->CreateLoad(i8Ty, charPtr, "ch");
                auto newPos = ctx.builder->CreateAdd(pos,
                    llvm::ConstantInt::get(i64Ty, 1), "newpos");
                ctx.builder->CreateStore(newPos, posStorage);
                return ch;
            }
            if (method == "putchar") {
                if (!posStorage) return llvm::ConstantInt::get(i64Ty, 0);
                auto ch = args[0]->codegen(ctx);
                auto pos = ctx.builder->CreateLoad(i64Ty, posStorage, "pos");
                auto charPtr = ctx.builder->CreateGEP(i8Ty, dataPtr, pos, "chptr");
                ctx.builder->CreateStore(ch, charPtr);
                auto newPos = ctx.builder->CreateAdd(pos,
                    llvm::ConstantInt::get(i64Ty, 1), "newpos");
                ctx.builder->CreateStore(newPos, posStorage);
                return llvm::ConstantInt::get(i64Ty, 0);
            }
            if (method == "sub") {
                auto start = args[0]->codegen(ctx);
                auto len = args[1]->codegen(ctx);
                return ctx.builder->CreateCall(ctx.textSubFunc, {dataPtr, start, len}, "sub");
            }
            if (method == "strip") {
                return ctx.builder->CreateCall(ctx.textStripFunc, {dataPtr}, "stripped");
            }
            std::cerr << "Error: unknown TEXT method '." << method << "'\n";
            return nullptr;
        }
    }

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
    argsV.push_back(obj);
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

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
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

    llvm::Value* result = ctx.builder->getFalse();
    for (int id : ids) {
        auto cmp = ctx.builder->CreateICmpEQ(classId,
            llvm::ConstantInt::get(i64Ty, id), "in_cmp");
        result = ctx.builder->CreateOr(result, cmp, "in_or");
    }
    return result;
}

// ---- Conditional expression ----

llvm::Value* ConditionalExpr::codegen(CodeGenContext& ctx) {
    auto condV = condition->codegen(ctx);
    if (!condV) return nullptr;

    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto thenBB = llvm::BasicBlock::Create(*ctx.llvmContext, "cond_then", func);
    auto elseBB = llvm::BasicBlock::Create(*ctx.llvmContext, "cond_else");
    auto mergeBB = llvm::BasicBlock::Create(*ctx.llvmContext, "cond_merge");

    ctx.builder->CreateCondBr(condV, thenBB, elseBB);

    // Then
    ctx.builder->SetInsertPoint(thenBB);
    auto thenV = thenExpr->codegen(ctx);
    if (!thenV) return nullptr;
    ctx.builder->CreateBr(mergeBB);
    thenBB = ctx.builder->GetInsertBlock(); // update in case codegen changed block

    // Else
    func->insert(func->end(), elseBB);
    ctx.builder->SetInsertPoint(elseBB);
    auto elseV = elseExpr->codegen(ctx);
    if (!elseV) return nullptr;
    ctx.builder->CreateBr(mergeBB);
    elseBB = ctx.builder->GetInsertBlock();

    // Merge with PHI
    func->insert(func->end(), mergeBB);
    ctx.builder->SetInsertPoint(mergeBB);

    // Type-match: promote if needed
    if (thenV->getType() != elseV->getType()) {
        if (thenV->getType()->isDoubleTy() && elseV->getType()->isIntegerTy())
            elseV = ctx.builder->CreateSIToFP(elseV, thenV->getType());
        else if (elseV->getType()->isDoubleTy() && thenV->getType()->isIntegerTy())
            thenV = ctx.builder->CreateSIToFP(thenV, elseV->getType());
    }

    auto phi = ctx.builder->CreatePHI(thenV->getType(), 2, "condval");
    phi->addIncoming(thenV, thenBB);
    phi->addIncoming(elseV, elseBB);
    return phi;
}

// ---- Input expressions ----

llvm::Value* InIntExpression::codegen(CodeGenContext& ctx) {
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, "inint_tmp", i64Ty);
    ctx.builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), alloca);
    auto fmt = ctx.builder->CreateGlobalString("%lld", "intinfmt");
    ctx.builder->CreateCall(ctx.scanfFunc, {fmt, alloca});
    return ctx.builder->CreateLoad(i64Ty, alloca, "inint");
}

llvm::Value* InRealExpression::codegen(CodeGenContext& ctx) {
    auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, "inreal_tmp", doubleTy);
    ctx.builder->CreateStore(llvm::ConstantFP::get(doubleTy, 0.0), alloca);
    auto fmt = ctx.builder->CreateGlobalString("%lf", "realinfmt");
    ctx.builder->CreateCall(ctx.scanfFunc, {fmt, alloca});
    return ctx.builder->CreateLoad(doubleTy, alloca, "inreal");
}

llvm::Value* InCharExpression::codegen(CodeGenContext& ctx) {
    auto i8Ty = llvm::Type::getInt8Ty(*ctx.llvmContext);
    auto ch = ctx.builder->CreateCall(ctx.getcharFunc, {}, "getch");
    return ctx.builder->CreateTrunc(ch, i8Ty, "inchar");
}

// ============================================================
// Statement codegen
// ============================================================

llvm::Value* Block::codegen(CodeGenContext& ctx) {
    auto saved = ctx.saveScope();
    auto savedRefTypes = ctx.refTypes;
    auto savedTextVars = ctx.textVars;
    llvm::Value* last = nullptr;
    for (auto& stmt : statements) {
        last = stmt->codegen(ctx);
    }
    ctx.restoreScope(saved);
    ctx.refTypes = savedRefTypes;
    ctx.textVars = savedTextVars;
    return last;
}

llvm::Value* CompoundStmt::codegen(CodeGenContext& ctx) {
    llvm::Value* last = nullptr;
    for (auto& stmt : statements) {
        last = stmt->codegen(ctx);
    }
    return last;
}

llvm::Value* VarDeclaration::codegen(CodeGenContext& ctx) {
    auto ty = ctx.getLLVMType(type);
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, name, ty);
    ctx.locals[name] = alloca;

    if (init) {
        auto val = init->codegen(ctx);
        if (val) {
            // Type convert if needed
            auto destTy = alloca->getAllocatedType();
            if (val->getType() != destTy) {
                if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
                    val = ctx.builder->CreateSIToFP(val, destTy);
                else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                    val = ctx.builder->CreateFPToSI(val, destTy);
            }
            ctx.builder->CreateStore(val, alloca);
        }
    } else {
        ctx.builder->CreateStore(llvm::Constant::getNullValue(ty), alloca);
    }

    // For TEXT variables, create an associated position counter
    if (type == TEXT) {
        auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
        auto posAlloca = ctx.createEntryBlockAlloca(func, name + "__pos", i64Ty);
        ctx.locals[name + "__pos"] = posAlloca;
        ctx.builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), posAlloca);
        ctx.textVars.insert(name);
    }
    return alloca;
}

llvm::Value* ArrayDeclaration::codegen(CodeGenContext& ctx) {
    auto elemTy = ctx.getLLVMType(elementType);
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);

    // Evaluate bounds
    auto loVal = lowerBound->codegen(ctx);
    auto hiVal = upperBound->codegen(ctx);
    if (!loVal || !hiVal) return nullptr;

    long long lo = 0, hi = 0;
    bool constBounds = false;
    if (auto* loCI = llvm::dyn_cast<llvm::ConstantInt>(loVal)) {
        if (auto* hiCI = llvm::dyn_cast<llvm::ConstantInt>(hiVal)) {
            lo = loCI->getSExtValue();
            hi = hiCI->getSExtValue();
            constBounds = true;
        }
    }

    if (constBounds) {
        long long size = hi - lo + 1;
        if (size <= 0) size = 1;

        auto arrTy = llvm::ArrayType::get(elemTy, size);
        auto func = ctx.builder->GetInsertBlock()->getParent();
        auto alloca = ctx.createEntryBlockAlloca(func, name, arrTy);
        ctx.builder->CreateStore(llvm::Constant::getNullValue(arrTy), alloca);

        ArrayInfo info;
        info.basePtr = alloca;
        info.elementType = elemTy;
        info.lowerBound = lo;
        info.size = size;
        info.isStackArray = true;
        ctx.arrays[name] = info;
        return alloca;
    } else {
        // Dynamic bounds: heap-allocate via simula_alloc
        auto sizeVal = ctx.builder->CreateAdd(
            ctx.builder->CreateSub(hiVal, loVal, "range"),
            llvm::ConstantInt::get(i64Ty, 1), "arrsize");
        auto elemSize = ctx.module->getDataLayout().getTypeAllocSize(elemTy);
        auto byteSize = ctx.builder->CreateMul(sizeVal,
            llvm::ConstantInt::get(i64Ty, elemSize), "bytes");
        auto ptr = ctx.builder->CreateCall(ctx.allocFunc, {byteSize}, name + "_data");

        // Store pointer in a local alloca
        auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto func = ctx.builder->GetInsertBlock()->getParent();
        auto alloca = ctx.createEntryBlockAlloca(func, name, ptrTy);
        ctx.builder->CreateStore(ptr, alloca);

        // Store lower bound for index adjustment
        // For dynamic bounds, store loVal in a separate alloca
        auto loAlloca = ctx.createEntryBlockAlloca(func, name + "__lo", i64Ty);
        ctx.builder->CreateStore(loVal, loAlloca);

        ArrayInfo info;
        info.basePtr = ptr;
        info.elementType = elemTy;
        info.lowerBound = 0; // will use dynamic adjustment
        info.size = 0;
        info.isStackArray = false;
        ctx.arrays[name] = info;
        // Store lo alloca name for dynamic index adjustment
        ctx.locals[name + "__lo"] = loAlloca;
        return ptr;
    }
}

llvm::Value* ArrayAssignment::codegen(CodeGenContext& ctx) {
    auto ait = ctx.arrays.find(name);
    if (ait == ctx.arrays.end()) {
        std::cerr << "Error: unknown array '" << name << "'\n";
        return nullptr;
    }
    auto& info = ait->second;
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);

    auto idxVal = index->codegen(ctx);
    if (!idxVal) return nullptr;

    // Compute adjusted index = index - lowerBound
    llvm::Value* loBound;
    auto loIt = ctx.locals.find(name + "__lo");
    if (loIt != ctx.locals.end()) {
        loBound = ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
    } else {
        loBound = llvm::ConstantInt::get(i64Ty, info.lowerBound);
    }
    auto adjusted = ctx.builder->CreateSub(idxVal, loBound, "adj_idx");

    llvm::Value* gep;
    if (info.isStackArray) {
        auto arrTy = llvm::ArrayType::get(info.elementType, info.size);
        gep = ctx.builder->CreateGEP(arrTy, info.basePtr,
            {llvm::ConstantInt::get(i64Ty, 0), adjusted}, "arr_elem");
    } else {
        gep = ctx.builder->CreateGEP(info.elementType, info.basePtr,
            adjusted, "arr_elem");
    }

    auto val = value->codegen(ctx);
    if (!val) return nullptr;

    // Type convert if needed
    if (val->getType() != info.elementType) {
        if (info.elementType->isDoubleTy() && val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, info.elementType);
        else if (info.elementType->isIntegerTy(64) && val->getType()->isDoubleTy())
            val = ctx.builder->CreateFPToSI(val, info.elementType);
    }

    ctx.builder->CreateStore(val, gep);
    return val;
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
        auto destTy = ctx.returnValueAlloca->getAllocatedType();
        if (val->getType() != destTy) {
            if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
                val = ctx.builder->CreateSIToFP(val, destTy);
            else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                val = ctx.builder->CreateFPToSI(val, destTy);
        }
        ctx.builder->CreateStore(val, ctx.returnValueAlloca);
        return val;
    }

    auto it = ctx.locals.find(name);
    if (it != ctx.locals.end()) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
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
    auto [varPtr, varTy] = ctx.getVarPtr(name);
    if (!varPtr) {
        std::cerr << "Error: unknown REF variable '" << name << "'\n";
        return nullptr;
    }
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    ctx.builder->CreateStore(val, varPtr);
    return val;
}

llvm::Value* ExprStatement::codegen(CodeGenContext& ctx) {
    return expr->codegen(ctx);
}

// ---- Labels and GOTO ----

llvm::Value* LabelDeclaration::codegen(CodeGenContext& ctx) {
    // Pre-create the basic blocks for each declared label
    for (auto& lbl : labels) {
        ctx.getOrCreateLabel(lbl);
    }
    return nullptr;
}

llvm::Value* LabeledStatement::codegen(CodeGenContext& ctx) {
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto labelBB = ctx.getOrCreateLabel(label);

    // If current block has no terminator, branch to the label block
    auto curBB = ctx.builder->GetInsertBlock();
    if (!curBB->getTerminator()) {
        ctx.builder->CreateBr(labelBB);
    }

    // Add label block to function if not already added
    if (!labelBB->getParent()) {
        func->insert(func->end(), labelBB);
    }
    ctx.builder->SetInsertPoint(labelBB);

    // Codegen the labeled statement
    if (statement) {
        statement->codegen(ctx);
    }
    return nullptr;
}

llvm::Value* GotoStatement::codegen(CodeGenContext& ctx) {
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto targetBB = ctx.getOrCreateLabel(label);

    // Create unconditional branch to the target
    ctx.builder->CreateBr(targetBB);

    // Create a new basic block for any dead code that follows
    auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "after_goto", func);
    ctx.builder->SetInsertPoint(afterBB);

    return nullptr;
}

// ---- Control flow ----

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
    if (!ctx.builder->GetInsertBlock()->getTerminator())
        ctx.builder->CreateBr(mergeBB);

    if (elseBranch) {
        func->insert(func->end(), elseBB);
        ctx.builder->SetInsertPoint(elseBB);
        elseBranch->codegen(ctx);
        if (!ctx.builder->GetInsertBlock()->getTerminator())
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
    if (!ctx.builder->GetInsertBlock()->getTerminator())
        ctx.builder->CreateBr(condBB);

    func->insert(func->end(), afterBB);
    ctx.builder->SetInsertPoint(afterBB);
    return nullptr;
}

llvm::Value* ForStatement::codegen(CodeGenContext& ctx) {
    auto func = ctx.builder->GetInsertBlock()->getParent();

    auto startV = start->codegen(ctx);
    auto [varPtr, varTy] = ctx.getVarPtr(var);
    if (!varPtr) {
        std::cerr << "Error: FOR variable '" << var << "' not declared\n";
        return nullptr;
    }
    ctx.builder->CreateStore(startV, varPtr);

    auto condBB = llvm::BasicBlock::Create(*ctx.llvmContext, "forcond", func);
    auto bodyBB = llvm::BasicBlock::Create(*ctx.llvmContext, "forbody");
    auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "forend");

    ctx.builder->CreateBr(condBB);
    ctx.builder->SetInsertPoint(condBB);

    auto curVal = ctx.builder->CreateLoad(varTy, varPtr, var);
    auto limitV = limit->codegen(ctx);
    auto condV = ctx.builder->CreateICmpSLE(curVal, limitV, "forcmp");
    ctx.builder->CreateCondBr(condV, bodyBB, afterBB);

    func->insert(func->end(), bodyBB);
    ctx.builder->SetInsertPoint(bodyBB);
    body->codegen(ctx);

    // Re-get the pointer (class field GEP may have been invalidated)
    auto [varPtr2, varTy2] = ctx.getVarPtr(var);
    auto curVal2 = ctx.builder->CreateLoad(varTy2, varPtr2, var);
    auto stepV = step->codegen(ctx);
    auto nextVal = ctx.builder->CreateAdd(curVal2, stepV, "forstep");
    ctx.builder->CreateStore(nextVal, varPtr2);
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
    auto savedArrays = ctx.arrays;
    auto savedLabelBlocks = ctx.labelBlocks;

    // Create entry block
    auto entry = llvm::BasicBlock::Create(*ctx.llvmContext, "entry", func);
    ctx.builder->SetInsertPoint(entry);
    ctx.locals.clear();
    ctx.arrays.clear();
    ctx.labelBlocks.clear();

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

    // If this is a method, load array pointers from struct fields into ctx.arrays
    if (isMethod && ctx.currentThis) {
        auto& ci = ctx.classes[ctx.currentClassName];
        auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
        for (auto& fi : ci.fields) {
            if (fi.structIndex >= 2 && ci.structType->getElementType(fi.structIndex)->isPointerTy()
                && fi.type != -1) {
                // Array field — load the pointer and register in ctx.arrays
                auto gep = ctx.builder->CreateStructGEP(ci.structType, ctx.currentThis,
                    fi.structIndex, fi.name + "_aptr");
                auto arrPtr = ctx.builder->CreateLoad(ptrTy, gep, fi.name + "_arr");
                ArrayInfo ainfo;
                ainfo.basePtr = arrPtr;
                ainfo.elementType = ctx.getLLVMType(fi.type);
                ainfo.isStackArray = false;
                // Get bounds from class metadata
                auto mit = ci.arrayMeta.find(fi.name);
                if (mit != ci.arrayMeta.end()) {
                    ainfo.lowerBound = mit->second.first;
                    ainfo.size = mit->second.second;
                } else {
                    ainfo.lowerBound = 0;
                    ainfo.size = 0;
                }
                ctx.arrays[fi.name] = ainfo;
            }
        }
        // Set up TEXT field position tracking for methods
        ctx.setupTextFieldTracking(func);
    }

    // Generate body
    body->codegen(ctx);

    // Return
    if (!ctx.builder->GetInsertBlock()->getTerminator()) {
        if (hasReturnType) {
            auto retVal = ctx.builder->CreateLoad(retTy, ctx.returnValueAlloca, "retval");
            ctx.builder->CreateRet(retVal);
        } else {
            ctx.builder->CreateRetVoid();
        }
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
    ctx.arrays = savedArrays;
    ctx.labelBlocks = savedLabelBlocks;

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
                if (pf.structIndex < 2) continue;
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
        if (p.type == VarDeclaration::TEXT) {
            ClassInfo::FieldInfo posfi;
            posfi.name = p.name + "__pos";
            posfi.type = VarDeclaration::INTEGER;
            posfi.structIndex = (int)fieldTypes.size();
            ci.fields.push_back(posfi);
            fieldTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext));
        }
    }

    // Scan body for VarDeclarations and RefDeclarations — add as struct fields
    for (auto& stmt : bodyStmts) {
        if (auto* vd = dynamic_cast<VarDeclaration*>(stmt.get())) {
            ClassInfo::FieldInfo fi;
            fi.name = vd->name;
            fi.type = vd->type;
            fi.structIndex = (int)fieldTypes.size();
            ci.fields.push_back(fi);
            fieldTypes.push_back(ctx.getLLVMType(vd->type));
            // For TEXT fields, add a companion position field
            if (vd->type == VarDeclaration::TEXT) {
                ClassInfo::FieldInfo posfi;
                posfi.name = vd->name + "__pos";
                posfi.type = VarDeclaration::INTEGER;
                posfi.structIndex = (int)fieldTypes.size();
                ci.fields.push_back(posfi);
                fieldTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext));
            }
        } else if (auto* rd = dynamic_cast<RefDeclaration*>(stmt.get())) {
            ClassInfo::FieldInfo fi;
            fi.name = rd->varName;
            fi.type = -1;
            fi.refClassName = rd->className;
            fi.structIndex = (int)fieldTypes.size();
            ci.fields.push_back(fi);
            fieldTypes.push_back(ctx.getRefType());
        } else if (auto* ad = dynamic_cast<ArrayDeclaration*>(stmt.get())) {
            // Arrays in class bodies become pointer fields (allocated at NEW time)
            ClassInfo::FieldInfo fi;
            fi.name = ad->name;
            fi.type = ad->elementType;
            fi.structIndex = (int)fieldTypes.size();
            ci.fields.push_back(fi);
            fieldTypes.push_back(ctx.getRefType()); // pointer to array data
        } else if (auto* cs = dynamic_cast<CompoundStmt*>(stmt.get())) {
            // Multi-var declarations like "INTEGER a, b, c"
            for (auto& inner : cs->statements) {
                if (auto* vd2 = dynamic_cast<VarDeclaration*>(inner.get())) {
                    ClassInfo::FieldInfo fi;
                    fi.name = vd2->name;
                    fi.type = vd2->type;
                    fi.structIndex = (int)fieldTypes.size();
                    ci.fields.push_back(fi);
                    fieldTypes.push_back(ctx.getLLVMType(vd2->type));
                }
            }
        }
    }

    // Create struct type
    ci.structType = llvm::StructType::create(*ctx.llvmContext, fieldTypes, name);

    // Register class before processing body
    ctx.classes[name] = ci;

    // Save state
    auto savedBlock = ctx.builder->GetInsertBlock();
    auto savedThis = ctx.currentThis;
    auto savedClassName = ctx.currentClassName;
    auto savedLocals = ctx.saveScope();
    auto savedRefTypes = ctx.refTypes;
    auto savedProcName = ctx.currentProcName;
    auto savedRetAlloca = ctx.returnValueAlloca;
    auto savedArrays = ctx.arrays;
    auto savedLabelBlocks = ctx.labelBlocks;

    ctx.currentClassName = name;
    ctx.currentProcName = "";
    ctx.returnValueAlloca = nullptr;

    // First pass: compile procedure declarations as methods
    for (auto& stmt : bodyStmts) {
        if (dynamic_cast<ProcedureDecl*>(stmt.get())) {
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
    ctx.arrays.clear();
    ctx.labelBlocks.clear();

    // Set up ref types for REF fields
    for (auto& fi : ctx.classes[name].fields) {
        if (fi.type == -1) {
            ctx.refTypes[fi.name] = fi.refClassName;
        }
    }

    // Set up TEXT field position tracking
    ctx.setupTextFieldTracking(bodyFunc);

    // Execute body statements
    // Skip: ProcedureDecls (compiled above), VarDeclarations/RefDeclarations (struct fields),
    //        CompoundStmt of all VarDecls (multi-var decls as struct fields)
    // DO execute: ArrayDeclarations (allocate and store pointer in struct field),
    //             LabelDeclarations, executable statements
    for (auto& stmt : bodyStmts) {
        if (dynamic_cast<ProcedureDecl*>(stmt.get())) continue;
        if (dynamic_cast<VarDeclaration*>(stmt.get())) continue;
        if (dynamic_cast<RefDeclaration*>(stmt.get())) continue;
        if (auto* cs = dynamic_cast<CompoundStmt*>(stmt.get())) {
            bool allVarDecl = true;
            for (auto& s : cs->statements) {
                if (!dynamic_cast<VarDeclaration*>(s.get())) { allVarDecl = false; break; }
            }
            if (allVarDecl) continue;
        }
        if (auto* ad = dynamic_cast<ArrayDeclaration*>(stmt.get())) {
            // Execute the array declaration (creates local alloca + ctx.arrays entry)
            ad->codegen(ctx);
            // Store the array pointer in the struct field so methods can find it
            int idx = ctx.getFieldIndex(name, ad->name);
            if (idx >= 0) {
                auto& info = ctx.arrays[ad->name];
                auto gep = ctx.builder->CreateStructGEP(ctx.classes[name].structType,
                    ctx.currentThis, idx, ad->name + "_fptr");
                ctx.builder->CreateStore(info.basePtr, gep);
                // Save array metadata for methods
                ctx.classes[name].arrayMeta[ad->name] = {info.lowerBound, info.size};
            }
            continue;
        }
        stmt->codegen(ctx);
    }

    if (!ctx.builder->GetInsertBlock()->getTerminator()) {
        ctx.builder->CreateRetVoid();
    }
    llvm::verifyFunction(*bodyFunc, &llvm::errs());

    // Restore state
    ctx.builder->SetInsertPoint(savedBlock);
    ctx.currentThis = savedThis;
    ctx.currentClassName = savedClassName;
    ctx.restoreScope(savedLocals);
    ctx.refTypes = savedRefTypes;
    ctx.currentProcName = savedProcName;
    ctx.returnValueAlloca = savedRetAlloca;
    ctx.arrays = savedArrays;
    ctx.labelBlocks = savedLabelBlocks;

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
        if (!ctx.builder->GetInsertBlock()->getTerminator())
            ctx.builder->CreateBr(mergeBB);

        func->insert(func->end(), nextBB);
        ctx.builder->SetInsertPoint(nextBB);
    }

    // OTHERWISE
    if (otherwiseBody) {
        otherwiseBody->codegen(ctx);
    }
    if (!ctx.builder->GetInsertBlock()->getTerminator())
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
    auto fmt = ctx.builder->CreateGlobalString("%.*f", "realfmt");
    auto decI32 = ctx.builder->CreateTrunc(dec, llvm::Type::getInt32Ty(*ctx.llvmContext), "dec32");
    return ctx.builder->CreateCall(ctx.printfFunc, {fmt, decI32, val});
}

llvm::Value* OutFixStatement::codegen(CodeGenContext& ctx) {
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    auto dec = decimals->codegen(ctx);
    if (!dec) return nullptr;
    auto w = width->codegen(ctx);
    if (!w) return nullptr;
    auto fmt = ctx.builder->CreateGlobalString("%*.*f", "fixfmt");
    auto widthI32 = ctx.builder->CreateTrunc(w, llvm::Type::getInt32Ty(*ctx.llvmContext), "width32");
    auto decI32 = ctx.builder->CreateTrunc(dec, llvm::Type::getInt32Ty(*ctx.llvmContext), "dec32");
    return ctx.builder->CreateCall(ctx.printfFunc, {fmt, widthI32, decI32, val});
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

llvm::Value* InImageStatement::codegen(CodeGenContext& ctx) {
    // No-op for now
    return nullptr;
}

// ---- Program ----

llvm::Value* Program::codegen(CodeGenContext& ctx) {
    return block->codegen(ctx);
}
