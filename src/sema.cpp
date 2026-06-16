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
    "abs","sign","entier","round","trunc","truncate","mod",
    "sqrt","sin","cos","tan","arctan","arcsin","arccos","exp","ln","log","log10",
    "max","min","maxval","minval","maxint","minint","maxreal","minreal","pi",
    "char","rank","digit","letter","isodigit","isoletter","lowcase","upcase",
    "copy","blanks","sub","strip","length","main",
    "randint","uniform","normal","negexp","poisson","erlang","draw",
    "discrete","linear","histd","histo",
    "time","current","hold","passivate","cancel","wait","accum","simterm",
    "lowerbound","upperbound","infrac","breakoutimage","outchar","error",
    "inopen","inclose","inreadline","inreadtext","outopen","outclose",
    "finint","finreal","flastitem","fouttext","foutint","foutimage",
    "sysin","sysout","__lastitem","detach","resume",
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
void SemanticAnalyzer::declare(const std::string& name) {
    if (name.empty() || scopes.empty()) return;
    scopes.back().insert(lower(name));
}

bool SemanticAnalyzer::resolved(const std::string& name) const {
    std::string n = lower(name);
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it)
        if (it->count(n)) return true;
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
    if (auto* l = dynamic_cast<LabeledStatement*>(s)) { collectClasses(l->statement.get()); return; }
    if (auto* in = dynamic_cast<InspectStatement*>(s)) {
        for (auto& wc : in->whenClauses) collectClasses(wc.body.get());
        collectClasses(in->doBody.get()); collectClasses(in->otherwiseBody.get());
        return;
    }
}

// All members of a class and its prefix ancestors (params + fields + methods +
// nested class names declared in the body).
void SemanticAnalyzer::addClassMembers(ClassDecl* cd, std::set<std::string>& out) {
    if (!cd) return;
    for (auto& p : cd->params) out.insert(lower(p.name));
    for (auto& st : cd->bodyStmts) {
        Statement* s = st.get();
        if (auto* v = dynamic_cast<VarDeclaration*>(s)) out.insert(lower(v->name));
        else if (auto* a = dynamic_cast<ArrayDeclaration*>(s)) out.insert(lower(a->name));
        else if (auto* r = dynamic_cast<RefDeclaration*>(s)) out.insert(lower(r->varName));
        else if (auto* pr = dynamic_cast<ProcedureDecl*>(s)) out.insert(lower(pr->name));
        else if (auto* c = dynamic_cast<ClassDecl*>(s)) out.insert(lower(c->name));
        else if (auto* cs = dynamic_cast<CompoundStmt*>(s)) {
            for (auto& in : cs->statements) {
                if (auto* v = dynamic_cast<VarDeclaration*>(in.get())) out.insert(lower(v->name));
                else if (auto* r = dynamic_cast<RefDeclaration*>(in.get())) out.insert(lower(r->varName));
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

    // Block / CompoundStmt: a new scope; collect its declarations first.
    if (auto* b = dynamic_cast<Block*>(s)) {
        scopes.emplace_back();
        for (auto& st : b->statements) {
            Statement* d = st.get();
            if (auto* v = dynamic_cast<VarDeclaration*>(d)) declare(v->name);
            else if (auto* a = dynamic_cast<ArrayDeclaration*>(d)) declare(a->name);
            else if (auto* r = dynamic_cast<RefDeclaration*>(d)) declare(r->varName);
            else if (auto* p = dynamic_cast<ProcedureDecl*>(d)) declare(p->name);
            else if (auto* c = dynamic_cast<ClassDecl*>(d)) declare(c->name);
            else if (auto* sw = dynamic_cast<SwitchDeclaration*>(d)) declare(sw->name);
            else if (auto* ld = dynamic_cast<LabelDeclaration*>(d))
                for (auto& l : ld->labels) declare(l);
            else if (auto* ls = dynamic_cast<LabeledStatement*>(d)) declare(ls->label);
            else if (auto* cs = dynamic_cast<CompoundStmt*>(d)) {
                for (auto& in : cs->statements) {
                    if (auto* v = dynamic_cast<VarDeclaration*>(in.get())) declare(v->name);
                    else if (auto* r = dynamic_cast<RefDeclaration*>(in.get())) declare(r->varName);
                    else if (auto* a = dynamic_cast<ArrayDeclaration*>(in.get())) declare(a->name);
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
        // Class body scope: all members (own + inherited) + a 'this' marker.
        scopes.emplace_back();
        addClassMembers(cd, scopes.back());
        scopes.back().insert("this");
        for (auto& st : cd->bodyStmts) checkStmt(st.get());
        scopes.pop_back();
        return;
    }
    if (auto* p = dynamic_cast<ProcedureDecl*>(s)) {
        for (auto& ps : p->params)
            if (!ps.refClassName.empty()) checkClassRef(ps.refClassName, p->line);
        scopes.emplace_back();
        for (auto& ps : p->params) declare(ps.name);
        declare(p->name);  // a typed procedure can assign to its own name (result)
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
        return;
    }
    if (auto* a = dynamic_cast<Assignment*>(s)) { checkIdent(a->name, a->line); checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<RefAssignment*>(s)) { checkIdent(a->name, a->line); checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<ArrayAssignment*>(s)) {
        checkIdent(a->name, a->line);
        checkExpr(a->index.get()); checkExpr(a->index2.get()); checkExpr(a->value.get());
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
        checkExpr(i->condition.get());
        checkStmt(i->thenBranch.get()); checkStmt(i->elseBranch.get());
        return;
    }
    if (auto* w = dynamic_cast<WhileStatement*>(s)) { checkExpr(w->condition.get()); checkStmt(w->body.get()); return; }
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
    if (auto* in = dynamic_cast<InspectStatement*>(s)) {
        checkExpr(in->object.get());
        for (auto& wc : in->whenClauses) {
            checkClassRef(wc.className, in->line);
            // Connected: the WHEN class's members are visible unqualified.
            scopes.emplace_back();
            auto cit = classByName.find(lower(wc.className));
            if (cit != classByName.end()) addClassMembers(cit->second, scopes.back());
            checkStmt(wc.body.get());
            scopes.pop_back();
        }
        // INSPECT ... DO connects an object whose class we can't always resolve
        // here; suppress undefined-identifier reports inside the connected body.
        suppressUndef++;
        checkStmt(in->doBody.get());
        suppressUndef--;
        checkStmt(in->otherwiseBody.get());  // unconnected
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
        // The callee name may be a procedure, builtin, array, or class-field
        // array; checking it has high false-positive risk, so only check args.
        for (auto& a : pc->args) checkExpr(a.get());
        return;
    }
    if (auto* ne = dynamic_cast<NewExpression*>(e)) {
        checkClassRef(ne->className, ne->line);
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
