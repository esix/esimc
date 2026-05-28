#include "codegen.h"
#include "ast.h"

#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <algorithm>
#include <functional>

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

// Build the value to pass for a LABEL argument named `labelName`:
//  - a local label: allocate a {jmp_buf*, id} record pointing at this function's buf
//  - a LABEL parameter: forward the record pointer we were given
// Returns nullptr if the name isn't a recognized label.
llvm::Value* CodeGenContext::makeLabelArg(const std::string& labelName) {
    auto ptrTy = getRefType();
    auto i64Ty = llvm::Type::getInt64Ty(*llvmContext);
    // Forwarding a LABEL parameter: pass the record pointer through unchanged.
    if (labelParamNames.count(labelName)) {
        auto lit = locals.find(labelName);
        if (lit != locals.end())
            return builder->CreateLoad(ptrTy, lit->second, labelName + "_fwd");
    }
    // A local label that's a non-local target: build a fresh record.
    auto idIt = nonLocalLabelIds.find(labelName);
    if (idIt != nonLocalLabelIds.end() && currentJmpBuf && labelRecordType) {
        auto func = builder->GetInsertBlock()->getParent();
        auto rec = createEntryBlockAlloca(func, labelName + "_lblrec", labelRecordType);
        auto bufSlot = builder->CreateStructGEP(labelRecordType, rec, 0, "lr_buf");
        builder->CreateStore(currentJmpBuf, bufSlot);
        auto idSlot = builder->CreateStructGEP(labelRecordType, rec, 1, "lr_id");
        builder->CreateStore(llvm::ConstantInt::get(i64Ty, idIt->second), idSlot);
        return rec;
    }
    return nullptr;
}

void CodeGenContext::buildAllVtables() {
    auto i64Ty = llvm::Type::getInt64Ty(*llvmContext);
    auto ptrTy = getRefType();

    // First: collect ALL method names across each class hierarchy
    // For each class, gather methods from itself AND all descendants
    for (auto& [cname, ci] : classes) {
        // Start with inherited vtable
        if (!ci.parentName.empty()) {
            auto pit = classes.find(ci.parentName);
            if (pit != classes.end()) {
                ci.vtableMethodOrder = pit->second.vtableMethodOrder;
                ci.vtableIndex = pit->second.vtableIndex;
            }
        }
        // Add own methods
        for (auto& [mname, mfunc] : ci.methods) {
            if (ci.vtableIndex.find(mname) == ci.vtableIndex.end()) {
                int idx = (int)ci.vtableMethodOrder.size() + 1;
                ci.vtableIndex[mname] = idx;
                ci.vtableMethodOrder.push_back(mname);
            }
        }
    }

    // Second pass: propagate child methods UP to parents
    // (for virtual methods declared in parent but only implemented in children)
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto& [cname, ci] : classes) {
            if (ci.parentName.empty()) continue;
            auto pit = classes.find(ci.parentName);
            if (pit == classes.end()) continue;
            for (auto& [mname, idx] : ci.vtableIndex) {
                if (pit->second.vtableIndex.find(mname) == pit->second.vtableIndex.end()) {
                    int newIdx = (int)pit->second.vtableMethodOrder.size() + 1;
                    pit->second.vtableIndex[mname] = newIdx;
                    pit->second.vtableMethodOrder.push_back(mname);
                    changed = true;
                }
            }
        }
    }

    // Ensure child vtable indices match parent indices
    for (auto& [cname, ci] : classes) {
        if (ci.parentName.empty()) continue;
        auto pit = classes.find(ci.parentName);
        if (pit == classes.end()) continue;
        // Adopt parent's indices
        for (auto& [mname, idx] : pit->second.vtableIndex) {
            ci.vtableIndex[mname] = idx;
        }
        ci.vtableMethodOrder = pit->second.vtableMethodOrder;
        // Add own new methods after parent's
        for (auto& [mname, mfunc] : ci.methods) {
            if (ci.vtableIndex.find(mname) == ci.vtableIndex.end()) {
                int newIdx = (int)ci.vtableMethodOrder.size() + 1;
                ci.vtableIndex[mname] = newIdx;
                ci.vtableMethodOrder.push_back(mname);
            }
        }
    }

    // Build vtable types and globals for each class
    for (auto& [cname, ci] : classes) {
        std::vector<llvm::Type*> vtFields;
        vtFields.push_back(i64Ty); // classId at index 0
        for (size_t i = 0; i < ci.vtableMethodOrder.size(); i++) {
            vtFields.push_back(ptrTy);
        }
        // Update the existing placeholder vtable type's body
        // (preserves all existing references to this type)
        if (ci.vtableType && ci.vtableType->isOpaque()) {
            ci.vtableType->setBody(vtFields);
        } else if (ci.vtableType) {
            // Already has a body — set it to the new fields
            // setBody on a non-opaque type may fail; recreate if so
            ci.vtableType->setBody(vtFields);
        } else {
            ci.vtableType = llvm::StructType::create(*llvmContext, vtFields,
                                                      cname + "_vtable_t");
        }

        std::vector<llvm::Constant*> vtValues;
        vtValues.push_back(llvm::ConstantInt::get(i64Ty, ci.classId));
        for (auto& mname : ci.vtableMethodOrder) {
            auto mit = ci.methods.find(mname);
            if (mit != ci.methods.end()) {
                vtValues.push_back(mit->second);
            } else {
                // Check parent chain for inherited implementation
                llvm::Function* inherited = nullptr;
                std::string search = ci.parentName;
                while (!search.empty()) {
                    auto sc = classes.find(search);
                    if (sc == classes.end()) break;
                    auto sm = sc->second.methods.find(mname);
                    if (sm != sc->second.methods.end()) {
                        inherited = sm->second;
                        break;
                    }
                    search = sc->second.parentName;
                }
                if (inherited)
                    vtValues.push_back(inherited);
                else
                    vtValues.push_back(llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*llvmContext)));
            }
        }
        auto vtInit = llvm::ConstantStruct::get(ci.vtableType, vtValues);
        auto newVtGlobal = new llvm::GlobalVariable(
            *module, ci.vtableType, true, llvm::GlobalValue::InternalLinkage,
            vtInit, cname + "_vtable_real");

        // Replace placeholder with real vtable
        if (ci.vtableGlobal) {
            ci.vtableGlobal->replaceAllUsesWith(newVtGlobal);
            ci.vtableGlobal->eraseFromParent();
        }
        ci.vtableGlobal = newVtGlobal;
    }
}

llvm::Value* CodeGenContext::loadClassId(llvm::Value* obj) {
    auto ptrTy = llvm::PointerType::getUnqual(*llvmContext);
    auto i64Ty = llvm::Type::getInt64Ty(*llvmContext);
    // Object index 0 is vtable pointer. Vtable index 0 is classId.
    auto baseTy = llvm::StructType::get(*llvmContext, {ptrTy, ptrTy});
    auto vtSlot = builder->CreateStructGEP(baseTy, obj, 0, "vt_slot");
    auto vtPtr = builder->CreateLoad(ptrTy, vtSlot, "vtptr");
    // vtable layout: {i64 classId, ptr method1, ...} — load classId at offset 0
    auto cidSlot = builder->CreateGEP(i64Ty, vtPtr,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(*llvmContext), 0), "cid_slot");
    return builder->CreateLoad(i64Ty, cidSlot, "classid");
}

std::pair<llvm::Value*, llvm::Type*> CodeGenContext::getVarPtr(const std::string& name) {
    // Check NAME parameters first (pass-by-reference)
    auto npit = nameParams.find(name);
    if (npit != nameParams.end()) {
        return {npit->second.first, npit->second.second};
    }
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
    // Check global variables (from outermost block)
    auto git = globals.find(name);
    if (git != globals.end()) {
        return {git->second, git->second->getValueType()};
    }
    return {nullptr, nullptr};
}

