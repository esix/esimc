#include "sema.h"
#include "ast.h"

#include <iostream>
#include <cctype>

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Names codegen accepts without a user declaration: math/char/text builtins,
// drawing + simulation procedures, environment constants, file primitives, and
// the special pseudo-variables sysin/sysout/__lastitem.
const char* const BUILTINS[] = {
    "abs","sign","entier","round","trunc","truncate","mod","rem",
    "sqrt","sin","cos","tan","arctan","arcsin","arccos","exp","ln","log","log10",
    "max","min","maxval","minval","maxint","minint","maxreal","minreal","pi",
    "char","rank","digit","letter","isodigit","isoletter","lowcase","upcase",
    "isorank","isochar",
    "copy","blanks","sub","strip","length","main",
    "randint","uniform","normal","negexp","poisson","erlang","draw",
    "discrete","linear","histd","histo",
    "time","current","hold","passivate","cancel","wait","accum","simterm",
    "lowerbound","upperbound","infrac","breakoutimage","outchar","outfrac","error",
    "inopen","inclose","inreadline","inreadtext","outopen","outclose",
    "finint","finreal","flastitem","fouttext","foutint","foutimage",
    "foutfix","foutreal","foutfrac","foutchar",
    "sysin","sysout","__lastitem","detach","resume","__isnone","__ensure",
};
}

void SemanticAnalyzer::analyze(Program& program) {
    auto* root = dynamic_cast<Statement*>(program.block.get());
    if (!root) return;
    for (const char* n : BUILTINS) builtins.insert(n);
    // Standard-library classes codegen recognizes without a user declaration.
    for (const char* n : {"printfile", "infile", "outfile", "directfile",
                          "bytefile", "imagefile", "file", "link", "head",
                          "process", "simset", "simulation"})
        classNames.insert(n);
    collectClasses(root);
    checkStmt(root);
}

// ---- scope helpers ----
void SemanticAnalyzer::declare(const std::string& name, Sym sym) {
    if (name.empty() || scopes.empty()) return;
    scopes.back()[lower(name)] = sym;
}

const Sym* SemanticAnalyzer::lookup(const std::string& name) const {
    std::string n = lower(name);
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto f = it->find(n);
        if (f != it->end()) return &f->second;
    }
    return nullptr;
}

bool SemanticAnalyzer::resolved(const std::string& name) const {
    std::string n = lower(name);
    if (lookup(n)) return true;
    if (builtins.count(n)) return true;
    if (classNames.count(n)) return true;
    return false;
}

void SemanticAnalyzer::checkClassRef(const std::string& name, int line) {
    if (name.empty()) return;
    if (classNames.count(lower(name))) return;
    hadError = true;
    std::cerr << "Error";
    if (line > 0) std::cerr << " at line " << line;
    std::cerr << ": unknown class '" << name << "'\n";
}

void SemanticAnalyzer::checkIdent(const std::string& name, int line) {
    if (suppressUndef > 0) return;
    if (name.empty()) return;
    if (resolved(name)) return;
    hadError = true;
    std::cerr << "Error";
    if (line > 0) std::cerr << " at line " << line;
    std::cerr << ": unknown variable '" << name << "'\n";
}

// Arity check for a call site. Only fires when the innermost binding is a
// procedure with a known parameter count — so arrays (A(I)), formal-procedure
// params, variables, and builtins are all skipped (no false positives).
void SemanticAnalyzer::checkCallArity(const std::string& name, int argc, int line) {
    const Sym* s = lookup(name);
    if (!s || s->kind != Sym::PROC || s->arity < 0) return;
    if (argc == s->arity) return;
    hadError = true;
    std::cerr << "Error";
    if (line > 0) std::cerr << " at line " << line;
    std::cerr << ": procedure '" << name << "' expects " << s->arity
              << " argument(s), got " << argc << "\n";
}

