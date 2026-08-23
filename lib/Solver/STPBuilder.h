//===-- STPBuilder.h --------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef __UTIL_STPBUILDER_H__
#define __UTIL_STPBUILDER_H__

#include "klee/util/ExprHashMap.h"
#include "klee/util/ArrayExprHash.h"
#include "klee/Config/config.h"

#include <vector>

#define Expr VCExpr
#include <stp/c_interface.h>
#undef Expr

namespace klee {
  class ExprHolder {
    friend class ExprHandle;
    ::VCExpr expr;
    unsigned count;
    
  public:
    ExprHolder(const ::VCExpr _expr) : expr(_expr), count(0) {}
    ~ExprHolder() { 
      if (expr) vc_DeleteExpr(expr); 
    }
  };

  class ExprHandle {
    ExprHolder *H;
    
  public:
    ExprHandle() : H(new ExprHolder(0)) { H->count++; }
    ExprHandle(::VCExpr _expr) : H(new ExprHolder(_expr)) { H->count++; }
    ExprHandle(const ExprHandle &b) : H(b.H) { H->count++; }
    ~ExprHandle() { if (--H->count == 0) delete H; }
    
    ExprHandle &operator=(const ExprHandle &b) {
      if (--H->count == 0) delete H;
      H = b.H;
      H->count++;
      return *this;
    }

    operator bool () { return H->expr; }
    operator ::VCExpr () { return H->expr; }
  };
  
  class STPArrayExprHash : public ArrayExprHash< ::VCExpr > {
    
    friend class STPBuilder;
    
  public:
    STPArrayExprHash() {};
    virtual ~STPArrayExprHash();
  };

class STPBuilder {
  ::VC vc;
  ExprHashMap< std::pair<ExprHandle, unsigned> > constructed;

  /// optimizeDivides - Rewrite division and reminders by constants
  /// into multiplies and shifts. STP should probably handle this for
  /// use.
  bool optimizeDivides;

  STPArrayExprHash _arr_hash;

private:  

  ExprHandle bvOne(unsigned width);
  ExprHandle bvZero(unsigned width);
  ExprHandle bvMinusOne(unsigned width);
  ExprHandle bvConst32(unsigned width, uint32_t value);
  ExprHandle bvConst64(unsigned width, uint64_t value);
  ExprHandle bvZExtConst(unsigned width, uint64_t value);
  ExprHandle bvSExtConst(unsigned width, uint64_t value);

  ExprHandle bvBoolExtract(ExprHandle expr, int bit);
  ExprHandle bvExtract(ExprHandle expr, unsigned top, unsigned bottom);
  ExprHandle eqExpr(ExprHandle a, ExprHandle b);

  //logical left and right shift (not arithmetic)
  ExprHandle bvLeftShift(ExprHandle expr, unsigned shift);
  ExprHandle bvRightShift(ExprHandle expr, unsigned shift);
  ExprHandle bvVarLeftShift(ExprHandle expr, ExprHandle shift);
  ExprHandle bvVarRightShift(ExprHandle expr, ExprHandle shift);
  ExprHandle bvVarArithRightShift(ExprHandle expr, ExprHandle shift);
  ExprHandle extractPartialShiftValue(ExprHandle shift, unsigned width,
                                      unsigned &shiftBits);

  // Floating point.
  //
  // As in the Z3 builder, expressions are carried around as bitvectors and
  // only converted to STP's floating-point sort where a floating-point
  // operation actually needs one. castToFloat()/castToBitVector() do that
  // conversion on demand, and the helpers that can be handed either sort
  // (eqExpr, iteExpr, the bitvector operations) coerce their operands.
  static bool isFloat(::VCExpr e);
  /// Width in bits of the IEEE-754 format `e` has, i.e. exponent bits plus
  /// significand bits *including* the hidden bit. Note this is 79, not 80,
  /// for the format used to model x87 fp80 -- see castToFloat().
  static unsigned getFloatBitWidth(::VCExpr e);
  /// Maps one of Expr's float widths to the (exponent, significand) format
  /// that models it. The significand count includes the hidden bit.
  static void getFloatFormatFromBitWidth(unsigned bitWidth, int &expBits,
                                         int &sigBits);
  ExprHandle castToFloat(ExprHandle e);
  ExprHandle castToBitVector(ExprHandle e);
  ExprHandle getRoundingModeExpr(llvm::APFloat::roundingMode rm);
  ExprHandle getx87FP80ExplicitSignificandIntegerBit(ExprHandle e);

  // ITE that tolerates a mix of float and bitvector operands.
  ExprHandle iteExpr(ExprHandle cond, ExprHandle whenTrue,
                     ExprHandle whenFalse);

  ExprHandle constructAShrByConstant(ExprHandle expr, unsigned shift, 
                                     ExprHandle isSigned);
  ExprHandle constructMulByConstant(ExprHandle expr, unsigned width, uint64_t x);
  ExprHandle constructUDivByConstant(ExprHandle expr_n, unsigned width, uint64_t d);
  ExprHandle constructSDivByConstant(ExprHandle expr_n, unsigned width, uint64_t d);

  ::VCExpr getInitialArray(const Array *os);
  ::VCExpr getArrayForUpdate(const Array *root, const UpdateNode *un);

  ExprHandle constructActual(ref<Expr> e, int *width_out);
  ExprHandle construct(ref<Expr> e, int *width_out);
  
  ::VCExpr buildVar(const char *name, unsigned width);
  ::VCExpr buildArray(const char *name, unsigned indexWidth, unsigned valueWidth);
 
public:
  /// Constraints generated as a side effect of translating to STP's
  /// constraint language, rather than by any one Expr: casting an x87 fp80
  /// bit pattern to a float pins the explicit significand integer bit (see
  /// castToFloat()). Clients must assert these alongside the query, once the
  /// whole query has been constructed, and clear them afterwards.
  std::vector<ExprHandle> sideConstraints;
  void clearSideConstraints() { sideConstraints.clear(); }

  STPBuilder(::VC _vc, bool _optimizeDivides=true);
  ~STPBuilder();

  ExprHandle getTrue();
  ExprHandle getFalse();
  ExprHandle getInitialRead(const Array *os, unsigned index);

  ExprHandle construct(ref<Expr> e) { 
    ExprHandle res = construct(e, 0);
    constructed.clear();
    return res;
  }
};

}

#endif