void CodeGenContext::setupTextFieldTracking(llvm::Function* func) {
    if (currentClassName.empty() || !currentThis) return;
    auto cit = classes.find(currentClassName);
    if (cit == classes.end()) return;
    for (auto& fi : cit->second.fields) {
        if (fi.type == VarDeclaration::TEXT && fi.structIndex >= 2 && fi.refClassName.empty()) {
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

    // setjmp(ptr) -> i32 ; longjmp(ptr, i32) for non-local GOTO (LABEL params)
    setjmpFunc = llvm::Function::Create(
        llvm::FunctionType::get(i32Ty, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "setjmp", module.get());
    longjmpFunc = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {ptrTy, i32Ty}, false),
        llvm::Function::ExternalLinkage, "longjmp", module.get());
    longjmpFunc->addFnAttr(llvm::Attribute::NoReturn);
    // Label record: { ptr jmpbuf, i64 id }
    labelRecordType = llvm::StructType::create(*llvmContext,
        {ptrTy, i64Ty}, "simula_label");

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

    // Create global LASTITEM flag (i1, initially false)
    auto lastitemGv = new llvm::GlobalVariable(
        *module, llvm::Type::getInt1Ty(*llvmContext), false,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantInt::getFalse(*llvmContext), "g___lastitem");
    globals["__lastitem"] = lastitemGv;

    // Stub globals for Simula standard files (SYSIN, SYSOUT, etc.)
    auto ptrTy = llvm::PointerType::getUnqual(*llvmContext);
    for (auto stdName : {"sysin", "sysout"}) {
        auto gv = new llvm::GlobalVariable(
            *module, ptrTy, false, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(ptrTy), std::string("g_") + stdName);
        globals[stdName] = gv;
    }

    inMainBlock = true;
    program.codegen(*this);
    inMainBlock = false;

    // Build vtables after all classes are declared
    buildAllVtables();

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

    // Check NAME parameters (load through pointer)
    auto npit = ctx.nameParams.find(name);
    if (npit != ctx.nameParams.end()) {
        return ctx.builder->CreateLoad(npit->second.second, npit->second.first, name);
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

    // Check global variables (from outermost block, accessible by procedures)
    auto git = ctx.globals.find(name);
    if (git != ctx.globals.end()) {
        return ctx.builder->CreateLoad(git->second->getValueType(), git->second, name);
    }

    // Check if it's an array name (return pointer to array data)
    auto ait = ctx.arrays.find(name);
    if (ait != ctx.arrays.end()) {
        return ait->second.basePtr;
    }

    // Check if it's a no-arg function/procedure call (Simula allows omitting parens)
    auto func = ctx.module->getFunction(name);
    if (func) {
        // Check if the function has only captured params (no explicit params)
        auto capIt = ctx.capturedVars.find(name);
        size_t numCaptured = (capIt != ctx.capturedVars.end()) ? capIt->second.size() : 0;
        if (func->arg_size() == numCaptured) {
            std::vector<llvm::Value*> callArgs;
            if (capIt != ctx.capturedVars.end()) {
                for (auto& capName : capIt->second) {
                    if (capName == "__this") {
                        if (ctx.currentThis)
                            callArgs.push_back(ctx.currentThis);
                        else
                            callArgs.push_back(llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*ctx.llvmContext)));
                    } else if (capName.substr(0, 6) == "__arr_") {
                        std::string arrName = capName.substr(6);
                        auto ait2 = ctx.arrays.find(arrName);
                        if (ait2 != ctx.arrays.end())
                            callArgs.push_back(ait2->second.basePtr);
                        else
                            callArgs.push_back(llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*ctx.llvmContext)));
                    } else {
                        auto [ptr, ty] = ctx.getVarPtr(capName);
                        if (ptr) callArgs.push_back(ptr);
                        else callArgs.push_back(llvm::ConstantPointerNull::get(
                            llvm::PointerType::getUnqual(*ctx.llvmContext)));
                    }
                }
            }
            if (func->getReturnType()->isVoidTy())
                return ctx.builder->CreateCall(func, callArgs);
            return ctx.builder->CreateCall(func, callArgs, name + "_ret");
        }
    }
    // Check for no-arg class method
    if (ctx.currentThis && !ctx.currentClassName.empty()) {
        auto& ci = ctx.classes[ctx.currentClassName];
        auto mit = ci.methods.find(name);
        if (mit != ci.methods.end() && mit->second->arg_size() == 1) {
            if (mit->second->getReturnType()->isVoidTy())
                return ctx.builder->CreateCall(mit->second, {ctx.currentThis});
            return ctx.builder->CreateCall(mit->second, {ctx.currentThis}, name + "_ret");
        }
    }

    // If the name refers to a function, return its pointer (procedure-as-value)
    if (func) {
        return func;
    }

    // If the name is a known label, return its index (LABEL parameter passing)
    if (ctx.labelBlocks.count(name)) {
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), 0);
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

    // POWER: use pow() from libm
    if (op == POWER) {
        auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
        auto Lf = L, Rf = R;
        if (!Lf->getType()->isDoubleTy())
            Lf = ctx.builder->CreateSIToFP(L, doubleTy, "tofp");
        if (!Rf->getType()->isDoubleTy())
            Rf = ctx.builder->CreateSIToFP(R, doubleTy, "tofp");
        auto powFunc = ctx.module->getOrInsertFunction("pow",
            llvm::FunctionType::get(doubleTy, {doubleTy, doubleTy}, false));
        return ctx.builder->CreateCall(powFunc, {Lf, Rf}, "powtmp");
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
            case AND: case OR: case CONCAT: case POWER: break;
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
        case CONCAT: case POWER: break; // handled above
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

    // UPPERBOUND(arr, dim) / LOWERBOUND(arr, dim)
    if (name == "upperbound" || name == "lowerbound") {
        if (args.empty()) return nullptr;
        // First arg should be an identifier naming an array
        if (auto* id = dynamic_cast<Identifier*>(args[0].get())) {
            auto ait2 = ctx.arrays.find(id->name);
            if (ait2 != ctx.arrays.end()) {
                auto& info = ait2->second;
                if (name == "lowerbound") {
                    auto loIt = ctx.locals.find(id->name + "__lo");
                    if (loIt != ctx.locals.end())
                        return ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
                    return llvm::ConstantInt::get(i64Ty, info.lowerBound);
                } else {
                    // Prefer dynamic hi (set for array params); fall back to static
                    auto hiIt = ctx.locals.find(id->name + "__hi");
                    if (hiIt != ctx.locals.end())
                        return ctx.builder->CreateLoad(i64Ty, hiIt->second, "hi");
                    auto loIt = ctx.locals.find(id->name + "__lo");
                    llvm::Value* lo;
                    if (loIt != ctx.locals.end())
                        lo = ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
                    else
                        lo = llvm::ConstantInt::get(i64Ty, info.lowerBound);
                    return ctx.builder->CreateAdd(lo,
                        llvm::ConstantInt::get(i64Ty, info.size > 0 ? info.size - 1 : 0), "hi");
                }
            }
        }
        std::cerr << "Error: cannot determine array bounds\n";
        return llvm::ConstantInt::get(i64Ty, 0);
    }

    // OUTCHAR(ch) — print a single character
    // ERROR(msg) — print error and abort
    if (name == "error") {
        auto exitFunc = ctx.module->getOrInsertFunction("exit",
            llvm::FunctionType::get(llvm::Type::getVoidTy(*ctx.llvmContext),
                {llvm::Type::getInt32Ty(*ctx.llvmContext)}, false));
        if (!args.empty()) {
            auto msg = args[0]->codegen(ctx);
            if (msg) {
                auto fmt = ctx.builder->CreateGlobalString("Error: %s\n", "errfmt");
                ctx.builder->CreateCall(ctx.printfFunc, {fmt, msg});
            }
        }
        ctx.builder->CreateCall(exitFunc, {llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(*ctx.llvmContext), 1)});
        return llvm::ConstantInt::get(i64Ty, 0);
    }

    // UNIFORM(lo, hi, seed) — random real. Uses C rand().
    if (name == "uniform") {
        auto lo = args.size() > 0 ? args[0]->codegen(ctx) : nullptr;
        auto hi = args.size() > 1 ? args[1]->codegen(ctx) : nullptr;
        if (!lo || !hi) return llvm::ConstantFP::get(doubleTy, 0.0);
        auto randFunc = ctx.module->getOrInsertFunction("rand",
            llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx.llvmContext), false));
        auto rval = ctx.builder->CreateCall(randFunc, {}, "rval");
        auto rvalD = ctx.builder->CreateSIToFP(rval, doubleTy, "rvald");
        auto maxD = llvm::ConstantFP::get(doubleTy, 2147483647.0);
        auto norm = ctx.builder->CreateFDiv(rvalD, maxD, "norm");
        auto loD = lo->getType()->isDoubleTy() ? lo : ctx.builder->CreateSIToFP(lo, doubleTy);
        auto hiD = hi->getType()->isDoubleTy() ? hi : ctx.builder->CreateSIToFP(hi, doubleTy);
        auto range = ctx.builder->CreateFSub(hiD, loD, "range");
        return ctx.builder->CreateFAdd(loD, ctx.builder->CreateFMul(norm, range), "uniform");
    }

    // RANDINT(low, high, seed) — random integer. Uses C rand() ignoring seed.
    if (name == "randint") {
        auto lo = args[0]->codegen(ctx);
        auto hi = args[1]->codegen(ctx);
        if (!lo || !hi) return nullptr;
        auto randFunc = ctx.module->getOrInsertFunction("rand",
            llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx.llvmContext), false));
        auto rval = ctx.builder->CreateCall(randFunc, {}, "rval");
        auto rval64 = ctx.builder->CreateSExt(rval, i64Ty, "rval64");
        auto range = ctx.builder->CreateAdd(
            ctx.builder->CreateSub(hi, lo, "range"), llvm::ConstantInt::get(i64Ty, 1), "range1");
        auto modval = ctx.builder->CreateSRem(rval64, range, "modval");
        // Make positive
        auto absmod = ctx.builder->CreateSelect(
            ctx.builder->CreateICmpSLT(modval, llvm::ConstantInt::get(i64Ty, 0)),
            ctx.builder->CreateNeg(modval), modval, "absmod");
        return ctx.builder->CreateAdd(lo, absmod, "randint");
    }

    if (name == "outchar") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        // putchar expects i32
        auto i32Val = ctx.builder->CreateZExt(val,
            llvm::Type::getInt32Ty(*ctx.llvmContext), "chi32");
        auto putcharFunc = ctx.module->getOrInsertFunction("putchar",
            llvm::FunctionType::get(llvm::Type::getInt32Ty(*ctx.llvmContext),
                {llvm::Type::getInt32Ty(*ctx.llvmContext)}, false));
        return ctx.builder->CreateCall(putcharFunc, {i32Val});
    }

    // FACTORIAL — not built-in, but let's handle common utility functions
    // (users define their own; this just avoids confusing error messages)

    // Check if it's a method call on THIS (within a class)
    if (ctx.currentThis && !ctx.currentClassName.empty()) {
        auto& ci = ctx.classes[ctx.currentClassName];
        auto mit = ci.methods.find(name);
        if (mit != ci.methods.end()) {
            std::vector<llvm::Value*> argsV;
            argsV.push_back(ctx.currentThis);
            auto fnTy = mit->second->getFunctionType();
            std::string mFuncName = ctx.currentClassName + "_" + name;
            auto mLpiIt = ctx.labelParamIndices.find(mFuncName);
            const std::set<int>* mLabelIdx = (mLpiIt != ctx.labelParamIndices.end())
                ? &mLpiIt->second : nullptr;
            // arg index in callee: 0 is 'this', so user args start at 1
            for (size_t ai = 0; ai < args.size(); ai++) {
                size_t paramIdx = ai + 1;
                // LABEL parameter: pass a {jmp_buf, id} record for non-local GOTO.
                if (mLabelIdx && mLabelIdx->count((int)ai)) {
                    if (auto* id = dynamic_cast<Identifier*>(args[ai].get())) {
                        if (auto rec = ctx.makeLabelArg(id->name)) {
                            argsV.push_back(rec);
                            continue;
                        }
                    }
                }
                // If callee expects ptr at this position and arg is an Identifier value,
                // pass the variable's address (NAME parameter)
                if (paramIdx < fnTy->getNumParams() &&
                    fnTy->getParamType(paramIdx)->isPointerTy()) {
                    if (auto* id = dynamic_cast<Identifier*>(args[ai].get())) {
                        auto [ptr, ty] = ctx.getVarPtr(id->name);
                        if (ptr && !ty->isPointerTy()) {
                            argsV.push_back(ptr);
                            continue;
                        }
                    }
                }
                auto v = args[ai]->codegen(ctx);
                if (!v) return nullptr;
                if (paramIdx < fnTy->getNumParams()) {
                    auto destTy = fnTy->getParamType(paramIdx);
                    if (v->getType() != destTy) {
                        if (v->getType()->isIntegerTy() && destTy->isDoubleTy())
                            v = ctx.builder->CreateSIToFP(v, destTy, "tofp");
                        else if (v->getType()->isDoubleTy() && destTy->isIntegerTy())
                            v = ctx.builder->CreateFPToSI(v, destTy, "tosi");
                        else if (v->getType()->isIntegerTy() && destTy->isIntegerTy()) {
                            if (v->getType()->getIntegerBitWidth() < destTy->getIntegerBitWidth())
                                v = ctx.builder->CreateZExt(v, destTy);
                            else if (v->getType()->getIntegerBitWidth() > destTy->getIntegerBitWidth())
                                v = ctx.builder->CreateTrunc(v, destTy);
                        } else if (destTy->isPointerTy() && v->getType()->isIntegerTy()) {
                            v = llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(destTy));
                        }
                    }
                }
                argsV.push_back(v);
            }
            return ctx.builder->CreateCall(mit->second, argsV);
        }
    }

    auto coerceArg = [&](llvm::Value* v, llvm::Type* destTy) -> llvm::Value* {
        if (v->getType() == destTy) return v;
        if (v->getType()->isIntegerTy() && destTy->isDoubleTy())
            return ctx.builder->CreateSIToFP(v, destTy, "tofp");
        if (v->getType()->isDoubleTy() && destTy->isIntegerTy())
            return ctx.builder->CreateFPToSI(v, destTy, "tosi");
        if (v->getType()->isIntegerTy() && destTy->isIntegerTy()) {
            if (v->getType()->getIntegerBitWidth() < destTy->getIntegerBitWidth())
                return ctx.builder->CreateZExt(v, destTy);
            return ctx.builder->CreateTrunc(v, destTy);
        }
        if (destTy->isPointerTy() && v->getType()->isIntegerTy())
            return llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(destTy));
        return v;
    };

    // Try finding the method on the current method's owning class (handles INSPECT)
    if (ctx.methodThis && !ctx.methodThisClassName.empty()) {
        std::string searchCls = ctx.methodThisClassName;
        while (!searchCls.empty()) {
            auto cit = ctx.classes.find(searchCls);
            if (cit == ctx.classes.end()) break;
            auto mit = cit->second.methods.find(name);
            if (mit != cit->second.methods.end()) {
                std::vector<llvm::Value*> argsV;
                argsV.push_back(ctx.methodThis);
                auto fnTy = mit->second->getFunctionType();
                unsigned nParams = fnTy->getNumParams();
                for (size_t ai = 0; ai < args.size(); ai++) {
                    auto v = args[ai]->codegen(ctx);
                    if (!v) return nullptr;
                    if (ai + 1 < nParams)
                        v = coerceArg(v, fnTy->getParamType(ai + 1));
                    argsV.push_back(v);
                }
                if (mit->second->getReturnType()->isVoidTy())
                    return ctx.builder->CreateCall(mit->second, argsV);
                return ctx.builder->CreateCall(mit->second, argsV, name + "_ret");
            }
            searchCls = cit->second.parentName;
        }
    }
    // Last resort: any class (less safe)
    if (ctx.currentThis) {
        for (auto& [clsN, clsI] : ctx.classes) {
            auto mit = clsI.methods.find(name);
            if (mit != clsI.methods.end()) {
                std::vector<llvm::Value*> argsV;
                argsV.push_back(ctx.currentThis);
                auto fnTy = mit->second->getFunctionType();
                unsigned nParams = fnTy->getNumParams();
                for (size_t ai = 0; ai < args.size(); ai++) {
                    auto v = args[ai]->codegen(ctx);
                    if (!v) return nullptr;
                    if (ai + 1 < nParams)
                        v = coerceArg(v, fnTy->getParamType(ai + 1));
                    argsV.push_back(v);
                }
                if (mit->second->getReturnType()->isVoidTy())
                    return ctx.builder->CreateCall(mit->second, argsV);
                return ctx.builder->CreateCall(mit->second, argsV, name + "_ret");
            }
        }
    }

    // Check if name is a local variable holding a function pointer (procedure parameter)
    {
        auto lit = ctx.locals.find(name);
        if (lit != ctx.locals.end()) {
            // It's a local — load the function pointer and call it indirectly
            auto ptrTy2 = ctx.getRefType();
            auto fnPtr = ctx.builder->CreateLoad(ptrTy2, lit->second, name + "_fn");
            // Build a function type based on argument types
            std::vector<llvm::Value*> argsV;
            std::vector<llvm::Type*> argTys;
            for (auto& arg : args) {
                auto v = arg->codegen(ctx);
                if (!v) return nullptr;
                argsV.push_back(v);
                argTys.push_back(v->getType());
            }
            // Assume return type matches the first arg (simplification)
            llvm::Type* retTy = args.empty() ?
                llvm::Type::getVoidTy(*ctx.llvmContext) :
                argsV[0]->getType();
            auto fnTy = llvm::FunctionType::get(retTy, argTys, false);
            if (retTy->isVoidTy())
                return ctx.builder->CreateCall(fnTy, fnPtr, argsV);
            return ctx.builder->CreateCall(fnTy, fnPtr, argsV, name + "_iret");
        }
    }

    // Look up in module functions
    auto func = ctx.module->getFunction(name);
    if (!func) {
        std::cerr << "Error: unknown function '" << name << "'\n";
        return nullptr;
    }
    std::vector<llvm::Value*> argsV;
    auto fnTy = func->getFunctionType();
    size_t paramIdx = 0;
    auto npiIt = ctx.nameParamIndices.find(name);
    const std::set<int>* nameIdxSet = (npiIt != ctx.nameParamIndices.end())
        ? &npiIt->second : nullptr;
    auto lpiIt = ctx.labelParamIndices.find(name);
    const std::set<int>* labelIdxSet = (lpiIt != ctx.labelParamIndices.end())
        ? &lpiIt->second : nullptr;
    for (size_t userArgIdx = 0; userArgIdx < args.size(); userArgIdx++) {
        auto& arg = args[userArgIdx];
        bool isNameParam = nameIdxSet && nameIdxSet->count((int)userArgIdx);
        bool isLabelParam = labelIdxSet && labelIdxSet->count((int)userArgIdx);
        // For LABEL parameters: pass a {jmp_buf, id} record so the callee can
        // perform a non-local GOTO back into this function.
        if (isLabelParam) {
            if (auto* id = dynamic_cast<Identifier*>(arg.get())) {
                if (auto rec = ctx.makeLabelArg(id->name)) {
                    argsV.push_back(rec);
                    paramIdx++;
                    continue;
                }
            }
        }
        // For NAME parameters: pass the address of the caller's storage even if
        // the variable is itself a pointer type (e.g. REF). This is what makes
        // assignments inside the callee propagate back to the caller's variable.
        if (isNameParam) {
            if (auto* id = dynamic_cast<Identifier*>(arg.get())) {
                auto [ptr, ty] = ctx.getVarPtr(id->name);
                if (ptr) {
                    argsV.push_back(ptr);
                    paramIdx++;
                    continue;
                }
            }
        }
        // Check if the callee expects a pointer at this position (NAME or ARRAY param)
        if (paramIdx < fnTy->getNumParams() && fnTy->getParamType(paramIdx)->isPointerTy()) {
            if (auto* id = dynamic_cast<Identifier*>(arg.get())) {
                // If it's an array, pass the array's base pointer + lo + hi
                auto ait2 = ctx.arrays.find(id->name);
                if (ait2 != ctx.arrays.end()) {
                    auto i64Ty2 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                    argsV.push_back(ait2->second.basePtr);
                    paramIdx++;
                    // Push lo
                    auto loIt = ctx.locals.find(id->name + "__lo");
                    llvm::Value* loV = loIt != ctx.locals.end()
                        ? (llvm::Value*)ctx.builder->CreateLoad(i64Ty2, loIt->second, "lo_arg")
                        : (llvm::Value*)llvm::ConstantInt::get(i64Ty2, ait2->second.lowerBound);
                    if (paramIdx < fnTy->getNumParams() &&
                        fnTy->getParamType(paramIdx)->isIntegerTy(64)) {
                        argsV.push_back(loV);
                        paramIdx++;
                    }
                    // Push hi
                    auto hiIt = ctx.locals.find(id->name + "__hi");
                    llvm::Value* hiV;
                    if (hiIt != ctx.locals.end()) {
                        hiV = ctx.builder->CreateLoad(i64Ty2, hiIt->second, "hi_arg");
                    } else {
                        long long hiC = ait2->second.lowerBound +
                            (ait2->second.size > 0 ? ait2->second.size - 1 : 0);
                        hiV = llvm::ConstantInt::get(i64Ty2, hiC);
                    }
                    if (paramIdx < fnTy->getNumParams() &&
                        fnTy->getParamType(paramIdx)->isIntegerTy(64)) {
                        argsV.push_back(hiV);
                        paramIdx++;
                    }
                    continue;
                }
                auto [ptr, ty] = ctx.getVarPtr(id->name);
                if (ptr && !ty->isPointerTy()) {
                    // Variable is a value (not already a pointer) — pass its address
                    argsV.push_back(ptr);
                    paramIdx++;
                    continue;
                }
            }
        }
        auto v = arg->codegen(ctx);
        if (!v) return nullptr;
        // Type-coerce arg to match expected param type
        if (paramIdx < fnTy->getNumParams()) {
            auto expectedTy = fnTy->getParamType(paramIdx);
            if (v->getType() != expectedTy) {
                if (expectedTy->isDoubleTy() && v->getType()->isIntegerTy())
                    v = ctx.builder->CreateSIToFP(v, expectedTy, "tofp");
                else if (expectedTy->isIntegerTy(64) && v->getType()->isDoubleTy())
                    v = ctx.builder->CreateFPToSI(v, expectedTy, "toint");
                else if (expectedTy->isIntegerTy() && v->getType()->isIntegerTy()) {
                    if (expectedTy->getIntegerBitWidth() > v->getType()->getIntegerBitWidth())
                        v = ctx.builder->CreateZExt(v, expectedTy, "zext");
                    else
                        v = ctx.builder->CreateTrunc(v, expectedTy, "trunc");
                } else if (expectedTy->isPointerTy() && v->getType()->isIntegerTy()) {
                    // Caller provided an integer where a pointer is expected (e.g. a
                    // LABEL parameter we can't resolve). Pass null pointer instead.
                    v = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(expectedTy));
                }
            }
        }
        argsV.push_back(v);
        paramIdx++;
    }
    // Append captured variables if this function has closures
    auto capIt = ctx.capturedVars.find(name);
    if (capIt != ctx.capturedVars.end()) {
        for (auto& capName : capIt->second) {
            if (capName == "__this") {
                if (ctx.currentThis)
                    argsV.push_back(ctx.currentThis);
                else
                    argsV.push_back(llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*ctx.llvmContext)));
            } else if (capName.substr(0, 6) == "__arr_") {
                std::string arrName = capName.substr(6);
                auto ait2 = ctx.arrays.find(arrName);
                if (ait2 != ctx.arrays.end())
                    argsV.push_back(ait2->second.basePtr);
                else
                    argsV.push_back(llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*ctx.llvmContext)));
            } else {
                auto [ptr, ty] = ctx.getVarPtr(capName);
                if (ptr) {
                    argsV.push_back(ptr);
                } else {
                    argsV.push_back(llvm::ConstantPointerNull::get(
                        llvm::PointerType::getUnqual(*ctx.llvmContext)));
                }
            }
        }
    }
    if (func->getReturnType()->isVoidTy())
        return ctx.builder->CreateCall(func, argsV);
    return ctx.builder->CreateCall(func, argsV, name + "_ret");
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

    // Store vtable pointer at index 0
    auto vtableSlot = ctx.builder->CreateStructGEP(ci.structType, obj, 0, "vtable_ptr");
    ctx.builder->CreateStore(ci.vtableGlobal, vtableSlot);

    // Create coroutine context and store at index 1
    auto coro = ctx.builder->CreateCall(ctx.coroCreateFunc);
    auto coroPtr = ctx.builder->CreateStructGEP(ci.structType, obj, 1, "coro_ptr");
    ctx.builder->CreateStore(coro, coroPtr);

    // Store constructor arguments into fields.
    // Constructor params start at field index 2. Skip companion fields like
    // "FIELD__pos" that aren't real constructor parameters — those are paired
    // bookkeeping fields the compiler adds and shouldn't consume an argument.
    size_t argIdx = 0;
    for (auto& param : ci.fields) {
        if (param.structIndex < 2) continue;
        if (param.name.size() >= 5 &&
            param.name.compare(param.name.size() - 5, 5, "__pos") == 0) continue;
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
    // Special case: printfile/sysout no-arg I/O methods (e.g., obj.outimage)
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        std::string clsN = ctx.resolveRefType(ident->name);
        if (clsN == "printfile" || ident->name == "sysout") {
            if (member == "outimage") {
                return OutImageStatement().codegen(ctx);
            }
        }
    }
    // Special case: sysin.image — read a line from stdin
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        if (ident->name == "sysin" && member == "image") {
            // Allocate a 1024-byte buffer, call fgets(buf, 1024, stdin)
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto bufSize = llvm::ConstantInt::get(i64Ty, 1024);
            auto buf = ctx.builder->CreateCall(ctx.allocFunc, {bufSize}, "sysin_buf");
            auto fgetsFunc = ctx.module->getOrInsertFunction("fgets",
                llvm::FunctionType::get(ptrTy, {ptrTy, i32Ty, ptrTy}, false));
            auto stdinGv = ctx.module->getOrInsertGlobal("__stdinp", ptrTy);
            auto stdinPtr = ctx.builder->CreateLoad(ptrTy, stdinGv, "stdin");
            ctx.builder->CreateCall(fgetsFunc,
                {buf, llvm::ConstantInt::get(i32Ty, 1024), stdinPtr});
            return buf;
        }
    }

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

    // If the result is a pointer and member is a known TEXT operation, dispatch to it
    if (obj->getType()->isPointerTy()) {
        if (member == "strip") {
            return ctx.builder->CreateCall(ctx.textStripFunc, {obj}, "stripped");
        }
        if (member == "length") {
            return ctx.builder->CreateCall(ctx.textLengthFunc, {obj}, "len");
        }
    }

    std::string clsName;
    if (auto* qua = dynamic_cast<QuaExpression*>(object.get())) {
        clsName = qua->className;
    } else if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }
    if (clsName.empty()) {
        if (auto* call = dynamic_cast<ProcedureCall*>(object.get()))
            clsName = ctx.resolveRefType(call->name);
    }
    if (clsName.empty()) {
        if (auto* ne = dynamic_cast<NewExpression*>(object.get()))
            clsName = ne->className;
    }

    if (clsName.empty()) {
        std::cerr << "Error: cannot determine class type for member access '." << member << "'\n";
        return nullptr;
    }

    int idx = ctx.getFieldIndex(clsName, member);
    if (idx >= 0) {
        auto& ci = ctx.classes[clsName];
        auto gep = ctx.builder->CreateStructGEP(ci.structType, obj, idx, member + "_ptr");
        auto fieldTy = ci.structType->getElementType(idx);
        return ctx.builder->CreateLoad(fieldTy, gep, member);
    }

    // No field — try as a no-arg method call via vtable (virtual dispatch)
    {
        auto cit2 = ctx.classes.find(clsName);
        if (cit2 != ctx.classes.end()) {
            auto vtIt = cit2->second.vtableIndex.find(member);
            if (vtIt != cit2->second.vtableIndex.end() && cit2->second.vtableType) {
                int vtIdx = vtIt->second;
                auto ptrTy2 = ctx.getRefType();
                auto baseObjTy = llvm::StructType::get(*ctx.llvmContext, {ptrTy2, ptrTy2});
                auto vtSlot = ctx.builder->CreateStructGEP(baseObjTy, obj, 0, "vt_slot");
                auto vtPtr = ctx.builder->CreateLoad(ptrTy2, vtSlot, "vtptr");
                auto fpSlot = ctx.builder->CreateStructGEP(cit2->second.vtableType, vtPtr,
                                                            vtIdx, "fp_slot");
                auto fp = ctx.builder->CreateLoad(ptrTy2, fpSlot, "method_fp");
                // Find function type
                llvm::FunctionType* funcTy = nullptr;
                for (auto& [cn, ci2] : ctx.classes) {
                    auto mm = ci2.methods.find(member);
                    if (mm != ci2.methods.end()) { funcTy = mm->second->getFunctionType(); break; }
                }
                if (funcTy) {
                    if (funcTy->getReturnType()->isVoidTy())
                        return ctx.builder->CreateCall(funcTy, fp, {obj});
                    return ctx.builder->CreateCall(funcTy, fp, {obj}, member + "_vret");
                }
            }
        }
    }
    // Static dispatch fallback
    std::string searchClass = clsName;
    while (!searchClass.empty()) {
        auto cit3 = ctx.classes.find(searchClass);
        if (cit3 == ctx.classes.end()) break;
        auto mit = cit3->second.methods.find(member);
        if (mit != cit3->second.methods.end()) {
            if (mit->second->getReturnType()->isVoidTy())
                return ctx.builder->CreateCall(mit->second, {obj});
            return ctx.builder->CreateCall(mit->second, {obj}, member + "_ret");
        }
        searchClass = cit3->second.parentName;
    }

    std::cerr << "Error: class '" << clsName << "' has no field or method '" << member << "'\n";
    return nullptr;
}