// Conservative type inference. Returns VarDeclaration::Type, -1 for REF, or
// -100 (unknown). Only confidently-determinable cases are typed; every
// uncertain construct (method calls, builtins, fields) returns unknown so the
// caller never flags it.
static const int UNKNOWN_TYPE = -100;
int SemanticAnalyzer::inferType(Expression* e) {
    using V = VarDeclaration;
    if (!e) return UNKNOWN_TYPE;
    if (dynamic_cast<IntegerLiteral*>(e)) return V::INTEGER;
    if (dynamic_cast<RealLiteral*>(e)) return V::REAL;
    if (dynamic_cast<BooleanLiteral*>(e)) return V::BOOLEAN;
    if (dynamic_cast<CharLiteral*>(e)) return V::CHARACTER;
    if (dynamic_cast<TextLiteral*>(e)) return V::TEXT;
    if (dynamic_cast<NoneLiteral*>(e)) return -1;  // NONE/NOTEXT
    if (dynamic_cast<IsExpression*>(e) || dynamic_cast<InExpression*>(e)) return V::BOOLEAN;
    if (auto* id = dynamic_cast<Identifier*>(e)) {
        const Sym* s = lookup(id->name);
        if (s && (s->kind == Sym::VAR || s->kind == Sym::PARAM || s->kind == Sym::PROC))
            return s->type;
        return UNKNOWN_TYPE;  // builtins, classes, etc.
    }
    if (auto* u = dynamic_cast<UnaryOp*>(e))
        return u->op == UnaryOp::NOT ? (int)V::BOOLEAN : inferType(u->operand.get());
    if (auto* b = dynamic_cast<BinaryOp*>(e)) {
        switch (b->op) {
            case BinaryOp::EQ: case BinaryOp::NE: case BinaryOp::LT: case BinaryOp::LE:
            case BinaryOp::GT: case BinaryOp::GE: case BinaryOp::REF_EQ:
            case BinaryOp::REF_NE: case BinaryOp::AND: case BinaryOp::OR:
            case BinaryOp::ANDTHEN: case BinaryOp::ORELSE:
                return V::BOOLEAN;
            case BinaryOp::CONCAT: return V::TEXT;
            default: {  // arithmetic
                int lt = inferType(b->lhs.get()), rt = inferType(b->rhs.get());
                if (lt == V::REAL || rt == V::REAL) return V::REAL;
                if (lt == V::INTEGER && rt == V::INTEGER) return V::INTEGER;
                return UNKNOWN_TYPE;
            }
        }
    }
    if (auto* pc = dynamic_cast<ProcedureCall*>(e)) {
        const Sym* s = lookup(pc->name);
        if (s && s->kind == Sym::PROC) return s->type;
        return UNKNOWN_TYPE;  // builtins / arrays — don't guess
    }
    if (auto* c = dynamic_cast<ConditionalExpr*>(e)) {
        int t = inferType(c->thenExpr.get());
        return t == inferType(c->elseExpr.get()) ? t : UNKNOWN_TYPE;
    }
    if (dynamic_cast<QuaExpression*>(e)) return -1;  // ref
    return UNKNOWN_TYPE;  // MemberAccess, MethodCall, This, In*Expression
}

// A condition (IF/WHILE) must be BOOLEAN. Flag only when we are confident the
// type is something else; unknown types are left alone.
void SemanticAnalyzer::checkCondition(Expression* cond, int line) {
    int t = inferType(cond);
    if (t == UNKNOWN_TYPE || t == VarDeclaration::BOOLEAN) return;
    if (cond && cond->line > 0) line = cond->line;
    const char* tn = "value";
    switch (t) {
        case VarDeclaration::INTEGER: tn = "INTEGER"; break;
        case VarDeclaration::REAL: tn = "REAL"; break;
        case VarDeclaration::CHARACTER: tn = "CHARACTER"; break;
        case VarDeclaration::TEXT: tn = "TEXT"; break;
        case -1: tn = "REF"; break;
    }
    hadError = true;
    std::cerr << "Error";
    if (line > 0) std::cerr << " at line " << line;
    std::cerr << ": condition must be BOOLEAN, not " << tn << "\n";
}

