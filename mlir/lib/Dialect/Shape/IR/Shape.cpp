//===- Shape.cpp - MLIR Shape Operations ----------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Shape/IR/Shape.h"

#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/Traits.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/DialectImplementation.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/InliningUtils.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/raw_ostream.h"

using namespace mlir;
using namespace mlir::shape;

namespace {
#include "ShapeCanonicalization.inc"
}

RankedTensorType shape::getExtentTensorType(MLIRContext *ctx) {
  return RankedTensorType::get({ShapedType::kDynamicSize}, IndexType::get(ctx));
}

static bool isErrorPropagationPossible(TypeRange operandTypes) {
  return llvm::any_of(operandTypes, [](Type ty) {
    return ty.isa<SizeType, ShapeType, ValueShapeType>();
  });
}

static LogicalResult verifySizeOrIndexOp(Operation *op) {
  assert(op != nullptr && op->getNumResults() == 1);
  Type resultTy = op->getResultTypes().front();
  if (isErrorPropagationPossible(op->getOperandTypes())) {
    if (!resultTy.isa<SizeType>())
      return op->emitOpError()
             << "if at least one of the operands can hold error values then "
                "the result must be of type `size` to propagate them";
  }
  return success();
}

static LogicalResult verifyShapeOrExtentTensorOp(Operation *op) {
  assert(op != nullptr && op->getNumResults() == 1);
  Type resultTy = op->getResultTypes().front();
  if (isErrorPropagationPossible(op->getOperandTypes())) {
    if (!resultTy.isa<ShapeType>())
      return op->emitOpError()
             << "if at least one of the operands can hold error values then "
                "the result must be of type `shape` to propagate them";
  }
  return success();
}

//===----------------------------------------------------------------------===//
// InlinerInterface
//===----------------------------------------------------------------------===//

namespace {
/// This class defines the interface for inlining shape dialect ops.
struct ShapeInlinerInterface : public DialectInlinerInterface {
  using DialectInlinerInterface::DialectInlinerInterface;

  // Returns true if the given region 'src' can be inlined into the region
  // 'dest' that is attached to an operation registered to the current dialect.
  bool isLegalToInline(Region *dest, Region *src, bool wouldBeCloned,
                       BlockAndValueMapping &) const final {
    return true;
  }

  // Returns true if the given operation 'op', that is registered to this
  // dialect, can be inlined into the region 'dest' that is attached to an
  // operation registered to the current dialect.
  bool isLegalToInline(Operation *op, Region *dest, bool wouldBeCloned,
                       BlockAndValueMapping &) const final {
    return true;
  }
};
} // namespace

void ShapeDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "mlir/Dialect/Shape/IR/ShapeOps.cpp.inc"
      >();
  addTypes<ShapeType, SizeType, ValueShapeType, WitnessType>();
  addInterfaces<ShapeInlinerInterface>();
  // Allow unknown operations during prototyping and testing. As the dialect is
  // still evolving it makes it simple to start with an unregistered ops and
  // try different variants before actually defining the op.
  allowUnknownOperations();
}

Operation *ShapeDialect::materializeConstant(OpBuilder &builder,
                                             Attribute value, Type type,
                                             Location loc) {
  if (type.isa<ShapeType>() ||
      type == getExtentTensorType(builder.getContext()))
    return builder.create<ConstShapeOp>(loc, type,
                                        value.cast<DenseIntElementsAttr>());
  if (type.isa<SizeType>())
    return builder.create<ConstSizeOp>(loc, type, value.cast<IntegerAttr>());
  if (type.isa<WitnessType>())
    return builder.create<ConstWitnessOp>(loc, type, value.cast<BoolAttr>());
  if (type.isa<IndexType>())
    return builder.create<ConstantOp>(loc, type, value);
  return nullptr;
}

/// Parse a type registered to this dialect.
Type ShapeDialect::parseType(DialectAsmParser &parser) const {
  StringRef keyword;
  if (parser.parseKeyword(&keyword))
    return Type();

  if (keyword == "shape")
    return ShapeType::get(getContext());
  if (keyword == "size")
    return SizeType::get(getContext());
  if (keyword == "value_shape")
    return ValueShapeType::get(getContext());
  if (keyword == "witness")
    return WitnessType::get(getContext());

  parser.emitError(parser.getNameLoc(), "unknown shape type: ") << keyword;
  return Type();
}