llvm::Value* MethodCall::codegen(CodeGenContext& ctx) {
    // Special case: I/O method calls on printfile/sysout — dispatch to global procs
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        std::string clsN = ctx.resolveRefType(ident->name);
        if (clsN == "printfile" || ident->name == "sysout") {
            // Dispatch obj.OUTFIX(args) to global OutFix, etc.
            if (method == "outfix" && args.size() == 3) {
                return OutFixStatement(
                    ExprPtr(args[0].release()),
                    ExprPtr(args[1].release()),
                    ExprPtr(args[2].release())).codegen(ctx);
            }
            if (method == "outint" && args.size() == 2) {
                return OutIntStatement(
                    ExprPtr(args[0].release()),
                    ExprPtr(args[1].release())).codegen(ctx);
            }
            if (method == "outtext" && args.size() == 1) {
                return OutTextStatement(ExprPtr(args[0].release())).codegen(ctx);
            }
            if (method == "outimage" && args.empty()) {
                return OutImageStatement().codegen(ctx);
            }
        }
    }

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
    if (auto* qua = dynamic_cast<QuaExpression*>(object.get())) {
        clsName = qua->className;
    } else if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }
    if (clsName.empty()) {
        if (auto* call = dynamic_cast<ProcedureCall*>(object.get())) {
            clsName = ctx.resolveRefType(call->name);
        }
    }
    if (clsName.empty()) {
        if (auto* ne = dynamic_cast<NewExpression*>(object.get()))
            clsName = ne->className;
    }
    if (clsName.empty()) {
        // Inner MemberAccess like obj.field returning REF — get the field's refClass
        if (auto* ma = dynamic_cast<MemberAccess*>(object.get())) {
            std::string innerCls;
            if (auto* id = dynamic_cast<Identifier*>(ma->object.get())) {
                innerCls = ctx.resolveRefType(id->name);
            }
            if (innerCls.empty() && dynamic_cast<ThisExpression*>(ma->object.get())) {
                innerCls = ctx.currentClassName;
            }
            if (!innerCls.empty()) {
                auto cit2 = ctx.classes.find(innerCls);
                if (cit2 != ctx.classes.end()) {
                    for (auto& f : cit2->second.fields) {
                        if (f.name == ma->member && !f.refClassName.empty()) {
                            clsName = f.refClassName;
                            break;
                        }
                    }
                }
            }
        }
    }
    if (clsName.empty()) {
        // Inner MethodCall like obj.field(i) returning REF — find the field's refClass
        if (auto* mc = dynamic_cast<MethodCall*>(object.get())) {
            // Resolve the inner method's owning class, then check if its field's
            // arrayMeta indicates a REF array
            std::string innerCls;
            if (auto* id = dynamic_cast<Identifier*>(mc->object.get())) {
                innerCls = ctx.resolveRefType(id->name);
            } else if (auto* mc2 = dynamic_cast<MethodCall*>(mc->object.get())) {
                // Recursive: get type from another nested call
                // (try same logic — for one level of nesting)
                if (auto* id2 = dynamic_cast<Identifier*>(mc2->object.get())) {
                    auto innerCls2 = ctx.resolveRefType(id2->name);
                    if (!innerCls2.empty()) {
                        auto cit2 = ctx.classes.find(innerCls2);
                        if (cit2 != ctx.classes.end()) {
                            for (auto& f : cit2->second.fields) {
                                if (f.name == mc2->method && !f.refClassName.empty()) {
                                    innerCls = f.refClassName;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (!innerCls.empty()) {
                auto cit2 = ctx.classes.find(innerCls);
                if (cit2 != ctx.classes.end()) {
                    for (auto& f : cit2->second.fields) {
                        if (f.name == mc->method && !f.refClassName.empty()) {
                            clsName = f.refClassName;
                            break;
                        }
                    }
                }
            }
            // Fallback: use ctx.refTypes for method name (registered when method's ProcedureDecl
            // was compiled — works when method names are unique in the program)
            if (clsName.empty()) {
                clsName = ctx.resolveRefType(mc->method);
            }
        }
    }

    if (clsName.empty()) {
        std::cerr << "Error: cannot determine class type for method call '." << method << "'\n";
        return nullptr;
    }

    // Look up the method — try vtable dispatch first
    auto cit = ctx.classes.find(clsName);
    if (cit != ctx.classes.end()) {
        auto vtIt = cit->second.vtableIndex.find(method);
        if (vtIt != cit->second.vtableIndex.end() && cit->second.vtableType) {
            // Virtual dispatch through vtable
            int vtIdx = vtIt->second;
            auto ptrTy2 = ctx.getRefType();
            auto baseObjTy = llvm::StructType::get(*ctx.llvmContext, {ptrTy2, ptrTy2});
            auto vtSlot = ctx.builder->CreateStructGEP(baseObjTy, obj, 0, "vt_slot");
            auto vtPtr = ctx.builder->CreateLoad(ptrTy2, vtSlot, "vtptr");
            auto fpSlot = ctx.builder->CreateStructGEP(cit->second.vtableType, vtPtr,
                                                        vtIdx, "fp_slot");
            auto fp = ctx.builder->CreateLoad(ptrTy2, fpSlot, "method_fp");

            // Build the function type from the method we know about
            // Find ANY implementation to get the function type
            llvm::FunctionType* funcTy = nullptr;
            std::string searchCls = clsName;
            while (!searchCls.empty()) {
                auto sc = ctx.classes.find(searchCls);
                if (sc == ctx.classes.end()) break;
                auto mm = sc->second.methods.find(method);
                if (mm != sc->second.methods.end()) {
                    funcTy = mm->second->getFunctionType();
                    break;
                }
                searchCls = sc->second.parentName;
            }
            // Also check child classes if not found in parent chain
            if (!funcTy) {
                for (auto& [cn, ci2] : ctx.classes) {
                    auto mm = ci2.methods.find(method);
                    if (mm != ci2.methods.end()) {
                        funcTy = mm->second->getFunctionType();
                        break;
                    }
                }
            }
            // Fallback: if no implementation exists yet, infer return type as i1
            // (likely BOOLEAN — common for virtual methods like LESS, EQUAL, MORE)
            if (!funcTy) {
                std::vector<llvm::Type*> paramTypes;
                paramTypes.push_back(ptrTy2); // this
                std::vector<llvm::Value*> argVals;
                for (auto& arg : args) {
                    auto v = arg->codegen(ctx);
                    if (!v) return nullptr;
                    argVals.push_back(v);
                    paramTypes.push_back(v->getType());
                }
                auto i1Ty = llvm::Type::getInt1Ty(*ctx.llvmContext);
                funcTy = llvm::FunctionType::get(i1Ty, paramTypes, false);
                std::vector<llvm::Value*> argsV;
                argsV.push_back(obj);
                for (auto& v : argVals) argsV.push_back(v);
                return ctx.builder->CreateCall(funcTy, fp, argsV, method + "_vret");
            }
            if (funcTy) {
                std::vector<llvm::Value*> argsV;
                argsV.push_back(obj);
                unsigned nParams = funcTy->getNumParams();
                auto doubleTy3 = llvm::Type::getDoubleTy(*ctx.llvmContext);
                for (size_t ai = 0; ai < args.size(); ai++) {
                    auto v = args[ai]->codegen(ctx);
                    if (!v) return nullptr;
                    unsigned paramIdx = ai + 1;
                    if (paramIdx < nParams) {
                        auto destTy = funcTy->getParamType(paramIdx);
                        if (v->getType() != destTy) {
                            if (v->getType()->isIntegerTy() && destTy->isDoubleTy())
                                v = ctx.builder->CreateSIToFP(v, destTy, "tofp");
                            else if (v->getType()->isDoubleTy() && destTy->isIntegerTy())
                                v = ctx.builder->CreateFPToSI(v, destTy, "tosi");
                            else if (v->getType()->isIntegerTy() && destTy->isIntegerTy()) {
                                if (v->getType()->getIntegerBitWidth() < destTy->getIntegerBitWidth())
                                    v = ctx.builder->CreateZExt(v, destTy);
                                else if (v->getType()->getIntegerBitWidth() > destTy->getIntegerBitWidth())
                                    v = ctx.builder->CreateTrunc(v, destTy);
                            } else if (destTy->isPointerTy() && v->getType()->isIntegerTy()) {
                                v = llvm::ConstantPointerNull::get(
                                    llvm::cast<llvm::PointerType>(destTy));
                            }
                        }
                    }
                    argsV.push_back(v);
                }
                if (funcTy->getReturnType()->isVoidTy())
                    return ctx.builder->CreateCall(funcTy, fp, argsV);
                return ctx.builder->CreateCall(funcTy, fp, argsV, method + "_vret");
            }
        }
    }

    // Try as member array access: obj.field(index)
    auto cit3 = ctx.classes.find(clsName);
    if (cit3 != ctx.classes.end()) {
        auto mit3 = cit3->second.arrayMeta.find(method);
        if (mit3 != cit3->second.arrayMeta.end() && !args.empty()) {
            // Member array access — get the field (pointer to data) and index
            int fldIdx = ctx.getFieldIndex(clsName, method);
            if (fldIdx >= 0) {
                auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
                auto ptrTy2 = ctx.getRefType();
                auto& ci = cit3->second;
                auto gep = ctx.builder->CreateStructGEP(ci.structType, obj, fldIdx, method + "_aptr");
                auto arrPtr = ctx.builder->CreateLoad(ptrTy2, gep, method + "_arr");
                auto idxVal = args[0]->codegen(ctx);
                if (!idxVal) return nullptr;
                long long lo = mit3->second.first;
                auto adjusted = ctx.builder->CreateSub(idxVal,
                    llvm::ConstantInt::get(i64Ty, lo), "adj_idx");
                // Element type — check refClassName for REF arrays
                llvm::Type* elemTy = ptrTy2;
                for (auto& f : ci.fields) {
                    if (f.name == method) {
                        if (f.refClassName.empty()) elemTy = ctx.getLLVMType(f.type);
                        break;
                    }
                }
                auto elemGep = ctx.builder->CreateGEP(elemTy, arrPtr, adjusted, "marr_elem");
                return ctx.builder->CreateLoad(elemTy, elemGep, method + "_val");
            }
        }
    }

    // Fallback: direct call (static dispatch)
    llvm::Function* methodFunc = nullptr;
    std::string searchClass = clsName;
    while (!searchClass.empty()) {
        auto sc = ctx.classes.find(searchClass);
        if (sc == ctx.classes.end()) break;
        auto mit = sc->second.methods.find(method);
        if (mit != sc->second.methods.end()) {
            methodFunc = mit->second;
            break;
        }
        searchClass = sc->second.parentName;
    }

    if (!methodFunc) {
        std::cerr << "Error: class '" << clsName << "' has no method '" << method << "'\n";
        return nullptr;
    }

    std::vector<llvm::Value*> argsV;
    argsV.push_back(obj);
    auto fnTy = methodFunc->getFunctionType();
    unsigned nParams = fnTy->getNumParams();
    for (size_t ai = 0; ai < args.size(); ai++) {
        auto v = args[ai]->codegen(ctx);
        if (!v) return nullptr;
        unsigned pi = ai + 1;
        if (pi < nParams) {
            auto destTy = fnTy->getParamType(pi);
            if (v->getType() != destTy) {
                if (v->getType()->isIntegerTy() && destTy->isDoubleTy())
                    v = ctx.builder->CreateSIToFP(v, destTy, "tofp");
                else if (v->getType()->isDoubleTy() && destTy->isIntegerTy())
                    v = ctx.builder->CreateFPToSI(v, destTy, "tosi");
                else if (v->getType()->isIntegerTy() && destTy->isIntegerTy()) {
                    if (v->getType()->getIntegerBitWidth() < destTy->getIntegerBitWidth())
                        v = ctx.builder->CreateZExt(v, destTy);
                    else if (v->getType()->getIntegerBitWidth() > destTy->getIntegerBitWidth())
                        v = ctx.builder->CreateTrunc(v, destTy);
                } else if (destTy->isPointerTy() && v->getType()->isIntegerTy()) {
                    v = llvm::ConstantPointerNull::get(
                        llvm::cast<llvm::PointerType>(destTy));
                }
            }
        }
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
    auto classId = ctx.loadClassId(obj);
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
    auto classId = ctx.loadClassId(obj);

    llvm::Value* result = ctx.builder->getFalse();
    for (int id : ids) {
        auto cmp = ctx.builder->CreateICmpEQ(classId,
            llvm::ConstantInt::get(i64Ty, id), "in_cmp");
        result = ctx.builder->CreateOr(result, cmp, "in_or");
    }
    return result;
}

// ---- Conditional expression ----

llvm::Value* QuaExpression::codegen(CodeGenContext& ctx) {
    return object->codegen(ctx);
}

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
    auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, "inint_tmp", i64Ty);
    ctx.builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), alloca);
    auto fmt = ctx.builder->CreateGlobalString("%lld", "intinfmt");
    auto ret = ctx.builder->CreateCall(ctx.scanfFunc, {fmt, alloca}, "scanf_ret");
    // Set LASTITEM if scanf returned <= 0 (EOF or error)
    auto lastitemGv = ctx.globals.find("__lastitem");
    if (lastitemGv != ctx.globals.end()) {
        auto isEof = ctx.builder->CreateICmpSLE(ret,
            llvm::ConstantInt::get(i32Ty, 0), "iseof");
        ctx.builder->CreateStore(isEof, lastitemGv->second);
    }
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

    // First pass: process all declarations (vars, arrays, refs, labels)
    // so that procedures declared later in the block can reference them
    for (auto& stmt : statements) {
        if (dynamic_cast<VarDeclaration*>(stmt.get()) ||
            dynamic_cast<CompoundStmt*>(stmt.get()) ||
            dynamic_cast<ArrayDeclaration*>(stmt.get()) ||
            dynamic_cast<RefDeclaration*>(stmt.get()) ||
            dynamic_cast<LabelDeclaration*>(stmt.get())) {
            stmt->codegen(ctx);
        }
    }

    // Forward-declare all top-level procedures so class methods (and earlier-declared
    // procedures) can reference procedures that appear later in the same block.
    // Only safe at the main (outermost) block where top-level VarDecls become globals
    // and procedures don't capture locals.
    bool isOutermostBlock = ctx.inMainBlock && !ctx.currentThis &&
        ctx.builder->GetInsertBlock() &&
        ctx.builder->GetInsertBlock()->getParent()->getName() == "main";
    if (isOutermostBlock) {
        auto ptrTy = ctx.getRefType();
        for (auto& stmt : statements) {
            auto* pd = dynamic_cast<ProcedureDecl*>(stmt.get());
            if (!pd) continue;
            if (ctx.module->getFunction(pd->name)) continue; // already declared
            llvm::Type* retTy = pd->hasReturnType ?
                ctx.getLLVMType(pd->returnType) :
                llvm::Type::getVoidTy(*ctx.llvmContext);
            std::vector<llvm::Type*> paramTypes;
            auto i64Ty3 = llvm::Type::getInt64Ty(*ctx.llvmContext);
            for (auto& p : pd->params) {
                if (p.isName) {
                    paramTypes.push_back(ptrTy);
                } else if (p.isArray) {
                    paramTypes.push_back(ptrTy);
                    paramTypes.push_back(i64Ty3);
                    paramTypes.push_back(i64Ty3);
                } else {
                    paramTypes.push_back(ctx.getLLVMType(p.type));
                }
            }
            auto funcType = llvm::FunctionType::get(retTy, paramTypes, false);
            llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                   pd->name, ctx.module.get());
            // Register return-ref-class so callers can resolve method calls
            if (!pd->returnRefClass.empty()) {
                ctx.refTypes[pd->name] = pd->returnRefClass;
            }
        }
    }

    // Second pass: execute all statements (declarations will be no-ops
    // since the variables already exist)
    llvm::Value* last = nullptr;
    for (auto& stmt : statements) {
        if (dynamic_cast<VarDeclaration*>(stmt.get()) ||
            dynamic_cast<CompoundStmt*>(stmt.get()) ||
            dynamic_cast<ArrayDeclaration*>(stmt.get()) ||
            dynamic_cast<RefDeclaration*>(stmt.get()) ||
            dynamic_cast<LabelDeclaration*>(stmt.get())) {
            continue; // already processed
        }
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
    // Skip if already declared (two-pass block processing)
    if (ctx.locals.count(name) || ctx.globals.count(name)) {
        // But still process initializer if present
        if (init) {
            auto [varPtr, varTy] = ctx.getVarPtr(name);
            if (varPtr) {
                auto val = init->codegen(ctx);
                if (val) {
                    if (val->getType() != varTy) {
                        if (varTy->isDoubleTy() && val->getType()->isIntegerTy())
                            val = ctx.builder->CreateSIToFP(val, varTy);
                        else if (varTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                            val = ctx.builder->CreateFPToSI(val, varTy);
                    }
                    ctx.builder->CreateStore(val, varPtr);
                }
            }
        }
        return nullptr;
    }
    auto ty = ctx.getLLVMType(type);

    // In the main block, create global variables so procedures can access them
    if (ctx.inMainBlock && !ctx.currentThis) {
        auto gv = new llvm::GlobalVariable(
            *ctx.module, ty, false, llvm::GlobalValue::InternalLinkage,
            llvm::Constant::getNullValue(ty), "g_" + name);
        ctx.globals[name] = gv;
        // Also add as a "local" so main() code can access it via the same path
        // We create a fake alloca that's actually backed by the global
        // Simpler: just use the global directly. locals stores AllocaInst*,
        // but globals are GlobalVariable*. Both are pointer-like.
        // Since locals expects AllocaInst*, let's still create a local alloca
        // and keep the global in sync... or just skip locals and rely on getVarPtr.
        // Simplest: don't use locals for globals. Update getVarPtr to check globals.
        if (init) {
            auto val = init->codegen(ctx);
            if (val) {
                if (val->getType() != ty) {
                    if (ty->isDoubleTy() && val->getType()->isIntegerTy())
                        val = ctx.builder->CreateSIToFP(val, ty);
                    else if (ty->isIntegerTy(64) && val->getType()->isDoubleTy())
                        val = ctx.builder->CreateFPToSI(val, ty);
                }
                ctx.builder->CreateStore(val, gv);
            }
        }
        if (type == TEXT) {
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto posGv = new llvm::GlobalVariable(
                *ctx.module, i64Ty, false, llvm::GlobalValue::InternalLinkage,
                llvm::ConstantInt::get(i64Ty, 0), "g_" + name + "__pos");
            ctx.globals[name + "__pos"] = posGv;
            ctx.textVars.insert(name);
        }
        return gv;
    }

    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto alloca = ctx.createEntryBlockAlloca(func, name, ty);
    ctx.locals[name] = alloca;

    if (init) {
        auto val = init->codegen(ctx);
        if (val) {
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
    // Skip if already declared (two-pass block processing)
    if (ctx.arrays.count(name)) return nullptr;

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

        // In main block, use a global array so procedures can access it
        if (ctx.inMainBlock && !ctx.currentThis) {
            auto arrTy = llvm::ArrayType::get(elemTy, size);
            auto gv = new llvm::GlobalVariable(
                *ctx.module, arrTy, false, llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(arrTy), "g_" + name);
            ArrayInfo info;
            info.basePtr = gv;
            info.elementType = elemTy;
            info.lowerBound = lo;
            info.size = size;
            info.isStackArray = true; // same layout as stack array
            ctx.arrays[name] = info;
            if (!refClassName.empty()) ctx.refTypes[name] = refClassName;
            return gv;
        }

        // Inside a class body (currentThis set), heap-allocate so the array
        // outlives the body coroutine. Otherwise it'd be a stack alloca that
        // gets freed when the body returns, leaving the field with a dangling ptr.
        if (ctx.currentThis) {
            auto elemSize = ctx.module->getDataLayout().getTypeAllocSize(elemTy);
            auto byteSize = llvm::ConstantInt::get(i64Ty, (long long)elemSize * size);
            auto ptr = ctx.builder->CreateCall(ctx.allocFunc, {byteSize}, name + "_data");
            ArrayInfo info;
            info.basePtr = ptr;
            info.elementType = elemTy;
            info.lowerBound = lo;
            info.size = size;
            info.isStackArray = false;
            ctx.arrays[name] = info;
            if (!refClassName.empty()) ctx.refTypes[name] = refClassName;
            return ptr;
        }
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
        if (!refClassName.empty()) ctx.refTypes[name] = refClassName;
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
        if (!refClassName.empty()) ctx.refTypes[name] = refClassName;
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
    // In the main block, create a global so top-level procedures can access REF
    // variables without needing closure capture.
    if (ctx.inMainBlock && !ctx.currentThis &&
        ctx.builder->GetInsertBlock() &&
        ctx.builder->GetInsertBlock()->getParent()->getName() == "main") {
        auto gv = new llvm::GlobalVariable(
            *ctx.module, ty, false, llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*ctx.llvmContext)),
            "g_" + varName);
        ctx.globals[varName] = gv;
        ctx.refTypes[varName] = className;
        return gv;
    }
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

    auto [varPtr, varTy] = ctx.getVarPtr(name);
    if (varPtr) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType() != varTy) {
            if (varTy->isDoubleTy() && val->getType()->isIntegerTy())
                val = ctx.builder->CreateSIToFP(val, varTy);
            else if (varTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                val = ctx.builder->CreateFPToSI(val, varTy);
            else if (varTy->isIntegerTy(8) && val->getType()->isIntegerTy())
                val = ctx.builder->CreateTrunc(val, varTy);
        }
        ctx.builder->CreateStore(val, varPtr);
        return val;
    }

    std::cerr << "Error: unknown variable '" << name << "'\n";
    return nullptr;
}

llvm::Value* MemberAssignment::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    std::string clsName;
    if (auto* qua = dynamic_cast<QuaExpression*>(object.get())) {
        clsName = qua->className;
    } else if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }
    if (clsName.empty()) {
        if (auto* call = dynamic_cast<ProcedureCall*>(object.get())) {
            clsName = ctx.resolveRefType(call->name);
        }
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

llvm::Value* MemberArrayAssignment::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;
    std::string clsName;
    if (auto* qua = dynamic_cast<QuaExpression*>(object.get())) {
        clsName = qua->className;
    } else if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }
    // Inner method-call-on-identifier returning a REF (e.g. ROOT.LINK(0).OTHER)
    if (clsName.empty()) {
        if (auto* mc = dynamic_cast<MethodCall*>(object.get())) {
            std::string innerCls;
            if (auto* id = dynamic_cast<Identifier*>(mc->object.get())) {
                innerCls = ctx.resolveRefType(id->name);
            } else if (dynamic_cast<ThisExpression*>(mc->object.get())) {
                innerCls = ctx.currentClassName;
            }
            if (!innerCls.empty()) {
                auto cit2 = ctx.classes.find(innerCls);
                if (cit2 != ctx.classes.end()) {
                    for (auto& f : cit2->second.fields) {
                        if (f.name == mc->method && !f.refClassName.empty()) {
                            clsName = f.refClassName;
                            break;
                        }
                    }
                }
            }
            if (clsName.empty()) {
                clsName = ctx.resolveRefType(mc->method);
            }
        }
    }
    if (clsName.empty()) {
        if (auto* call = dynamic_cast<ProcedureCall*>(object.get())) {
            clsName = ctx.resolveRefType(call->name);
        }
    }
    if (clsName.empty()) {
        std::cerr << "Error: cannot determine class for member array assignment '." << member << "'\n";
        return nullptr;
    }
    int fldIdx = ctx.getFieldIndex(clsName, member);
    if (fldIdx < 0) {
        std::cerr << "Error: class '" << clsName << "' has no array field '" << member << "'\n";
        return nullptr;
    }
    auto& ci = ctx.classes[clsName];
    // Load the array pointer from the field
    auto fldPtr = ctx.builder->CreateStructGEP(ci.structType, obj, fldIdx, member + "_aptr");
    auto arrPtr = ctx.builder->CreateLoad(ctx.getRefType(), fldPtr, member + "_arr");
    // Determine element type from arrayMeta + class field
    auto& fi = ci.fields[0];
    llvm::Type* elemTy = nullptr;
    for (auto& f : ci.fields) {
        if (f.name == member) {
            elemTy = ctx.getLLVMType(f.type);
            (void)fi;
            break;
        }
    }
    if (!elemTy) elemTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto idxV = index->codegen(ctx);
    if (!idxV) return nullptr;
    long long lo = 1;
    auto amIt = ci.arrayMeta.find(member);
    if (amIt != ci.arrayMeta.end()) lo = amIt->second.first;
    auto adj = ctx.builder->CreateSub(idxV, llvm::ConstantInt::get(i64Ty, lo), "adj_idx");
    auto gep = ctx.builder->CreateGEP(elemTy, arrPtr, adj, "arr_elem");
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    if (val->getType() != elemTy) {
        if (elemTy->isDoubleTy() && val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, elemTy);
        else if (elemTy->isIntegerTy(64) && val->getType()->isDoubleTy())
            val = ctx.builder->CreateFPToSI(val, elemTy);
    }
    ctx.builder->CreateStore(val, gep);
    return val;
}