// Full constructor arity for NEW: sum params up the whole prefix chain.
int SemanticAnalyzer::newArity(const std::string& className) const {
    std::string cur = lower(className);
    int sum = 0;
    while (!cur.empty()) {
        auto it = classByName.find(cur);
        if (it == classByName.end()) return -1;  // unknown / standard class
        sum += (int)it->second->params.size();
        cur = lower(it->second->parentName);
    }
    return sum;
}

// ---- Pass 1: collect every class name + a name->decl map ----
void SemanticAnalyzer::collectClasses(Statement* s) {
    if (!s) return;
    if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
        classNames.insert(lower(cd->name));
        classByName[lower(cd->name)] = cd;
        for (auto& st : cd->bodyStmts) collectClasses(st.get());
        return;
    }
    if (auto* b = dynamic_cast<Block*>(s)) {
        for (auto& st : b->statements) collectClasses(st.get());
        return;
    }
    if (auto* c = dynamic_cast<CompoundStmt*>(s)) {
        for (auto& st : c->statements) collectClasses(st.get());
        return;
    }
    if (auto* p = dynamic_cast<ProcedureDecl*>(s)) { collectClasses(p->body.get()); return; }
    if (auto* i = dynamic_cast<IfStatement*>(s)) {
        collectClasses(i->thenBranch.get()); collectClasses(i->elseBranch.get()); return;
    }
    if (auto* w = dynamic_cast<WhileStatement*>(s)) { collectClasses(w->body.get()); return; }
    if (auto* f = dynamic_cast<ForStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* f = dynamic_cast<ForListStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* f = dynamic_cast<ForMultiRangeStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* f = dynamic_cast<ForGeneralStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* l = dynamic_cast<LabeledStatement*>(s)) { collectClasses(l->statement.get()); return; }
    if (auto* in = dynamic_cast<InspectStatement*>(s)) {
        for (auto& wc : in->whenClauses) collectClasses(wc.body.get());
        collectClasses(in->doBody.get()); collectClasses(in->otherwiseBody.get());
        return;
    }
}

// All members of a class and its prefix ancestors (params + fields + methods +
// nested class names). Child members take precedence over inherited ones.
void SemanticAnalyzer::addClassMembers(ClassDecl* cd, std::map<std::string, Sym>& out) {
    if (!cd) return;
    for (auto& p : cd->params)
        out.emplace(lower(p.name), Sym{Sym::PARAM, p.type, p.refClassName, -1});
    for (auto& st : cd->bodyStmts) {
        Statement* s = st.get();
        if (auto* v = dynamic_cast<VarDeclaration*>(s))
            out.emplace(lower(v->name), Sym{Sym::VAR, v->type, "", -1});
        else if (auto* a = dynamic_cast<ArrayDeclaration*>(s))
            out.emplace(lower(a->name), Sym{Sym::ARRAY, a->elementType, a->refClassName, -1});
        else if (auto* r = dynamic_cast<RefDeclaration*>(s))
            out.emplace(lower(r->varName), Sym{Sym::VAR, -1, r->className, -1});
        else if (auto* pr = dynamic_cast<ProcedureDecl*>(s))
            out.emplace(lower(pr->name), Sym{Sym::PROC,
                pr->hasReturnType ? pr->returnType : -100, "", (int)pr->params.size()});
        else if (auto* c = dynamic_cast<ClassDecl*>(s))
            out.emplace(lower(c->name), Sym{Sym::KLASS, -1, c->name, (int)c->params.size()});
        else if (auto* vd = dynamic_cast<VirtualDecl*>(s)) {
            // VIRTUAL: PROCEDURE M ... — a method whose body may live only in a
            // subclass. Register the name so unqualified calls resolve; arity
            // is unknown here, so calls to it are not arity-checked.
            for (auto& m : vd->methodNames)
                out.emplace(lower(m), Sym{Sym::PROC, -100, "", -1});
        }
        else if (auto* cs = dynamic_cast<CompoundStmt*>(s)) {
            for (auto& in : cs->statements) {
                if (auto* v = dynamic_cast<VarDeclaration*>(in.get()))
                    out.emplace(lower(v->name), Sym{Sym::VAR, v->type, "", -1});
                else if (auto* r = dynamic_cast<RefDeclaration*>(in.get()))
                    out.emplace(lower(r->varName), Sym{Sym::VAR, -1, r->className, -1});
            }
        }
    }
    if (!cd->parentName.empty()) {
        auto it = classByName.find(lower(cd->parentName));
        if (it != classByName.end()) addClassMembers(it->second, out);
    }
}

