//===- WebAssemblyInstrControl.td-WebAssembly control-flow ------*- tablegen -*-
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief WebAssembly control-flow code-gen constructs.
///
//===----------------------------------------------------------------------===//

let Defs = [ARGUMENTS] in {

let isBranch = 1, isTerminator = 1, hasCtrlDep = 1 in {
// The condition operand is a boolean value which WebAssembly represents as i32.
def BR_IF : I<(outs), (ins I32:$cond, bb_op:$dst),
              [(brcond I32:$cond, bb:$dst)],
               "br_if   \t$cond, $dst">;
let isCodeGenOnly = 1 in
def BR_UNLESS : I<(outs), (ins I32:$cond, bb_op:$dst), [],
                   "br_unless\t$cond, $dst">;
let isBarrier = 1 in {
def BR   : I<(outs), (ins bb_op:$dst),
             [(br bb:$dst)],
             "br      \t$dst">;
} // isBarrier = 1
} // isBranch = 1, isTerminator = 1, hasCtrlDep = 1

} // Defs = [ARGUMENTS]

def : Pat<(brcond (i32 (setne I32:$cond, 0)), bb:$dst),
          (BR_IF I32:$cond, bb_op:$dst)>;
def : Pat<(brcond (i32 (seteq I32:$cond, 0)), bb:$dst),
          (BR_UNLESS I32:$cond, bb_op:$dst)>;

let Defs = [ARGUMENTS] in {

// TODO: SelectionDAG's lowering insists on using a pointer as the index for
// jump tables, so in practice we don't ever use TABLESWITCH_I64 in wasm32 mode
// currently.
let isTerminator = 1, hasCtrlDep = 1, isBarrier = 1 in {
def TABLESWITCH_I32 : I<(outs), (ins I32:$index, bb_op:$default, variable_ops),
                        [(WebAssemblytableswitch I32:$index, bb:$default)],
                        "tableswitch\t$index, $default">;
def TABLESWITCH_I64 : I<(outs), (ins I64:$index, bb_op:$default, variable_ops),
                        [(WebAssemblytableswitch I64:$index, bb:$default)],
                        "tableswitch\t$index, $default">;
} // isTerminator = 1, hasCtrlDep = 1, isBarrier = 1

// Placemarkers to indicate the start of a block or loop scope.
def BLOCK     : I<(outs), (ins bb_op:$dst), [], "block   \t$dst">;
def LOOP      : I<(outs), (ins bb_op:$dst), [], "loop    \t$dst">;

// No-op to indicate to the AsmPrinter that a loop ends here, so a
// basic block label is needed even if it wouldn't otherwise appear so.
let isTerminator = 1, hasCtrlDep = 1 in
def LOOP_END : I<(outs), (ins), []>;

multiclass RETURN<WebAssemblyRegClass vt> {
  def RETURN_#vt : I<(outs), (ins vt:$val), [(WebAssemblyreturn vt:$val)],
                     "return  \t$val">;
}

let isTerminator = 1, hasCtrlDep = 1, isBarrier = 1 in {
let isReturn = 1 in {
  defm : RETURN<I32>;
  defm : RETURN<I64>;
  defm : RETURN<F32>;
  defm : RETURN<F64>;
  def RETURN_VOID : I<(outs), (ins), [(WebAssemblyreturn)], "return">;
} // isReturn = 1
  def UNREACHABLE : I<(outs), (ins), [(trap)], "unreachable">;
} // isTerminator = 1, hasCtrlDep = 1, isBarrier = 1

} // Defs = [ARGUMENTS]
