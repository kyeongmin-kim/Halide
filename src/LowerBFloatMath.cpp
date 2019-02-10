#include "LowerBFloatMath.h"
#include "IRMutator.h"
#include "IROperator.h"

namespace Halide {
namespace Internal {

namespace {
class LowerBFloatMath : public IRMutator {
public:
    using IRMutator::mutate;

    Expr mutate(const Expr &e) override {
        Expr new_e = IRMutator::mutate(e);
        if (e.type().is_bfloat()) {
            Type expected = UInt(16, e.type().lanes());
            internal_assert(new_e.type() == expected)
                << "Did not successfully remove bfloat math: " << e << " -> " << new_e << "\n";
        }
        return new_e;
    }

protected:
    using IRMutator::visit;

    Expr bfloat_to_float(Expr e) {
        e = cast(UInt(32, e.type().lanes()), e);
        e = e << 16;
        e = reinterpret(Float(32, e.type().lanes()), e);
        return e;
    }

    Expr float_to_bfloat(Expr e) {
        e = reinterpret(UInt(32, e.type().lanes()), e);
        e = e >> 16;
        e = cast(UInt(16, e.type().lanes()), e);
        return e;
    }

    template<typename Op>
    Expr visit_bin_op(const Op *op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        if (op->type.is_bfloat()) {
            a = bfloat_to_float(a);
            b = bfloat_to_float(b);
            return float_to_bfloat(Op::make(a, b));
        } else {
            return Op::make(a, b);
        }
    }

    Expr visit(const Add *op) override { return visit_bin_op(op); }
    Expr visit(const Sub *op) override { return visit_bin_op(op); }
    Expr visit(const Mod *op) override { return visit_bin_op(op); }
    Expr visit(const Mul *op) override { return visit_bin_op(op); }
    Expr visit(const Div *op) override { return visit_bin_op(op); }
    Expr visit(const LE *op) override { return visit_bin_op(op); }
    Expr visit(const LT *op) override { return visit_bin_op(op); }
    Expr visit(const GE *op) override { return visit_bin_op(op); }
    Expr visit(const GT *op) override { return visit_bin_op(op); }
    Expr visit(const Min *op) override { return visit_bin_op(op); }
    Expr visit(const Max *op) override { return visit_bin_op(op); }

    Expr visit(const FloatImm *op) override {
        if (op->type.is_bfloat()) {
            return Expr(bfloat16_t(op->value).to_bits());
        } else {
            return op;
        }
    }

    Expr visit(const Cast *op) override {
        if (op->type.is_bfloat()) {
            // Cast via float
            return float_to_bfloat(mutate(cast(Float(32, op->type.lanes()), op->value)));
        } else if (op->value.type().is_bfloat()) {
            return cast(op->type, bfloat_to_float(mutate(op->value)));
        } else {
            return IRMutator::visit(op);
        }
    }

    Expr visit(const Load *op) override {
        if (op->type.is_bfloat()) {
            // Load as uint16_t then widen to float
            Expr index = mutate(op->index);
            return Load::make(op->type.with_code(Type::UInt), op->name, index,
                              op->image, op->param, mutate(op->predicate), op->alignment);
        } else {
            return IRMutator::visit(op);
        }
    }

    // TODO: Call args and return values

    Stmt visit(const For *op) override {
        // Check the device_api and only enter body if the device does
        // not support bfloat16 math. Currently no devices support
        // bfloat16 math, so we always enter the body.
        return IRMutator::visit(op);
    }
};

}  // anonymous namespace

Stmt lower_bfloat_math(Stmt s) {
    return LowerBFloatMath().mutate(s);
}

}  // namespace Internal
}  // namespace Halide