void SemanticAnalyzer::checkStmt(Statement* s) {
    if (!s) return;

    if (auto* b = dynamic_cast<Block*>(s)) {
        scopes.emplace_back();
        // Two-pass: declarations are visible across the whole block.
        for (auto& st : b->statements) {
            Statement* d = st.get();
            if (auto* v = dynamic_cast<VarDeclaration*>(d)) declare(v->name, {Sym::VAR, v->type, "", -1});
            else if (auto* a = dynamic_cast<ArrayDeclaration*>(d)) declare(a->name, {Sym::ARRAY, a->elementType, a->refClassName, -1});
            else if (auto* r = dynamic_cast<RefDeclaration*>(d)) declare(r->varName, {Sym::VAR, -1, r->className, -1});
            else if (auto* p = dynamic_cast<ProcedureDecl*>(d)) declare(p->name, {Sym::PROC, p->hasReturnType ? p->returnType : -100, "", (int)p->params.size()});
            else if (auto* c = dynamic_cast<ClassDecl*>(d)) declare(c->name, {Sym::KLASS, -1, c->name, (int)c->params.size()});
            else if (auto* sw = dynamic_cast<SwitchDeclaration*>(d)) declare(sw->name, {Sym::SWITCH, -100, "", -1});
            else if (auto* ld = dynamic_cast<LabelDeclaration*>(d))
                for (auto& l : ld->labels) declare(l, {Sym::LABEL, -100, "", -1});
            else if (auto* ls = dynamic_cast<LabeledStatement*>(d)) declare(ls->label, {Sym::LABEL, -100, "", -1});
            else if (auto* cs = dynamic_cast<CompoundStmt*>(d)) {
                for (auto& in : cs->statements) {
                    if (auto* v = dynamic_cast<VarDeclaration*>(in.get())) declare(v->name, {Sym::VAR, v->type, "", -1});
                    else if (auto* r = dynamic_cast<RefDeclaration*>(in.get())) declare(r->varName, {Sym::VAR, -1, r->className, -1});
                    else if (auto* a = dynamic_cast<ArrayDeclaration*>(in.get())) declare(a->name, {Sym::ARRAY, a->elementType, a->refClassName, -1});
                }
            }
        }
        for (auto& st : b->statements) checkStmt(st.get());
        scopes.pop_back();
        return;
    }
    if (auto* c = dynamic_cast<CompoundStmt*>(s)) {
        for (auto& st : c->statements) checkStmt(st.get());
        return;
    }
    if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
        if (!cd->parentName.empty()) checkClassRef(cd->parentName, cd->line);
        for (auto& p : cd->params)
            if (!p.refClassName.empty()) checkClassRef(p.refClassName, cd->line);
        scopes.emplace_back();
        addClassMembers(cd, scopes.back());
        scopes.back().emplace("this", Sym{Sym::VAR, -1, cd->name, -1});
        for (auto& st : cd->bodyStmts) checkStmt(st.get());
        scopes.pop_back();
        return;
    }
    if (auto* p = dynamic_cast<ProcedureDecl*>(s)) {
        for (auto& ps : p->params)
            if (!ps.refClassName.empty()) checkClassRef(ps.refClassName, p->line);
        scopes.emplace_back();
        for (auto& ps : p->params)
            declare(ps.name, {Sym::PARAM, ps.type, ps.refClassName, -1});
        // The procedure's own name (for result assignment and recursive calls).
        declare(p->name, {Sym::PROC, p->hasReturnType ? p->returnType : -100, "", (int)p->params.size()});
        checkStmt(p->body.get());
        scopes.pop_back();
        return;
    }
    if (auto* rd = dynamic_cast<RefDeclaration*>(s)) { checkClassRef(rd->className, rd->line); return; }
    if (auto* vd = dynamic_cast<VarDeclaration*>(s)) { checkExpr(vd->init.get()); return; }
    if (auto* ad = dynamic_cast<ArrayDeclaration*>(s)) {
        if (!ad->refClassName.empty()) checkClassRef(ad->refClassName, ad->line);
        checkExpr(ad->lowerBound.get()); checkExpr(ad->upperBound.get());
        checkExpr(ad->lowerBound2.get()); checkExpr(ad->upperBound2.get());
        checkExpr(ad->lowerBound3.get()); checkExpr(ad->upperBound3.get());
        return;
    }
    if (auto* a = dynamic_cast<Assignment*>(s)) { checkIdent(a->name, a->line); checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<RefAssignment*>(s)) { checkIdent(a->name, a->line); checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<ArrayAssignment*>(s)) {
        checkIdent(a->name, a->line);
        checkExpr(a->index.get()); checkExpr(a->index2.get()); checkExpr(a->index3.get()); checkExpr(a->value.get());
        return;
    }
    if (auto* a = dynamic_cast<MemberAssignment*>(s)) { checkExpr(a->object.get()); checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<MemberArrayAssignment*>(s)) {
        checkExpr(a->object.get()); checkExpr(a->index.get());
        checkExpr(a->index2.get()); checkExpr(a->value.get());
        return;
    }
    if (auto* e = dynamic_cast<ExprStatement*>(s)) { checkExpr(e->expr.get()); return; }
    if (auto* l = dynamic_cast<LabeledStatement*>(s)) { checkStmt(l->statement.get()); return; }
    if (auto* i = dynamic_cast<IfStatement*>(s)) {
        checkExpr(i->condition.get()); checkCondition(i->condition.get(), i->line);
        checkStmt(i->thenBranch.get()); checkStmt(i->elseBranch.get());
        return;
    }
    if (auto* w = dynamic_cast<WhileStatement*>(s)) { checkExpr(w->condition.get()); checkCondition(w->condition.get(), w->line); checkStmt(w->body.get()); return; }
    if (auto* f = dynamic_cast<ForStatement*>(s)) {
        checkExpr(f->start.get()); checkExpr(f->step.get()); checkExpr(f->limit.get());
        checkStmt(f->body.get());
        return;
    }
    if (auto* f = dynamic_cast<ForListStatement*>(s)) {
        for (auto& v : f->values) checkExpr(v.get());
        checkStmt(f->body.get());
        return;
    }
    if (auto* f = dynamic_cast<ForMultiRangeStatement*>(s)) {
        for (auto& r : f->ranges) { checkExpr(r.start.get()); checkExpr(r.step.get()); checkExpr(r.limit.get()); }
        checkStmt(f->body.get());
        return;
    }
    if (auto* f = dynamic_cast<ForGeneralStatement*>(s)) {
        for (auto& e : f->elements) {
            checkExpr(e.value.get()); checkExpr(e.cond.get());
            checkExpr(e.step.get()); checkExpr(e.limit.get());
        }
        checkStmt(f->body.get());
        return;
    }
    if (auto* in = dynamic_cast<InspectStatement*>(s)) {
        checkExpr(in->object.get());
        for (auto& wc : in->whenClauses) {
            checkClassRef(wc.className, in->line);
            scopes.emplace_back();
            auto cit = classByName.find(lower(wc.className));
            if (cit != classByName.end()) addClassMembers(cit->second, scopes.back());
            checkStmt(wc.body.get());
            scopes.pop_back();
        }
        // The connected class of INSPECT ... DO is not always resolvable here;
        // suppress undefined-identifier reports inside the connected body.
        suppressUndef++;
        checkStmt(in->doBody.get());
        suppressUndef--;
        checkStmt(in->otherwiseBody.get());
        return;
    }
    if (auto* a = dynamic_cast<ActivateStatement*>(s)) {
        checkExpr(a->process.get()); checkExpr(a->timeExpr.get()); checkExpr(a->otherProc.get());
        return;
    }
    if (auto* r = dynamic_cast<ResumeStatement*>(s)) { checkExpr(r->object.get()); return; }
    if (auto* c = dynamic_cast<CallStatement*>(s)) { checkExpr(c->object.get()); return; }
    if (auto* cg = dynamic_cast<ComputedGoto*>(s)) { checkExpr(cg->index.get()); return; }
    if (auto* o = dynamic_cast<OutIntStatement*>(s)) { checkExpr(o->value.get()); checkExpr(o->width.get()); return; }
    if (auto* o = dynamic_cast<OutRealStatement*>(s)) { checkExpr(o->value.get()); checkExpr(o->width.get()); checkExpr(o->decimals.get()); return; }
    if (auto* o = dynamic_cast<OutFixStatement*>(s)) { checkExpr(o->value.get()); checkExpr(o->decimals.get()); checkExpr(o->width.get()); return; }
    if (auto* o = dynamic_cast<OutTextStatement*>(s)) { checkExpr(o->text.get()); return; }
}

