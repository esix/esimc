#include "codegen.h"
#include "ast.h"

#include <llvm/IR/Verifier.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

#include <iostream>
#include <algorithm>
#include <functional>
#include <cfloat>
#include <cmath>

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
    // INSPECT connection: inside `INSPECT X DO`, X's attributes are connected
    // (currentThis = X), but the enclosing block's own object stays accessible.
    // methodThis still points at that enclosing object, so fall back to it for
    // names that aren't attributes of the inspected object.
    if (methodThis && methodThis != currentThis && !methodThisClassName.empty()) {
        int idx = getFieldIndex(methodThisClassName, name);
        if (idx >= 0) {
            auto& ci = classes[methodThisClassName];
            auto gep = builder->CreateStructGEP(ci.structType, methodThis, idx, name + "_fptr");
            auto fieldTy = ci.structType->getElementType(idx);
            return {gep, fieldTy};
        }
    }
    // Nested INSPECT: outer connected objects (innermost first)
    for (auto it = inspectStack.rbegin(); it != inspectStack.rend(); ++it) {
        if (it->first == currentThis) continue;
        int idx = getFieldIndex(it->second, name);
        if (idx >= 0) {
            auto& ci = classes[it->second];
            auto gep = builder->CreateStructGEP(ci.structType, it->first, idx, name + "_fptr");
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

std::set<int> CodeGenContext::getDescendantIdSet(const std::string& className) {
    std::set<int> ids;
    if (!classes.count(className)) return ids;
    // A class is included if walking its prefix chain reaches className.
    for (auto& [name, info] : classes) {
        std::string cur = name;
        while (!cur.empty()) {
            if (cur == className) { ids.insert(info.classId); break; }
            auto it = classes.find(cur);
            if (it == classes.end()) break;
            cur = it->second.parentName;
        }
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

    // File input primitives (INFILE support)
    inopenFunc = llvm::Function::Create(
        llvm::FunctionType::get(i64Ty, {ptrTy}, false),
        llvm::Function::ExternalLinkage, "simula_inopen", module.get());
    inreadlineFunc = llvm::Function::Create(
        llvm::FunctionType::get(ptrTy, {i64Ty, i64Ty}, false),
        llvm::Function::ExternalLinkage, "simula_inreadline", module.get());
    incloseFunc = llvm::Function::Create(
        llvm::FunctionType::get(voidTy, {i64Ty}, false),
        llvm::Function::ExternalLinkage, "simula_inclose", module.get());

    // simula_lastitem() -> i64  (SYSIN end-of-file look-ahead)
    lastitemFunc = llvm::Function::Create(
        llvm::FunctionType::get(i64Ty, {}, false),
        llvm::Function::ExternalLinkage, "simula_lastitem", module.get());
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
        (hadError = true, std::cerr) << "Error: generated main function is invalid\n";
    }
    if (llvm::verifyModule(*module, &llvm::errs())) {
        (hadError = true, std::cerr) << "Error: generated module is invalid\n";
    }
}

void CodeGenContext::writeIR(const std::string& filename) {
    std::error_code ec;
    llvm::raw_fd_ostream out(filename, ec);
    if (ec) {
        (hadError = true, std::cerr) << "Error opening output file: " << ec.message() << "\n";
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
    // Copy the string constant into a fresh writable TEXT frame (a Simula text
    // constant is a writable text object; sharing read-only .rodata would crash
    // on PUTCHAR/:=).
    auto cstr = ctx.builder->CreateGlobalString(value, "str");
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto fn = ctx.module->getOrInsertFunction("simula_text_dup",
        llvm::FunctionType::get(ptrTy, {ptrTy}, false));
    return ctx.builder->CreateCall(fn, {cstr}, "txtlit");
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
    // SYSIN LASTITEM: a look-ahead end-of-file test (skip whitespace, peek).
    // Must not consume the next item, so it can't be a flag set by the prior
    // read — call the runtime peek each time it's evaluated.
    if (name == "__lastitem" && ctx.lastitemFunc) {
        auto r = ctx.builder->CreateCall(ctx.lastitemFunc, {}, "lastitem");
        return ctx.builder->CreateICmpNE(r,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), 0), "lastitembool");
    }

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

    // INSPECT connection: fall back to the enclosing block's object for names
    // that aren't attributes of the currently-connected (inspected) object.
    if (ctx.methodThis && ctx.methodThis != ctx.currentThis &&
        !ctx.methodThisClassName.empty()) {
        int idx = ctx.getFieldIndex(ctx.methodThisClassName, name);
        if (idx >= 0) {
            auto& ci = ctx.classes[ctx.methodThisClassName];
            auto gep = ctx.builder->CreateStructGEP(ci.structType, ctx.methodThis, idx, name);
            auto fieldTy = ci.structType->getElementType(idx);
            return ctx.builder->CreateLoad(fieldTy, gep, name);
        }
    }

    // Nested INSPECT: outer connected objects (innermost first)
    for (auto sit = ctx.inspectStack.rbegin(); sit != ctx.inspectStack.rend(); ++sit) {
        if (sit->first == ctx.currentThis) continue;
        int idx = ctx.getFieldIndex(sit->second, name);
        if (idx >= 0) {
            auto& ci = ctx.classes[sit->second];
            auto gep = ctx.builder->CreateStructGEP(ci.structType, sit->first, idx, name);
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
                    } else if (capName.size() > 6 && capName.substr(0, 6) == "__arr_") {
                        std::string arrName = capName.substr(6);
                        auto ait2 = ctx.arrays.find(arrName);
                        if (ait2 != ctx.arrays.end())
                            callArgs.push_back(ait2->second.basePtr);
                        else
                            callArgs.push_back(llvm::ConstantPointerNull::get(
                                llvm::PointerType::getUnqual(*ctx.llvmContext)));
                    } else if (capName.size() > 8 && capName.substr(0, 8) == "__arrlo_") {
                        // Dynamic lo (first dim) capture
                        std::string arrName = capName.substr(8);
                        auto i64Ty5 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                        auto loIt = ctx.locals.find(arrName + "__lo");
                        if (loIt != ctx.locals.end())
                            callArgs.push_back(ctx.builder->CreateLoad(i64Ty5, loIt->second, "lov"));
                        else {
                            auto ait3 = ctx.arrays.find(arrName);
                            callArgs.push_back(llvm::ConstantInt::get(i64Ty5,
                                ait3 != ctx.arrays.end() ? ait3->second.lowerBound : 0));
                        }
                    } else if (capName.size() > 8 && capName.substr(0, 8) == "__arr2d_") {
                        // 2D array lo2/stride: pass as i64 values
                        std::string rest = capName.substr(8);
                        auto i64Ty5 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                        if (rest.size() > 4 && rest.substr(rest.size()-4) == "_lo2") {
                            std::string arrName = rest.substr(0, rest.size()-4);
                            auto lo2It = ctx.locals.find(arrName + "__lo2");
                            if (lo2It != ctx.locals.end())
                                callArgs.push_back(ctx.builder->CreateLoad(i64Ty5, lo2It->second, "lo2v"));
                            else {
                                auto ait3 = ctx.arrays.find(arrName);
                                callArgs.push_back(llvm::ConstantInt::get(i64Ty5,
                                    ait3 != ctx.arrays.end() ? ait3->second.lowerBound2 : 0));
                            }
                        } else if (rest.size() > 7 && rest.substr(rest.size()-7) == "_stride") {
                            std::string arrName = rest.substr(0, rest.size()-7);
                            auto strIt = ctx.locals.find(arrName + "__stride");
                            if (strIt != ctx.locals.end())
                                callArgs.push_back(ctx.builder->CreateLoad(i64Ty5, strIt->second, "stridev"));
                            else {
                                auto ait3 = ctx.arrays.find(arrName);
                                callArgs.push_back(llvm::ConstantInt::get(i64Ty5,
                                    (ait3 != ctx.arrays.end() && ait3->second.stride > 0) ? ait3->second.stride : 1));
                            }
                        } else {
                            callArgs.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), 0));
                        }
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

    // If the name refers to a function, return its pointer (procedure-as-value).
    // "main" is excluded: in Simula it denotes the main program process, not
    // the C entry point.
    if (func && name != "main") {
        return func;
    }

    // If the name is a known label, return its index (LABEL parameter passing)
    if (ctx.labelBlocks.count(name)) {
        return llvm::ConstantInt::get(llvm::Type::getInt64Ty(*ctx.llvmContext), 0);
    }

    // Environment constants (standard bare-identifier form)
    {
        auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
        auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
        if (name == "maxint")  return llvm::ConstantInt::get(i64Ty, INT64_MAX);
        if (name == "minint")  return llvm::ConstantInt::get(i64Ty, INT64_MIN);
        if (name == "maxreal") return llvm::ConstantFP::get(doubleTy, DBL_MAX);
        if (name == "minreal") return llvm::ConstantFP::get(doubleTy, DBL_MIN);
        if (name == "pi")      return llvm::ConstantFP::get(doubleTy, M_PI);
        if (name == "breakoutimage") {
            // Output goes to stdout unbuffered by line already; BREAKOUTIMAGE
            // just flushes so a partial line becomes visible.
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
            auto fflushFn = ctx.module->getOrInsertFunction("fflush",
                llvm::FunctionType::get(i32Ty, {ptrTy}, false));
            ctx.builder->CreateCall(fflushFn,
                {llvm::ConstantPointerNull::get(ptrTy)});
            return llvm::ConstantInt::get(i64Ty, 0);
        }
        if (name == "infrac") {
            auto fn = ctx.module->getOrInsertFunction("simula_infrac",
                llvm::FunctionType::get(i64Ty, {}, false));
            return ctx.builder->CreateCall(fn, {}, "infrac");
        }
        // SIMULATION environment
        if (name == "time") {
            auto fn = ctx.module->getOrInsertFunction("simula_sim_time",
                llvm::FunctionType::get(doubleTy, {}, false));
            return ctx.builder->CreateCall(fn, {}, "time");
        }
        if (name == "current") {
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto fn = ctx.module->getOrInsertFunction("simula_sim_current",
                llvm::FunctionType::get(ptrTy, {}, false));
            return ctx.builder->CreateCall(fn, {}, "current");
        }
        if (name == "main") {
            // The main program process: represented by the null object (the
            // scheduler uses NULL for "main is current").
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            return llvm::ConstantPointerNull::get(ptrTy);
        }
        if (name == "passivate") {
            auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);
            auto fn = ctx.module->getOrInsertFunction("simula_sim_passivate",
                llvm::FunctionType::get(voidTy, {}, false));
            ctx.builder->CreateCall(fn, {});
            return llvm::ConstantInt::get(i64Ty, 0);
        }
    }

    (ctx.hadError = true, std::cerr) << "Error: unknown variable '" << name << "'\n";
    return nullptr;
}

// Implicit real-to-integer conversion: Simula rounds (ENTIER(r + 0.5)), unlike
// C's truncation. Used at every assignment/parameter/array-store coercion site;
// explicit ENTIER/TRUNCATE keep their own semantics.
static llvm::Value* simulaRealToInt(CodeGenContext& ctx, llvm::Value* v,
                                    llvm::Type* destTy) {
    auto half = llvm::ConstantFP::get(v->getType(), 0.5);
    auto shifted = ctx.builder->CreateFAdd(v, half, "rnd_shift");
    auto floorF = llvm::Intrinsic::getOrInsertDeclaration(ctx.module.get(),
        llvm::Intrinsic::floor, {v->getType()});
    auto floored = ctx.builder->CreateCall(floorF, {shifted}, "rnd_floor");
    return ctx.builder->CreateFPToSI(floored, destTy, "rnd_toint");
}

static void emitTextValueAssign(CodeGenContext& ctx, llvm::Value* slot,
                                llvm::Value* rhsDesc);

// Heuristically decide whether an expression yields a TEXT value (descriptor),
// as opposed to a REF object pointer. Used to pick content/identity comparison
// and in-place vs rebind assignment. Conservative: only returns true for
// clearly-TEXT producers.
static bool exprIsText(Expression* e, CodeGenContext& ctx) {
    if (!e) return false;
    if (dynamic_cast<TextLiteral*>(e)) return true;
    if (auto* id = dynamic_cast<Identifier*>(e))
        return ctx.textVars.count(id->name) > 0;
    if (auto* b = dynamic_cast<BinaryOp*>(e))
        return b->op == BinaryOp::CONCAT;
    if (auto* pc = dynamic_cast<ProcedureCall*>(e)) {
        if (pc->name == "copy" || pc->name == "blanks" || pc->name == "sub" ||
            pc->name == "strip" || pc->name == "main") return true;
        // A TEXT array element, or a user procedure declared to return TEXT.
        auto ait = ctx.arrays.find(pc->name);
        if (ait != ctx.arrays.end()) return ait->second.isTextElem;
        return ctx.textReturningProcs.count(pc->name) > 0;
    }
    if (auto* mc = dynamic_cast<MethodCall*>(e)) {
        // intext is the INFILE TEXT-returning method (chains like
        // FILE.INTEXT(n).STRIP); sub/strip/main/copy are TEXT producers.
        return mc->method == "sub" || mc->method == "strip" ||
               mc->method == "main" || mc->method == "copy" ||
               mc->method == "intext" ||
               ctx.textReturningProcs.count(mc->method) > 0;
    }
    if (auto* ma = dynamic_cast<MemberAccess*>(e)) {
        if (ma->member == "sub" || ma->member == "strip" || ma->member == "main")
            return true;
        // SYSIN.IMAGE is a TEXT line buffer.
        if (ma->member == "image") {
            if (auto* bid = dynamic_cast<Identifier*>(ma->object.get()))
                if (bid->name == "sysin") return true;
        }
        // A TEXT field of the object's class.
        std::string cls;
        if (auto* bid = dynamic_cast<Identifier*>(ma->object.get()))
            cls = ctx.resolveRefType(bid->name);
        else if (dynamic_cast<ThisExpression*>(ma->object.get()))
            cls = ctx.currentClassName;
        std::string sc = cls;
        while (!sc.empty()) {
            auto cit = ctx.classes.find(sc);
            if (cit == ctx.classes.end()) break;
            for (auto& fi : cit->second.fields)
                if (fi.name == ma->member)
                    return fi.type == VarDeclaration::TEXT && fi.refClassName.empty();
            sc = cit->second.parentName;
        }
    }
    return false;
}