llvm::Value* RefAssignment::codegen(CodeGenContext& ctx) {
    // Check if assigning to procedure return variable (return-by-name with :-)
    if (name == ctx.currentProcName && ctx.returnValueAlloca) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
        ctx.builder->CreateStore(val, ctx.returnValueAlloca);
        return val;
    }
    auto [varPtr, varTy] = ctx.getVarPtr(name);
    if (!varPtr) {
        std::cerr << "Error: unknown REF variable '" << name << "'\n";
        return nullptr;
    }
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    ctx.builder->CreateStore(val, varPtr);
    // For TEXT variables, a reference assignment (e.g. T :- COPY(X)) resets the
    // implicit read/write position to the start of the new text.
    auto [posPtr, posTy] = ctx.getVarPtr(name + "__pos");
    if (posPtr) {
        ctx.builder->CreateStore(
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), 0), posPtr);
    }
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

    // GOTO of a LABEL parameter is a non-local jump: load the {jmp_buf, id} record
    // the caller passed and longjmp back into the caller's setjmp dispatch.
    if (ctx.labelParamNames.count(label)) {
        auto recPtrAlloca = ctx.locals.find(label);
        if (recPtrAlloca != ctx.locals.end()) {
            auto ptrTy = ctx.getRefType();
            auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto recPtr = ctx.builder->CreateLoad(ptrTy, recPtrAlloca->second, label + "_rec");
            auto bufSlot = ctx.builder->CreateStructGEP(ctx.labelRecordType, recPtr, 0, "lbl_buf");
            auto buf = ctx.builder->CreateLoad(ptrTy, bufSlot, "lbl_buf_v");
            auto idSlot = ctx.builder->CreateStructGEP(ctx.labelRecordType, recPtr, 1, "lbl_id");
            auto id64 = ctx.builder->CreateLoad(i64Ty, idSlot, "lbl_id_v");
            auto id32 = ctx.builder->CreateTrunc(id64, i32Ty, "lbl_id32");
            ctx.builder->CreateCall(ctx.longjmpFunc, {buf, id32});
            ctx.builder->CreateUnreachable();
            auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "after_goto", func);
            ctx.builder->SetInsertPoint(afterBB);
            return nullptr;
        }
    }

    // Local GOTO: branch to the label block in this function.
    auto it = ctx.labelBlocks.find(label);
    if (it == ctx.labelBlocks.end() ||
        (it->second->getParent() && it->second->getParent() != func)) {
        // Unknown / cross-function label without a LABEL-param record: keep the
        // module valid rather than emit an invalid branch.
        ctx.builder->CreateUnreachable();
        auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "after_goto", func);
        ctx.builder->SetInsertPoint(afterBB);
        return nullptr;
    }

    ctx.builder->CreateBr(it->second);

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
    if (!condV) {
        // Codegen of condition failed (error already reported). Emit a constant
        // false so the module is at least valid.
        condV = llvm::ConstantInt::getFalse(*ctx.llvmContext);
    }
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

