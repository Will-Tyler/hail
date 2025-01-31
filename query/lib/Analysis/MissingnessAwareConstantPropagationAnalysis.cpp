#include "hail/Analysis/MissingnessAwareConstantPropagationAnalysis.h"
#include "hail/Analysis/MissingnessAnalysis.h"
#include "hail/Dialect/Missing/IR/Missing.h"
#include "hail/Support/MLIR.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/OpDefinition.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "missingness-aware-constant-propagation"

using namespace hail::ir;

using mlir::dataflow::ConstantValue;
using mlir::dataflow::Lattice;

void MissingnessAwareConstantPropagation::visitOperation(
    Operation *op, ArrayRef<Lattice<ConstantValue> const *> operands,
    ArrayRef<Lattice<ConstantValue> *> results) {
  LLVM_DEBUG(llvm::dbgs() << "MACP: Visiting operation: " << *op << "\n");

  auto builder = Builder(op->getContext());

  // FIXME: move missingness op semantics to an interface
  if (auto missingOp = dyn_cast<IsMissingOp>(op)) {
    auto const *missingness =
        getOrCreateFor<Lattice<MissingnessValue>>(missingOp, missingOp.getOperand());
    if (missingness->isUninitialized())
      return;
    if (missingness->getValue().isMissing()) {
      propagateIfChanged(results.front(), results.front()->join(builder.getBoolAttr(true)));
    } else if (missingness->getValue().isPresent()) {
      propagateIfChanged(results.front(), results.front()->join(builder.getBoolAttr(false)));
    } else {
      propagateIfChanged(results.front(), results.front()->join(ConstantValue()));
    }
    return;
  };

  // Don't try to simulate the results of a region operation as we can't
  // guarantee that folding will be out-of-place. We don't allow in-place
  // folds as the desire here is for simulated execution, and not general
  // folding.
  if (op->getNumRegions() != 0U)
    return;

  // By default, only propagate constants if there are no missing operands.
  bool const anyMissing =
      std::any_of(op->operand_begin(), op->operand_end(), [this, op](auto operand) {
        auto missingness = getOrCreateFor<Lattice<MissingnessValue>>(op, operand);
        return missingness->isUninitialized() || missingness->getValue().isMissing();
      });

  if (anyMissing)
    return;

  SmallVector<Attribute> constantOperands;
  constantOperands.reserve(op->getNumOperands());
  for (auto const *operandLattice : operands)
    constantOperands.push_back(operandLattice->getValue().getConstantValue());

  // Save the original operands and attributes just in case the operation
  // folds in-place. The constant passed in may not correspond to the real
  // runtime value, so in-place updates are not allowed.
  SmallVector<Value> const originalOperands(op->getOperands());
  DictionaryAttr const originalAttrs = op->getAttrDictionary();

  // Simulate the result of folding this operation to a constant. If folding
  // fails or was an in-place fold, mark the results as overdefined.
  SmallVector<OpFoldResult> foldResults;
  foldResults.reserve(op->getNumResults());
  if (failed(op->fold(constantOperands, foldResults))) {
    markAllPessimisticFixpoint(results);
    return;
  }

  // If the folding was in-place, mark the results as overdefined and reset
  // the operation. We don't allow in-place folds as the desire here is for
  // simulated execution, and not general folding.
  if (foldResults.empty()) {
    op->setOperands(originalOperands);
    op->setAttrs(originalAttrs);
    markAllPessimisticFixpoint(results);
    return;
  }

  // Merge the fold results into the lattice for this operation.
  assert(foldResults.size() == op->getNumResults() && "invalid result size");
  for (auto const it : llvm::zip(results, foldResults)) {
    Lattice<ConstantValue> *lattice = std::get<0>(it);

    // Merge in the result of the fold, either a constant or a value.
    OpFoldResult const foldResult = std::get<1>(it);
    if (auto const attr = foldResult.dyn_cast<Attribute>()) {
      LLVM_DEBUG(llvm::dbgs() << "Folded to constant: " << attr << "\n");
      propagateIfChanged(lattice, lattice->join(ConstantValue(attr, op->getDialect())));
    } else {
      LLVM_DEBUG(llvm::dbgs() << "Folded to value: " << foldResult.get<Value>() << "\n");
      AbstractSparseDataFlowAnalysis::join(lattice, *getLatticeElement(foldResult.get<Value>()));
    }
  }
}
