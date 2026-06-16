#include "sema.h"
#include "ast.h"

#include <iostream>
#include <cctype>

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
}

void SemanticAnalyzer::analyze(Program& program) {
    auto* root = dynamic_cast<Statement*>(program.block.get());
    if (!root) return;
    // Standard-library classes recognized by codegen without a user declaration
    // (SYSIN/SYSOUT are printfiles; the file classes and SIMSET/SIMULATION
    // classes may be referenced even when not injected as ClassDecls).
    for (const char* n : {"printfile", "infile", "outfile", "directfile",
                          "bytefile", "imagefile", "file", "link", "head",
                          "process", "simset", "simulation"})
        classNames.insert(n);
    collectClasses(root);
    checkStmt(root);
}

void SemanticAnalyzer::checkClassRef(const std::string& name, int line) {
    if (name.empty()) return;
    if (classNames.count(lower(name))) return;
    hadError = true;
    std::cerr << "Error";
    if (line > 0) std::cerr << " at line " << line;
    std::cerr << ": unknown class '" << name << "'\n";
}

// ---- Pass 1: collect every class name declared anywhere (incl. nested) ----
void SemanticAnalyzer::collectClasses(Statement* s) {
    if (!s) return;
    if (auto* cd = dynamic_cast<ClassDecl*>(s)) {
        classNames.insert(lower(cd->name));
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
    if (auto* p = dynamic_cast<ProcedureDecl*>(s)) {
        collectClasses(p->body.get());
        return;
    }
    // Classes can be nested inside any statement that carries a body.
    if (auto* i = dynamic_cast<IfStatement*>(s)) {
        collectClasses(i->thenBranch.get());
        collectClasses(i->elseBranch.get());
        return;
    }
    if (auto* w = dynamic_cast<WhileStatement*>(s)) { collectClasses(w->body.get()); return; }
    if (auto* f = dynamic_cast<ForStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* f = dynamic_cast<ForListStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* f = dynamic_cast<ForMultiRangeStatement*>(s)) { collectClasses(f->body.get()); return; }
    if (auto* l = dynamic_cast<LabeledStatement*>(s)) { collectClasses(l->statement.get()); return; }
    if (auto* in = dynamic_cast<InspectStatement*>(s)) {
        for (auto& wc : in->whenClauses) collectClasses(wc.body.get());
        collectClasses(in->doBody.get());
        collectClasses(in->otherwiseBody.get());
        return;
    }
}

// ---- Pass 2: walk statements, checking class references ----
void SemanticAnalyzer::checkStmt(Statement* s) {
    if (!s) return;

    if (auto* b = dynamic_cast<Block*>(s)) {
        for (auto& st : b->statements) checkStmt(st.get());
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
        for (auto& st : cd->bodyStmts) checkStmt(st.get());
        return;
    }
    if (auto* p = dynamic_cast<ProcedureDecl*>(s)) {
        for (auto& ps : p->params)
            if (!ps.refClassName.empty()) checkClassRef(ps.refClassName, p->line);
        checkStmt(p->body.get());
        return;
    }
    if (auto* rd = dynamic_cast<RefDeclaration*>(s)) {
        checkClassRef(rd->className, rd->line);
        return;
    }
    if (auto* vd = dynamic_cast<VarDeclaration*>(s)) { checkExpr(vd->init.get()); return; }
    if (auto* ad = dynamic_cast<ArrayDeclaration*>(s)) {
        if (!ad->refClassName.empty()) checkClassRef(ad->refClassName, ad->line);
        checkExpr(ad->lowerBound.get()); checkExpr(ad->upperBound.get());
        checkExpr(ad->lowerBound2.get()); checkExpr(ad->upperBound2.get());
        return;
    }
    if (auto* a = dynamic_cast<Assignment*>(s)) { checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<RefAssignment*>(s)) { checkExpr(a->value.get()); return; }
    if (auto* a = dynamic_cast<ArrayAssignment*>(s)) {
        checkExpr(a->index.get()); checkExpr(a->index2.get()); checkExpr(a->value.get());
        return;
    }
    if (auto* a = dynamic_cast<MemberAssignment*>(s)) {
        checkExpr(a->object.get()); checkExpr(a->value.get());
        return;
    }
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
    if (auto* w = dynamic_cast<WhileStatement*>(s)) {
        checkExpr(w->condition.get()); checkStmt(w->body.get());
        return;
    }
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
        for (auto& r : f->ranges) {
            checkExpr(r.start.get()); checkExpr(r.step.get()); checkExpr(r.limit.get());
        }
        checkStmt(f->body.get());
        return;
    }
    if (auto* in = dynamic_cast<InspectStatement*>(s)) {
        checkExpr(in->object.get());
        for (auto& wc : in->whenClauses) {
            checkClassRef(wc.className, in->line);
            checkStmt(wc.body.get());
        }
        checkStmt(in->doBody.get());
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
    if (auto* o = dynamic_cast<OutIntStatement*>(s)) {
        checkExpr(o->value.get()); checkExpr(o->width.get()); return;
    }
    if (auto* o = dynamic_cast<OutRealStatement*>(s)) {
        checkExpr(o->value.get()); checkExpr(o->width.get()); checkExpr(o->decimals.get()); return;
    }
    if (auto* o = dynamic_cast<OutFixStatement*>(s)) {
        checkExpr(o->value.get()); checkExpr(o->decimals.get()); checkExpr(o->width.get()); return;
    }
    if (auto* o = dynamic_cast<OutTextStatement*>(s)) { checkExpr(o->text.get()); return; }
    // Other statements (labels, goto, switch, detach, inner, virtual, image) carry
    // no class references or sub-expressions to check.
}

// ---- Expression walk: check class references inside expressions ----
void SemanticAnalyzer::checkExpr(Expression* e) {
    if (!e) return;
    if (auto* b = dynamic_cast<BinaryOp*>(e)) { checkExpr(b->lhs.get()); checkExpr(b->rhs.get()); return; }
    if (auto* u = dynamic_cast<UnaryOp*>(e)) { checkExpr(u->operand.get()); return; }
    if (auto* pc = dynamic_cast<ProcedureCall*>(e)) {
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
    if (auto* is = dynamic_cast<IsExpression*>(e)) {
        checkExpr(is->object.get()); checkClassRef(is->className, is->line);
        return;
    }
    if (auto* in = dynamic_cast<InExpression*>(e)) {
        checkExpr(in->object.get()); checkClassRef(in->className, in->line);
        return;
    }
    if (auto* q = dynamic_cast<QuaExpression*>(e)) {
        checkExpr(q->object.get()); checkClassRef(q->className, q->line);
        return;
    }
    if (auto* c = dynamic_cast<ConditionalExpr*>(e)) {
        checkExpr(c->condition.get()); checkExpr(c->thenExpr.get()); checkExpr(c->elseExpr.get());
        return;
    }
    // Literals, Identifier, This, In*Expression: nothing to check yet.
}