llvm::Value* ForListStatement::codegen(CodeGenContext& ctx) {
    auto [varPtr, varTy] = ctx.getVarPtr(var);
    if (!varPtr) {
        std::cerr << "Error: FOR variable '" << var << "' not declared\n";
        return nullptr;
    }
    // Simply iterate: for each value, assign to var and execute body
    for (auto& valExpr : values) {
        auto val = valExpr->codegen(ctx);
        if (!val) continue;
        ctx.builder->CreateStore(val, varPtr);
        body->codegen(ctx);
    }
    return nullptr;
}

llvm::Value* ForMultiRangeStatement::codegen(CodeGenContext& ctx) {
    // Generate a separate loop for each range, all using the same body
    for (auto& range : ranges) {
        auto func = ctx.builder->GetInsertBlock()->getParent();
        auto [varPtr, varTy] = ctx.getVarPtr(var);
        if (!varPtr) {
            std::cerr << "Error: FOR variable '" << var << "' not declared\n";
            return nullptr;
        }
        auto startV = range.start->codegen(ctx);
        ctx.builder->CreateStore(startV, varPtr);

        auto condBB = llvm::BasicBlock::Create(*ctx.llvmContext, "fmr_cond", func);
        auto bodyBB = llvm::BasicBlock::Create(*ctx.llvmContext, "fmr_body");
        auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "fmr_end");

        ctx.builder->CreateBr(condBB);
        ctx.builder->SetInsertPoint(condBB);

        auto cur = ctx.builder->CreateLoad(varTy, varPtr, var);
        auto limitV = range.limit->codegen(ctx);
        auto cond = ctx.builder->CreateICmpSLE(cur, limitV, "fmr_cmp");
        ctx.builder->CreateCondBr(cond, bodyBB, afterBB);

        func->insert(func->end(), bodyBB);
        ctx.builder->SetInsertPoint(bodyBB);
        body->codegen(ctx);

        auto cur2 = ctx.builder->CreateLoad(varTy, varPtr, var);
        auto stepV = range.step->codegen(ctx);
        auto next = ctx.builder->CreateAdd(cur2, stepV, "fmr_step");
        ctx.builder->CreateStore(next, varPtr);
        ctx.builder->CreateBr(condBB);

        func->insert(func->end(), afterBB);
        ctx.builder->SetInsertPoint(afterBB);
    }
    return nullptr;
}