/// Print a type registered to this dialect.
void ShapeDialect::printType(Type type, DialectAsmPrinter &os) const {
  TypeSwitch<Type>(type)
      .Case<ShapeType>([&](Type) { os << "shape"; })
      .Case<SizeType>([&](Type) { os << "size"; })
      .Case<ValueShapeType>([&](Type) { os << "value_shape"; })
      .Case<WitnessType>([&](Type) { os << "witness"; })
      .Default([](Type) { llvm_unreachable("unexpected 'shape' type kind"); });
}

//===----------------------------------------------------------------------===//
// AnyOp
//===----------------------------------------------------------------------===//

// TODO: Canonicalization should be implemented for shapes that can be
// determined through mixtures of the known dimensions of the inputs.
OpFoldResult AnyOp::fold(ArrayRef<Attribute> operands) {
  // Only the last operand is checked because AnyOp is commutative.
  if (operands.back())
    return operands.back();

  return nullptr;
}

//===----------------------------------------------------------------------===//
// AssumingOp
//===----------------------------------------------------------------------===//

static ParseResult parseAssumingOp(OpAsmParser &parser,
                                   OperationState &result) {
  result.regions.reserve(1);
  Region *doRegion = result.addRegion();

  auto &builder = parser.getBuilder();
  OpAsmParser::OperandType cond;
  if (parser.parseOperand(cond) ||
      parser.resolveOperand(cond, builder.getType<WitnessType>(),
                            result.operands))
    return failure();

  // Parse optional results type list.
  if (parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Parse the region and add a terminator if elided.
  if (parser.parseRegion(*doRegion, /*arguments=*/{}, /*argTypes=*/{}))
    return failure();
  AssumingOp::ensureTerminator(*doRegion, parser.getBuilder(), result.location);

  // Parse the optional attribute list.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  return success();
}

static void print(OpAsmPrinter &p, AssumingOp op) {
  bool yieldsResults = !op.results().empty();

  p << AssumingOp::getOperationName() << " " << op.witness();
  if (yieldsResults) {
    p << " -> (" << op.getResultTypes() << ")";
  }
  p.printRegion(op.doRegion(),
                /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/yieldsResults);
  p.printOptionalAttrDict(op.getAttrs());
}

namespace {
// Removes AssumingOp with a passing witness and inlines the region.
struct AssumingWithTrue : public OpRewritePattern<AssumingOp> {
  using OpRewritePattern<AssumingOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(AssumingOp op,
                                PatternRewriter &rewriter) const override {
    auto witness = op.witness().getDefiningOp<ConstWitnessOp>();
    if (!witness || !witness.passingAttr())
      return failure();

    AssumingOp::inlineRegionIntoParent(op, rewriter);
    return success();
  }
};
} // namespace

void AssumingOp::getCanonicalizationPatterns(OwningRewritePatternList &patterns,
                                             MLIRContext *context) {
  // If taking a passing witness, inline region.
  patterns.insert<AssumingWithTrue>(context);
}

// See RegionBranchOpInterface in Interfaces/ControlFlowInterfaces.td
void AssumingOp::getSuccessorRegions(
    Optional<unsigned> index, ArrayRef<Attribute> operands,
    SmallVectorImpl<RegionSuccessor> &regions) {
  // AssumingOp has unconditional control flow into the region and back to the
  // parent, so return the correct RegionSuccessor purely based on the index
  // being None or 0.
  if (index.hasValue()) {
    regions.push_back(RegionSuccessor(getResults()));
    return;
  }

  regions.push_back(RegionSuccessor(&doRegion()));
}

void AssumingOp::inlineRegionIntoParent(AssumingOp &op,
                                        PatternRewriter &rewriter) {
  auto *blockBeforeAssuming = rewriter.getInsertionBlock();
  auto *assumingBlock = op.getBody();
  auto initPosition = rewriter.getInsertionPoint();
  auto *blockAfterAssuming =
      rewriter.splitBlock(blockBeforeAssuming, initPosition);

  // Remove the AssumingOp and AssumingYieldOp.
  auto &yieldOp = assumingBlock->back();
  rewriter.inlineRegionBefore(op.doRegion(), blockAfterAssuming);
  rewriter.replaceOp(op, yieldOp.getOperands());
  rewriter.eraseOp(&yieldOp);

  // Merge blocks together as there was no branching behavior from the
  // AssumingOp.
  rewriter.mergeBlocks(assumingBlock, blockBeforeAssuming);
  rewriter.mergeBlocks(blockAfterAssuming, blockBeforeAssuming);
}

//===----------------------------------------------------------------------===//
// AssumingAllOp
//===----------------------------------------------------------------------===//

void AssumingAllOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<AssumingAllOneOp>(context);
}