void SemanticAnalyzer::checkExpr(Expression* e) {
    if (!e) return;
    if (auto* id = dynamic_cast<Identifier*>(e)) { checkIdent(id->name, id->line); return; }
    if (auto* b = dynamic_cast<BinaryOp*>(e)) { checkExpr(b->lhs.get()); checkExpr(b->rhs.get()); return; }
    if (auto* u = dynamic_cast<UnaryOp*>(e)) { checkExpr(u->operand.get()); return; }
    if (auto* pc = dynamic_cast<ProcedureCall*>(e)) {
        checkIdent(pc->name, pc->line);
        if (suppressUndef == 0) checkCallArity(pc->name, (int)pc->args.size(), pc->line);
        for (auto& a : pc->args) checkExpr(a.get());
        return;
    }
    if (auto* ne = dynamic_cast<NewExpression*>(e)) {
        checkClassRef(ne->className, ne->line);
        int expect = newArity(ne->className);
        if (expect >= 0 && (int)ne->args.size() != expect) {
            hadError = true;
            std::cerr << "Error";
            if (ne->line > 0) std::cerr << " at line " << ne->line;
            std::cerr << ": class '" << ne->className << "' expects " << expect
                      << " argument(s), got " << ne->args.size() << "\n";
        }
        for (auto& a : ne->args) checkExpr(a.get());
        return;
    }
    if (auto* ma = dynamic_cast<MemberAccess*>(e)) { checkExpr(ma->object.get()); return; }
    if (auto* mc = dynamic_cast<MethodCall*>(e)) {
        checkExpr(mc->object.get());
        for (auto& a : mc->args) checkExpr(a.get());
        return;
    }
    if (auto* is = dynamic_cast<IsExpression*>(e)) { checkExpr(is->object.get()); checkClassRef(is->className, is->line); return; }
    if (auto* in = dynamic_cast<InExpression*>(e)) { checkExpr(in->object.get()); checkClassRef(in->className, in->line); return; }
    if (auto* q = dynamic_cast<QuaExpression*>(e)) { checkExpr(q->object.get()); checkClassRef(q->className, q->line); return; }
    if (auto* c = dynamic_cast<ConditionalExpr*>(e)) {
        checkExpr(c->condition.get()); checkExpr(c->thenExpr.get()); checkExpr(c->elseExpr.get());
        return;
    }
}