// ---- Procedure declaration ----

llvm::Value* ProcedureDecl::codegen(CodeGenContext& ctx) {
    // Register return REF class so callers can resolve method calls on the result
    if (!returnRefClass.empty()) {
        ctx.refTypes[name] = returnRefClass;
    }

    // Determine return type
    llvm::Type* retTy;
    if (hasReturnType) {
        retTy = ctx.getLLVMType(returnType);
    } else {
        retTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    }

    // Build parameter types
    std::vector<llvm::Type*> paramTypes;
    bool isMethod = !ctx.currentClassName.empty() && !ctx.insideMethod;
    if (isMethod) {
        paramTypes.push_back(ctx.getRefType());
    }
    for (auto& p : params) {
        if (p.isName) {
            paramTypes.push_back(ctx.getRefType()); // NAME = pointer
        } else if (p.isArray) {
            // ARRAY: ptr to data, then i64 lo, i64 hi (bounds passed at runtime)
            paramTypes.push_back(ctx.getRefType());
            paramTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext));
            paramTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext));
        } else {
            paramTypes.push_back(ctx.getLLVMType(p.type));
        }
    }

    // Closure: collect outer local variables to pass as extra ptr params
    std::vector<std::string> captured;
    std::vector<llvm::Type*> capturedTys;
    if (!isMethod) {
        auto ptrTy = ctx.getRefType();
        for (auto& [vname, valloca] : ctx.locals) {
            if (vname.find("__") != std::string::npos) continue;
            captured.push_back(vname);
            capturedTys.push_back(ptrTy);
            paramTypes.push_back(ptrTy);
        }
        // Capture outer arrays (pass base pointer)
        for (auto& [aname, ainfo] : ctx.arrays) {
            if (llvm::isa<llvm::GlobalVariable>(ainfo.basePtr)) continue;
            captured.push_back("__arr_" + aname);
            capturedTys.push_back(ptrTy);
            paramTypes.push_back(ptrTy);
        }
        // If inside a method, also capture 'this' pointer
        if (ctx.insideMethod && ctx.currentThis) {
            captured.push_back("__this");
            capturedTys.push_back(ptrTy);
            paramTypes.push_back(ptrTy);
        }
    }

    std::string funcName = isMethod ? (ctx.currentClassName + "_" + name) : name;

    // Reuse existing function declaration if already created (e.g. by class pre-pass)
    auto func = ctx.module->getFunction(funcName);
    if (!func) {
        auto funcType = llvm::FunctionType::get(retTy, paramTypes, false);
        func = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
                                      funcName, ctx.module.get());
    }

    // Store captured variable info for call sites
    if (!captured.empty()) {
        ctx.capturedVars[funcName] = captured;
        ctx.capturedTypes[funcName] = capturedTys;
    }

    // Record which param indices are NAME (pass-by-reference) so callers can pass
    // the address of the caller's variable instead of its value. Index is in the
    // user-visible param list (excluding 'this' for methods).
    {
        std::set<int> nameIdx;
        std::set<int> labelIdx;
        for (size_t i = 0; i < params.size(); i++) {
            if (params[i].isName) nameIdx.insert((int)i);
            if (params[i].isLabel) labelIdx.insert((int)i);
        }
        if (!nameIdx.empty()) {
            ctx.nameParamIndices[funcName] = nameIdx;
        }
        if (!labelIdx.empty()) {
            ctx.labelParamIndices[funcName] = labelIdx;
        }
    }

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
    auto savedInsideMethod = ctx.insideMethod;
    auto savedMethodThis = ctx.methodThis;
    auto savedMethodThisClassName = ctx.methodThisClassName;
    auto savedNameParams = ctx.nameParams;
    auto savedArrays = ctx.arrays;
    auto savedLabelBlocks = ctx.labelBlocks;
    auto savedInMainBlock = ctx.inMainBlock;
    ctx.inMainBlock = false;
    auto savedJmpBuf = ctx.currentJmpBuf;
    auto savedNonLocalIds = ctx.nonLocalLabelIds;
    auto savedLabelParamNames = ctx.labelParamNames;
    ctx.currentJmpBuf = nullptr;
    ctx.nonLocalLabelIds.clear();
    ctx.labelParamNames.clear();

    // Create entry block
    auto entry = llvm::BasicBlock::Create(*ctx.llvmContext, "entry", func);
    ctx.builder->SetInsertPoint(entry);
    ctx.locals.clear();
    ctx.nameParams.clear();
    // Keep arrays that use global storage (from main block)
    {
        auto savedArr = ctx.arrays;
        ctx.arrays.clear();
        for (auto& [aname, ainfo] : savedArr) {
            if (llvm::isa<llvm::GlobalVariable>(ainfo.basePtr)) {
                ctx.arrays[aname] = ainfo;
            }
        }
    }
    ctx.labelBlocks.clear();

    // Set up parameters
    auto argIt = func->arg_begin();
    if (isMethod) {
        ctx.currentThis = &*argIt;
        ctx.currentThis->setName("this");
        ++argIt;
        ctx.insideMethod = true;
        // Track the "outer" this and class name for INSPECT-aware cross-method calls
        ctx.methodThis = ctx.currentThis;
        ctx.methodThisClassName = ctx.currentClassName;
    } else if (!ctx.insideMethod) {
        // Only clear class context if not inside a method
        // (nested procedures inside methods should still see class fields)
        ctx.currentThis = nullptr;
        ctx.currentClassName = "";
        ctx.insideMethod = false;
        ctx.methodThis = nullptr;
        ctx.methodThisClassName = "";
    }

    for (auto& p : params) {
        auto ty = ctx.getLLVMType(p.type);
        if (p.isName) {
            // NAME param: arg is a pointer to the caller's storage.
            ctx.nameParams[p.name] = {&*argIt, ty};
            (&*argIt)->setName(p.name);
            ++argIt;
            continue;
        }
        if (p.isArray) {
            // Array param: ptr to data, plus i64 lo, i64 hi.
            (&*argIt)->setName(p.name);
            llvm::Value* basePtr = &*argIt;
            ++argIt;
            llvm::Value* loArg = &*argIt;
            (&*argIt)->setName(p.name + "_lo");
            ++argIt;
            llvm::Value* hiArg = &*argIt;
            (&*argIt)->setName(p.name + "_hi");
            ++argIt;
            // Store lo and hi in allocas so UPPERBOUND/LOWERBOUND can read them.
            auto i64Ty2 = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto loAlloca = ctx.createEntryBlockAlloca(func, p.name + "__lo", i64Ty2);
            ctx.builder->CreateStore(loArg, loAlloca);
            ctx.locals[p.name + "__lo"] = loAlloca;
            auto hiAlloca = ctx.createEntryBlockAlloca(func, p.name + "__hi", i64Ty2);
            ctx.builder->CreateStore(hiArg, hiAlloca);
            ctx.locals[p.name + "__hi"] = hiAlloca;
            ArrayInfo info;
            info.basePtr = basePtr;
            info.elementType = ctx.getLLVMType(p.arrayElemType);
            info.lowerBound = 1; // overridden by __lo at runtime
            info.size = 0; // unknown statically; UPPERBOUND reads __hi
            info.isStackArray = false;
            ctx.arrays[p.name] = info;
            continue;
        }
        if (p.isLabel) {
            // LABEL param: arg is a ptr to a {jmpbuf, id} record. Store as a plain
            // ptr local and remember the name so GOTO can longjmp to it.
            auto ptrTy = ctx.getRefType();
            auto alloca = ctx.createEntryBlockAlloca(func, p.name, ptrTy);
            ctx.builder->CreateStore(&*argIt, alloca);
            ctx.locals[p.name] = alloca;
            ctx.labelParamNames.insert(p.name);
            ++argIt;
            continue;
        }
        auto alloca = ctx.createEntryBlockAlloca(func, p.name, ty);
        ctx.builder->CreateStore(&*argIt, alloca);
        ctx.locals[p.name] = alloca;
        ++argIt;
        // Register REF class name for member access resolution
        if (!p.refClassName.empty()) {
            ctx.refTypes[p.name] = p.refClassName;
        }
        // For TEXT parameters, create position tracking
        if (p.type == VarDeclaration::TEXT && p.refClassName.empty() && !p.isArray) {
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto posAlloca = ctx.createEntryBlockAlloca(func, p.name + "__pos", i64Ty);
            ctx.builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), posAlloca);
            ctx.locals[p.name + "__pos"] = posAlloca;
            ctx.textVars.insert(p.name);
        }
    }

    // Set up captured outer variables (closure)
    for (size_t ci = 0; ci < captured.size(); ci++) {
        auto& capName = captured[ci];
        llvm::Value* capPtr = &*argIt;
        capPtr->setName("cap_" + capName);
        ++argIt;
        // The captured ptr points to the outer variable's storage.
        // We store it as if it were a local alloca so getVarPtr/Identifier can find it.
        // Create a local alloca that holds the pointer, then use indirection.
        // Actually, the captured ptr IS a pointer to the outer alloca.
        // To make loads/stores work, we need to treat it AS the alloca.
        // Since AllocaInst* is expected in locals, and capPtr is an Argument,
        // we create a local alloca, store the ptr, and use it for indirection.
        auto ptrTy = ctx.getRefType();
        auto localPtr = ctx.createEntryBlockAlloca(func, capName + "_cap", ptrTy);
        ctx.builder->CreateStore(capPtr, localPtr);
        // For the variable to be accessible, we need loads to go through the pointer.
        // The simplest: store the captured pointer as a "fake" alloca. When Identifier
        // loads from it, it gets the pointer value. But that's wrong — it would load
        // the pointer itself, not the pointed-to value.
        //
        // Better: create an alloca of the actual variable type, and at entry,
        // load the value from the outer alloca via the captured pointer and store locally.
        // But this only captures the value at call time, not by reference.
        //
        // For true by-reference capture: the captured ptr IS the storage location.
        // We need locals[name] to point to the OUTER alloca. But locals expects
        // AllocaInst*, not an arbitrary Value*.
        //
        // Workaround: store the outer alloca pointer in a local, and when accessing
        // the variable, load through indirection. This requires changing how locals work.
        //
        // Handle __this capture: restore class context
        if (capName == "__this") {
            ctx.currentThis = capPtr;
            continue;
        }
        // Handle array captures
        if (capName.substr(0, 6) == "__arr_") {
            std::string arrName = capName.substr(6);
            // Look up the outer array info to get metadata
            auto outerIt = savedArrays.find(arrName);
            if (outerIt != savedArrays.end()) {
                ArrayInfo ainfo = outerIt->second;
                ainfo.basePtr = capPtr;
                ainfo.isStackArray = false; // accessed through pointer
                ctx.arrays[arrName] = ainfo;
            }
            continue;
        }
        // True by-reference capture: register the captured pointer in nameParams
        // so reads/writes go through the outer storage. Determine the value type
        // from the saved outer alloca (or fall back to i64).
        auto outerTy = savedLocals.count(capName) ?
            savedLocals[capName]->getAllocatedType() :
            llvm::Type::getInt64Ty(*ctx.llvmContext);
        ctx.nameParams[capName] = {capPtr, outerTy};
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
        // Set up REF field type info and TEXT tracking for methods
        for (auto& fi : ci.fields) {
            if (!fi.refClassName.empty()) {
                ctx.refTypes[fi.name] = fi.refClassName;
            }
        }
        ctx.setupTextFieldTracking(func);
    }

    // Pre-scan body for labels so they're available for forward references.
    // Also collect local label names and detect which are used as non-local
    // GOTO targets (passed as a LABEL argument to a call).
    std::set<std::string> localLabels;
    std::set<std::string> nonLocalTargets;
    {
        std::function<void(Expression*)> scanExpr = [&](Expression* e) {
            if (!e) return;
            auto checkArgs = [&](ExprList& args) {
                for (auto& a : args) {
                    if (auto* id = dynamic_cast<Identifier*>(a.get()))
                        nonLocalTargets.insert(id->name);
                    scanExpr(a.get());
                }
            };
            if (auto* pc = dynamic_cast<ProcedureCall*>(e)) { checkArgs(pc->args); }
            else if (auto* mc = dynamic_cast<MethodCall*>(e)) { scanExpr(mc->object.get()); checkArgs(mc->args); }
            else if (auto* ne = dynamic_cast<NewExpression*>(e)) { checkArgs(ne->args); }
            else if (auto* bo = dynamic_cast<BinaryOp*>(e)) { scanExpr(bo->lhs.get()); scanExpr(bo->rhs.get()); }
            else if (auto* ma = dynamic_cast<MemberAccess*>(e)) { scanExpr(ma->object.get()); }
        };
        std::function<void(Statement*)> scan = [&](Statement* s) {
            if (!s) return;
            if (auto* ld = dynamic_cast<LabelDeclaration*>(s)) {
                for (auto& n : ld->labels) { ctx.getOrCreateLabel(n); localLabels.insert(n); }
            }
            if (auto* ls = dynamic_cast<LabeledStatement*>(s)) {
                ctx.getOrCreateLabel(ls->label);
                localLabels.insert(ls->label);
                if (ls->statement) scan(ls->statement.get());
            }
            if (auto* b = dynamic_cast<Block*>(s)) {
                for (auto& st : b->statements) scan(st.get());
            }
            if (auto* cs = dynamic_cast<CompoundStmt*>(s)) {
                for (auto& st : cs->statements) scan(st.get());
            }
            if (auto* ifs = dynamic_cast<IfStatement*>(s)) {
                if (ifs->thenBranch) scan(ifs->thenBranch.get());
                if (ifs->elseBranch) scan(ifs->elseBranch.get());
            }
            if (auto* w = dynamic_cast<WhileStatement*>(s)) {
                if (w->body) scan(w->body.get());
            }
            if (auto* fs = dynamic_cast<ForStatement*>(s)) {
                if (fs->body) scan(fs->body.get());
            }
            if (auto* es = dynamic_cast<ExprStatement*>(s)) {
                scanExpr(es->expr.get());
            }
        };
        if (body) scan(body.get());
    }

    // Set up non-local GOTO machinery: any local label that is passed as a call
    // argument may be the target of a longjmp from a callee. Allocate a jmp_buf,
    // assign each such label an id, and emit a setjmp dispatch at function entry.
    {
        std::vector<std::string> nlLabels;
        for (auto& l : localLabels)
            if (nonLocalTargets.count(l)) nlLabels.push_back(l);
        if (!nlLabels.empty()) {
            auto i8Ty = llvm::Type::getInt8Ty(*ctx.llvmContext);
            auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
            // jmp_buf: generously sized byte buffer (macOS arm64 needs ~192 bytes).
            auto bufTy = llvm::ArrayType::get(i8Ty, 512);
            ctx.currentJmpBuf = ctx.createEntryBlockAlloca(func, "jmpbuf", bufTy);
            int nextId = 1;
            for (auto& l : nlLabels) ctx.nonLocalLabelIds[l] = nextId++;
            // Emit: rc = setjmp(buf); switch rc -> [0:body, id:label]
            auto rc = ctx.builder->CreateCall(ctx.setjmpFunc, {ctx.currentJmpBuf}, "setjmp_rc");
            auto bodyStart = llvm::BasicBlock::Create(*ctx.llvmContext, "body_start", func);
            auto sw = ctx.builder->CreateSwitch(rc, bodyStart, (unsigned)nlLabels.size());
            for (auto& l : nlLabels) {
                sw->addCase(llvm::ConstantInt::get(i32Ty, ctx.nonLocalLabelIds[l]),
                            ctx.getOrCreateLabel(l));
            }
            ctx.builder->SetInsertPoint(bodyStart);
        }
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
    ctx.insideMethod = savedInsideMethod;
    ctx.methodThis = savedMethodThis;
    ctx.methodThisClassName = savedMethodThisClassName;
    ctx.nameParams = savedNameParams;
    ctx.arrays = savedArrays;
    ctx.labelBlocks = savedLabelBlocks;
    ctx.inMainBlock = savedInMainBlock;
    ctx.currentJmpBuf = savedJmpBuf;
    ctx.nonLocalLabelIds = savedNonLocalIds;
    ctx.labelParamNames = savedLabelParamNames;

    return func;
}