// Emit a TEXT member/method operation on a descriptor value. `args` are already
// codegen'd. Returns nullptr if the op name isn't a known TEXT operation. The
// descriptor carries its own cursor (pos), so this works uniformly for named
// variables, fields, array elements, and temporaries.
static llvm::Value* emitTextOp(CodeGenContext& ctx, llvm::Value* desc,
                               const std::string& op,
                               std::vector<llvm::Value*>& args) {
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto i8Ty = llvm::Type::getInt8Ty(*ctx.llvmContext);
    auto dblTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
    auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    auto call0 = [&](const char* fn, llvm::Type* ret) {
        auto f = ctx.module->getOrInsertFunction(fn,
            llvm::FunctionType::get(ret, {ptrTy}, false));
        return ctx.builder->CreateCall(f, {desc}, op);
    };
    auto toI = [&](llvm::Value* v) {
        return v->getType()->isDoubleTy() ? simulaRealToInt(ctx, v, i64Ty) : v;
    };
    auto toD = [&](llvm::Value* v) {
        return v->getType()->isIntegerTy()
            ? ctx.builder->CreateSIToFP(v, dblTy, "tofp") : v;
    };

    if (op == "length") return call0("simula_text_length", i64Ty);
    if (op == "more") {
        auto r = call0("simula_text_more", i64Ty);
        return ctx.builder->CreateICmpNE(r, llvm::ConstantInt::get(i64Ty, 0), "moreb");
    }
    if (op == "pos")     return call0("simula_text_pos", i64Ty);
    if (op == "getchar") return call0("simula_text_getchar", i8Ty);
    if (op == "strip")   return call0("simula_text_strip", ptrTy);
    if (op == "main")    return call0("simula_text_main", ptrTy);
    if (op == "getint")  return call0("simula_text_getint", i64Ty);
    if (op == "getreal") return call0("simula_text_getreal", dblTy);
    if (op == "getfrac") return call0("simula_text_getfrac", i64Ty);
    if (op == "constant") return ctx.builder->getFalse();
    if (op == "start")    return llvm::ConstantInt::get(i64Ty, 1);
    if (op == "setpos" && args.size() == 1) {
        auto f = ctx.module->getOrInsertFunction("simula_text_setpos",
            llvm::FunctionType::get(voidTy, {ptrTy, i64Ty}, false));
        ctx.builder->CreateCall(f, {desc, toI(args[0])});
        return llvm::ConstantInt::get(i64Ty, 0);
    }
    if (op == "getchar" ) return call0("simula_text_getchar", i8Ty);
    if (op == "putchar" && args.size() == 1) {
        auto f = ctx.module->getOrInsertFunction("simula_text_putchar",
            llvm::FunctionType::get(voidTy, {ptrTy, i8Ty}, false));
        auto ch = args[0];
        if (!ch->getType()->isIntegerTy(8))
            ch = ctx.builder->CreateTrunc(ch, i8Ty, "ch8");
        ctx.builder->CreateCall(f, {desc, ch});
        return llvm::ConstantInt::get(i64Ty, 0);
    }
    if (op == "sub" && args.size() == 2) {
        auto f = ctx.module->getOrInsertFunction("simula_text_sub",
            llvm::FunctionType::get(ptrTy, {ptrTy, i64Ty, i64Ty}, false));
        return ctx.builder->CreateCall(f, {desc, toI(args[0]), toI(args[1])}, "sub");
    }
    if (op == "putint" && args.size() == 1) {
        auto f = ctx.module->getOrInsertFunction("simula_text_putint",
            llvm::FunctionType::get(voidTy, {ptrTy, i64Ty}, false));
        ctx.builder->CreateCall(f, {desc, toI(args[0])});
        return llvm::ConstantInt::get(i64Ty, 0);
    }
    if ((op == "putfix" || op == "putreal") && args.size() == 2) {
        auto f = ctx.module->getOrInsertFunction(
            op == "putfix" ? "simula_text_putfix" : "simula_text_putreal",
            llvm::FunctionType::get(voidTy, {ptrTy, dblTy, i64Ty}, false));
        ctx.builder->CreateCall(f, {desc, toD(args[0]), toI(args[1])});
        return llvm::ConstantInt::get(i64Ty, 0);
    }
    if (op == "putfrac" && args.size() == 2) {
        auto f = ctx.module->getOrInsertFunction("simula_text_putfrac",
            llvm::FunctionType::get(voidTy, {ptrTy, i64Ty, i64Ty}, false));
        ctx.builder->CreateCall(f, {desc, toI(args[0]), toI(args[1])});
        return llvm::ConstantInt::get(i64Ty, 0);
    }
    return nullptr;
}

