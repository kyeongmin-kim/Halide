#include "Simplify.h"
#include "Simplify_Internal.h"

#include "IRMutator.h"
#include "Substitute.h"

namespace Halide {
namespace Internal {

using std::string;
using std::map;
using std::pair;
using std::ostringstream;
using std::vector;

#if LOG_EXPR_MUTATIONS || LOG_STMT_MUTATIONS
int Simplify::debug_indent = 0;
#endif

Simplify::Simplify(bool r, const Scope<Interval> *bi, const Scope<ModulusRemainder> *ai) :
    remove_dead_lets(r), no_float_simplify(false) {
    alignment_info.set_containing_scope(ai);

    // Only respect the constant bounds from the containing scope.
    for (Scope<Interval>::const_iterator iter = bi->cbegin(); iter != bi->cend(); ++iter) {
        ConstBounds bounds;
        if (const int64_t *i_min = as_const_int(iter.value().min)) {
            bounds.min_defined = true;
            bounds.min = *i_min;
        }
        if (const int64_t *i_max = as_const_int(iter.value().max)) {
            bounds.max_defined = true;
            bounds.max = *i_max;
        }

        if (bounds.min_defined || bounds.max_defined) {
            bounds_info.push(iter.name(), bounds);
        }
    }

}

void Simplify::found_buffer_reference(const string &name, size_t dimensions) {
    for (size_t i = 0; i < dimensions; i++) {
        string stride = name + ".stride." + std::to_string(i);
        if (var_info.contains(stride)) {
            var_info.ref(stride).old_uses++;
        }

        string min = name + ".min." + std::to_string(i);
        if (var_info.contains(min)) {
            var_info.ref(min).old_uses++;
        }
    }

    if (var_info.contains(name)) {
        var_info.ref(name).old_uses++;
    }
}

bool Simplify::const_float(const Expr &e, double *f) {
    if (e.type().is_vector()) {
        return false;
    } else if (const double *p = as_const_float(e)) {
        *f = *p;
        return true;
    } else {
        return false;
    }
}

bool Simplify::const_int(const Expr &e, int64_t *i) {
    if (e.type().is_vector()) {
        return false;
    } else if (const int64_t *p = as_const_int(e)) {
        *i = *p;
        return true;
    } else {
        return false;
    }
}

bool Simplify::const_uint(const Expr &e, uint64_t *u) {
    if (e.type().is_vector()) {
        return false;
    } else if (const uint64_t *p = as_const_uint(e)) {
        *u = *p;
        return true;
    } else {
        return false;
    }
}

void Simplify::ScopedFact::learn_false(const Expr &fact) {
    Simplify::VarInfo info;
    info.old_uses = info.new_uses = 0;
    if (const Variable *v = fact.as<Variable>()) {
        info.replacement = const_false(fact.type().lanes());
        var_info.push(v->name, info);
        pop_list.push_back(v);
    } else if (const NE *ne = fact.as<NE>()) {
        const Variable *v = ne->a.as<Variable>();
        if (v && is_const(ne->b)) {
            info.replacement = ne->b;
            var_info.push(v->name, info);
            pop_list.push_back(v);
        }
    } else if (const Or *o = fact.as<Or>()) {
        // Two things to learn!
        learn_false(o->a);
        learn_false(o->b);
    } else if (const Not *n = fact.as<Not>()) {
        learn_true(n->a);
    }
}

void Simplify::ScopedFact::learn_true(const Expr &fact) {
    // TODO: Also exploit < and > by updating bounds_info
    Simplify::VarInfo info;
    info.old_uses = info.new_uses = 0;
    if (const Variable *v = fact.as<Variable>()) {
        info.replacement = const_true(fact.type().lanes());
        var_info.push(v->name, info);
        pop_list.push_back(v);
    } else if (const EQ *eq = fact.as<EQ>()) {
        const Variable *v = eq->a.as<Variable>();
        if (v && is_const(eq->b)) {
            info.replacement = eq->b;
            var_info.push(v->name, info);
            pop_list.push_back(v);
        }
    } else if (const And *a = fact.as<And>()) {
        // Two things to learn!
        learn_true(a->a);
        learn_true(a->b);
    } else if (const Not *n = fact.as<Not>()) {
        learn_false(n->a);
    }
}

Simplify::ScopedFact::~ScopedFact() {
    for (auto v : pop_list) {
        var_info.pop(v->name);
    }
}

Expr simplify(Expr e, bool remove_dead_lets,
              const Scope<Interval> &bounds,
              const Scope<ModulusRemainder> &alignment) {
    return Simplify(remove_dead_lets, &bounds, &alignment).mutate(e, nullptr);
}

Stmt simplify(Stmt s, bool remove_dead_lets,
              const Scope<Interval> &bounds,
              const Scope<ModulusRemainder> &alignment) {
    return Simplify(remove_dead_lets, &bounds, &alignment).mutate(s);
}

class SimplifyExprs : public IRMutator2 {
public:
    using IRMutator2::mutate;
    Expr mutate(const Expr &e) override {
        return simplify(e);
    }
};

Stmt simplify_exprs(Stmt s) {
    return SimplifyExprs().mutate(s);
}

bool can_prove(Expr e) {
    // Remove likelies
    struct RemoveLikelies : public IRMutator2 {
        using IRMutator2::visit;
        Expr visit(const Call *op) override {
            if (op->is_intrinsic(Call::likely) ||
                op->is_intrinsic(Call::likely_if_innermost)) {
                return mutate(op->args[0]);
            } else {
                return IRMutator2::visit(op);
            }
        }
    };
    e = RemoveLikelies().mutate(e);

    internal_assert(e.type().is_bool())
        << "Argument to can_prove is not a boolean Expr: " << e << "\n";
    e = simplify(e);
    // likely(const-bool) is deliberately left unsimplified, because
    // things like max(likely(1), x) are meaningful, but we do want to
    // have can_prove(likely(1)) return true.
    if (const Call *c = e.as<Call>()) {
        if (c->is_intrinsic(Call::likely)) {
            e = c->args[0];
        }
    }

    // Take a closer look at all failed proof attempts to hunt for
    // simplifier weaknesses
    if (!is_const(e)) {
        struct RenameVariables : public IRMutator2 {
            using IRMutator2::visit;

            Expr visit(const Variable *op) {
                auto it = vars.find(op->name);
                if (lets.contains(op->name)) {
                    return Variable::make(op->type, lets.get(op->name));
                } else if (it == vars.end()) {
                    std::string name = "v" + std::to_string(count++);
                    vars[op->name] = name;
                    out_vars.emplace_back(op->type, name);
                    return Variable::make(op->type, name);
                } else {
                    return Variable::make(op->type, it->second);
                }
            }

            Expr visit(const Let *op) {
                std::string name = "v" + std::to_string(count++);
                ScopedBinding<string> bind(lets, op->name, name);
                return Let::make(name, mutate(op->value), mutate(op->body));
            }

            int count = 0;
            map<string, string> vars;
            Scope<string> lets;
            std::vector<pair<Type, string>> out_vars;
        } renamer;

        e = renamer.mutate(e);

        // Look for a concrete counter-example with random probing
        static std::mt19937 rng(0);
        for (int i = 0; i < 100; i++) {
            map<string, Expr> s;
            for (auto p : renamer.out_vars) {
                s[p.second] = make_const(p.first, (int)(rng() & 0xffff) - 0x7fff);
            }
            Expr probe = simplify(substitute(s, e));
            if (const Call *c = probe.as<Call>()) {
                if (c->is_intrinsic(Call::likely) ||
                    c->is_intrinsic(Call::likely_if_innermost)) {
                    probe = c->args[0];
                }
            }
            if (!is_one(probe)) {
                // Found a counter-example, or something that fails to fold
                return false;
            }
        }

        debug(0) << "Failed to prove, but could not find a counter-example:\n " << e << "\n";
        return false;
    }

    return is_one(e);
}

}
}