OpFoldResult AssumingAllOp::fold(ArrayRef<Attribute> operands) {
  // Iterate in reverse to first handle all constant operands. They are
  // guaranteed to be the tail of the inputs because this is commutative.
  for (int idx = operands.size() - 1; idx >= 0; idx--) {
    Attribute a = operands[idx];
    // Cannot fold if any inputs are not constant;
    if (!a)
      return nullptr;

    // We do not need to keep statically known values after handling them in
    // this method.
    getOperation()->eraseOperand(idx);

    // Always false if any input is statically known false
    if (!a.cast<BoolAttr>().getValue())
      return a;
  }
  // If this is reached, all inputs were statically known passing.
  return BoolAttr::get(true, getContext());
}

static LogicalResult verify(AssumingAllOp op) {
  // Ensure that AssumingAllOp contains at least one operand
  if (op.getNumOperands() == 0)
    return op.emitOpError("no operands specified");

  return success();
}

//===----------------------------------------------------------------------===//
// BroadcastOp
//===----------------------------------------------------------------------===//

OpFoldResult BroadcastOp::fold(ArrayRef<Attribute> operands) {
  if (!operands[1])
    return nullptr;

  auto rhsShape = llvm::to_vector<6>(
      operands[1].cast<DenseIntElementsAttr>().getValues<int64_t>());
  if (rhsShape.empty())
    return lhs();

  if (!operands[0])
    return nullptr;

  auto lhsShape = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  if (lhsShape.empty())
    return rhs();

  SmallVector<int64_t, 6> resultShape;
  // If the shapes are not compatible, we can't fold it.
  // TODO: Fold to an "error".
  if (!OpTrait::util::getBroadcastedShape(lhsShape, rhsShape, resultShape))
    return nullptr;
  Builder builder(getContext());
  return builder.getIndexTensorAttr(resultShape);
}

//===----------------------------------------------------------------------===//
// ConcatOp
//===----------------------------------------------------------------------===//

OpFoldResult ConcatOp::fold(ArrayRef<Attribute> operands) {
  if (!operands[0] || !operands[1])
    return nullptr;
  auto lhsShape = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto rhsShape = llvm::to_vector<6>(
      operands[1].cast<DenseIntElementsAttr>().getValues<int64_t>());
  SmallVector<int64_t, 6> resultShape;
  resultShape.append(lhsShape.begin(), lhsShape.end());
  resultShape.append(rhsShape.begin(), rhsShape.end());
  Builder builder(getContext());
  return builder.getIndexTensorAttr(resultShape);
}

//===----------------------------------------------------------------------===//
// ConstShapeOp
//===----------------------------------------------------------------------===//

static void print(OpAsmPrinter &p, ConstShapeOp &op) {
  p << "shape.const_shape ";
  p.printOptionalAttrDict(op.getAttrs(), /*elidedAttrs=*/{"shape"});
  p << "[";
  interleaveComma(op.shape().getValues<int64_t>(), p,
                  [&](int64_t i) { p << i; });
  p << "] : ";
  p.printType(op.getType());
}

static ParseResult parseConstShapeOp(OpAsmParser &parser,
                                     OperationState &result) {
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();
  // We piggy-back on ArrayAttr parsing, though we don't internally store the
  // shape as an ArrayAttr.
  // TODO: Implement custom parser and maybe make syntax a bit more concise.
  Attribute extentsRaw;
  NamedAttrList dummy;
  if (parser.parseAttribute(extentsRaw, "dummy", dummy))
    return failure();
  auto extentsArray = extentsRaw.dyn_cast<ArrayAttr>();
  if (!extentsArray)
    return failure();
  SmallVector<int64_t, 6> ints;
  for (Attribute extent : extentsArray) {
    IntegerAttr attr = extent.dyn_cast<IntegerAttr>();
    if (!attr)
      return failure();
    ints.push_back(attr.getInt());
  }
  Builder &builder = parser.getBuilder();
  result.addAttribute("shape", builder.getIndexTensorAttr(ints));
  Type resultTy;
  if (parser.parseColonType(resultTy))
    return failure();
  result.types.push_back(resultTy);
  return success();
}