llvm::Value* BinaryOp::codegen(CodeGenContext& ctx) {
    // Short-circuit operators (Simula AND THEN / OR ELSE): evaluate the right
    // operand only when needed, so a guard like `X == NONE OR ELSE X.M(..)`
    // doesn't dereference a null reference.
    if (op == ANDTHEN || op == ORELSE) {
        auto toBool = [&](llvm::Value* v) -> llvm::Value* {
            if (v->getType()->isIntegerTy(1)) return v;
            return ctx.builder->CreateICmpNE(v,
                llvm::ConstantInt::get(v->getType(), 0), "tobool");
        };
        auto func = ctx.builder->GetInsertBlock()->getParent();
        auto Lv = lhs->codegen(ctx);
        if (!Lv) return nullptr;
        auto Lb = toBool(Lv);
        auto entryBB = ctx.builder->GetInsertBlock();
        auto rhsBB = llvm::BasicBlock::Create(*ctx.llvmContext, "sc_rhs", func);
        auto contBB = llvm::BasicBlock::Create(*ctx.llvmContext, "sc_cont", func);
        if (op == ORELSE)
            ctx.builder->CreateCondBr(Lb, contBB, rhsBB);   // true short-circuits
        else
            ctx.builder->CreateCondBr(Lb, rhsBB, contBB);   // false short-circuits

        ctx.builder->SetInsertPoint(rhsBB);
        auto Rv = rhs->codegen(ctx);
        if (!Rv) return nullptr;
        auto Rb = toBool(Rv);
        auto rhsEndBB = ctx.builder->GetInsertBlock();
        ctx.builder->CreateBr(contBB);

        ctx.builder->SetInsertPoint(contBB);
        auto i1Ty = llvm::Type::getInt1Ty(*ctx.llvmContext);
        auto phi = ctx.builder->CreatePHI(i1Ty, 2, "sctmp");
        phi->addIncoming(llvm::ConstantInt::get(i1Ty, op == ORELSE ? 1 : 0), entryBB);
        phi->addIncoming(Rb, rhsEndBB);
        return phi;
    }

    auto L = lhs->codegen(ctx);
    auto R = rhs->codegen(ctx);
    if (!L || !R) return nullptr;

    // CONCAT: call simula_text_concat
    if (op == CONCAT) {
        return ctx.builder->CreateCall(ctx.textConcatFunc, {L, R}, "concat");
    }

    // POWER: integer**integer is exact integer exponentiation (i64); any real
    // operand falls back to pow() from libm.
    if (op == POWER) {
        auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
        if (L->getType()->isIntegerTy(64) && R->getType()->isIntegerTy(64)) {
            auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto ipowFunc = ctx.module->getOrInsertFunction("simula_ipow",
                llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty}, false));
            return ctx.builder->CreateCall(ipowFunc, {L, R}, "ipow");
        }
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

    // Simula's "/" always yields a real result, even for integer operands
    // (integer division is the separate "//" operator, handled as IDIV). Force
    // real arithmetic for DIV so e.g. 3/2 is 1.5, not 1.
    bool isReal = L->getType()->isDoubleTy() || R->getType()->isDoubleTy() ||
                  op == DIV;

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

    auto i64TyC = llvm::Type::getInt64Ty(*ctx.llvmContext);
    bool isPtr = L->getType()->isPointerTy() && R->getType()->isPointerTy();
    bool anyText = exprIsText(lhs.get(), ctx) || exprIsText(rhs.get(), ctx);

    // Reference identity (== / =/=): pointer identity for REF objects; for TEXT,
    // same frame+start+length (NOT content).
    if (op == REF_EQ || op == REF_NE) {
        llvm::Value* eq;
        if (isPtr && anyText) {
            auto fn = ctx.module->getOrInsertFunction("simula_text_ref_eq",
                llvm::FunctionType::get(i64TyC,
                    {ctx.getRefType(), ctx.getRefType()}, false));
            auto r = ctx.builder->CreateCall(fn, {L, R}, "refeq");
            eq = ctx.builder->CreateICmpNE(r, llvm::ConstantInt::get(i64TyC, 0), "refeqb");
        } else {
            eq = ctx.builder->CreateICmpEQ(L, R, "ptr_eq");
        }
        return (op == REF_NE) ? ctx.builder->CreateNot(eq, "ref_ne") : eq;
    }

    // Content equality (= / <>) on two pointers -> TEXT window compare. This is
    // always content for TEXT; REF objects use == / =/= (REF_EQ/REF_NE) for
    // identity, so a plain = on pointers is a text comparison in practice.
    if (isPtr && (op == EQ || op == NE)) {
        auto eqVal = ctx.builder->CreateCall(ctx.textEqFunc, {L, R}, "texteq");
        auto cmp = ctx.builder->CreateICmpNE(eqVal,
            llvm::ConstantInt::get(i64TyC, 0), "txtcmp");
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
        case CONCAT: case POWER: case REF_EQ: case REF_NE: break; // handled above
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
            (ctx.hadError = true, std::cerr) << "Error: array '" << name << "' access requires an index\n";
            return nullptr;
        }
        auto idxVal = args[0]->codegen(ctx);
        if (!idxVal) return nullptr;
        // Compute adjusted first-dimension index: index - lowerBound
        llvm::Value* loBound;
        auto loIt = ctx.locals.find(name + "__lo");
        if (loIt != ctx.locals.end()) {
            loBound = ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
        } else {
            loBound = llvm::ConstantInt::get(i64Ty, info.lowerBound);
        }
        auto adjusted = ctx.builder->CreateSub(idxVal, loBound, "adj_idx");

        // 2D array: flat_idx = (i - lo1) * stride + (j - lo2)
        if (args.size() >= 2 && (info.stride != 0 || info.hasDynStride)) {
            auto idxVal2 = args[1]->codegen(ctx);
            if (!idxVal2) return nullptr;
            llvm::Value* stride;
            llvm::Value* lo2;
            if (info.hasDynStride) {
                stride = ctx.builder->CreateLoad(i64Ty,
                    ctx.locals[name + "__stride"], "stride");
                lo2 = ctx.builder->CreateLoad(i64Ty,
                    ctx.locals[name + "__lo2"], "lo2");
            } else {
                stride = llvm::ConstantInt::get(i64Ty, info.stride);
                lo2 = llvm::ConstantInt::get(i64Ty, info.lowerBound2);
            }
            auto row = ctx.builder->CreateMul(adjusted, stride, "row_off");
            auto col = ctx.builder->CreateSub(idxVal2, lo2, "col_adj");
            adjusted = ctx.builder->CreateAdd(row, col, "flat_idx");
        }

        llvm::Value* gep;
        if (info.isStackArray) {
            auto totalSize = (info.stride > 0) ? info.size : info.size;
            auto arrTy = llvm::ArrayType::get(info.elementType, totalSize > 0 ? (size_t)totalSize : 1);
            gep = ctx.builder->CreateGEP(arrTy, info.basePtr,
                {llvm::ConstantInt::get(i64Ty, 0), adjusted}, "arr_elem");
        } else {
            gep = ctx.builder->CreateGEP(info.elementType, info.basePtr,
                adjusted, "arr_elem");
        }
        return ctx.builder->CreateLoad(info.elementType, gep, "arr_val");
    }

    // Class-field array access inside a method/body: NAME(idx) where NAME is an
    // ARRAY field of the current class (or an inherited one). The parser can't
    // tell this apart from a function call, so resolve it here against `this`.
    if (ctx.currentThis && !args.empty()) {
        std::string searchCls = ctx.currentClassName;
        while (!searchCls.empty()) {
            auto cit = ctx.classes.find(searchCls);
            if (cit == ctx.classes.end()) break;
            auto amIt = cit->second.arrayMeta.find(name);
            if (amIt != cit->second.arrayMeta.end()) {
                int fldIdx = ctx.getFieldIndex(searchCls, name);
                if (fldIdx >= 0) {
                    auto ptrTy2 = ctx.getRefType();
                    auto& ci = cit->second;
                    auto gep = ctx.builder->CreateStructGEP(ci.structType, ctx.currentThis,
                                                            fldIdx, name + "_aptr");
                    auto arrPtr = ctx.builder->CreateLoad(ptrTy2, gep, name + "_arr");
                    auto idxVal = args[0]->codegen(ctx);
                    if (!idxVal) return nullptr;
                    long long lo = amIt->second.first;
                    auto adjusted = ctx.builder->CreateSub(idxVal,
                        llvm::ConstantInt::get(i64Ty, lo), "adj_idx");
                    llvm::Type* elemTy = ptrTy2;
                    for (auto& f : ci.fields) {
                        if (f.name == name) {
                            if (f.refClassName.empty()) elemTy = ctx.getLLVMType(f.type);
                            break;
                        }
                    }
                    auto elemGep = ctx.builder->CreateGEP(elemTy, arrPtr, adjusted, "marr_elem");
                    return ctx.builder->CreateLoad(elemTy, elemGep, name + "_val");
                }
            }
            searchCls = cit->second.parentName;
        }
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
        // Simula 67: MOD(a,b) = a - b*ENTIER(a/b) — floor-mod, result takes the
        // divisor's sign. C SRem truncates toward zero; adjust by b whenever the
        // remainder is nonzero and its sign differs from b's.
        if (args.size() < 2) return nullptr;
        auto a = args[0]->codegen(ctx);
        auto b = args[1]->codegen(ctx);
        if (!a || !b) return nullptr;
        auto rem = ctx.builder->CreateSRem(a, b, "srem");
        auto zero = llvm::ConstantInt::get(rem->getType(), 0);
        auto remNeg = ctx.builder->CreateICmpSLT(rem, zero, "rem_neg");
        auto bNeg = ctx.builder->CreateICmpSLT(b, zero, "b_neg");
        auto signMismatch = ctx.builder->CreateICmpNE(remNeg, bNeg, "sign_mismatch");
        auto remNonzero = ctx.builder->CreateICmpNE(rem, zero, "rem_nz");
        auto needAdj = ctx.builder->CreateAnd(signMismatch, remNonzero, "need_adj");
        auto adj = ctx.builder->CreateAdd(rem, b, "rem_adj");
        return ctx.builder->CreateSelect(needAdj, adj, rem, "mod");
    }

    if (name == "entier") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy()) return val; // integer already is entier
        // Floor toward -inf: use floor intrinsic then convert
        auto floorF = llvm::Intrinsic::getOrInsertDeclaration(ctx.module.get(),
            llvm::Intrinsic::floor, {doubleTy});
        auto floored = ctx.builder->CreateCall(floorF, {val}, "floored");
        return ctx.builder->CreateFPToSI(floored, i64Ty, "entier");
    }

    if (name == "round") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy()) return val;
        // ROUND = ENTIER(x + 0.5)
        auto half = llvm::ConstantFP::get(doubleTy, 0.5);
        auto shifted = ctx.builder->CreateFAdd(val, half, "round_shift");
        auto floorF = llvm::Intrinsic::getOrInsertDeclaration(ctx.module.get(),
            llvm::Intrinsic::floor, {doubleTy});
        auto floored = ctx.builder->CreateCall(floorF, {shifted}, "floored");
        return ctx.builder->CreateFPToSI(floored, i64Ty, "round");
    }

    if (name == "truncate" || name == "trunc") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy()) return val;
        return ctx.builder->CreateFPToSI(val, i64Ty, "truncate");
    }

    // Simula 67 math functions — map to C libm intrinsics
    auto mathFunc1 = [&](const char* intrin, llvm::Intrinsic::ID id) -> llvm::Value* {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, doubleTy, "tofp");
        auto fn = llvm::Intrinsic::getOrInsertDeclaration(ctx.module.get(), id, {doubleTy});
        return ctx.builder->CreateCall(fn, {val}, intrin);
    };

    if (name == "sqrt")   return mathFunc1("sqrt",   llvm::Intrinsic::sqrt);
    if (name == "sin")    return mathFunc1("sin",    llvm::Intrinsic::sin);
    if (name == "cos")    return mathFunc1("cos",    llvm::Intrinsic::cos);
    if (name == "exp")    return mathFunc1("exp",    llvm::Intrinsic::exp);
    if (name == "log")    return mathFunc1("log2",   llvm::Intrinsic::log2); // Simula LOG = log2
    if (name == "ln")     return mathFunc1("ln",     llvm::Intrinsic::log);
    if (name == "log10")  return mathFunc1("log10",  llvm::Intrinsic::log10);

    if (name == "tan") {
        // tan not in LLVM intrinsics; use libm
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, doubleTy, "tofp");
        auto tanFn = ctx.module->getOrInsertFunction("tan",
            llvm::FunctionType::get(doubleTy, {doubleTy}, false));
        return ctx.builder->CreateCall(tanFn, {val}, "tan");
    }
    if (name == "arctan") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, doubleTy, "tofp");
        auto atanFn = ctx.module->getOrInsertFunction("atan",
            llvm::FunctionType::get(doubleTy, {doubleTy}, false));
        return ctx.builder->CreateCall(atanFn, {val}, "arctan");
    }
    if (name == "arcsin") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, doubleTy, "tofp");
        auto asinFn = ctx.module->getOrInsertFunction("asin",
            llvm::FunctionType::get(doubleTy, {doubleTy}, false));
        return ctx.builder->CreateCall(asinFn, {val}, "arcsin");
    }
    if (name == "arccos") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, doubleTy, "tofp");
        auto acosFn = ctx.module->getOrInsertFunction("acos",
            llvm::FunctionType::get(doubleTy, {doubleTy}, false));
        return ctx.builder->CreateCall(acosFn, {val}, "arccos");
    }

    if (name == "max" || name == "maxval") {
        if (args.size() < 2) return nullptr;
        auto a = args[0]->codegen(ctx); auto b = args[1]->codegen(ctx);
        if (!a || !b) return nullptr;
        bool fp = a->getType()->isDoubleTy() || b->getType()->isDoubleTy();
        if (fp) {
            if (a->getType()->isIntegerTy()) a = ctx.builder->CreateSIToFP(a, doubleTy);
            if (b->getType()->isIntegerTy()) b = ctx.builder->CreateSIToFP(b, doubleTy);
            auto cmp = ctx.builder->CreateFCmpOGT(a, b, "cmp");
            return ctx.builder->CreateSelect(cmp, a, b, "max");
        }
        auto cmp = ctx.builder->CreateICmpSGT(a, b, "cmp");
        return ctx.builder->CreateSelect(cmp, a, b, "max");
    }
    if (name == "min" || name == "minval") {
        if (args.size() < 2) return nullptr;
        auto a = args[0]->codegen(ctx); auto b = args[1]->codegen(ctx);
        if (!a || !b) return nullptr;
        bool fp = a->getType()->isDoubleTy() || b->getType()->isDoubleTy();
        if (fp) {
            if (a->getType()->isIntegerTy()) a = ctx.builder->CreateSIToFP(a, doubleTy);
            if (b->getType()->isIntegerTy()) b = ctx.builder->CreateSIToFP(b, doubleTy);
            auto cmp = ctx.builder->CreateFCmpOLT(a, b, "cmp");
            return ctx.builder->CreateSelect(cmp, a, b, "min");
        }
        auto cmp = ctx.builder->CreateICmpSLT(a, b, "cmp");
        return ctx.builder->CreateSelect(cmp, a, b, "min");
    }

    // Character classification (Simula 67 standard)
    if (name == "digit") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        // val is a CHARACTER (i8); DIGIT returns BOOLEAN
        auto ch = ctx.builder->CreateZExt(val, i64Ty, "ch");
        auto c0 = llvm::ConstantInt::get(i64Ty, '0');
        auto c9 = llvm::ConstantInt::get(i64Ty, '9');
        auto ge0 = ctx.builder->CreateICmpSGE(ch, c0);
        auto le9 = ctx.builder->CreateICmpSLE(ch, c9);
        return ctx.builder->CreateAnd(ge0, le9, "digit");
    }
    if (name == "letter") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        auto ch = ctx.builder->CreateZExt(val, i64Ty, "ch");
        auto lo = llvm::ConstantInt::get(i64Ty, 'a'), hi = llvm::ConstantInt::get(i64Ty, 'z');
        auto lo2 = llvm::ConstantInt::get(i64Ty, 'A'), hi2 = llvm::ConstantInt::get(i64Ty, 'Z');
        auto lower = ctx.builder->CreateAnd(ctx.builder->CreateICmpSGE(ch,lo),ctx.builder->CreateICmpSLE(ch,hi));
        auto upper = ctx.builder->CreateAnd(ctx.builder->CreateICmpSGE(ch,lo2),ctx.builder->CreateICmpSLE(ch,hi2));
        return ctx.builder->CreateOr(lower, upper, "letter");
    }
    if (name == "lowcase") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        // lowercase: if 'A'-'Z', add 32
        auto ch = ctx.builder->CreateZExt(val, i64Ty, "ch");
        auto loA = llvm::ConstantInt::get(i64Ty, 'A'), hiZ = llvm::ConstantInt::get(i64Ty, 'Z');
        auto isUpper = ctx.builder->CreateAnd(ctx.builder->CreateICmpSGE(ch,loA),ctx.builder->CreateICmpSLE(ch,hiZ));
        auto diff = llvm::ConstantInt::get(i64Ty, 32);
        auto lower = ctx.builder->CreateAdd(ch, diff);
        auto result = ctx.builder->CreateSelect(isUpper, lower, ch, "lowcase");
        return ctx.builder->CreateTrunc(result, i8Ty, "lowch");
    }
    if (name == "upcase") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        auto ch = ctx.builder->CreateZExt(val, i64Ty, "ch");
        auto loa = llvm::ConstantInt::get(i64Ty, 'a'), hiz = llvm::ConstantInt::get(i64Ty, 'z');
        auto isLower = ctx.builder->CreateAnd(ctx.builder->CreateICmpSGE(ch,loa),ctx.builder->CreateICmpSLE(ch,hiz));
        auto diff = llvm::ConstantInt::get(i64Ty, 32);
        auto upper = ctx.builder->CreateSub(ch, diff);
        auto result = ctx.builder->CreateSelect(isLower, upper, ch, "upcase");
        return ctx.builder->CreateTrunc(result, i8Ty, "upch");
    }
    if (name == "isodigit") { // like digit but includes _
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        auto ch = ctx.builder->CreateZExt(val, i64Ty, "ch");
        auto c0 = llvm::ConstantInt::get(i64Ty, '0');
        auto c9 = llvm::ConstantInt::get(i64Ty, '9');
        auto ge0 = ctx.builder->CreateICmpSGE(ch, c0);
        auto le9 = ctx.builder->CreateICmpSLE(ch, c9);
        return ctx.builder->CreateAnd(ge0, le9, "isodigit");
    }
    if (name == "isoletter") { return nullptr; } // alias for letter

    // MAXINT / MININT / MAXREAL — manifest constants
    if (name == "maxint")  return llvm::ConstantInt::get(i64Ty, INT64_MAX);
    if (name == "minint")  return llvm::ConstantInt::get(i64Ty, INT64_MIN);
    if (name == "maxreal") return llvm::ConstantFP::get(doubleTy, DBL_MAX);
    if (name == "minreal") return llvm::ConstantFP::get(doubleTy, DBL_MIN);
    if (name == "pi")      return llvm::ConstantFP::get(doubleTy, M_PI);

    if (name == "sign") {
        if (args.empty()) return nullptr;
        auto val = args[0]->codegen(ctx);
        if (!val) return nullptr;
        // return -1, 0, or 1 (integer result for both integer and real args)
        auto one = llvm::ConstantInt::get(i64Ty, 1);
        auto neg1 = llvm::ConstantInt::getSigned(i64Ty, -1);
        auto izero = llvm::ConstantInt::get(i64Ty, 0);
        llvm::Value *isNeg, *isPos;
        if (val->getType()->isDoubleTy()) {
            auto fzero = llvm::ConstantFP::get(val->getType(), 0.0);
            isNeg = ctx.builder->CreateFCmpOLT(val, fzero, "isneg");
            isPos = ctx.builder->CreateFCmpOGT(val, fzero, "ispos");
        } else {
            isNeg = ctx.builder->CreateICmpSLT(val, izero, "isneg");
            isPos = ctx.builder->CreateICmpSGT(val, izero, "ispos");
        }
        auto sel1 = ctx.builder->CreateSelect(isPos, one, izero, "sel1");
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

    // File input primitives backing the injected INFILE class.
    if (name == "inopen") {
        if (args.empty()) return nullptr;
        auto v = args[0]->codegen(ctx);
        if (!v) return nullptr;
        return ctx.builder->CreateCall(ctx.inopenFunc, {v}, "inopen");
    }
    if (name == "inreadline" || name == "inreadtext") {
        // INFILE INIMAGE: read a line and wrap it as a TEXT descriptor.
        if (args.size() < 2) return nullptr;
        auto fh = args[0]->codegen(ctx);
        auto n = args[1]->codegen(ctx);
        if (!fh || !n) return nullptr;
        auto ptrTyR = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto fn = ctx.module->getOrInsertFunction("simula_inreadtext",
            llvm::FunctionType::get(ptrTyR, {i64Ty, i64Ty}, false));
        return ctx.builder->CreateCall(fn, {fh, n}, "inreadtext");
    }
    if (name == "inclose") {
        if (args.empty()) return nullptr;
        auto fh = args[0]->codegen(ctx);
        if (!fh) return nullptr;
        return ctx.builder->CreateCall(ctx.incloseFunc, {fh});
    }

    // File primitives backing the injected OUTFILE class and the item-level
    // INFILE methods. Lowered to runtime calls by name.
    {
        auto ptrTyF = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto voidTyF = llvm::Type::getVoidTy(*ctx.llvmContext);
        auto evalAll = [&](std::vector<llvm::Value*>& out) -> bool {
            for (auto& a : args) {
                auto v = a->codegen(ctx);
                if (!v) return false;
                out.push_back(v);
            }
            return true;
        };
        if (name == "outopen" && args.size() == 1) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_outopen",
                llvm::FunctionType::get(i64Ty, {ptrTyF}, false));
            return ctx.builder->CreateCall(fn, vs, "outopen");
        }
        if (name == "outclose" && args.size() == 1) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_outclose",
                llvm::FunctionType::get(voidTyF, {i64Ty}, false));
            return ctx.builder->CreateCall(fn, vs);
        }
        if (name == "fouttext" && args.size() == 2) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_file_outtext",
                llvm::FunctionType::get(voidTyF, {i64Ty, ptrTyF}, false));
            return ctx.builder->CreateCall(fn, vs);
        }
        if (name == "foutint" && args.size() == 3) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_file_outint",
                llvm::FunctionType::get(voidTyF, {i64Ty, i64Ty, i64Ty}, false));
            return ctx.builder->CreateCall(fn, vs);
        }
        if (name == "foutimage" && args.size() == 1) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_file_outimage",
                llvm::FunctionType::get(voidTyF, {i64Ty}, false));
            return ctx.builder->CreateCall(fn, vs);
        }
        if (name == "finint" && args.size() == 1) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_file_inint",
                llvm::FunctionType::get(i64Ty, {i64Ty}, false));
            return ctx.builder->CreateCall(fn, vs, "finint");
        }
        if (name == "finreal" && args.size() == 1) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_file_inreal",
                llvm::FunctionType::get(doubleTy, {i64Ty}, false));
            return ctx.builder->CreateCall(fn, vs, "finreal");
        }
        if (name == "flastitem" && args.size() == 1) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_file_lastitem",
                llvm::FunctionType::get(i64Ty, {i64Ty}, false));
            auto r = ctx.builder->CreateCall(fn, vs, "flastitem");
            return ctx.builder->CreateICmpNE(r,
                llvm::ConstantInt::get(i64Ty, 0), "flastitem_b");
        }
        if (name == "infrac" && args.empty()) {
            auto fn = ctx.module->getOrInsertFunction("simula_infrac",
                llvm::FunctionType::get(i64Ty, {}, false));
            return ctx.builder->CreateCall(fn, {}, "infrac");
        }
        if (name == "outfrac" && args.size() == 3) {
            std::vector<llvm::Value*> vs; if (!evalAll(vs)) return nullptr;
            for (auto& v : vs)
                if (v->getType()->isDoubleTy()) v = simulaRealToInt(ctx, v, i64Ty);
            auto fn = ctx.module->getOrInsertFunction("simula_outfrac",
                llvm::FunctionType::get(voidTyF, {i64Ty, i64Ty, i64Ty}, false));
            return ctx.builder->CreateCall(fn, vs);
        }
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
        (ctx.hadError = true, std::cerr) << "Error: cannot determine array bounds\n";
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

    // Random drawing procedures (Simula 67 ch. 9.9). The INTEGER seed actual
    // is taken by reference and advanced on every basic draw.
    {
        auto ptrTyR = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto toD = [&](llvm::Value* v) {
            return v->getType()->isDoubleTy() ? v
                 : ctx.builder->CreateSIToFP(v, doubleTy, "tofp");
        };
        auto toI = [&](llvm::Value* v) {
            return v->getType()->isDoubleTy() ? simulaRealToInt(ctx, v, i64Ty) : v;
        };
        // Address of the seed variable; 1D array elements get their element
        // address (so the stored seed advances); other rvalues use a temp.
        auto seedPtr = [&](Expression* e) -> llvm::Value* {
            if (auto* id = dynamic_cast<Identifier*>(e)) {
                auto [p, ty] = ctx.getVarPtr(id->name);
                if (p) return p;
            }
            if (auto* pc = dynamic_cast<ProcedureCall*>(e)) {
                auto ait = ctx.arrays.find(pc->name);
                if (ait != ctx.arrays.end() && pc->args.size() == 1) {
                    auto& ai = ait->second;
                    auto idx = pc->args[0]->codegen(ctx);
                    if (!idx) return nullptr;
                    llvm::Value* lo;
                    auto loIt = ctx.locals.find(pc->name + "__lo");
                    if (loIt != ctx.locals.end())
                        lo = ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
                    else
                        lo = llvm::ConstantInt::get(i64Ty, ai.lowerBound);
                    auto adj = ctx.builder->CreateSub(idx, lo, "adj_idx");
                    if (ai.isStackArray) {
                        auto arrTy = llvm::ArrayType::get(ai.elementType,
                            ai.size > 0 ? (size_t)ai.size : 1);
                        return ctx.builder->CreateGEP(arrTy, ai.basePtr,
                            {llvm::ConstantInt::get(i64Ty, 0), adj}, "seed_elem");
                    }
                    return ctx.builder->CreateGEP(ai.elementType, ai.basePtr,
                        adj, "seed_elem");
                }
            }
            auto v = e->codegen(ctx);
            if (!v) return nullptr;
            auto fn0 = ctx.builder->GetInsertBlock()->getParent();
            auto tmp = ctx.createEntryBlockAlloca(fn0, "seed_tmp", i64Ty);
            ctx.builder->CreateStore(toI(v), tmp);
            return tmp;
        };
        // REAL ARRAY actual: (data ptr, lo, size) from local/global array info.
        struct ArrArg { llvm::Value* base; long long lo; long long n; };
        auto arrArg = [&](Expression* e, ArrArg& out) -> bool {
            auto* id = dynamic_cast<Identifier*>(e);
            if (!id) return false;
            auto it = ctx.arrays.find(id->name);
            if (it == ctx.arrays.end()) return false;
            out = {it->second.basePtr, it->second.lowerBound, it->second.size};
            return true;
        };

        if (name == "uniform" && args.size() == 3) {
            auto a = args[0]->codegen(ctx); auto b = args[1]->codegen(ctx);
            auto u = seedPtr(args[2].get());
            if (!a || !b || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_uniform",
                llvm::FunctionType::get(doubleTy, {doubleTy, doubleTy, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {toD(a), toD(b), u}, "uniform");
        }
        if (name == "randint" && args.size() == 3) {
            auto a = args[0]->codegen(ctx); auto b = args[1]->codegen(ctx);
            auto u = seedPtr(args[2].get());
            if (!a || !b || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_randint",
                llvm::FunctionType::get(i64Ty, {i64Ty, i64Ty, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {toI(a), toI(b), u}, "randint");
        }
        if (name == "normal" && args.size() == 3) {
            auto m = args[0]->codegen(ctx); auto s = args[1]->codegen(ctx);
            auto u = seedPtr(args[2].get());
            if (!m || !s || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_normal",
                llvm::FunctionType::get(doubleTy, {doubleTy, doubleTy, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {toD(m), toD(s), u}, "normal");
        }
        if (name == "negexp" && args.size() == 2) {
            auto l = args[0]->codegen(ctx);
            auto u = seedPtr(args[1].get());
            if (!l || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_negexp",
                llvm::FunctionType::get(doubleTy, {doubleTy, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {toD(l), u}, "negexp");
        }
        if (name == "poisson" && args.size() == 2) {
            auto m = args[0]->codegen(ctx);
            auto u = seedPtr(args[1].get());
            if (!m || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_poisson",
                llvm::FunctionType::get(i64Ty, {doubleTy, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {toD(m), u}, "poisson");
        }
        if (name == "erlang" && args.size() == 3) {
            auto a = args[0]->codegen(ctx); auto b = args[1]->codegen(ctx);
            auto u = seedPtr(args[2].get());
            if (!a || !b || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_erlang",
                llvm::FunctionType::get(doubleTy, {doubleTy, doubleTy, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {toD(a), toD(b), u}, "erlang");
        }
        if (name == "draw" && args.size() == 2) {
            auto p = args[0]->codegen(ctx);
            auto u = seedPtr(args[1].get());
            if (!p || !u) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_draw",
                llvm::FunctionType::get(i64Ty, {doubleTy, ptrTyR}, false));
            auto r = ctx.builder->CreateCall(fn, {toD(p), u}, "draw");
            return ctx.builder->CreateICmpNE(r,
                llvm::ConstantInt::get(i64Ty, 0), "draw_b");
        }
        if (name == "discrete" && args.size() == 2) {
            ArrArg A;
            auto u = seedPtr(args[1].get());
            if (!arrArg(args[0].get(), A) || !u) {
                (ctx.hadError = true, std::cerr) << "Error: DISCRETE needs a REAL ARRAY variable\n";
                return nullptr;
            }
            auto fn = ctx.module->getOrInsertFunction("simula_discrete",
                llvm::FunctionType::get(i64Ty, {ptrTyR, i64Ty, i64Ty, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {A.base,
                llvm::ConstantInt::get(i64Ty, A.lo),
                llvm::ConstantInt::get(i64Ty, A.n), u}, "discrete");
        }
        if (name == "linear" && args.size() == 3) {
            ArrArg A, B;
            auto u = seedPtr(args[2].get());
            if (!arrArg(args[0].get(), A) || !arrArg(args[1].get(), B) || !u) {
                (ctx.hadError = true, std::cerr) << "Error: LINEAR needs REAL ARRAY variables\n";
                return nullptr;
            }
            auto fn = ctx.module->getOrInsertFunction("simula_linear",
                llvm::FunctionType::get(doubleTy, {ptrTyR, ptrTyR, i64Ty, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {A.base, B.base,
                llvm::ConstantInt::get(i64Ty, A.n), u}, "linear");
        }
        if (name == "histd" && args.size() == 2) {
            ArrArg A;
            auto u = seedPtr(args[1].get());
            if (!arrArg(args[0].get(), A) || !u) {
                (ctx.hadError = true, std::cerr) << "Error: HISTD needs a REAL ARRAY variable\n";
                return nullptr;
            }
            auto fn = ctx.module->getOrInsertFunction("simula_histd",
                llvm::FunctionType::get(i64Ty, {ptrTyR, i64Ty, i64Ty, ptrTyR}, false));
            return ctx.builder->CreateCall(fn, {A.base,
                llvm::ConstantInt::get(i64Ty, A.lo),
                llvm::ConstantInt::get(i64Ty, A.n), u}, "histd");
        }
        if (name == "histo" && args.size() == 4) {
            ArrArg A, B;
            if (!arrArg(args[0].get(), A) || !arrArg(args[1].get(), B)) {
                (ctx.hadError = true, std::cerr) << "Error: HISTO needs REAL ARRAY variables\n";
                return nullptr;
            }
            auto c = args[2]->codegen(ctx); auto w = args[3]->codegen(ctx);
            if (!c || !w) return nullptr;
            auto voidTyR = llvm::Type::getVoidTy(*ctx.llvmContext);
            auto fn = ctx.module->getOrInsertFunction("simula_histo",
                llvm::FunctionType::get(voidTyR,
                    {ptrTyR, i64Ty, ptrTyR, i64Ty, doubleTy, doubleTy}, false));
            return ctx.builder->CreateCall(fn, {A.base,
                llvm::ConstantInt::get(i64Ty, A.n), B.base,
                llvm::ConstantInt::get(i64Ty, B.n), toD(c), toD(w)});
        }
    }

    // SIMULATION scheduling builtins
    {
        auto ptrTyS = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto voidTyS = llvm::Type::getVoidTy(*ctx.llvmContext);
        if (name == "hold" && args.size() == 1) {
            auto dt = args[0]->codegen(ctx);
            if (!dt) return nullptr;
            if (dt->getType()->isIntegerTy())
                dt = ctx.builder->CreateSIToFP(dt, doubleTy, "tofp");
            auto fn = ctx.module->getOrInsertFunction("simula_sim_hold",
                llvm::FunctionType::get(voidTyS, {doubleTy}, false));
            return ctx.builder->CreateCall(fn, {dt});
        }
        if (name == "cancel" && args.size() == 1) {
            auto p = args[0]->codegen(ctx);
            if (!p) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_sim_cancel",
                llvm::FunctionType::get(voidTyS, {ptrTyS}, false));
            return ctx.builder->CreateCall(fn, {p});
        }
        if (name == "wait" && args.size() == 1) {
            // WAIT(Q): CURRENT.INTO(Q); PASSIVATE;
            auto q = args[0]->codegen(ctx);
            if (!q) return nullptr;
            auto curFn = ctx.module->getOrInsertFunction("simula_sim_current",
                llvm::FunctionType::get(ptrTyS, {}, false));
            auto cur = ctx.builder->CreateCall(curFn, {}, "current");
            auto linkIt = ctx.classes.find("link");
            if (linkIt != ctx.classes.end()) {
                auto mIt = linkIt->second.methods.find("into");
                if (mIt != linkIt->second.methods.end())
                    ctx.builder->CreateCall(mIt->second, {cur, q});
            }
            auto pasFn = ctx.module->getOrInsertFunction("simula_sim_passivate",
                llvm::FunctionType::get(voidTyS, {}, false));
            return ctx.builder->CreateCall(pasFn, {});
        }
        if (name == "simterm" && args.size() == 1) {
            auto p = args[0]->codegen(ctx);
            if (!p) return nullptr;
            auto fn = ctx.module->getOrInsertFunction("simula_sim_terminate",
                llvm::FunctionType::get(voidTyS, {ptrTyS}, false));
            return ctx.builder->CreateCall(fn, {p});
        }
        if (name == "accum" && args.size() == 4) {
            // ACCUM(A,B,C,D): A := A + C*(TIME-B); B := TIME; C := C + D
            auto getPtr = [&](Expression* e) -> llvm::Value* {
                if (auto* id = dynamic_cast<Identifier*>(e)) {
                    auto [p, ty] = ctx.getVarPtr(id->name);
                    return p;
                }
                return nullptr;
            };
            auto aP = getPtr(args[0].get());
            auto bP = getPtr(args[1].get());
            auto cP = getPtr(args[2].get());
            auto dV = args[3]->codegen(ctx);
            if (!aP || !bP || !cP || !dV) {
                (ctx.hadError = true, std::cerr) << "Error: ACCUM needs REAL variables for its first three arguments\n";
                return nullptr;
            }
            if (dV->getType()->isIntegerTy())
                dV = ctx.builder->CreateSIToFP(dV, doubleTy, "tofp");
            auto timeFn = ctx.module->getOrInsertFunction("simula_sim_time",
                llvm::FunctionType::get(doubleTy, {}, false));
            auto now = ctx.builder->CreateCall(timeFn, {}, "now");
            auto a = ctx.builder->CreateLoad(doubleTy, aP, "a");
            auto b = ctx.builder->CreateLoad(doubleTy, bP, "b");
            auto c = ctx.builder->CreateLoad(doubleTy, cP, "c");
            auto dt = ctx.builder->CreateFSub(now, b, "dt");
            auto add = ctx.builder->CreateFMul(c, dt, "c_dt");
            ctx.builder->CreateStore(ctx.builder->CreateFAdd(a, add), aP);
            ctx.builder->CreateStore(now, bP);
            ctx.builder->CreateStore(ctx.builder->CreateFAdd(c, dV), cP);
            return llvm::ConstantInt::get(i64Ty, 0);
        }
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
                            v = simulaRealToInt(ctx, v, destTy);
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
            return simulaRealToInt(ctx, v, destTy);
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
        (ctx.hadError = true, std::cerr) << "Error: unknown function '" << name << "'\n";
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
            // Array element actual A(I): pass the element's address so callee
            // writes reach the array. (The index is evaluated once at call
            // time — full per-access thunk re-evaluation is not implemented.)
            if (auto* pc = dynamic_cast<ProcedureCall*>(arg.get())) {
                auto ait3 = ctx.arrays.find(pc->name);
                if (ait3 != ctx.arrays.end() && !pc->args.empty()) {
                    auto& ainfo = ait3->second;
                    auto i64Ty3 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                    auto idxVal = pc->args[0]->codegen(ctx);
                    if (!idxVal) return nullptr;
                    llvm::Value* loBound;
                    auto loIt3 = ctx.locals.find(pc->name + "__lo");
                    if (loIt3 != ctx.locals.end())
                        loBound = ctx.builder->CreateLoad(i64Ty3, loIt3->second, "lo");
                    else
                        loBound = llvm::ConstantInt::get(i64Ty3, ainfo.lowerBound);
                    auto adjusted = ctx.builder->CreateSub(idxVal, loBound, "adj_idx");
                    if (pc->args.size() >= 2 && (ainfo.stride != 0 || ainfo.hasDynStride)) {
                        auto idxVal2 = pc->args[1]->codegen(ctx);
                        if (!idxVal2) return nullptr;
                        llvm::Value *stride, *lo2;
                        if (ainfo.hasDynStride) {
                            stride = ctx.builder->CreateLoad(i64Ty3,
                                ctx.locals[pc->name + "__stride"], "stride");
                            lo2 = ctx.builder->CreateLoad(i64Ty3,
                                ctx.locals[pc->name + "__lo2"], "lo2");
                        } else {
                            stride = llvm::ConstantInt::get(i64Ty3, ainfo.stride);
                            lo2 = llvm::ConstantInt::get(i64Ty3, ainfo.lowerBound2);
                        }
                        auto row = ctx.builder->CreateMul(adjusted, stride, "row_off");
                        auto col = ctx.builder->CreateSub(idxVal2, lo2, "col_adj");
                        adjusted = ctx.builder->CreateAdd(row, col, "flat_idx");
                    }
                    llvm::Value* gep;
                    if (ainfo.isStackArray) {
                        auto arrTy = llvm::ArrayType::get(ainfo.elementType,
                            ainfo.size > 0 ? (size_t)ainfo.size : 1);
                        gep = ctx.builder->CreateGEP(arrTy, ainfo.basePtr,
                            {llvm::ConstantInt::get(i64Ty3, 0), adjusted}, "name_elem");
                    } else {
                        gep = ctx.builder->CreateGEP(ainfo.elementType, ainfo.basePtr,
                            adjusted, "name_elem");
                    }
                    argsV.push_back(gep);
                    paramIdx++;
                    continue;
                }
            }
            // Any other expression: evaluate once into a temporary and pass
            // its address (reads see the value; writes go to the temp). This
            // avoids the null-pointer crash; true thunks are still missing.
            auto v = arg->codegen(ctx);
            if (!v) return nullptr;
            auto curFunc = ctx.builder->GetInsertBlock()->getParent();
            auto tmp = ctx.createEntryBlockAlloca(curFunc, "name_tmp", v->getType());
            ctx.builder->CreateStore(v, tmp);
            argsV.push_back(tmp);
            paramIdx++;
            continue;
        }
        // Check if the callee expects a pointer at this position (NAME or ARRAY param)
        if (paramIdx < fnTy->getNumParams() && fnTy->getParamType(paramIdx)->isPointerTy()) {
            if (auto* id = dynamic_cast<Identifier*>(arg.get())) {
                // If it's an array, pass: ptr, lo, hi, lo2, stride
                auto ait2 = ctx.arrays.find(id->name);
                if (ait2 != ctx.arrays.end()) {
                    auto& ainfo = ait2->second;
                    auto i64Ty2 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                    auto ptrTyArr = llvm::PointerType::getUnqual(*ctx.llvmContext);
                    llvm::Value* basePtr = ainfo.basePtr;
                    // For dynamic (heap-allocated) arrays, load ptr from alloca
                    if (!ainfo.isStackArray && !llvm::isa<llvm::GlobalVariable>(ainfo.basePtr)) {
                        auto ptrIt = ctx.locals.find(id->name);
                        if (ptrIt != ctx.locals.end())
                            basePtr = ctx.builder->CreateLoad(ptrTyArr, ptrIt->second, "arr_ptr");
                    }
                    argsV.push_back(basePtr);
                    paramIdx++;
                    // lo
                    auto loIt = ctx.locals.find(id->name + "__lo");
                    llvm::Value* loV = loIt != ctx.locals.end()
                        ? (llvm::Value*)ctx.builder->CreateLoad(i64Ty2, loIt->second, "lo_arg")
                        : (llvm::Value*)llvm::ConstantInt::get(i64Ty2, ainfo.lowerBound);
                    if (paramIdx < fnTy->getNumParams() && fnTy->getParamType(paramIdx)->isIntegerTy(64))
                        { argsV.push_back(loV); paramIdx++; }
                    // hi
                    auto hiIt = ctx.locals.find(id->name + "__hi");
                    llvm::Value* hiV;
                    if (hiIt != ctx.locals.end()) {
                        hiV = ctx.builder->CreateLoad(i64Ty2, hiIt->second, "hi_arg");
                    } else {
                        long long hiC = ainfo.lowerBound + (ainfo.size > 0 ? ainfo.size - 1 : 0);
                        hiV = llvm::ConstantInt::get(i64Ty2, hiC);
                    }
                    if (paramIdx < fnTy->getNumParams() && fnTy->getParamType(paramIdx)->isIntegerTy(64))
                        { argsV.push_back(hiV); paramIdx++; }
                    // lo2: lower bound of second dimension (0 for 1D)
                    llvm::Value* lo2V = llvm::ConstantInt::get(i64Ty2, ainfo.lowerBound2);
                    if (ainfo.hasDynLo2) {
                        auto lo2It = ctx.locals.find(id->name + "__lo2");
                        if (lo2It != ctx.locals.end())
                            lo2V = ctx.builder->CreateLoad(i64Ty2, lo2It->second, "lo2_arg");
                    }
                    if (paramIdx < fnTy->getNumParams() && fnTy->getParamType(paramIdx)->isIntegerTy(64))
                        { argsV.push_back(lo2V); paramIdx++; }
                    // stride: columns per row (1 for 1D)
                    llvm::Value* strideV = llvm::ConstantInt::get(i64Ty2, ainfo.stride > 0 ? ainfo.stride : 1);
                    if (ainfo.hasDynStride) {
                        auto strIt = ctx.locals.find(id->name + "__stride");
                        if (strIt != ctx.locals.end())
                            strideV = ctx.builder->CreateLoad(i64Ty2, strIt->second, "stride_arg");
                    }
                    if (paramIdx < fnTy->getNumParams() && fnTy->getParamType(paramIdx)->isIntegerTy(64))
                        { argsV.push_back(strideV); paramIdx++; }
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
                    v = simulaRealToInt(ctx, v, expectedTy);
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
            } else if (capName.size() > 6 && capName.substr(0, 6) == "__arr_") {
                // Array base ptr capture
                std::string arrName = capName.substr(6);
                auto ptrTyN = llvm::PointerType::getUnqual(*ctx.llvmContext);
                auto ait2 = ctx.arrays.find(arrName);
                if (ait2 != ctx.arrays.end())
                    argsV.push_back(ait2->second.basePtr);
                else
                    argsV.push_back(llvm::ConstantPointerNull::get(ptrTyN));
            } else if (capName.size() > 8 && capName.substr(0, 8) == "__arrlo_") {
                // Dynamic lo (first dim) value capture
                std::string arrName = capName.substr(8);
                auto i64Ty5 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                auto loIt = ctx.locals.find(arrName + "__lo");
                if (loIt != ctx.locals.end())
                    argsV.push_back(ctx.builder->CreateLoad(i64Ty5, loIt->second, "lov"));
                else {
                    auto ait3 = ctx.arrays.find(arrName);
                    argsV.push_back(llvm::ConstantInt::get(i64Ty5,
                        ait3 != ctx.arrays.end() ? ait3->second.lowerBound : 0));
                }
            } else if (capName.size() > 8 && capName.substr(0, 8) == "__arr2d_") {
                // 2D array extra captures: lo2 and stride as i64 values
                std::string rest = capName.substr(8);
                auto i64Ty4 = llvm::Type::getInt64Ty(*ctx.llvmContext);
                if (rest.size() > 4 && rest.substr(rest.size()-4) == "_lo2") {
                    std::string arrName = rest.substr(0, rest.size()-4);
                    auto lo2It = ctx.locals.find(arrName + "__lo2");
                    if (lo2It != ctx.locals.end())
                        argsV.push_back(ctx.builder->CreateLoad(i64Ty4, lo2It->second, "lo2v"));
                    else {
                        auto ait2 = ctx.arrays.find(arrName);
                        argsV.push_back(llvm::ConstantInt::get(i64Ty4,
                            ait2 != ctx.arrays.end() ? ait2->second.lowerBound2 : 0));
                    }
                } else if (rest.size() > 7 && rest.substr(rest.size()-7) == "_stride") {
                    std::string arrName = rest.substr(0, rest.size()-7);
                    auto strIt = ctx.locals.find(arrName + "__stride");
                    if (strIt != ctx.locals.end())
                        argsV.push_back(ctx.builder->CreateLoad(i64Ty4, strIt->second, "stridev"));
                    else {
                        auto ait2 = ctx.arrays.find(arrName);
                        argsV.push_back(llvm::ConstantInt::get(i64Ty4,
                            (ait2 != ctx.arrays.end() && ait2->second.stride > 0) ? ait2->second.stride : 1));
                    }
                } else {
                    argsV.push_back(llvm::ConstantInt::get(i64Ty4, 0));
                }
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
        (ctx.hadError = true, std::cerr) << "Error: unknown class '" << className << "'\n";
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

    // Store constructor arguments into this class's own parameter fields only.
    // Inherited parent fields are left for the parent body coroutine to set up.
    // Skip companion "__pos" fields — those are bookkeeping fields, not params.
    size_t argIdx = 0;
    for (auto& param : ci.fields) {
        if (param.structIndex < ci.firstOwnFieldIndex) continue;
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
                        val = simulaRealToInt(ctx, val, destTy);
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
            // Wrap the NUL-terminated line buffer as a TEXT descriptor.
            auto litFn = ctx.module->getOrInsertFunction("simula_text_lit",
                llvm::FunctionType::get(ptrTy, {ptrTy}, false));
            return ctx.builder->CreateCall(litFn, {buf}, "sysin_img");
        }
    }

    // Check for TEXT variable member access first
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        if (ctx.textVars.count(ident->name)) {
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto [varPtr, varTy] = ctx.getVarPtr(ident->name);
            if (!varPtr) {
                (ctx.hadError = true, std::cerr) << "Error: TEXT variable '" << ident->name << "' not accessible\n";
                return nullptr;
            }
            auto desc = ctx.builder->CreateLoad(ptrTy, varPtr, "txtdesc");
            std::vector<llvm::Value*> noArgs;
            auto r = emitTextOp(ctx, desc, member, noArgs);
            if (r) return r;
            (ctx.hadError = true, std::cerr) << "Error: unknown TEXT member '." << member << "'\n";
            return nullptr;
        }
    }

    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    // TEXT temporary (e.g. COPY(X).STRIP, FILE.INTEXT(n).LENGTH): if the object
    // is a TEXT-valued expression, dispatch the member through the descriptor.
    if (obj->getType()->isPointerTy() && exprIsText(object.get(), ctx)) {
        std::vector<llvm::Value*> noArgs;
        auto r = emitTextOp(ctx, obj, member, noArgs);
        if (r) return r;
    }

    std::string clsName;
    if (auto* qua = dynamic_cast<QuaExpression*>(object.get())) {
        clsName = qua->className;
    } else if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        clsName = ctx.resolveRefType(ident->name);
        // Also look in current class fields
        if (clsName.empty() && ctx.currentThis && !ctx.currentClassName.empty()) {
            std::string sc = ctx.currentClassName;
            while (!sc.empty() && clsName.empty()) {
                auto cit = ctx.classes.find(sc);
                if (cit == ctx.classes.end()) break;
                for (auto& fi : cit->second.fields)
                    if (fi.name == ident->name && !fi.refClassName.empty())
                        { clsName = fi.refClassName; break; }
                sc = cit->second.parentName;
            }
        }
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
    // Chained member access: resolve refClassName of intermediate member
    if (clsName.empty()) {
        if (auto* ma = dynamic_cast<MemberAccess*>(object.get())) {
            std::string outerCls;
            if (auto* id = dynamic_cast<Identifier*>(ma->object.get()))
                outerCls = ctx.resolveRefType(id->name);
            if (outerCls.empty() && ctx.currentThis) {
                if (auto* id = dynamic_cast<Identifier*>(ma->object.get())) {
                    std::string sc = ctx.currentClassName;
                    while (!sc.empty() && outerCls.empty()) {
                        auto cit = ctx.classes.find(sc);
                        if (cit == ctx.classes.end()) break;
                        for (auto& fi : cit->second.fields)
                            if (fi.name == id->name && !fi.refClassName.empty())
                                { outerCls = fi.refClassName; break; }
                        sc = cit->second.parentName;
                    }
                }
            }
            if (auto* th = dynamic_cast<ThisExpression*>(ma->object.get()))
                outerCls = ctx.currentClassName;
            if (!outerCls.empty()) {
                std::string sc = outerCls;
                while (!sc.empty() && clsName.empty()) {
                    auto cit = ctx.classes.find(sc);
                    if (cit == ctx.classes.end()) break;
                    for (auto& fi : cit->second.fields)
                        if (fi.name == ma->member && !fi.refClassName.empty())
                            { clsName = fi.refClassName; break; }
                    sc = cit->second.parentName;
                }
            }
        }
    }

    if (clsName.empty()) {
        (ctx.hadError = true, std::cerr) << "Error: cannot determine class type for member access '." << member << "'\n";
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
                // No implementation compiled yet (concrete override appears later
                // in the source). Build the type from the VIRTUAL declaration's
                // declared return type. A no-arg member access takes only `this`.
                if (!funcTy) {
                    auto rtIt = cit2->second.virtualReturnTypes.find(member);
                    if (rtIt != cit2->second.virtualReturnTypes.end()) {
                        llvm::Type* retTy;
                        if (rtIt->second == -1)
                            retTy = llvm::Type::getVoidTy(*ctx.llvmContext);
                        else if (rtIt->second == -2)
                            retTy = ptrTy2;
                        else
                            retTy = ctx.getLLVMType(rtIt->second);
                        funcTy = llvm::FunctionType::get(retTy, {ptrTy2}, false);
                    }
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

    // PROCESS scheduling attributes (SIMULATION)
    if (member == "evtime" || member == "idle" || member == "terminated") {
        auto i64TyP = llvm::Type::getInt64Ty(*ctx.llvmContext);
        auto doubleTyP = llvm::Type::getDoubleTy(*ctx.llvmContext);
        auto ptrTyP = llvm::PointerType::getUnqual(*ctx.llvmContext);
        if (member == "evtime") {
            auto fn = ctx.module->getOrInsertFunction("simula_sim_evtime",
                llvm::FunctionType::get(doubleTyP, {ptrTyP}, false));
            return ctx.builder->CreateCall(fn, {obj}, "evtime");
        }
        auto fn = ctx.module->getOrInsertFunction(
            member == "idle" ? "simula_sim_idle" : "simula_sim_terminated",
            llvm::FunctionType::get(i64TyP, {ptrTyP}, false));
        auto r = ctx.builder->CreateCall(fn, {obj}, member);
        return ctx.builder->CreateICmpNE(r,
            llvm::ConstantInt::get(i64TyP, 0), member + "_b");
    }

    (ctx.hadError = true, std::cerr) << "Error: class '" << clsName << "' has no field or method '" << member << "'\n";
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
            // Layout / lifecycle methods of PRINTFILE mapped onto stdout:
            // LINE(n)/EJECT(n) emit a newline, SPACING/OPEN/CLOSE are no-ops.
            if (method == "line" || method == "eject") {
                for (auto& a : args) a->codegen(ctx); // evaluate for effects
                return OutImageStatement().codegen(ctx);
            }
            if (method == "spacing" || method == "open" || method == "close") {
                for (auto& a : args) a->codegen(ctx);
                return llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(*ctx.llvmContext), 0);
            }
        }
    }

    // Check for TEXT variable method call first
    if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
        if (ctx.textVars.count(ident->name)) {
            auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
            auto [varPtr, varTy] = ctx.getVarPtr(ident->name);
            if (!varPtr) {
                (ctx.hadError = true, std::cerr) << "Error: TEXT variable '" << ident->name << "' not accessible\n";
                return nullptr;
            }
            auto desc = ctx.builder->CreateLoad(ptrTy, varPtr, "txtdesc");
            std::vector<llvm::Value*> argv;
            for (auto& a : args) {
                auto v = a->codegen(ctx);
                if (!v) return nullptr;
                argv.push_back(v);
            }
            auto r = emitTextOp(ctx, desc, method, argv);
            if (r) return r;
            (ctx.hadError = true, std::cerr) << "Error: unknown TEXT method '." << method << "'\n";
            return nullptr;
        }
    }

    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    // TEXT temporary as receiver (e.g. B.DATA.SUB(..), COPY(x).SUB(..)): the
    // method is a TEXT op on the descriptor.
    if (obj->getType()->isPointerTy() && exprIsText(object.get(), ctx)) {
        std::vector<llvm::Value*> argv;
        for (auto& a : args) {
            auto v = a->codegen(ctx);
            if (!v) return nullptr;
            argv.push_back(v);
        }
        auto r = emitTextOp(ctx, obj, method, argv);
        if (r) return r;
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
        (ctx.hadError = true, std::cerr) << "Error: cannot determine class type for method call '." << method << "'\n";
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
                                v = simulaRealToInt(ctx, v, destTy);
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
        (ctx.hadError = true, std::cerr) << "Error: class '" << clsName << "' has no method '" << method << "'\n";
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
                    v = simulaRealToInt(ctx, v, destTy);
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
        (ctx.hadError = true, std::cerr) << "Error: THIS used outside of a class body\n";
        return nullptr;
    }
    return ctx.currentThis;
}

// Shared by IS/IN: test the object's class id against a set of ids, with a
// runtime NONE guard (NONE IS/IN C is false, never a null dereference).
static llvm::Value* classIdTest(CodeGenContext& ctx, llvm::Value* obj,
                                const std::set<int>& ids, const char* label) {
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto func = ctx.builder->GetInsertBlock()->getParent();

    auto isNone = ctx.builder->CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy), "isin_none");
    auto entryBB = ctx.builder->GetInsertBlock();
    auto loadBB = llvm::BasicBlock::Create(*ctx.llvmContext, "isin_load", func);
    auto contBB = llvm::BasicBlock::Create(*ctx.llvmContext, "isin_cont", func);
    ctx.builder->CreateCondBr(isNone, contBB, loadBB);

    ctx.builder->SetInsertPoint(loadBB);
    auto classId = ctx.loadClassId(obj);
    llvm::Value* result = ctx.builder->getFalse();
    for (int id : ids) {
        auto cmp = ctx.builder->CreateICmpEQ(classId,
            llvm::ConstantInt::get(i64Ty, id), label);
        result = ctx.builder->CreateOr(result, cmp, "isin_or");
    }
    auto loadEndBB = ctx.builder->GetInsertBlock();
    ctx.builder->CreateBr(contBB);

    ctx.builder->SetInsertPoint(contBB);
    auto phi = ctx.builder->CreatePHI(ctx.builder->getInt1Ty(), 2, "isin_res");
    phi->addIncoming(ctx.builder->getFalse(), entryBB);
    phi->addIncoming(result, loadEndBB);
    return phi;
}

llvm::Value* IsExpression::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    auto cit = ctx.classes.find(className);
    if (cit == ctx.classes.end()) {
        (ctx.hadError = true, std::cerr) << "Error: unknown class '" << className << "' in IS expression\n";
        return nullptr;
    }
    return classIdTest(ctx, obj, {cit->second.classId}, "is_check");
}

llvm::Value* InExpression::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;

    // X IN C is true when X's class is C or any class prefixed by C.
    auto ids = ctx.getDescendantIdSet(className);
    if (ids.empty()) {
        (ctx.hadError = true, std::cerr) << "Error: unknown class '" << className << "' in IN expression\n";
        return nullptr;
    }
    return classIdTest(ctx, obj, ids, "in_cmp");
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

    // Pre-pass: declare all class skeletons in this block so sibling classes can
    // reference each other regardless of declaration order (Simula sibling classes
    // are mutually visible). Bodies are generated later in the execution pass.
    for (auto& stmt : statements) {
        if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get()))
            cd->declareSkeleton(ctx);
    }

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
                    // ptr, lo, hi, lo2, stride
                    paramTypes.push_back(ptrTy);
                    paramTypes.push_back(i64Ty3);
                    paramTypes.push_back(i64Ty3);
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
    // Skip if already declared in the current scope (two-pass block processing).
    // A like-named global only blocks re-declaration at global scope; inside a
    // procedure or class body it's a genuine local that shadows the global
    // (Simula block scoping).
    bool atGlobalScope = ctx.inMainBlock && !ctx.currentThis;
    if (ctx.locals.count(name) || (atGlobalScope && ctx.globals.count(name))) {
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
                            val = simulaRealToInt(ctx, val, varTy);
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
                        val = simulaRealToInt(ctx, val, ty);
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
                    val = simulaRealToInt(ctx, val, destTy);
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

    // Evaluate second dimension bounds if this is a 2D array
    llvm::Value* lo2Val = nullptr, *hi2Val = nullptr;
    long long lo2 = 0, hi2 = 0;
    bool has2D = (lowerBound2 != nullptr && upperBound2 != nullptr);
    bool constBounds2 = false;
    if (has2D) {
        lo2Val = lowerBound2->codegen(ctx);
        hi2Val = upperBound2->codegen(ctx);
        if (lo2Val && hi2Val) {
            if (auto* c1 = llvm::dyn_cast<llvm::ConstantInt>(lo2Val))
                if (auto* c2 = llvm::dyn_cast<llvm::ConstantInt>(hi2Val)) {
                    lo2 = c1->getSExtValue(); hi2 = c2->getSExtValue();
                    constBounds2 = true;
                }
        }
    }

    // Use fully-constant path only when ALL required bounds are compile-time constants
    bool fullyConst = constBounds && (!has2D || constBounds2);
    if (fullyConst) {
        long long size = hi - lo + 1;
        if (size <= 0) size = 1;
        long long stride = 0; // 0 = 1D
        if (has2D) {
            stride = hi2 - lo2 + 1;
            if (stride <= 0) stride = 1;
            size = size * stride; // total elements
        }

        auto allocArr = [&](llvm::Value* basePtr, bool isStack) {
            ArrayInfo info;
            info.basePtr = basePtr;
            info.elementType = elemTy;
            info.isTextElem = (elementType == VarDeclaration::TEXT);
            info.lowerBound = lo;
            info.size = size;
            info.isStackArray = isStack;
            info.lowerBound2 = lo2;
            info.stride = stride;
            ctx.arrays[name] = info;
            if (!refClassName.empty()) ctx.refTypes[name] = refClassName;
        };

        // In main block, use a global array so procedures can access it
        if (ctx.inMainBlock && !ctx.currentThis) {
            auto arrTy = llvm::ArrayType::get(elemTy, size);
            auto gv = new llvm::GlobalVariable(
                *ctx.module, arrTy, false, llvm::GlobalValue::InternalLinkage,
                llvm::Constant::getNullValue(arrTy), "g_" + name);
            allocArr(gv, true);
            return gv;
        }

        // Inside a class body (currentThis set), heap-allocate so the array
        // outlives the body coroutine.
        if (ctx.currentThis) {
            auto elemSize = ctx.module->getDataLayout().getTypeAllocSize(elemTy);
            auto byteSize = llvm::ConstantInt::get(i64Ty, (long long)elemSize * size);
            auto ptr = ctx.builder->CreateCall(ctx.allocFunc, {byteSize}, name + "_data");
            allocArr(ptr, false);
            return ptr;
        }
        auto arrTy = llvm::ArrayType::get(elemTy, size);
        auto func = ctx.builder->GetInsertBlock()->getParent();
        auto alloca = ctx.createEntryBlockAlloca(func, name, arrTy);
        ctx.builder->CreateStore(llvm::Constant::getNullValue(arrTy), alloca);
        allocArr(alloca, true);
        return alloca;
    } else {
        // Dynamic or mixed bounds: heap-allocate via simula_alloc
        // First dimension size
        llvm::Value* sizeVal;
        if (constBounds) {
            sizeVal = llvm::ConstantInt::get(i64Ty, hi - lo + 1);
        } else {
            sizeVal = ctx.builder->CreateAdd(
                ctx.builder->CreateSub(hiVal, loVal, "range"),
                llvm::ConstantInt::get(i64Ty, 1), "arrsize");
        }

        // If 2D, compute stride and multiply total size
        llvm::Value* strideVal = nullptr;
        if (has2D && lo2Val && hi2Val) {
            if (constBounds2) {
                strideVal = llvm::ConstantInt::get(i64Ty, hi2 - lo2 + 1);
            } else {
                strideVal = ctx.builder->CreateAdd(
                    ctx.builder->CreateSub(hi2Val, lo2Val, "range2"),
                    llvm::ConstantInt::get(i64Ty, 1), "stride");
            }
            sizeVal = ctx.builder->CreateMul(sizeVal, strideVal, "totalsize");
        }

        auto elemSize = ctx.module->getDataLayout().getTypeAllocSize(elemTy);
        auto byteSize = ctx.builder->CreateMul(sizeVal,
            llvm::ConstantInt::get(i64Ty, elemSize), "bytes");
        auto ptr = ctx.builder->CreateCall(ctx.allocFunc, {byteSize}, name + "_data");

        // Store pointer in a local alloca
        auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto func = ctx.builder->GetInsertBlock()->getParent();
        auto alloca = ctx.createEntryBlockAlloca(func, name, ptrTy);
        ctx.builder->CreateStore(ptr, alloca);

        // Store lower bound for first-dimension index adjustment
        ArrayInfo info;
        info.basePtr = ptr;
        info.elementType = elemTy;
        info.isTextElem = (elementType == VarDeclaration::TEXT);
        info.isStackArray = false;
        info.size = 0;
        info.hasDynLo2 = false;
        info.hasDynStride = false;
        if (constBounds) {
            info.lowerBound = lo; // use static lo directly
        } else {
            info.lowerBound = 0; // will use dynamic __lo
            auto loAlloca = ctx.createEntryBlockAlloca(func, name + "__lo", i64Ty);
            ctx.builder->CreateStore(loVal, loAlloca);
            ctx.locals[name + "__lo"] = loAlloca;
        }

        if (has2D && strideVal) {
            // Store dynamic lo2 and stride for 2D access
            auto lo2Alloca = ctx.createEntryBlockAlloca(func, name + "__lo2", i64Ty);
            ctx.builder->CreateStore(lo2Val ? lo2Val : llvm::ConstantInt::get(i64Ty, lo2),
                                     lo2Alloca);
            ctx.locals[name + "__lo2"] = lo2Alloca;
            auto strAlloca = ctx.createEntryBlockAlloca(func, name + "__stride", i64Ty);
            ctx.builder->CreateStore(strideVal, strAlloca);
            ctx.locals[name + "__stride"] = strAlloca;
            info.hasDynLo2 = true;
            info.hasDynStride = true;
        }

        ctx.arrays[name] = info;
        if (!refClassName.empty()) ctx.refTypes[name] = refClassName;
        return ptr;
    }
}

llvm::Value* ArrayAssignment::codegen(CodeGenContext& ctx) {
    auto ait = ctx.arrays.find(name);
    if (ait == ctx.arrays.end()) {
        (ctx.hadError = true, std::cerr) << "Error: unknown array '" << name << "'\n";
        return nullptr;
    }
    auto& info = ait->second;
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);

    auto idxVal = index->codegen(ctx);
    if (!idxVal) return nullptr;

    // Compute adjusted first-dimension index = index - lowerBound
    llvm::Value* loBound;
    auto loIt = ctx.locals.find(name + "__lo");
    if (loIt != ctx.locals.end()) {
        loBound = ctx.builder->CreateLoad(i64Ty, loIt->second, "lo");
    } else {
        loBound = llvm::ConstantInt::get(i64Ty, info.lowerBound);
    }
    auto adjusted = ctx.builder->CreateSub(idxVal, loBound, "adj_idx");

    // 2D array: flat_idx = (i - lo1) * stride + (j - lo2)
    if (index2 && (info.stride != 0 || info.hasDynStride)) {
        auto idxVal2 = index2->codegen(ctx);
        if (!idxVal2) return nullptr;
        llvm::Value* stride;
        llvm::Value* lo2;
        if (info.hasDynStride) {
            stride = ctx.builder->CreateLoad(i64Ty,
                ctx.locals[name + "__stride"], "stride");
            lo2 = ctx.builder->CreateLoad(i64Ty,
                ctx.locals[name + "__lo2"], "lo2");
        } else {
            stride = llvm::ConstantInt::get(i64Ty, info.stride);
            lo2 = llvm::ConstantInt::get(i64Ty, info.lowerBound2);
        }
        auto row = ctx.builder->CreateMul(adjusted, stride, "row_off");
        auto col = ctx.builder->CreateSub(idxVal2, lo2, "col_adj");
        adjusted = ctx.builder->CreateAdd(row, col, "flat_idx");
    }

    llvm::Value* gep;
    if (info.isStackArray) {
        auto totalSize = info.size > 0 ? (size_t)info.size : 1;
        auto arrTy = llvm::ArrayType::get(info.elementType, totalSize);
        gep = ctx.builder->CreateGEP(arrTy, info.basePtr,
            {llvm::ConstantInt::get(i64Ty, 0), adjusted}, "arr_elem");
    } else {
        gep = ctx.builder->CreateGEP(info.elementType, info.basePtr,
            adjusted, "arr_elem");
    }

    auto val = value->codegen(ctx);
    if (!val) return nullptr;

    // TEXT array element with := is an in-place character copy, not a rebind.
    if (!isRef && info.isTextElem) {
        emitTextValueAssign(ctx, gep, val);
        return val;
    }

    // Type convert if needed
    if (val->getType() != info.elementType) {
        if (info.elementType->isDoubleTy() && val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, info.elementType);
        else if (info.elementType->isIntegerTy(64) && val->getType()->isDoubleTy())
            val = simulaRealToInt(ctx, val, info.elementType);
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

// TEXT value assignment (:=): copy characters in place into the existing frame
// at *slot, or COPY(src) when the slot is NOTEXT/frameless (first assignment).
// simula_text_assign returns the descriptor to bind, so a single store covers
// both cases.
static void emitTextValueAssign(CodeGenContext& ctx, llvm::Value* slot,
                                llvm::Value* rhsDesc) {
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto cur = ctx.builder->CreateLoad(ptrTy, slot, "txtcur");
    auto fn = ctx.module->getOrInsertFunction("simula_text_assign",
        llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false));
    auto bound = ctx.builder->CreateCall(fn, {cur, rhsDesc}, "txtasg");
    ctx.builder->CreateStore(bound, slot);
}

llvm::Value* Assignment::codegen(CodeGenContext& ctx) {
    // Check if assigning to procedure return variable
    if (name == ctx.currentProcName && ctx.returnValueAlloca) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
        auto destTy = ctx.returnValueAlloca->getAllocatedType();
        // TEXT-returning proc: := copies into the return frame (or COPY-binds).
        if (destTy->isPointerTy() && exprIsText(value.get(), ctx)) {
            emitTextValueAssign(ctx, ctx.returnValueAlloca, val);
            return val;
        }
        if (val->getType() != destTy) {
            if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
                val = ctx.builder->CreateSIToFP(val, destTy);
            else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                val = simulaRealToInt(ctx, val, destTy);
        }
        ctx.builder->CreateStore(val, ctx.returnValueAlloca);
        return val;
    }

    // TEXT variable: := is an in-place character copy, not a pointer rebind.
    if (ctx.textVars.count(name)) {
        auto [slot, slotTy] = ctx.getVarPtr(name);
        if (slot) {
            auto val = value->codegen(ctx);
            if (!val) return nullptr;
            emitTextValueAssign(ctx, slot, val);
            return val;
        }
    }

    auto [varPtr, varTy] = ctx.getVarPtr(name);
    if (varPtr) {
        auto val = value->codegen(ctx);
        if (!val) return nullptr;
        if (val->getType() != varTy) {
            if (varTy->isDoubleTy() && val->getType()->isIntegerTy())
                val = ctx.builder->CreateSIToFP(val, varTy);
            else if (varTy->isIntegerTy(64) && val->getType()->isDoubleTy())
                val = simulaRealToInt(ctx, val, varTy);
            else if (varTy->isIntegerTy(8) && val->getType()->isIntegerTy())
                val = ctx.builder->CreateTrunc(val, varTy);
        }
        ctx.builder->CreateStore(val, varPtr);
        return val;
    }

    (ctx.hadError = true, std::cerr) << "Error: unknown variable '" << name << "'\n";
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
        // Also check if ident is a REF field of the current class
        if (clsName.empty() && ctx.currentThis && !ctx.currentClassName.empty()) {
            std::string sc = ctx.currentClassName;
            while (!sc.empty() && clsName.empty()) {
                auto cit = ctx.classes.find(sc);
                if (cit == ctx.classes.end()) break;
                for (auto& fi : cit->second.fields)
                    if (fi.name == ident->name && !fi.refClassName.empty())
                        { clsName = fi.refClassName; break; }
                sc = cit->second.parentName;
            }
        }
    }
    if (clsName.empty() && dynamic_cast<ThisExpression*>(object.get())) {
        clsName = ctx.currentClassName;
    }
    if (clsName.empty()) {
        if (auto* call = dynamic_cast<ProcedureCall*>(object.get())) {
            clsName = ctx.resolveRefType(call->name);
        }
    }
    // For chained access like S.fst_.pred_: resolve the refClassName of the
    // intermediate member (S.fst_) by looking up fst_ in S's class.
    if (clsName.empty()) {
        if (auto* ma = dynamic_cast<MemberAccess*>(object.get())) {
            // Determine the class of ma->object
            std::string outerCls;
            if (auto* id = dynamic_cast<Identifier*>(ma->object.get()))
                outerCls = ctx.resolveRefType(id->name);
            if (outerCls.empty()) {
                if (auto* id = dynamic_cast<Identifier*>(ma->object.get())) {
                    // Check class fields
                    std::string sc = ctx.currentClassName;
                    while (!sc.empty() && outerCls.empty()) {
                        auto cit = ctx.classes.find(sc);
                        if (cit == ctx.classes.end()) break;
                        for (auto& fi : cit->second.fields)
                            if (fi.name == id->name && !fi.refClassName.empty())
                                { outerCls = fi.refClassName; break; }
                        sc = cit->second.parentName;
                    }
                }
            }
            if (!outerCls.empty()) {
                // Find refClassName of ma->member in outerCls
                std::string sc = outerCls;
                while (!sc.empty() && clsName.empty()) {
                    auto cit = ctx.classes.find(sc);
                    if (cit == ctx.classes.end()) break;
                    for (auto& fi : cit->second.fields)
                        if (fi.name == ma->member && !fi.refClassName.empty())
                            { clsName = fi.refClassName; break; }
                    sc = cit->second.parentName;
                }
            }
        }
    }
    if (clsName.empty()) {
        (ctx.hadError = true, std::cerr) << "Error: cannot determine class for member assignment '." << member << "'\n";
        return nullptr;
    }

    int idx = ctx.getFieldIndex(clsName, member);
    if (idx < 0) {
        (ctx.hadError = true, std::cerr) << "Error: class '" << clsName << "' has no field '" << member << "'\n";
        return nullptr;
    }

    auto val = value->codegen(ctx);
    if (!val) return nullptr;

    auto& ci = ctx.classes[clsName];
    auto gep = ctx.builder->CreateStructGEP(ci.structType, obj, idx, member + "_ptr");
    auto destTy = ci.structType->getElementType(idx);
    // TEXT field with := is an in-place character copy, not a pointer rebind.
    bool fieldIsText = false;
    for (auto& fi : ci.fields)
        if (fi.name == member) { fieldIsText = (fi.type == VarDeclaration::TEXT && fi.refClassName.empty()); break; }
    if (!isRef && fieldIsText) {
        emitTextValueAssign(ctx, gep, val);
        return val;
    }
    if (val->getType() != destTy) {
        if (destTy->isDoubleTy() && val->getType()->isIntegerTy())
            val = ctx.builder->CreateSIToFP(val, destTy);
        else if (destTy->isIntegerTy(64) && val->getType()->isDoubleTy())
            val = simulaRealToInt(ctx, val, destTy);
    }
    ctx.builder->CreateStore(val, gep);
    return val;
}

llvm::Value* MemberArrayAssignment::codegen(CodeGenContext& ctx) {
    auto obj = object->codegen(ctx);
    if (!obj) return nullptr;
    // Write-through TEXT view: T.SUB(start,len) := expr (and STRIP/MAIN).
    // The view descriptor shares the parent's frame, so an in-place value
    // assignment into the view writes straight through to the underlying text.
    // (Reference :- onto a view window is not meaningful, so only := applies.)
    if (!isRef && obj->getType()->isPointerTy() && exprIsText(object.get(), ctx) &&
        (member == "sub" || member == "strip" || member == "main")) {
        std::vector<llvm::Value*> argv;
        if (index)  { auto v = index->codegen(ctx);  if (!v) return nullptr; argv.push_back(v); }
        if (index2) { auto v = index2->codegen(ctx); if (!v) return nullptr; argv.push_back(v); }
        auto view = emitTextOp(ctx, obj, member, argv);
        if (!view) return nullptr;
        auto rhs = value->codegen(ctx);
        if (!rhs) return nullptr;
        auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
        auto fn = ctx.module->getOrInsertFunction("simula_text_assign",
            llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false));
        return ctx.builder->CreateCall(fn, {view, rhs}, "subasg");
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
        (ctx.hadError = true, std::cerr) << "Error: cannot determine class for member array assignment '." << member << "'\n";
        return nullptr;
    }
    int fldIdx = ctx.getFieldIndex(clsName, member);
    if (fldIdx < 0) {
        (ctx.hadError = true, std::cerr) << "Error: class '" << clsName << "' has no array field '" << member << "'\n";
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
            val = simulaRealToInt(ctx, val, elemTy);
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
        (ctx.hadError = true, std::cerr) << "Error: unknown REF variable '" << name << "'\n";
        return nullptr;
    }
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    // Reference assignment (T :- X): rebind to the same descriptor. The cursor
    // now lives inside the descriptor, so producers (COPY/SUB/BLANKS) already
    // hand back pos=0 — no separate reset needed.
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

llvm::Value* InnerStatement::codegen(CodeGenContext& ctx) {
    // Split marker consumed during class-body chain emission; a leaf class's
    // own INNER point is empty, so reaching one directly is a no-op.
    return nullptr;
}

llvm::Value* ActivateStatement::codegen(CodeGenContext& ctx) {
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);

    auto obj = process->codegen(ctx);
    if (!obj) return nullptr;

    auto re = llvm::ConstantInt::get(i64Ty, reactivate ? 1 : 0);

    if (mode == BEFORE || mode == AFTER) {
        auto other = otherProc->codegen(ctx);
        if (!other) return nullptr;
        auto fn = ctx.module->getOrInsertFunction("simula_sim_activate_rel",
            llvm::FunctionType::get(voidTy, {ptrTy, ptrTy, i64Ty, i64Ty}, false));
        return ctx.builder->CreateCall(fn, {obj, other,
            llvm::ConstantInt::get(i64Ty, mode == BEFORE ? 1 : 0), re});
    }

    if (mode == DIRECT) {
        // Direct activation: the activated process runs immediately; the
        // activator continues after it yields (exact from a process, all
        // current-time events drain when activating from the main program).
        auto fnNow = ctx.module->getOrInsertFunction("simula_sim_activate_now",
            llvm::FunctionType::get(voidTy, {ptrTy, i64Ty}, false));
        return ctx.builder->CreateCall(fnNow, {obj, re});
    }

    auto timeFn = ctx.module->getOrInsertFunction("simula_sim_time",
        llvm::FunctionType::get(doubleTy, {}, false));
    llvm::Value* t;
    bool pr = prior;
    {
        auto e = timeExpr->codegen(ctx);
        if (!e) return nullptr;
        if (e->getType()->isIntegerTy())
            e = ctx.builder->CreateSIToFP(e, doubleTy, "tofp");
        if (mode == DELAY) {
            auto now = ctx.builder->CreateCall(timeFn, {}, "now");
            t = ctx.builder->CreateFAdd(now, e, "at");
        } else {
            t = e;
        }
    }
    auto fn = ctx.module->getOrInsertFunction("simula_sim_activate",
        llvm::FunctionType::get(voidTy, {ptrTy, doubleTy, i64Ty, i64Ty}, false));
    return ctx.builder->CreateCall(fn, {obj, t,
        llvm::ConstantInt::get(i64Ty, pr ? 1 : 0), re});
}

llvm::Value* SwitchDeclaration::codegen(CodeGenContext& ctx) {
    // Store label list; basic blocks will be resolved at GO TO S(I) time.
    ctx.switches[name] = labels;
    // Pre-create label blocks so forward references work.
    for (auto& lbl : labels)
        ctx.getOrCreateLabel(lbl);
    return nullptr;
}

llvm::Value* ComputedGoto::codegen(CodeGenContext& ctx) {
    // GO TO S(expr) — indirect branch to one of the labels in switch S.
    auto it = ctx.switches.find(switchName);
    if (it == ctx.switches.end()) {
        (ctx.hadError = true, std::cerr) << "Error: unknown switch '" << switchName << "'\n";
        return nullptr;
    }
    auto& labels = it->second;
    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);

    auto idxVal = index->codegen(ctx);
    if (!idxVal) return nullptr;
    // Switch index is 1-based; truncate to i32 for LLVM switch
    if (!idxVal->getType()->isIntegerTy(64))
        idxVal = ctx.builder->CreateSExtOrTrunc(idxVal, i64Ty);
    auto one = llvm::ConstantInt::get(i64Ty, 1);
    auto idx0 = ctx.builder->CreateSub(idxVal, one, "sw_idx0");
    auto idx32 = ctx.builder->CreateTrunc(idx0, llvm::Type::getInt32Ty(*ctx.llvmContext), "sw_i32");

    // Create a default block (unreachable — out-of-range)
    auto defaultBB = llvm::BasicBlock::Create(*ctx.llvmContext, "sw_default", func);
    llvm::IRBuilder<> tmpB(defaultBB);
    tmpB.CreateUnreachable();

    // LLVM switch instruction (i32 index, i32 case values)
    auto i32Ty = llvm::Type::getInt32Ty(*ctx.llvmContext);
    auto sw = ctx.builder->CreateSwitch(idx32, defaultBB, (unsigned)labels.size());
    for (size_t i = 0; i < labels.size(); i++) {
        auto lbb = ctx.getOrCreateLabel(labels[i]);
        if (!lbb->getParent()) func->insert(func->end(), lbb);
        sw->addCase(llvm::ConstantInt::get(i32Ty, (int32_t)i), lbb);
    }

    auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "after_cgoto", func);
    ctx.builder->SetInsertPoint(afterBB);
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
    // Use getOrCreateLabel so forward GOTO (jumping to a label not yet seen) works.
    auto labelBB = ctx.getOrCreateLabel(label);
    if (labelBB->getParent() && labelBB->getParent() != func) {
        // Cross-function: can't branch there — emit unreachable
        ctx.builder->CreateUnreachable();
        auto afterBB = llvm::BasicBlock::Create(*ctx.llvmContext, "after_goto", func);
        ctx.builder->SetInsertPoint(afterBB);
        return nullptr;
    }
    // Attach the block to this function if not yet done
    if (!labelBB->getParent()) func->insert(func->end(), labelBB);

    ctx.builder->CreateBr(labelBB);

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
        (ctx.hadError = true, std::cerr) << "Error: FOR variable '" << var << "' not declared\n";
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
    // Simula FOR-STEP-UNTIL: the continuation test depends on the sign of the
    // step. For a positive step, loop while cur <= limit; for a negative step,
    // loop while cur >= limit (sign(step)*(cur-limit) <= 0).
    auto stepSign = step->codegen(ctx);
    auto zero = llvm::ConstantInt::get(stepSign->getType(), 0);
    auto stepNonNeg = ctx.builder->CreateICmpSGE(stepSign, zero, "stepsign");
    auto leCond = ctx.builder->CreateICmpSLE(curVal, limitV, "forcmp_le");
    auto geCond = ctx.builder->CreateICmpSGE(curVal, limitV, "forcmp_ge");
    auto condV = ctx.builder->CreateSelect(stepNonNeg, leCond, geCond, "forcmp");
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
        (ctx.hadError = true, std::cerr) << "Error: FOR variable '" << var << "' not declared\n";
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
            (ctx.hadError = true, std::cerr) << "Error: FOR variable '" << var << "' not declared\n";
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
        auto stepSign = range.step->codegen(ctx);
        auto zero = llvm::ConstantInt::get(stepSign->getType(), 0);
        auto stepNonNeg = ctx.builder->CreateICmpSGE(stepSign, zero, "fmr_sign");
        auto leCond = ctx.builder->CreateICmpSLE(cur, limitV, "fmr_le");
        auto geCond = ctx.builder->CreateICmpSGE(cur, limitV, "fmr_ge");
        auto cond = ctx.builder->CreateSelect(stepNonNeg, leCond, geCond, "fmr_cmp");
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
    // Record TEXT-returning procedures so call results are treated as TEXT.
    if (hasReturnType && returnType == VarDeclaration::TEXT)
        ctx.textReturningProcs.insert(name);

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
            // ARRAY: ptr, lo1, hi1, lo2, stride (stride=1 for 1D; lo2 unused for 1D)
            paramTypes.push_back(ctx.getRefType());
            paramTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext)); // lo
            paramTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext)); // hi
            paramTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext)); // lo2
            paramTypes.push_back(llvm::Type::getInt64Ty(*ctx.llvmContext)); // stride
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
        // Capture outer arrays (pass base pointer; also pass lo, lo2+stride for dyn bounds)
        auto i64TyC = llvm::Type::getInt64Ty(*ctx.llvmContext);
        for (auto& [aname, ainfo] : ctx.arrays) {
            if (llvm::isa<llvm::GlobalVariable>(ainfo.basePtr)) continue;
            captured.push_back("__arr_" + aname);
            capturedTys.push_back(ptrTy);
            paramTypes.push_back(ptrTy);
            // Also capture lo (first dim lower bound) if it's dynamic (lowerBound==0 sentinel)
            if (ainfo.lowerBound == 0 && ctx.locals.count(aname + "__lo")) {
                captured.push_back("__arrlo_" + aname);
                capturedTys.push_back(i64TyC);
                paramTypes.push_back(i64TyC);
            }
            if ((ainfo.hasDynStride || ainfo.stride > 0) &&
                (ctx.locals.count(aname + "__lo2") || ainfo.lowerBound2 != 0 ||
                 ctx.locals.count(aname + "__stride") || ainfo.stride > 0)) {
                // Pass lo2 and stride as separate i64 value params
                captured.push_back("__arr2d_" + aname + "_lo2");
                capturedTys.push_back(i64TyC);
                paramTypes.push_back(i64TyC);
                captured.push_back("__arr2d_" + aname + "_stride");
                capturedTys.push_back(i64TyC);
                paramTypes.push_back(i64TyC);
            }
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
    auto savedSwitches = ctx.switches;
    auto savedInMainBlock = ctx.inMainBlock;
    ctx.inMainBlock = false;
    auto savedJmpBuf = ctx.currentJmpBuf;
    auto savedNonLocalIds = ctx.nonLocalLabelIds;
    auto savedLabelParamNames = ctx.labelParamNames;
    auto savedTextVars = ctx.textVars;
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
        // Register the owning class's TEXT fields (including inherited) as TEXT
        // variables so member TEXT operations (SETPOS/GETCHAR/PUTCHAR/...) inside
        // the method resolve to the field's cursor rather than class dispatch.
        {
            std::string sc = ctx.currentClassName;
            while (!sc.empty()) {
                auto cit = ctx.classes.find(sc);
                if (cit == ctx.classes.end()) break;
                for (auto& fi : cit->second.fields) {
                    if (fi.type == VarDeclaration::TEXT && fi.refClassName.empty())
                        ctx.textVars.insert(fi.name);
                }
                sc = cit->second.parentName;
            }
        }
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
            // A NAME TEXT param supports the TEXT cursor operations (SETPOS,
            // GETCHAR, PUTCHAR, MORE). Register it and give it a local position
            // counter so those member calls resolve. PUTCHAR writes through to
            // the caller's buffer (pass-by-reference), so in-place edits stick.
            if (p.type == VarDeclaration::TEXT && p.refClassName.empty() && !p.isArray) {
                auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
                auto posAlloca = ctx.createEntryBlockAlloca(func, p.name + "__pos", i64Ty);
                ctx.builder->CreateStore(llvm::ConstantInt::get(i64Ty, 0), posAlloca);
                ctx.locals[p.name + "__pos"] = posAlloca;
                ctx.textVars.insert(p.name);
            }
            continue;
        }
        if (p.isArray) {
            // Array param: ptr, lo, hi, lo2, stride
            (&*argIt)->setName(p.name);
            llvm::Value* basePtr = &*argIt; ++argIt;
            llvm::Value* loArg = &*argIt;  (&*argIt)->setName(p.name + "_lo");  ++argIt;
            llvm::Value* hiArg = &*argIt;  (&*argIt)->setName(p.name + "_hi");  ++argIt;
            llvm::Value* lo2Arg = &*argIt; (&*argIt)->setName(p.name + "_lo2"); ++argIt;
            llvm::Value* strideArg = &*argIt; (&*argIt)->setName(p.name + "_stride"); ++argIt;
            auto i64Ty2 = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto loAlloca = ctx.createEntryBlockAlloca(func, p.name + "__lo", i64Ty2);
            ctx.builder->CreateStore(loArg, loAlloca);
            ctx.locals[p.name + "__lo"] = loAlloca;
            auto hiAlloca = ctx.createEntryBlockAlloca(func, p.name + "__hi", i64Ty2);
            ctx.builder->CreateStore(hiArg, hiAlloca);
            ctx.locals[p.name + "__hi"] = hiAlloca;
            auto lo2Alloca = ctx.createEntryBlockAlloca(func, p.name + "__lo2", i64Ty2);
            ctx.builder->CreateStore(lo2Arg, lo2Alloca);
            ctx.locals[p.name + "__lo2"] = lo2Alloca;
            auto strAlloca = ctx.createEntryBlockAlloca(func, p.name + "__stride", i64Ty2);
            ctx.builder->CreateStore(strideArg, strAlloca);
            ctx.locals[p.name + "__stride"] = strAlloca;
            ArrayInfo info;
            info.basePtr = basePtr;
            info.elementType = ctx.getLLVMType(p.arrayElemType);
            info.lowerBound = 0; // overridden by __lo at runtime
            info.size = 0;
            info.isStackArray = false;
            info.hasDynLo2 = true;
            info.hasDynStride = true;
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
        // Handle array base ptr captures: __arr_NAME
        if (capName.size() > 6 && capName.substr(0, 6) == "__arr_") {
            std::string arrName = capName.substr(6);
            auto outerIt = savedArrays.find(arrName);
            if (outerIt != savedArrays.end()) {
                ArrayInfo ainfo = outerIt->second;
                ainfo.basePtr = capPtr;
                ainfo.isStackArray = false;
                ctx.arrays[arrName] = ainfo;
            }
            continue;
        }
        // Handle dynamic lo (first dim) captures: __arrlo_NAME (i64 value)
        if (capName.size() > 8 && capName.substr(0, 8) == "__arrlo_") {
            std::string arrName = capName.substr(8);
            auto i64Ty4 = llvm::Type::getInt64Ty(*ctx.llvmContext);
            auto loAlloca = ctx.createEntryBlockAlloca(func, arrName + "__lo", i64Ty4);
            ctx.builder->CreateStore(capPtr, loAlloca);
            ctx.locals[arrName + "__lo"] = loAlloca;
            continue;
        }
        // Handle 2D array lo2/stride captures: __arr2d_NAME_lo2 and __arr2d_NAME_stride
        // These are i64 VALUES passed as function arguments (not pointers)
        if (capName.size() > 8 && capName.substr(0, 8) == "__arr2d_") {
            std::string rest = capName.substr(8);
            auto i64Ty4 = llvm::Type::getInt64Ty(*ctx.llvmContext);
            if (rest.size() > 4 && rest.substr(rest.size()-4) == "_lo2") {
                std::string arrName = rest.substr(0, rest.size()-4);
                // capPtr is actually an i64 value (lo2); store in a new alloca
                auto lo2Alloca = ctx.createEntryBlockAlloca(func, arrName + "__lo2", i64Ty4);
                ctx.builder->CreateStore(capPtr, lo2Alloca);
                ctx.locals[arrName + "__lo2"] = lo2Alloca;
                if (ctx.arrays.count(arrName))
                    ctx.arrays[arrName].hasDynLo2 = true;
            } else if (rest.size() > 7 && rest.substr(rest.size()-7) == "_stride") {
                std::string arrName = rest.substr(0, rest.size()-7);
                auto strAlloca = ctx.createEntryBlockAlloca(func, arrName + "__stride", i64Ty4);
                ctx.builder->CreateStore(capPtr, strAlloca);
                ctx.locals[arrName + "__stride"] = strAlloca;
                if (ctx.arrays.count(arrName))
                    ctx.arrays[arrName].hasDynStride = true;
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
    ctx.switches = savedSwitches;
    ctx.inMainBlock = savedInMainBlock;
    ctx.currentJmpBuf = savedJmpBuf;
    ctx.nonLocalLabelIds = savedNonLocalIds;
    ctx.labelParamNames = savedLabelParamNames;
    ctx.textVars = savedTextVars;

    return func;
}

// ---- Class declaration ----

void ClassDecl::declareSkeleton(CodeGenContext& ctx) {
    // Idempotent: a prior block-level pre-pass may already have built this.
    {
        auto it = ctx.classes.find(name);
        if (it != ctx.classes.end() && it->second.bodyFunc) return;
    }

    ClassInfo ci;
    ci.name = name;
    ci.parentName = parentName;
    ci.classId = ctx.nextClassId++;
    ci.coroFieldIndex = 1;
    ci.decl = this;

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
    // Constructor args start after vtable, coro, and all inherited fields.
    ci.firstOwnFieldIndex = (int)fieldTypes.size();

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

    // Register class before declaring functions/vtable
    ctx.classes[name] = ci;

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
        for (int i = 0; i < 16; i++) vtFields.push_back(ptrTy);
        auto tmpVtTy = llvm::StructType::create(*ctx.llvmContext, vtFields, name + "_vtable_t");
        ctx.classes[name].vtableType = tmpVtTy;
        auto tmpInit = llvm::ConstantAggregateZero::get(tmpVtTy);
        ctx.classes[name].vtableGlobal = new llvm::GlobalVariable(
            *ctx.module, tmpVtTy, false, llvm::GlobalValue::InternalLinkage,
            tmpInit, name + "_vtable");
    }

    // Pre-pass: create LLVM function declarations for all methods
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

    // Build vtable metadata (indices) so method dispatch works during codegen.
    {
        auto& ciRef = ctx.classes[name];
        auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
        auto ptrTy = ctx.getRefType();

        if (!parentName.empty()) {
            auto pit = ctx.classes.find(parentName);
            if (pit != ctx.classes.end()) {
                ciRef.vtableMethodOrder = pit->second.vtableMethodOrder;
                ciRef.vtableIndex = pit->second.vtableIndex;
            }
        }

        for (auto& [mname, mfunc] : ciRef.methods) {
            if (ciRef.vtableIndex.find(mname) == ciRef.vtableIndex.end()) {
                int idx = (int)ciRef.vtableMethodOrder.size() + 1;
                ciRef.vtableIndex[mname] = idx;
                ciRef.vtableMethodOrder.push_back(mname);
            }
        }

        for (auto& stmt : bodyStmts) {
            if (auto* vd = dynamic_cast<VirtualDecl*>(stmt.get())) {
                for (size_t mi = 0; mi < vd->methodNames.size(); mi++) {
                    std::string lname = vd->methodNames[mi];
                    for (auto& c : lname) c = tolower((unsigned char)c);
                    if (ciRef.vtableIndex.find(lname) == ciRef.vtableIndex.end()) {
                        int idx = (int)ciRef.vtableMethodOrder.size() + 1;
                        ciRef.vtableIndex[lname] = idx;
                        ciRef.vtableMethodOrder.push_back(lname);
                    }
                    if (mi < vd->returnTypes.size())
                        ciRef.virtualReturnTypes[lname] = vd->returnTypes[mi];
                }
            }
        }

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
                std::vector<llvm::Type*> pvtFields = {i64Ty};
                for (size_t i = 0; i < pit->second.vtableMethodOrder.size(); i++)
                    pvtFields.push_back(ptrTy);
                pit->second.vtableType->setBody(pvtFields);

                ciRef.vtableMethodOrder = pit->second.vtableMethodOrder;
                ciRef.vtableIndex = pit->second.vtableIndex;
                for (auto& [mname, mfunc] : ciRef.methods) {
                    if (ciRef.vtableIndex.find(mname) == ciRef.vtableIndex.end()) {
                        int newIdx = (int)ciRef.vtableMethodOrder.size() + 1;
                        ciRef.vtableIndex[mname] = newIdx;
                        ciRef.vtableMethodOrder.push_back(mname);
                    }
                }
            }
        }
    }
}