// ---- Class declaration ----

llvm::Value* ClassDecl::codegen(CodeGenContext& ctx) {
    ClassInfo ci;
    ci.name = name;
    ci.parentName = parentName;
    ci.classId = ctx.nextClassId++;
    ci.coroFieldIndex = 1;

    // Build struct fields: [vtablePtr (ptr), coroPtr (ptr), inherited fields..., own fields...]
    std::vector<llvm::Type*> fieldTypes;
    fieldTypes.push_back(ctx.getRefType()); // vtable pointer (index 0)
    fieldTypes.push_back(ctx.getRefType()); // coro context ptr (index 1)

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
        fi.refClassName = p.refClassName; // store class name even if type stays TEXT
        fi.structIndex = (int)fieldTypes.size();
        ci.fields.push_back(fi);
        fieldTypes.push_back(ctx.getLLVMType(p.type));
        if (p.type == VarDeclaration::TEXT && p.refClassName.empty()) {
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
            fi.refClassName = ad->refClassName; // for REF(Class) ARRAY fields
            fi.structIndex = (int)fieldTypes.size();
            ci.fields.push_back(fi);
            fieldTypes.push_back(ctx.getRefType()); // pointer to array data
            // Pre-populate arrayMeta with the static lower bound (if known). Even when
            // the upper bound is dynamic, the lower bound is usually a literal. Member
            // array access from outside the class needs this for index adjustment.
            long long staticLo = 1;
            if (auto* il = dynamic_cast<IntegerLiteral*>(ad->lowerBound.get())) {
                staticLo = il->value;
            }
            ci.arrayMeta[ad->name] = {staticLo, 0};
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

    // Create struct type — reuse a forward-declared placeholder if one exists
    // (so any code already using the type sees the same type with body filled in).
    auto existingIt = ctx.classes.find(name);
    if (existingIt != ctx.classes.end() && existingIt->second.structType) {
        ci.structType = existingIt->second.structType;
        ci.structType->setBody(fieldTypes);
    } else {
        ci.structType = llvm::StructType::create(*ctx.llvmContext, fieldTypes, name);
    }

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

    // Pre-create the class body function declaration AND vtable placeholder
    // (so NEW ClassName inside methods can reference them)
    {
        auto bodyFuncType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(*ctx.llvmContext), {ctx.getRefType()}, false);
        auto bodyFunc = llvm::Function::Create(
            bodyFuncType, llvm::Function::ExternalLinkage,
            name + "_body", ctx.module.get());
        ctx.classes[name].bodyFunc = bodyFunc;

        // Pre-create vtable placeholder (size will be fixed up later)
        auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
        auto ptrTy = ctx.getRefType();
        std::vector<llvm::Type*> vtFields = {i64Ty};
        // Reserve room — will be expanded later
        for (int i = 0; i < 16; i++) vtFields.push_back(ptrTy);
        auto tmpVtTy = llvm::StructType::create(*ctx.llvmContext, vtFields, name + "_vtable_t");
        ctx.classes[name].vtableType = tmpVtTy;
        auto tmpInit = llvm::ConstantAggregateZero::get(tmpVtTy);
        ctx.classes[name].vtableGlobal = new llvm::GlobalVariable(
            *ctx.module, tmpVtTy, false, llvm::GlobalValue::InternalLinkage,
            tmpInit, name + "_vtable");
    }

    // Pre-pass: create LLVM function declarations for all methods
    // (so methods can reference each other regardless of declaration order)
    for (auto& stmt : bodyStmts) {
        if (auto* pd = dynamic_cast<ProcedureDecl*>(stmt.get())) {
            llvm::Type* retTy = pd->hasReturnType ?
                ctx.getLLVMType(pd->returnType) :
                llvm::Type::getVoidTy(*ctx.llvmContext);
            std::vector<llvm::Type*> paramTypes;
            paramTypes.push_back(ctx.getRefType()); // this
            for (auto& p : pd->params) {
                if (p.isName)
                    paramTypes.push_back(ctx.getRefType()); // NAME = ptr
                else
                    paramTypes.push_back(ctx.getLLVMType(p.type));
            }
            auto funcType = llvm::FunctionType::get(retTy, paramTypes, false);
            auto methodFunc = llvm::Function::Create(funcType,
                llvm::Function::ExternalLinkage,
                name + "_" + pd->name, ctx.module.get());
            ctx.classes[name].methods[pd->name] = methodFunc;
        }
    }

    // Pre-pass: compile nested class declarations (so they're available)
    {
        auto savedThis = ctx.currentThis;
        auto savedClassName = ctx.currentClassName;
        auto savedInsideMethod = ctx.insideMethod;
        ctx.currentThis = nullptr;
        ctx.currentClassName = "";
        ctx.insideMethod = false;
        for (auto& stmt : bodyStmts) {
            if (dynamic_cast<ClassDecl*>(stmt.get())) {
                stmt->codegen(ctx);
            }
        }
        ctx.currentThis = savedThis;
        ctx.currentClassName = savedClassName;
        ctx.insideMethod = savedInsideMethod;
    }

    // First pass: compile procedure bodies
    for (auto& stmt : bodyStmts) {
        if (dynamic_cast<ProcedureDecl*>(stmt.get())) {
            stmt->codegen(ctx);
        }
    }

    // Capture method return-ref-class additions before refTypes is cleared for the body
    // function. This lets callers resolve chained method calls on results of class methods
    // that return REF(Class) (e.g. obj.method(...).other_method(...)).
    std::map<std::string, std::string> methodRefAdditions;
    for (auto& [k, v] : ctx.refTypes) {
        if (savedRefTypes.find(k) == savedRefTypes.end()) {
            methodRefAdditions[k] = v;
        }
    }

    // Build vtable metadata (indices) now so method dispatch works during codegen.
    // The actual vtable global with function pointers is built later by buildAllVtables().
    {
        auto& ciRef = ctx.classes[name];
        auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
        auto ptrTy = ctx.getRefType();

        // Inherit parent vtable order
        if (!parentName.empty()) {
            auto pit = ctx.classes.find(parentName);
            if (pit != ctx.classes.end()) {
                ciRef.vtableMethodOrder = pit->second.vtableMethodOrder;
                ciRef.vtableIndex = pit->second.vtableIndex;
            }
        }

        // Add own methods
        for (auto& [mname, mfunc] : ciRef.methods) {
            if (ciRef.vtableIndex.find(mname) == ciRef.vtableIndex.end()) {
                int idx = (int)ciRef.vtableMethodOrder.size() + 1;
                ciRef.vtableIndex[mname] = idx;
                ciRef.vtableMethodOrder.push_back(mname);
            }
        }

        // Add VIRTUAL declarations (methods declared but not implemented in this class)
        for (auto& stmt : bodyStmts) {
            if (auto* vd = dynamic_cast<VirtualDecl*>(stmt.get())) {
                for (auto& mname : vd->methodNames) {
                    std::string lname = mname;
                    for (auto& c : lname) c = tolower((unsigned char)c);
                    if (ciRef.vtableIndex.find(lname) == ciRef.vtableIndex.end()) {
                        int idx = (int)ciRef.vtableMethodOrder.size() + 1;
                        ciRef.vtableIndex[lname] = idx;
                        ciRef.vtableMethodOrder.push_back(lname);
                    }
                }
            }
        }

        // Propagate new methods UP to parent vtable metadata
        if (!parentName.empty()) {
            auto pit = ctx.classes.find(parentName);
            if (pit != ctx.classes.end()) {
                for (auto& [mname, idx] : ciRef.vtableIndex) {
                    if (pit->second.vtableIndex.find(mname) == pit->second.vtableIndex.end()) {
                        int newIdx = (int)pit->second.vtableMethodOrder.size() + 1;
                        pit->second.vtableIndex[mname] = newIdx;
                        pit->second.vtableMethodOrder.push_back(mname);
                    }
                }
                // Rebuild parent vtable type to match new size
                std::vector<llvm::Type*> pvtFields = {i64Ty};
                for (size_t i = 0; i < pit->second.vtableMethodOrder.size(); i++)
                    pvtFields.push_back(ptrTy);
                pit->second.vtableType->setBody(pvtFields);

                // Re-sync own indices with parent
                ciRef.vtableMethodOrder = pit->second.vtableMethodOrder;
                ciRef.vtableIndex = pit->second.vtableIndex;
                // Add back own-only methods
                for (auto& [mname, mfunc] : ciRef.methods) {
                    if (ciRef.vtableIndex.find(mname) == ciRef.vtableIndex.end()) {
                        int newIdx = (int)ciRef.vtableMethodOrder.size() + 1;
                        ciRef.vtableIndex[mname] = newIdx;
                        ciRef.vtableMethodOrder.push_back(mname);
                    }
                }
            }
        }

        // vtable type and global already created above
        // (buildAllVtables will set the proper body and initializer)
    }

    // Use the pre-declared class body function: void @ClassName_body(ptr %this)
    auto bodyFunc = ctx.classes[name].bodyFunc;

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
        if (dynamic_cast<ClassDecl*>(stmt.get())) continue;
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
                // Save array metadata for methods. Preserve the pre-populated static
                // lower bound from the AST (info.lowerBound is 0 for dynamic-bound arrays).
                long long preservedLo = ctx.classes[name].arrayMeta.count(ad->name)
                    ? ctx.classes[name].arrayMeta[ad->name].first
                    : info.lowerBound;
                ctx.classes[name].arrayMeta[ad->name] = {preservedLo, info.size};
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
    for (auto& [k, v] : methodRefAdditions) {
        ctx.refTypes[k] = v;
    }
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

    // Determine the class of the inspected object from its REF type
    std::string inspectClass;
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        inspectClass = ctx.resolveRefType(ident->name);
    } else if (auto* call = dynamic_cast<ProcedureCall*>(object.get())) {
        // Array element access like BLOCKS(I) — get the array's ref class
        inspectClass = ctx.resolveRefType(call->name);
    }

    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);

    // Save current THIS context
    auto savedThis = ctx.currentThis;
    auto savedClassName = ctx.currentClassName;

    // INSPECT ref DO stmt (no WHEN clauses) — make fields accessible
    if (whenClauses.empty() && otherwiseBody) {
        if (!inspectClass.empty()) {
            ctx.currentThis = obj;
            ctx.currentClassName = inspectClass;
        }
        otherwiseBody->codegen(ctx);
        ctx.currentThis = savedThis;
        ctx.currentClassName = savedClassName;
        return nullptr;
    }

    // Load class ID for WHEN dispatch
    auto classId = ctx.loadClassId(obj);

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
        // Set THIS context for the WHEN body
        ctx.currentThis = obj;
        ctx.currentClassName = wc.className;
        wc.body->codegen(ctx);
        ctx.currentThis = savedThis;
        ctx.currentClassName = savedClassName;
        if (!ctx.builder->GetInsertBlock()->getTerminator())
            ctx.builder->CreateBr(mergeBB);

        func->insert(func->end(), nextBB);
        ctx.builder->SetInsertPoint(nextBB);
    }

    // OTHERWISE
    if (otherwiseBody) {
        if (!inspectClass.empty()) {
            ctx.currentThis = obj;
            ctx.currentClassName = inspectClass;
        }
        otherwiseBody->codegen(ctx);
        ctx.currentThis = savedThis;
        ctx.currentClassName = savedClassName;
    }
    if (!ctx.builder->GetInsertBlock()->getTerminator())
        ctx.builder->CreateBr(mergeBB);

    func->insert(func->end(), mergeBB);
    ctx.builder->SetInsertPoint(mergeBB);
    return nullptr;
}