OpFoldResult ConstShapeOp::fold(ArrayRef<Attribute>) { return shapeAttr(); }

void ConstShapeOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<TensorCastConstShape>(context);
}

//===----------------------------------------------------------------------===//
// CstrBroadcastableOp
//===----------------------------------------------------------------------===//

namespace {
// Given an input shape Value, try to obtain the shape's values.
LogicalResult getShapeVec(Value input, SmallVectorImpl<int64_t> &shapeValues) {
  if (auto inputOp = input.getDefiningOp<ShapeOfOp>()) {
    auto type = inputOp.arg().getType().dyn_cast<ShapedType>();
    if (!type.hasRank())
      return failure();
    shapeValues = llvm::to_vector<6>(type.getShape());
    return success();
  } else if (auto inputOp = input.getDefiningOp<ConstShapeOp>()) {
    shapeValues = llvm::to_vector<6>(inputOp.shape().getValues<int64_t>());
    return success();
  } else {
    return failure();
  }
}
} // namespace

void CstrBroadcastableOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  // Canonicalization patterns have overlap with the considerations during
  // folding in case additional shape information is inferred at some point that
  // does not result in folding.
  patterns.insert<CstrBroadcastableEqOps>(context);
}

OpFoldResult CstrBroadcastableOp::fold(ArrayRef<Attribute> operands) {
  // Both operands are not needed if one is a scalar.
  if (operands[0] &&
      operands[0].cast<DenseIntElementsAttr>().getNumElements() == 0)
    return BoolAttr::get(true, getContext());
  if (operands[1] &&
      operands[1].cast<DenseIntElementsAttr>().getNumElements() == 0)
    return BoolAttr::get(true, getContext());

  if (operands[0] && operands[1]) {
    auto lhsShape = llvm::to_vector<6>(
        operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
    auto rhsShape = llvm::to_vector<6>(
        operands[1].cast<DenseIntElementsAttr>().getValues<int64_t>());
    SmallVector<int64_t, 6> resultShape;
    if (OpTrait::util::staticallyKnownBroadcastable(lhsShape, rhsShape))
      return BoolAttr::get(true, getContext());
  }

  // Lastly, see if folding can be completed based on what constraints are known
  // on the input shapes.
  SmallVector<int64_t, 6> lhsShape, rhsShape;
  if (failed(getShapeVec(lhs(), lhsShape)))
    return nullptr;
  if (failed(getShapeVec(rhs(), rhsShape)))
    return nullptr;

  if (OpTrait::util::staticallyKnownBroadcastable(lhsShape, rhsShape))
    return BoolAttr::get(true, getContext());

  // Because a failing witness result here represents an eventual assertion
  // failure, we do not replace it with a constant witness.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// CstrEqOp
//===----------------------------------------------------------------------===//

void CstrEqOp::getCanonicalizationPatterns(OwningRewritePatternList &patterns,
                                           MLIRContext *context) {
  // If inputs are equal, return passing witness
  patterns.insert<CstrEqEqOps>(context);
}

OpFoldResult CstrEqOp::fold(ArrayRef<Attribute> operands) {
  if (llvm::all_of(operands,
                   [&](Attribute a) { return a && a == operands[0]; }))
    return BoolAttr::get(true, getContext());

  // Because a failing witness result here represents an eventual assertion
  // failure, we do not try to replace it with a constant witness. Similarly, we
  // cannot if there are any non-const inputs.
  return nullptr;
}

//===----------------------------------------------------------------------===//
// ConstSizeOp
//===----------------------------------------------------------------------===//

void ConstSizeOp::build(OpBuilder &builder, OperationState &result,
                        int64_t value) {
  build(builder, result, builder.getIndexAttr(value));
}

OpFoldResult ConstSizeOp::fold(ArrayRef<Attribute>) { return valueAttr(); }

void ConstSizeOp::getAsmResultNames(
    llvm::function_ref<void(Value, StringRef)> setNameFn) {
  SmallString<4> buffer;
  llvm::raw_svector_ostream os(buffer);
  os << "c" << value();
  setNameFn(getResult(), os.str());
}

//===----------------------------------------------------------------------===//
// ConstWitnessOp
//===----------------------------------------------------------------------===//

OpFoldResult ConstWitnessOp::fold(ArrayRef<Attribute>) { return passingAttr(); }

//===----------------------------------------------------------------------===//
// CstrRequireOp
//===----------------------------------------------------------------------===//

OpFoldResult CstrRequireOp::fold(ArrayRef<Attribute> operands) {
  return operands[0];
}

//===----------------------------------------------------------------------===//
// ShapeEqOp
//===----------------------------------------------------------------------===//

OpFoldResult ShapeEqOp::fold(ArrayRef<Attribute> operands) {
  auto lhs = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (lhs == nullptr)
    return {};
  auto rhs = operands[1].dyn_cast_or_null<DenseIntElementsAttr>();
  if (rhs == nullptr)
    return {};
  return BoolAttr::get(lhs == rhs, getContext());
}

//===----------------------------------------------------------------------===//
// IndexToSizeOp
//===----------------------------------------------------------------------===//

OpFoldResult IndexToSizeOp::fold(ArrayRef<Attribute> operands) {
  // Constant values of both types, `shape.size` and `index`, are represented as
  // `IntegerAttr`s which makes constant folding simple.
  if (Attribute arg = operands[0])
    return arg;
  return {};
}

void IndexToSizeOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<SizeToIndexToSizeCanonicalization>(context);
}

//===----------------------------------------------------------------------===//
// FromExtentsOp
//===----------------------------------------------------------------------===//

OpFoldResult FromExtentsOp::fold(ArrayRef<Attribute> operands) {
  if (llvm::any_of(operands, [](Attribute a) { return !a; }))
    return nullptr;
  SmallVector<int64_t, 6> extents;
  for (auto attr : operands)
    extents.push_back(attr.cast<IntegerAttr>().getInt());
  Builder builder(getContext());
  return builder.getIndexTensorAttr(extents);
}

//===----------------------------------------------------------------------===//
// FunctionLibraryOp
//===----------------------------------------------------------------------===//

void FunctionLibraryOp::build(OpBuilder &builder, OperationState &result,
                              StringRef name) {
  ensureTerminator(*result.addRegion(), builder, result.location);
  result.attributes.push_back(builder.getNamedAttr(
      ::mlir::SymbolTable::getSymbolAttrName(), builder.getStringAttr(name)));
}

FuncOp FunctionLibraryOp::getShapeFunction(Operation *op) {
  auto attr = mapping()
                  .get(op->getName().getIdentifier())
                  .dyn_cast_or_null<FlatSymbolRefAttr>();
  if (!attr)
    return nullptr;
  return lookupSymbol<FuncOp>(attr);
}

ParseResult parseFunctionLibraryOp(OpAsmParser &parser,
                                   OperationState &result) {
  // Parse the op name.
  StringAttr nameAttr;
  if (parser.parseSymbolName(nameAttr, ::mlir::SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();

  auto *bodyRegion = result.addRegion();
  if (parser.parseRegion(*bodyRegion))
    return failure();

  FunctionLibraryOp::ensureTerminator(*bodyRegion, parser.getBuilder(),
                                      result.location);
  if (parser.parseKeyword("mapping"))
    return failure();

  DictionaryAttr mappingAttr;
  if (parser.parseAttribute(mappingAttr,
                            parser.getBuilder().getType<NoneType>(), "mapping",
                            result.attributes))
    return failure();
  return success();
}

void print(OpAsmPrinter &p, FunctionLibraryOp op) {
  p << op.getOperationName() << ' ';
  p.printSymbolName(op.getName());
  p.printOptionalAttrDictWithKeyword(
      op.getAttrs(), {SymbolTable::getSymbolAttrName(), "mapping"});
  p.printRegion(op.getOperation()->getRegion(0), /*printEntryBlockArgs=*/false,
                /*printBlockTerminators=*/false);
  p << " mapping ";
  p.printAttributeWithoutType(op.mappingAttr());
}

//===----------------------------------------------------------------------===//
// GetExtentOp
//===----------------------------------------------------------------------===//

Optional<int64_t> GetExtentOp::getConstantDim() {
  if (auto constSizeOp = dim().getDefiningOp<ConstSizeOp>())
    return constSizeOp.value().getLimitedValue();
  if (auto constantOp = dim().getDefiningOp<ConstantOp>())
    return constantOp.value().cast<IntegerAttr>().getInt();
  return llvm::None;
}

OpFoldResult GetExtentOp::fold(ArrayRef<Attribute> operands) {
  auto elements = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!elements)
    return nullptr;
  Optional<int64_t> dim = getConstantDim();
  if (!dim.hasValue())
    return nullptr;
  if (dim.getValue() >= elements.getNumElements())
    return nullptr;
  return elements.getValue({(uint64_t)dim.getValue()});
}

void GetExtentOp::build(OpBuilder &builder, OperationState &result, Value shape,
                        int64_t dim) {
  auto loc = result.location;
  auto dimAttr = builder.getIndexAttr(dim);
  if (shape.getType().isa<ShapeType>()) {
    Value dim = builder.create<ConstSizeOp>(loc, dimAttr);
    build(builder, result, builder.getType<SizeType>(), shape, dim);
  } else {
    Value dim =
        builder.create<ConstantOp>(loc, builder.getIndexType(), dimAttr);
    build(builder, result, builder.getIndexType(), shape, dim);
  }
}

//===----------------------------------------------------------------------===//
// RankOp
//===----------------------------------------------------------------------===//

OpFoldResult shape::RankOp::fold(ArrayRef<Attribute> operands) {
  auto shape = operands[0].dyn_cast_or_null<DenseIntElementsAttr>();
  if (!shape)
    return {};
  int64_t rank = shape.getNumElements();
  Builder builder(getContext());
  return builder.getIndexAttr(rank);
}

/// Evaluate the `rank` operation for shapes of ranked tensors at compile time.
/// Constant folding fails in cases where only the rank is constant, not the
/// shape itself.
/// This canonicalization matches `shape.rank(shape.shape_of(%ranked_tensor))`.
///
/// Example:
///
/// %shape = shape.shape_of %ranked_tensor : tensor<1x2x?xf32>
/// %rank = shape.rank %shape
///
/// becomes
///
/// %rank = shape.const_size 3

namespace {
struct RankShapeOfCanonicalizationPattern
    : public OpRewritePattern<shape::RankOp> {
  using OpRewritePattern<shape::RankOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(shape::RankOp op,
                                PatternRewriter &rewriter) const override {
    auto shapeOfOp = op.shape().getDefiningOp<ShapeOfOp>();
    if (!shapeOfOp)
      return failure();
    auto rankedTensorType =
        shapeOfOp.arg().getType().dyn_cast<RankedTensorType>();
    if (!rankedTensorType)
      return failure();
    int64_t rank = rankedTensorType.getRank();
    if (op.getType().isa<IndexType>()) {
      rewriter.replaceOpWithNewOp<ConstantIndexOp>(op.getOperation(), rank);
    } else if (op.getType().isa<shape::SizeType>()) {
      rewriter.replaceOpWithNewOp<shape::ConstSizeOp>(op.getOperation(), rank);
    } else {
      return failure();
    }
    return success();
  }
};
} // namespace

void shape::RankOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<RankShapeOfCanonicalizationPattern>(context);
}

//===----------------------------------------------------------------------===//
// NumElementsOp
//===----------------------------------------------------------------------===//

OpFoldResult NumElementsOp::fold(ArrayRef<Attribute> operands) {

  // Fold only when argument constant.
  Attribute shape = operands[0];
  if (!shape)
    return {};

  APInt product(64, 1);
  for (auto value : shape.cast<DenseIntElementsAttr>())
    product *= value;
  Builder builder(getContext());
  return builder.getIndexAttr(product.getLimitedValue());
}

void NumElementsOp::build(OpBuilder &builder, OperationState &result,
                          Value shape) {
  if (shape.getType().isa<ShapedType>()) {
    auto type = builder.getIndexType();
    return build(builder, result, type, shape);
  }
  auto type = SizeType::get(builder.getContext());
  return build(builder, result, type, shape);
}

//===----------------------------------------------------------------------===//
// MulOp
//===----------------------------------------------------------------------===//

OpFoldResult MulOp::fold(ArrayRef<Attribute> operands) {
  auto lhs = operands[0].dyn_cast_or_null<IntegerAttr>();
  if (!lhs)
    return nullptr;
  auto rhs = operands[1].dyn_cast_or_null<IntegerAttr>();
  if (!rhs)
    return nullptr;
  APInt folded = lhs.getValue() * rhs.getValue();
  Type indexTy = IndexType::get(getContext());
  return IntegerAttr::get(indexTy, folded);
}

//===----------------------------------------------------------------------===//
// ShapeOfOp
//===----------------------------------------------------------------------===//

OpFoldResult ShapeOfOp::fold(ArrayRef<Attribute>) {
  auto type = getOperand().getType().dyn_cast<ShapedType>();
  if (!type || !type.hasStaticShape())
    return nullptr;
  Builder builder(getContext());
  return builder.getIndexTensorAttr(type.getShape());
}

void ShapeOfOp::build(OpBuilder &builder, OperationState &result, Value arg) {
  Type type = arg.getType().isa<ShapedType>()
                  ? (Type)getExtentTensorType(builder.getContext())
                  : (Type)builder.getType<ShapeType>();
  return ShapeOfOp::build(builder, result, type, arg);
}

namespace {
struct ShapeOfWithTensor : public OpRewritePattern<shape::ShapeOfOp> {
  using OpRewritePattern<shape::ShapeOfOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(shape::ShapeOfOp op,
                                PatternRewriter &rewriter) const override {
    if (!op.arg().getType().isa<ShapedType>())
      return failure();
    if (op.getType().isa<ShapedType>())
      return failure();

    rewriter.replaceOpWithNewOp<shape::ShapeOfOp>(op.getOperation(), op.arg());
    return success();
  }
};
} // namespace

void ShapeOfOp::getCanonicalizationPatterns(OwningRewritePatternList &patterns,
                                            MLIRContext *context) {
  patterns.insert<ShapeOfWithTensor>(context);
}

//===----------------------------------------------------------------------===//
// SizeToIndexOp
//===----------------------------------------------------------------------===//

OpFoldResult SizeToIndexOp::fold(ArrayRef<Attribute> operands) {
  // Constant values of both types, `shape.size` and `index`, are represented as
  // `IntegerAttr`s which makes constant folding simple.
  if (Attribute arg = operands[0])
    return arg;
  return impl::foldCastOp(*this);
}

void SizeToIndexOp::getCanonicalizationPatterns(
    OwningRewritePatternList &patterns, MLIRContext *context) {
  patterns.insert<IndexToSizeToIndexCanonicalization>(context);
}

//===----------------------------------------------------------------------===//
// YieldOp
//===----------------------------------------------------------------------===//

static LogicalResult verify(shape::YieldOp op) {
  auto *parentOp = op->getParentOp();
  auto results = parentOp->getResults();
  auto operands = op.getOperands();

  if (parentOp->getNumResults() != op.getNumOperands())
    return op.emitOpError() << "number of operands does not match number of "
                               "results of its parent";
  for (auto e : llvm::zip(results, operands))
    if (std::get<0>(e).getType() != std::get<1>(e).getType())
      return op.emitOpError()
             << "types mismatch between yield op and its parent";

  return success();
}

//===----------------------------------------------------------------------===//
// SplitAtOp
//===----------------------------------------------------------------------===//

LogicalResult SplitAtOp::fold(ArrayRef<Attribute> operands,
                              SmallVectorImpl<OpFoldResult> &results) {
  if (!operands[0] || !operands[1])
    return failure();
  auto shapeVec = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto shape = llvm::makeArrayRef(shapeVec);
  auto splitPoint = operands[1].cast<IntegerAttr>().getInt();
  // Verify that the split point is in the correct range.
  // TODO: Constant fold to an "error".
  int64_t rank = shape.size();
  if (!(-rank <= splitPoint && splitPoint <= rank))
    return failure();
  if (splitPoint < 0)
    splitPoint += shape.size();
  Builder builder(operands[0].getContext());
  results.push_back(builder.getIndexTensorAttr(shape.take_front(splitPoint)));
  results.push_back(builder.getIndexTensorAttr(shape.drop_front(splitPoint)));
  return success();
}

//===----------------------------------------------------------------------===//
// ToExtentTensorOp
//===----------------------------------------------------------------------===//

OpFoldResult ToExtentTensorOp::fold(ArrayRef<Attribute> operands) {
  if (!operands[0])
    return impl::foldCastOp(*this);
  Builder builder(getContext());
  auto shape = llvm::to_vector<6>(
      operands[0].cast<DenseIntElementsAttr>().getValues<int64_t>());
  auto type = RankedTensorType::get({static_cast<int64_t>(shape.size())},
                                    builder.getIndexType());
  return DenseIntElementsAttr::get(type, shape);
}

//===----------------------------------------------------------------------===//
// ReduceOp
//===----------------------------------------------------------------------===//

void ReduceOp::build(OpBuilder &builder, OperationState &result, Value shape,
                     ValueRange initVals) {
  result.addOperands(shape);
  result.addOperands(initVals);

  Region *bodyRegion = result.addRegion();
  bodyRegion->push_back(new Block);
  Block &bodyBlock = bodyRegion->front();
  bodyBlock.addArgument(builder.getIndexType());

  Type elementType;
  if (auto tensorType = shape.getType().dyn_cast<TensorType>())
    elementType = tensorType.getElementType();
  else
    elementType = SizeType::get(builder.getContext());
  bodyBlock.addArgument(elementType);

  for (Type initValType : initVals.getTypes()) {
    bodyBlock.addArgument(initValType);
    result.addTypes(initValType);
  }
}

static LogicalResult verify(ReduceOp op) {
  // Verify block arg types.
  Block &block = op.region().front();

  // The block takes index, extent, and aggregated values as arguments.
  auto blockArgsCount = op.initVals().size() + 2;
  if (block.getNumArguments() != blockArgsCount)
    return op.emitOpError() << "ReduceOp body is expected to have "
                            << blockArgsCount << " arguments";

  // The first block argument is the index and must always be of type `index`.
  if (!block.getArgument(0).getType().isa<IndexType>())
    return op.emitOpError(
        "argument 0 of ReduceOp body is expected to be of IndexType");

  // The second block argument is the extent and must be of type `size` or
  // `index`, depending on whether the reduce operation is applied to a shape or
  // to an extent tensor.
  Type extentTy = block.getArgument(1).getType();
  if (op.shape().getType().isa<ShapeType>()) {
    if (!extentTy.isa<SizeType>())
      return op.emitOpError("argument 1 of ReduceOp body is expected to be of "
                            "SizeType if the ReduceOp operates on a ShapeType");
  } else {
    if (!extentTy.isa<IndexType>())
      return op.emitOpError(
          "argument 1 of ReduceOp body is expected to be of IndexType if the "
          "ReduceOp operates on an extent tensor");
  }

  for (auto type : llvm::enumerate(op.initVals()))
    if (block.getArgument(type.index() + 2).getType() != type.value().getType())
      return op.emitOpError()
             << "type mismatch between argument " << type.index() + 2
             << " of ReduceOp body and initial value " << type.index();
  return success();
}

static ParseResult parseReduceOp(OpAsmParser &parser, OperationState &result) {
  // Parse operands.
  SmallVector<OpAsmParser::OperandType, 3> operands;
  Type shapeOrExtentTensorType;
  if (parser.parseOperandList(operands, /*requiredOperandCount=*/-1,
                              OpAsmParser::Delimiter::Paren) ||
      parser.parseColonType(shapeOrExtentTensorType) ||
      parser.parseOptionalArrowTypeList(result.types))
    return failure();

  // Resolve operands.
  auto initVals = llvm::makeArrayRef(operands).drop_front();
  if (parser.resolveOperand(operands.front(), shapeOrExtentTensorType,
                            result.operands) ||
      parser.resolveOperands(initVals, result.types, parser.getNameLoc(),
                             result.operands))
    return failure();

  // Parse the body.
  Region *body = result.addRegion();
  if (parser.parseRegion(*body, /*args=*/{}, /*argTypes=*/{}))
    return failure();

  // Parse attributes.
  if (parser.parseOptionalAttrDict(result.attributes))
    return failure();

  return success();
}

static void print(OpAsmPrinter &p, ReduceOp op) {
  p << op.getOperationName() << '(' << op.shape() << ", " << op.initVals()
    << ") : " << op.shape().getType();
  p.printOptionalArrowTypeList(op.getResultTypes());
  p.printRegion(op.region());
  p.printOptionalAttrDict(op.getAttrs());
}

#define GET_OP_CLASSES
#include "mlir/Dialect/Shape/IR/ShapeOps.cpp.inc"