llvm::Value* ClassDecl::codegen(CodeGenContext& ctx) {
    // Ensure the skeleton (struct type, body func, method decls, vtable meta)
    // exists. A block-level pre-pass usually did this already so that sibling
    // classes can reference each other regardless of order.
    declareSkeleton(ctx);

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

    // Pre-pass: compile nested class declarations (so they're available).
    // Declare every nested skeleton first so nested siblings can reference each
    // other regardless of order, then generate their bodies.
    {
        auto savedThis = ctx.currentThis;
        auto savedClassName = ctx.currentClassName;
        auto savedInsideMethod = ctx.insideMethod;
        ctx.currentThis = nullptr;
        ctx.currentClassName = "";
        ctx.insideMethod = false;
        for (auto& stmt : bodyStmts) {
            if (auto* cd = dynamic_cast<ClassDecl*>(stmt.get()))
                cd->declareSkeleton(ctx);
        }
        for (auto& stmt : bodyStmts) {
            if (dynamic_cast<ClassDecl*>(stmt.get()))
                stmt->codegen(ctx);
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

    // vtable metadata was built in declareSkeleton(); buildAllVtables() fills in
    // the function pointers later.

    // Use the pre-declared class body function: void @ClassName_body(ptr %this)
    auto bodyFunc = ctx.classes[name].bodyFunc;

    // Generate body function
    auto bodyEntry = llvm::BasicBlock::Create(*ctx.llvmContext, "entry", bodyFunc);
    ctx.builder->SetInsertPoint(bodyEntry);
    ctx.currentThis = bodyFunc->arg_begin();
    ctx.currentThis->setName("this");
    ctx.locals.clear();
    // Keep the enclosing block's REF types (globals stay visible inside class
    // bodies); class fields are overlaid below.
    ctx.refTypes = savedRefTypes;
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

    // Execute body statements with INNER semantics: the prefix chain's bodies
    // wrap this class's own body. Emission order for chain Root -> ... -> this:
    // Root_pre, ..., this_pre, this_post, ..., Root_post, where pre/post split
    // at the first top-level INNER (no INNER = whole body is pre, per the
    // implicit-INNER-at-end rule).
    //
    // Per statement: skip ProcedureDecls (compiled separately), nested
    // ClassDecls, Var/RefDeclarations (struct fields); execute
    // ArrayDeclarations (allocate + store pointer into the struct field),
    // LabelDeclarations, and ordinary statements.
    auto emitClassStmts = [&](const StmtList& stmts, size_t from, size_t to) {
        for (size_t si = from; si < to && si < stmts.size(); si++) {
            auto& stmt = stmts[si];
            if (dynamic_cast<ProcedureDecl*>(stmt.get())) continue;
            if (dynamic_cast<ClassDecl*>(stmt.get())) continue;
            if (dynamic_cast<VarDeclaration*>(stmt.get())) continue;
            if (dynamic_cast<RefDeclaration*>(stmt.get())) continue;
            if (dynamic_cast<InnerStatement*>(stmt.get())) continue;
            if (auto* cs = dynamic_cast<CompoundStmt*>(stmt.get())) {
                bool allVarDecl = true;
                for (auto& s : cs->statements) {
                    if (!dynamic_cast<VarDeclaration*>(s.get())) { allVarDecl = false; break; }
                }
                if (allVarDecl) continue;
            }
            if (auto* ad = dynamic_cast<ArrayDeclaration*>(stmt.get())) {
                ad->codegen(ctx);
                int idx = ctx.getFieldIndex(name, ad->name);
                if (idx >= 0) {
                    auto& info = ctx.arrays[ad->name];
                    auto gep = ctx.builder->CreateStructGEP(ctx.classes[name].structType,
                        ctx.currentThis, idx, ad->name + "_fptr");
                    ctx.builder->CreateStore(info.basePtr, gep);
                    // Preserve the pre-populated static lower bound from the AST
                    // (info.lowerBound is 0 for dynamic-bound arrays).
                    long long preservedLo = ctx.classes[name].arrayMeta.count(ad->name)
                        ? ctx.classes[name].arrayMeta[ad->name].first
                        : info.lowerBound;
                    ctx.classes[name].arrayMeta[ad->name] = {preservedLo, info.size};
                }
                continue;
            }
            stmt->codegen(ctx);
        }
    };

    // Prefix chain, outermost first.
    std::vector<ClassDecl*> chain;
    for (ClassDecl* cd = this; cd; ) {
        chain.insert(chain.begin(), cd);
        if (cd->parentName.empty()) break;
        auto pit = ctx.classes.find(cd->parentName);
        cd = (pit != ctx.classes.end()) ? pit->second.decl : nullptr;
    }
    auto innerSplit = [](ClassDecl* cd) -> size_t {
        for (size_t i = 0; i < cd->bodyStmts.size(); i++)
            if (dynamic_cast<InnerStatement*>(cd->bodyStmts[i].get())) return i;
        return cd->bodyStmts.size();
    };
    std::vector<size_t> splits;
    for (auto* cd : chain) {
        splits.push_back(innerSplit(cd));
        emitClassStmts(cd->bodyStmts, 0, splits.back());
    }
    for (size_t i = chain.size(); i-- > 0; ) {
        size_t postFrom = splits[i] < chain[i]->bodyStmts.size() ? splits[i] + 1
                                                                 : splits[i];
        emitClassStmts(chain[i]->bodyStmts, postFrom, chain[i]->bodyStmts.size());
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
    } else if (auto* ma = dynamic_cast<MemberAccess*>(object.get())) {
        // Field access like H.IMAP — resolve the base object's class, then the
        // declared ref class of the named field.
        std::string baseCls;
        if (auto* bid = dynamic_cast<Identifier*>(ma->object.get()))
            baseCls = ctx.resolveRefType(bid->name);
        else if (dynamic_cast<ThisExpression*>(ma->object.get()))
            baseCls = ctx.currentClassName;
        std::string searchCls = baseCls;
        while (!searchCls.empty()) {
            auto cit = ctx.classes.find(searchCls);
            if (cit == ctx.classes.end()) break;
            bool found = false;
            for (auto& f : cit->second.fields) {
                if (f.name == ma->member && f.type == -1) {
                    inspectClass = f.refClassName;
                    found = true;
                    break;
                }
            }
            if (found) break;
            searchCls = cit->second.parentName;
        }
    }

    // The inspected object may be a field of the enclosing connected/class
    // scope (e.g. INSPECT OUTER DO INSPECT INNERFIELD DO ...); resolve its
    // declared REF class through the current class chain too.
    if (inspectClass.empty()) {
        if (auto* ident = dynamic_cast<Identifier*>(object.get())) {
            std::string sc = ctx.currentClassName;
            while (!sc.empty() && inspectClass.empty()) {
                auto cit = ctx.classes.find(sc);
                if (cit == ctx.classes.end()) break;
                for (auto& fi : cit->second.fields)
                    if (fi.name == ident->name && !fi.refClassName.empty())
                        { inspectClass = fi.refClassName; break; }
                sc = cit->second.parentName;
            }
        }
    }

    auto func = ctx.builder->GetInsertBlock()->getParent();
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);

    // Save current THIS context
    auto savedThis = ctx.currentThis;
    auto savedClassName = ctx.currentClassName;

    // Connect: set THIS to the inspected object and remember the enclosing
    // connection so nested INSPECT bodies can still reach outer attributes.
    auto connect = [&](const std::string& cls) {
        if (savedThis && !savedClassName.empty())
            ctx.inspectStack.push_back({savedThis, savedClassName});
        ctx.currentThis = obj;
        ctx.currentClassName = cls;
    };
    auto disconnect = [&]() {
        if (savedThis && !savedClassName.empty())
            ctx.inspectStack.pop_back();
        ctx.currentThis = savedThis;
        ctx.currentClassName = savedClassName;
    };

    // INSPECT is a runtime NONE test: NONE skips the connected bodies and runs
    // OTHERWISE (unconnected), per the Simula 67 connection statement rules.
    auto isNone = ctx.builder->CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy), "inspect_none");

    auto mergeBB = llvm::BasicBlock::Create(*ctx.llvmContext, "inspect_end");
    auto otherBB = otherwiseBody
        ? llvm::BasicBlock::Create(*ctx.llvmContext, "inspect_otherwise")
        : nullptr;
    auto noneTarget = otherBB ? otherBB : mergeBB;

    // INSPECT ref DO stmt (no WHEN clauses)
    if (whenClauses.empty()) {
        auto bodyBB = llvm::BasicBlock::Create(*ctx.llvmContext, "inspect_do", func);
        ctx.builder->CreateCondBr(isNone, noneTarget, bodyBB);
        ctx.builder->SetInsertPoint(bodyBB);
        if (doBody) {
            if (!inspectClass.empty()) {
                connect(inspectClass);
                doBody->codegen(ctx);
                disconnect();
            } else {
                doBody->codegen(ctx);
            }
        }
        if (!ctx.builder->GetInsertBlock()->getTerminator())
            ctx.builder->CreateBr(mergeBB);
    } else {
        // WHEN dispatch: guard against NONE before touching the class id.
        auto dispatchBB = llvm::BasicBlock::Create(*ctx.llvmContext, "inspect_dispatch", func);
        ctx.builder->CreateCondBr(isNone, noneTarget, dispatchBB);
        ctx.builder->SetInsertPoint(dispatchBB);
        auto classId = ctx.loadClassId(obj);

        for (size_t i = 0; i < whenClauses.size(); i++) {
            auto& wc = whenClauses[i];
            // WHEN C matches when the object's class is C or a subclass of C.
            auto ids = ctx.getDescendantIdSet(wc.className);
            if (ids.empty()) {
                (ctx.hadError = true, std::cerr) << "Error: unknown class '" << wc.className << "' in WHEN clause\n";
                continue;
            }
            llvm::Value* cmpV = ctx.builder->getFalse();
            for (int id : ids) {
                auto c = ctx.builder->CreateICmpEQ(classId,
                    llvm::ConstantInt::get(i64Ty, id), "when_cmp");
                cmpV = ctx.builder->CreateOr(cmpV, c, "when_or");
            }

            auto whenBB = llvm::BasicBlock::Create(*ctx.llvmContext, "when_" + wc.className, func);
            auto nextBB = llvm::BasicBlock::Create(*ctx.llvmContext, "when_next");

            ctx.builder->CreateCondBr(cmpV, whenBB, nextBB);

            ctx.builder->SetInsertPoint(whenBB);
            connect(wc.className);
            wc.body->codegen(ctx);
            disconnect();
            if (!ctx.builder->GetInsertBlock()->getTerminator())
                ctx.builder->CreateBr(mergeBB);

            func->insert(func->end(), nextBB);
            ctx.builder->SetInsertPoint(nextBB);
        }
        // No WHEN matched: fall through to OTHERWISE (or merge).
        if (!ctx.builder->GetInsertBlock()->getTerminator())
            ctx.builder->CreateBr(noneTarget);
    }

    // OTHERWISE runs unconnected (the object may be NONE here).
    if (otherBB) {
        func->insert(func->end(), otherBB);
        ctx.builder->SetInsertPoint(otherBB);
        otherwiseBody->codegen(ctx);
        if (!ctx.builder->GetInsertBlock()->getTerminator())
            ctx.builder->CreateBr(mergeBB);
    }

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
        (ctx.hadError = true, std::cerr) << "Error: DETACH used outside of a class body\n";
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
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    // A REAL argument converts with Simula rounding; smaller ints widen. This
    // also fixes ** results (double) being reinterpreted as integer bits.
    if (val->getType()->isDoubleTy())
        val = simulaRealToInt(ctx, val, i64Ty);
    else if (val->getType()->isIntegerTy(1))
        val = ctx.builder->CreateZExt(val, i64Ty, "widen");
    else if (!val->getType()->isIntegerTy(64))
        val = ctx.builder->CreateSExt(val, i64Ty, "widen");
    // OUTINT(i, w): right-justify in a field of width w; the runtime fills the
    // field with asterisks when the number doesn't fit (standard editing rule).
    // w == 0 prints with no padding (common Simula idiom).
    llvm::Value* w = width ? width->codegen(ctx) : llvm::ConstantInt::get(i64Ty, 0);
    if (!w) return nullptr;
    if (w->getType()->isDoubleTy())
        w = simulaRealToInt(ctx, w, i64Ty);
    else if (!w->getType()->isIntegerTy(64))
        w = ctx.builder->CreateSExt(w, i64Ty, "widen");
    auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    auto fn = ctx.module->getOrInsertFunction("simula_outint",
        llvm::FunctionType::get(voidTy, {i64Ty, i64Ty}, false));
    return ctx.builder->CreateCall(fn, {val, w});
}