// ---- Coroutines ----

llvm::Value* VirtualDecl::codegen(CodeGenContext& ctx) {
    return nullptr; // vtables are built by buildAllVtables()
}

llvm::Value* DetachStatement::codegen(CodeGenContext& ctx) {
    if (!ctx.currentThis) {
        std::cerr << "Error: DETACH used outside of a class body\n";
        return nullptr;
    }

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {ptrTy, ptrTy});

    auto coroSlot = ctx.builder->CreateStructGEP(baseTy, ctx.currentThis, 1, "coro_slot");
    auto coro = ctx.builder->CreateLoad(ptrTy, coroSlot, "coro");

    return ctx.builder->CreateCall(ctx.coroDetachFunc, {coro});
}

llvm::Value* ResumeStatement::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {ptrTy, ptrTy});

    auto coroSlot = ctx.builder->CreateStructGEP(baseTy, obj, 1, "coro_slot");
    auto coro = ctx.builder->CreateLoad(ptrTy, coroSlot, "coro");

    return ctx.builder->CreateCall(ctx.coroResumeFunc, {coro});
}

llvm::Value* CallStatement::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto baseTy = llvm::StructType::get(*ctx.llvmContext, {ptrTy, ptrTy});

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
    auto r = ctx.builder->CreateCall(ctx.putsFunc, {nl});
    // Flush stdout: OUTIMAGE conceptually emits a completed line, and flushing
    // makes output visible promptly (and not lost if the program later crashes).
    auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
    auto fflushFn = ctx.module->getOrInsertFunction(
        "fflush", llvm::FunctionType::get(i32Ty, {ptrTy}, false));
    ctx.builder->CreateCall(fflushFn,
        {llvm::ConstantPointerNull::get(ptrTy)});
    (void)voidTy;
    return r;
}

llvm::Value* InImageStatement::codegen(CodeGenContext& ctx) {
    // No-op for now
    return nullptr;
}

// ---- Program ----

llvm::Value* Program::codegen(CodeGenContext& ctx) {
    return block->codegen(ctx);
}