llvm::Value* OutRealStatement::codegen(CodeGenContext& ctx) {
    // OUTREAL(v, d, w): d significant digits in scientific form with the '&'
    // exponent marker, right-justified in a field of width w (asterisk fill).
    // NOTE the AST field names predate this: `width` holds the 2nd argument
    // (d, significant digits) and `decimals` the 3rd (w, field width).
    auto val = value->codegen(ctx);
    if (!val) return nullptr;
    auto i64Ty = llvm::Type::getInt64Ty(*ctx.llvmContext);
    auto doubleTy = llvm::Type::getDoubleTy(*ctx.llvmContext);
    if (val->getType()->isIntegerTy())
        val = ctx.builder->CreateSIToFP(val, doubleTy, "tofp");
    auto d = width->codegen(ctx);
    auto w = decimals->codegen(ctx);
    if (!d || !w) return nullptr;
    if (d->getType()->isDoubleTy()) d = simulaRealToInt(ctx, d, i64Ty);
    if (w->getType()->isDoubleTy()) w = simulaRealToInt(ctx, w, i64Ty);
    auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    auto fn = ctx.module->getOrInsertFunction("simula_outreal",
        llvm::FunctionType::get(voidTy, {doubleTy, i64Ty, i64Ty}, false));
    return ctx.builder->CreateCall(fn, {val, d, w});
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
    // TEXT is a descriptor whose frame is not NUL-terminated (subwindows), so
    // print the exact window via the runtime rather than printf %s.
    auto ptrTy = llvm::PointerType::getUnqual(*ctx.llvmContext);
    auto voidTy = llvm::Type::getVoidTy(*ctx.llvmContext);
    auto fn = ctx.module->getOrInsertFunction("simula_outtext",
        llvm::FunctionType::get(voidTy, {ptrTy}, false));
    return ctx.builder->CreateCall(fn, {val});
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
