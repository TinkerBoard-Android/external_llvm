//===-- SPUISelDAGToDAG.cpp - CellSPU pattern matching inst selector ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines a pattern matching instruction selector for the Cell SPU,
// converting from a legalized dag to a SPU-target dag.
//
//===----------------------------------------------------------------------===//

#include "SPU.h"
#include "SPUTargetMachine.h"
#include "SPUISelLowering.h"
#include "SPUHazardRecognizers.h"
#include "SPUFrameInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Constants.h"
#include "llvm/GlobalValue.h"
#include "llvm/Intrinsics.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/Compiler.h"
#include <iostream>
#include <queue>
#include <set>

using namespace llvm;

namespace {
  //! ConstantSDNode predicate for i32 sign-extended, 10-bit immediates
  bool
  isI64IntS10Immediate(ConstantSDNode *CN)
  {
    return isS10Constant(CN->getValue());
  }

  //! ConstantSDNode predicate for i32 sign-extended, 10-bit immediates
  bool
  isI32IntS10Immediate(ConstantSDNode *CN)
  {
    return isS10Constant((int) CN->getValue());
  }

#if 0
  //! SDNode predicate for sign-extended, 10-bit immediate values
  bool
  isI32IntS10Immediate(SDNode *N)
  {
    return (N->getOpcode() == ISD::Constant
            && isI32IntS10Immediate(cast<ConstantSDNode>(N)));
  }
#endif

  //! ConstantSDNode predicate for i32 unsigned 10-bit immediate values
  bool
  isI32IntU10Immediate(ConstantSDNode *CN)
  {
    return isU10Constant((int) CN->getValue());
  }

  //! ConstantSDNode predicate for i16 sign-extended, 10-bit immediate values
  bool
  isI16IntS10Immediate(ConstantSDNode *CN)
  {
    return isS10Constant((short) CN->getValue());
  }

  //! SDNode predicate for i16 sign-extended, 10-bit immediate values
  bool
  isI16IntS10Immediate(SDNode *N)
  {
    return (N->getOpcode() == ISD::Constant
            && isI16IntS10Immediate(cast<ConstantSDNode>(N)));
  }

  //! ConstantSDNode predicate for i16 unsigned 10-bit immediate values
  bool
  isI16IntU10Immediate(ConstantSDNode *CN)
  {
    return isU10Constant((short) CN->getValue());
  }

  //! SDNode predicate for i16 sign-extended, 10-bit immediate values
  bool
  isI16IntU10Immediate(SDNode *N)
  {
    return (N->getOpcode() == ISD::Constant
            && isI16IntU10Immediate(cast<ConstantSDNode>(N)));
  }

  //! ConstantSDNode predicate for signed 16-bit values
  /*!
    \arg CN The constant SelectionDAG node holding the value
    \arg Imm The returned 16-bit value, if returning true

    This predicate tests the value in \a CN to see whether it can be
    represented as a 16-bit, sign-extended quantity. Returns true if
    this is the case.
   */
  bool
  isIntS16Immediate(ConstantSDNode *CN, short &Imm)
  {
    MVT::ValueType vt = CN->getValueType(0);
    Imm = (short) CN->getValue();
    if (vt >= MVT::i1 && vt <= MVT::i16) {
      return true;
    } else if (vt == MVT::i32) {
      int32_t i_val = (int32_t) CN->getValue();
      short s_val = (short) i_val;
      return i_val == s_val;
    } else {
      int64_t i_val = (int64_t) CN->getValue();
      short s_val = (short) i_val;
      return i_val == s_val;
    }

    return false;
  }

  //! SDNode predicate for signed 16-bit values.
  bool
  isIntS16Immediate(SDNode *N, short &Imm)
  {
    return (N->getOpcode() == ISD::Constant
            && isIntS16Immediate(cast<ConstantSDNode>(N), Imm));
  }

  //! ConstantFPSDNode predicate for representing floats as 16-bit sign ext.
  static bool
  isFPS16Immediate(ConstantFPSDNode *FPN, short &Imm)
  {
    MVT::ValueType vt = FPN->getValueType(0);
    if (vt == MVT::f32) {
      int val = FloatToBits(FPN->getValueAPF().convertToFloat());
      int sval = (int) ((val << 16) >> 16);
      Imm = (short) val;
      return val == sval;
    }

    return false;
  }

  //===------------------------------------------------------------------===//
  //! MVT::ValueType to "useful stuff" mapping structure:

  struct valtype_map_s {
    MVT::ValueType VT;
    unsigned ldresult_ins;	/// LDRESULT instruction (0 = undefined)
    int prefslot_byte;		/// Byte offset of the "preferred" slot
    unsigned brcc_eq_ins;	/// br_cc equal instruction
    unsigned brcc_neq_ins;	/// br_cc not equal instruction
    unsigned load_aform;        /// A-form load instruction for this VT
    unsigned store_aform;       /// A-form store instruction for this VT
  };

  const valtype_map_s valtype_map[] = {
    { MVT::i1,    0,            3, 0,         0,          0,
      0 },
    { MVT::i8,    SPU::ORBIr8,  3, 0,         0,          SPU::LQAr8,
      SPU::STQAr8 },
    { MVT::i16,   SPU::ORHIr16, 2, SPU::BRHZ, SPU::BRHNZ, SPU::LQAr16,
      SPU::STQAr16 },
    { MVT::i32,   SPU::ORIr32,  0, SPU::BRZ,  SPU::BRNZ,  SPU::LQAr32,
      SPU::STQAr32 },
    { MVT::i64,   SPU::ORIr64,  0, 0,         0,          SPU::LQAr64,
      SPU::STQAr64 },
    { MVT::f32,   0,            0, 0,         0,          SPU::LQAf32,
      SPU::STQAf32 },
    { MVT::f64,   0,            0, 0,         0,          SPU::LQAf64,
      SPU::STQAf64 },
    // vector types... (sigh!)
    { MVT::v16i8, 0,            0, 0,         0,          SPU::LQAv16i8,
      SPU::STQAv16i8 },
    { MVT::v8i16, 0,            0, 0,         0,          SPU::LQAv8i16,
      SPU::STQAv8i16 },
    { MVT::v4i32, 0,            0, 0,         0,          SPU::LQAv4i32,
      SPU::STQAv4i32 },
    { MVT::v2i64, 0,            0, 0,         0,          SPU::LQAv2i64,
      SPU::STQAv2i64 },
    { MVT::v4f32, 0,            0, 0,         0,          SPU::LQAv4f32,
      SPU::STQAv4f32 },
    { MVT::v2f64, 0,            0, 0,         0,          SPU::LQAv2f64,
      SPU::STQAv2f64 },
  };

  const size_t n_valtype_map = sizeof(valtype_map) / sizeof(valtype_map[0]);

  const valtype_map_s *getValueTypeMapEntry(MVT::ValueType VT)
  {
    const valtype_map_s *retval = 0;
    for (size_t i = 0; i < n_valtype_map; ++i) {
      if (valtype_map[i].VT == VT) {
	retval = valtype_map + i;
	break;
      }
    }


#ifndef NDEBUG
    if (retval == 0) {
      cerr << "SPUISelDAGToDAG.cpp: getValueTypeMapEntry returns NULL for "
	   << MVT::getValueTypeString(VT)
	   << "\n";
      abort();
    }
#endif

    return retval;
  }
}

//===--------------------------------------------------------------------===//
/// SPUDAGToDAGISel - Cell SPU-specific code to select SPU machine
/// instructions for SelectionDAG operations.
///
class SPUDAGToDAGISel :
  public SelectionDAGISel
{
  SPUTargetMachine &TM;
  SPUTargetLowering &SPUtli;
  unsigned GlobalBaseReg;

public:
  SPUDAGToDAGISel(SPUTargetMachine &tm) :
    SelectionDAGISel(*tm.getTargetLowering()),
    TM(tm),
    SPUtli(*tm.getTargetLowering())
  {}
    
  virtual bool runOnFunction(Function &Fn) {
    // Make sure we re-emit a set of the global base reg if necessary
    GlobalBaseReg = 0;
    SelectionDAGISel::runOnFunction(Fn);
    return true;
  }
   
  /// getI32Imm - Return a target constant with the specified value, of type
  /// i32.
  inline SDOperand getI32Imm(uint32_t Imm) {
    return CurDAG->getTargetConstant(Imm, MVT::i32);
  }

  /// getI64Imm - Return a target constant with the specified value, of type
  /// i64.
  inline SDOperand getI64Imm(uint64_t Imm) {
    return CurDAG->getTargetConstant(Imm, MVT::i64);
  }
    
  /// getSmallIPtrImm - Return a target constant of pointer type.
  inline SDOperand getSmallIPtrImm(unsigned Imm) {
    return CurDAG->getTargetConstant(Imm, SPUtli.getPointerTy());
  }

  /// Select - Convert the specified operand from a target-independent to a
  /// target-specific node if it hasn't already been changed.
  SDNode *Select(SDOperand Op);

  /// Return true if the address N is a RI7 format address [r+imm]
  bool SelectDForm2Addr(SDOperand Op, SDOperand N, SDOperand &Disp,
			SDOperand &Base);

  //! Returns true if the address N is an A-form (local store) address
  bool SelectAFormAddr(SDOperand Op, SDOperand N, SDOperand &Base,
		       SDOperand &Index);

  //! D-form address predicate
  bool SelectDFormAddr(SDOperand Op, SDOperand N, SDOperand &Base,
		       SDOperand &Index);

  //! Address predicate if N can be expressed as an indexed [r+r] operation.
  bool SelectXFormAddr(SDOperand Op, SDOperand N, SDOperand &Base,
		       SDOperand &Index);

  /// SelectInlineAsmMemoryOperand - Implement addressing mode selection for
  /// inline asm expressions.
  virtual bool SelectInlineAsmMemoryOperand(const SDOperand &Op,
					    char ConstraintCode,
					    std::vector<SDOperand> &OutOps,
					    SelectionDAG &DAG) {
    SDOperand Op0, Op1;
    switch (ConstraintCode) {
    default: return true;
    case 'm':   // memory
      if (!SelectDFormAddr(Op, Op, Op0, Op1) 
	  && !SelectAFormAddr(Op, Op, Op0, Op1))
	SelectXFormAddr(Op, Op, Op0, Op1);
      break;
    case 'o':   // offsetable
      if (!SelectDFormAddr(Op, Op, Op0, Op1)
	  && !SelectAFormAddr(Op, Op, Op0, Op1)) {
	Op0 = Op;
	AddToISelQueue(Op0);     // r+0.
	Op1 = getSmallIPtrImm(0);
      }
      break;
    case 'v':   // not offsetable
#if 1
      assert(0 && "InlineAsmMemoryOperand 'v' constraint not handled.");
#else
      SelectAddrIdxOnly(Op, Op, Op0, Op1);
#endif
      break;
    }
      
    OutOps.push_back(Op0);
    OutOps.push_back(Op1);
    return false;
  }

  /// InstructionSelectBasicBlock - This callback is invoked by
  /// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
  virtual void InstructionSelectBasicBlock(SelectionDAG &DAG);

  virtual const char *getPassName() const {
    return "Cell SPU DAG->DAG Pattern Instruction Selection";
  } 
    
  /// CreateTargetHazardRecognizer - Return the hazard recognizer to use for
  /// this target when scheduling the DAG.
  virtual HazardRecognizer *CreateTargetHazardRecognizer() {
    const TargetInstrInfo *II = SPUtli.getTargetMachine().getInstrInfo();
    assert(II && "No InstrInfo?");
    return new SPUHazardRecognizer(*II); 
  }

  // Include the pieces autogenerated from the target description.
#include "SPUGenDAGISel.inc"
};

/// InstructionSelectBasicBlock - This callback is invoked by
/// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
void
SPUDAGToDAGISel::InstructionSelectBasicBlock(SelectionDAG &DAG)
{
  DEBUG(BB->dump());

  // Select target instructions for the DAG.
  DAG.setRoot(SelectRoot(DAG.getRoot()));
  DAG.RemoveDeadNodes();
  
  // Emit machine code to BB.
  ScheduleAndEmitDAG(DAG);
}

bool 
SPUDAGToDAGISel::SelectDForm2Addr(SDOperand Op, SDOperand N, SDOperand &Disp,
				  SDOperand &Base) {
  unsigned Opc = N.getOpcode();
  unsigned VT = N.getValueType();
  MVT::ValueType PtrVT = SPUtli.getPointerTy();
  ConstantSDNode *CN = 0;
  int Imm;

  if (Opc == ISD::ADD) {
    SDOperand Op0 = N.getOperand(0);
    SDOperand Op1 = N.getOperand(1);
    if (Op1.getOpcode() == ISD::Constant ||
	Op1.getOpcode() == ISD::TargetConstant) {
      CN = cast<ConstantSDNode>(Op1);
      Imm = int(CN->getValue());
      if (Imm <= 0xff) {
	Disp = CurDAG->getTargetConstant(Imm, SPUtli.getPointerTy());
	Base = Op0;
	return true;
      }
    }
  } else if (Opc == ISD::GlobalAddress
	     || Opc == ISD::TargetGlobalAddress
	     || Opc == ISD::Register) {
    // Plain old local store address: 
    Disp = CurDAG->getTargetConstant(0, VT);
    Base = N;
    return true;
  } else if (Opc == SPUISD::DFormAddr) {
    // D-Form address: This is pretty straightforward, naturally...
    CN = cast<ConstantSDNode>(N.getOperand(1));
    assert(CN != 0 && "SelectDFormAddr/SPUISD::DForm2Addr expecting constant");
    Imm = unsigned(CN->getValue());
    if (Imm < 0xff) {
      Disp = CurDAG->getTargetConstant(CN->getValue(), PtrVT);
      Base = N.getOperand(0);
      return true;
    }
  }

  return false;
}

/*!
 \arg Op The ISD instructio operand
 \arg N The address to be tested
 \arg Base The base address
 \arg Index The base address index
 */
bool
SPUDAGToDAGISel::SelectAFormAddr(SDOperand Op, SDOperand N, SDOperand &Base,
		    SDOperand &Index) {
  // These match the addr256k operand type:
  MVT::ValueType OffsVT = MVT::i16;
  MVT::ValueType PtrVT = SPUtli.getPointerTy();

  switch (N.getOpcode()) {
  case ISD::Constant:
  case ISD::ConstantPool:
  case ISD::GlobalAddress:
    cerr << "SPU SelectAFormAddr: Constant/Pool/Global not lowered.\n";
    abort();
    /*NOTREACHED*/

  case ISD::TargetConstant: {
    // Loading from a constant address.
    ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
    int Imm = (int)CN->getValue();
    if (Imm < 0x3ffff && (Imm & 0x3) == 0) {
      Base = CurDAG->getTargetConstant(Imm, PtrVT);
      // Note that this operand will be ignored by the assembly printer...
      Index = CurDAG->getTargetConstant(0, OffsVT);
      return true;
    }
  }
  case ISD::TargetGlobalAddress:
  case ISD::TargetConstantPool:
  case SPUISD::AFormAddr: {
    // The address is in Base. N is a dummy that will be ignored by
    // the assembly printer.
    Base = N;
    Index = CurDAG->getTargetConstant(0, OffsVT);
    return true;
  }
  }

  return false;
}

/*!
  \arg Op The ISD instruction (ignored)
  \arg N The address to be tested
  \arg Base Base address register/pointer
  \arg Index Base address index

  Examine the input address by a base register plus a signed 10-bit
  displacement, [r+I10] (D-form address).

  \return true if \a N is a D-form address with \a Base and \a Index set
  to non-empty SDOperand instances.
*/
bool
SPUDAGToDAGISel::SelectDFormAddr(SDOperand Op, SDOperand N, SDOperand &Base,
				 SDOperand &Index) {
  unsigned Opc = N.getOpcode();
  unsigned PtrTy = SPUtli.getPointerTy();

  if (Opc == ISD::Register) {
    Base = N;
    Index = CurDAG->getTargetConstant(0, PtrTy);
    return true;
  } else if (Opc == ISD::FrameIndex) {
    FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N);
    DEBUG(cerr << "SelectDFormAddr: ISD::FrameIndex = "
	       << FI->getIndex() << "\n");
    if (FI->getIndex() < SPUFrameInfo::maxFrameOffset()) {
      Base = CurDAG->getTargetConstant(0, PtrTy);
      Index = CurDAG->getTargetFrameIndex(FI->getIndex(), PtrTy);
      return true;
    }
  } else if (Opc == ISD::ADD) {
    // Generated by getelementptr
    const SDOperand Op0 = N.getOperand(0); // Frame index/base
    const SDOperand Op1 = N.getOperand(1); // Offset within base

    if ((Op1.getOpcode() == ISD::Constant
	 || Op1.getOpcode() == ISD::TargetConstant)
	&& Op0.getOpcode() != SPUISD::XFormAddr) {
      ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Op1);
      assert(CN != 0 && "SelectDFormAddr: Expected a constant");

      int32_t offset = (int32_t) CN->getSignExtended();
      unsigned Opc0 = Op0.getOpcode();

      if (Opc0 == ISD::FrameIndex) {
        FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(Op0);
        DEBUG(cerr << "SelectDFormAddr: ISD::ADD offset = " << offset
              << " frame index = " << FI->getIndex() << "\n");

        if (FI->getIndex() < SPUFrameInfo::maxFrameOffset()) {
          Base = CurDAG->getTargetConstant(offset, PtrTy);
          Index = CurDAG->getTargetFrameIndex(FI->getIndex(), PtrTy);
          return true;
        }
      } else if (offset > SPUFrameInfo::minFrameOffset()
                 && offset < SPUFrameInfo::maxFrameOffset()) {
        Base = CurDAG->getTargetConstant(offset, PtrTy);
        if (Opc0 == ISD::GlobalAddress) {
          // Convert global address to target global address
          GlobalAddressSDNode *GV = dyn_cast<GlobalAddressSDNode>(Op0);
          Index = CurDAG->getTargetGlobalAddress(GV->getGlobal(), PtrTy);
          return true;
        } else {
          // Otherwise, just take operand 0
          Index = Op0;
          return true;
        }
      }
    } else
      return false;
  } else if (Opc == SPUISD::DFormAddr) {
    // D-Form address: This is pretty straightforward,
    // naturally... but make sure that this isn't a D-form address
    // with a X-form address embedded within:
    const SDOperand Op0 = N.getOperand(0); // Frame index/base
    const SDOperand Op1 = N.getOperand(1); // Offset within base

    if (Op0.getOpcode() == ISD::Constant
        || Op0.getOpcode() == ISD::TargetConstant) {
      ConstantSDNode *CN = cast<ConstantSDNode>(Op1);
      assert(CN != 0 && "SelectDFormAddr/SPUISD::DFormAddr expecting constant"); 
      Base = CurDAG->getTargetConstant(CN->getValue(), PtrTy);
      Index = Op0;
      return true;
    }
  } else if (Opc == ISD::FrameIndex) {
    // Stack frame index must be less than 512 (divided by 16):
    FrameIndexSDNode *FI = dyn_cast<FrameIndexSDNode>(N);
    DEBUG(cerr << "SelectDFormAddr: ISD::FrameIndex = "
	  << FI->getIndex() << "\n");
    if (FI->getIndex() < SPUFrameInfo::maxFrameOffset()) {
      Base = CurDAG->getTargetConstant(0, PtrTy);
      Index = CurDAG->getTargetFrameIndex(FI->getIndex(), PtrTy);
      return true;
    }
  } else if (Opc == SPUISD::LDRESULT) {
    // It's a load result dereference
    Base = CurDAG->getTargetConstant(0, PtrTy);
    Index = N.getOperand(0);
    return true;
  }

  return false;
}

/*!
  \arg Op The ISD instruction operand
  \arg N The address operand
  \arg Base The base pointer operand
  \arg Index The offset/index operand

  If the address \a N can be expressed as a [r + s10imm] address, returns false.
  Otherwise, creates two operands, Base and Index that will become the [r+r]
  address.
*/
bool
SPUDAGToDAGISel::SelectXFormAddr(SDOperand Op, SDOperand N, SDOperand &Base,
				 SDOperand &Index) {
  if (SelectAFormAddr(Op, N, Base, Index)
      || SelectDFormAddr(Op, N, Base, Index))
    return false;

  unsigned Opc = N.getOpcode();

  if (Opc == ISD::ADD) {
    SDOperand N1 = N.getOperand(0);
    SDOperand N2 = N.getOperand(1);
    Base = N.getOperand(0);
    Index = N.getOperand(1);
    return true;
  } else if (Opc == SPUISD::XFormAddr) {
    Base = N;
    Index = N.getOperand(1);
    return true;
  } else if (Opc == SPUISD::DFormAddr) {
    // Must be a D-form address with an X-form address embedded
    // within:
    Base = N.getOperand(0);
    Index = N.getOperand(1);
    return true;
  } else if (N.getNumOperands() == 2) {
    SDOperand N1 = N.getOperand(0);
    SDOperand N2 = N.getOperand(1);
    unsigned N1Opc = N1.getOpcode();
    unsigned N2Opc = N2.getOpcode();

    if ((N1Opc == ISD::CopyToReg || N1Opc == ISD::Register)
	&& (N2Opc == ISD::CopyToReg || N2Opc == ISD::Register)) {
      Base = N.getOperand(0);
      Index = N.getOperand(1);
      return true;
      /*UNREACHED*/
    } else {
      cerr << "SelectXFormAddr: 2-operand unhandled operand:\n";
      N.Val->dump(CurDAG);
      cerr << "\n";
      abort();
    /*UNREACHED*/
    }
  } else {
    cerr << "SelectXFormAddr: Unhandled operand type:\n";
    N.Val->dump(CurDAG);
    cerr << "\n";
    abort();
    /*UNREACHED*/
  }

  return false;
}

//! Emit load for A-form addresses
/*
 */
SDNode *
Emit_LOAD_AFormAddr(SDOperand Op, SelectionDAG &CurDAG, SPUDAGToDAGISel &ISel)
{
  SDNode *Result;
  MVT::ValueType OpVT = Op.getValueType();
  SDOperand Chain = Op.getOperand(0);
  SDOperand Ptr = Op.getOperand(1);
  SDOperand PtrArg = Ptr.getOperand(0);
  SDOperand PtrOffs = Ptr.getOperand(1);
  const valtype_map_s *vtm = getValueTypeMapEntry(OpVT);

  if (PtrOffs.getOpcode() == ISD::Constant) {
    ConstantSDNode *CN = cast<ConstantSDNode>(PtrOffs);
    MVT::ValueType PVT = PtrOffs.getValueType();
    PtrOffs = CurDAG.getTargetConstant(CN->getValue(), PVT);
  }
  ISel.AddToISelQueue(PtrArg);
  ISel.AddToISelQueue(PtrOffs);
  ISel.AddToISelQueue(Chain);
  Result = CurDAG.getTargetNode(vtm->load_aform, OpVT, MVT::Other, PtrArg, PtrOffs, Chain);
  Chain = SDOperand(Result, 1);
  return Result;
}

//! Emit store for A-form addresses
/*
 */
SDNode *
Emit_STORE_AFormAddr(SDOperand Op, SelectionDAG &CurDAG, SPUDAGToDAGISel &ISel)
{
  SDNode *Result;
  SDOperand Chain = Op.getOperand(0);
  SDOperand Val = Op.getOperand(1);
  SDOperand Ptr = Op.getOperand(2);
  SDOperand PtrArg = Ptr.getOperand(0);
  SDOperand PtrOffs = Ptr.getOperand(1);
  const valtype_map_s *vtm = getValueTypeMapEntry(Val.getValueType());

  if (PtrOffs.getOpcode() == ISD::Constant) {
    ConstantSDNode *CN = cast<ConstantSDNode>(PtrOffs);
    MVT::ValueType PVT = PtrOffs.getValueType();
    PtrOffs = CurDAG.getTargetConstant(CN->getValue(), PVT);
  }
  ISel.AddToISelQueue(Val);
  ISel.AddToISelQueue(PtrArg);
  ISel.AddToISelQueue(PtrOffs);
  ISel.AddToISelQueue(Chain);
  SDOperand Ops[4] = { Val, PtrArg, PtrOffs, Chain };
  Result = CurDAG.getTargetNode(vtm->store_aform, MVT::Other, Ops, 4);
  Chain = SDOperand(Result, 1);
  return Result;
}

//! Convert the operand from a target-independent to a target-specific node
/*!
 */
SDNode *
SPUDAGToDAGISel::Select(SDOperand Op) {
  SDNode *N = Op.Val;
  unsigned Opc = N->getOpcode();
  int n_ops = -1;
  unsigned NewOpc;
  MVT::ValueType OpVT = Op.getValueType();
  SDOperand Ops[8];

  if (Opc >= ISD::BUILTIN_OP_END && Opc < SPUISD::FIRST_NUMBER) {
    return NULL;   // Already selected.
  } else if (Opc == ISD::FrameIndex) {
    // Selects to AIr32 FI, 0 which in turn will become AIr32 SP, imm.
    int FI = cast<FrameIndexSDNode>(N)->getIndex();
    MVT::ValueType PtrVT = SPUtli.getPointerTy();
    SDOperand Zero = CurDAG->getTargetConstant(0, PtrVT);
    SDOperand TFI = CurDAG->getTargetFrameIndex(FI, PtrVT);

    DEBUG(cerr << "SPUDAGToDAGISel: Replacing FrameIndex with AI32 <FI>, 0\n");
    NewOpc = SPU::AIr32;
    Ops[0] = TFI;
    Ops[1] = Zero;
    n_ops = 2;
  } else if (Opc == ISD::LOAD
             && Op.getOperand(1).getOpcode() == SPUISD::AFormAddr) {
    return Emit_LOAD_AFormAddr(Op, *CurDAG, *this);
  } else if (Opc == ISD::STORE
             && Op.getOperand(2).getOpcode() == SPUISD::AFormAddr) {
    return Emit_STORE_AFormAddr(Op, *CurDAG, *this);
  } else if (Opc == ISD::ZERO_EXTEND) {
    // (zero_extend:i16 (and:i8 <arg>, <const>))
    const SDOperand &Op1 = N->getOperand(0);

    if (Op.getValueType() == MVT::i16 && Op1.getValueType() == MVT::i8) {
      if (Op1.getOpcode() == ISD::AND) {
        // Fold this into a single ANDHI. This is often seen in expansions of i1
        // to i8, then i8 to i16 in logical/branching operations.
        DEBUG(cerr << "CellSPU: Coalescing (zero_extend:i16 (and:i8 "
                      "<arg>, <const>))\n");
        NewOpc = SPU::ANDHI1To2;
        Ops[0] = Op1.getOperand(0);
        Ops[1] = Op1.getOperand(1);
        n_ops = 2;
      }
    }
  } else if (Opc == SPUISD::LDRESULT) {
    // Custom select instructions for LDRESULT
    unsigned VT = N->getValueType(0);
    SDOperand Arg = N->getOperand(0);
    SDOperand Chain = N->getOperand(1);
    SDNode *Result;

    AddToISelQueue(Arg);
    if (!MVT::isFloatingPoint(VT)) {
      SDOperand Zero = CurDAG->getTargetConstant(0, VT);
      const valtype_map_s *vtm = getValueTypeMapEntry(VT);

      if (vtm->ldresult_ins == 0) {
	cerr << "LDRESULT for unsupported type: "
	     << MVT::getValueTypeString(VT)
	     << "\n";
	abort();
      } else
	Opc = vtm->ldresult_ins;

      AddToISelQueue(Zero);
      Result = CurDAG->getTargetNode(Opc, VT, MVT::Other, Arg, Zero, Chain);
    } else {
      Opc = (VT == MVT::f32 ? SPU::ORf32 : SPU::ORf64);
      Result = CurDAG->getTargetNode(Opc, MVT::Other, Arg, Arg, Chain);
    }

    Chain = SDOperand(Result, 1);
    AddToISelQueue(Chain);

    return Result;
  } else if (Opc == SPUISD::XFormAddr) {
    SDOperand Op0 = Op.getOperand(0);
    if (Op0.getOpcode() == SPUISD::LDRESULT
        || Op0.getOpcode() == SPUISD::AFormAddr) {
      // (XFormAddr (LDRESULT|AFormAddr, imm))
      SDOperand Op1 = Op.getOperand(1);
      MVT::ValueType VT = Op.getValueType();

      DEBUG(cerr << "CellSPU: XFormAddr("
            << (Op0.getOpcode() == SPUISD::LDRESULT
                ? "LDRESULT"
                : "AFormAddr")
            << ", imm):\nOp0 = ");
      DEBUG(Op.getOperand(0).Val->dump(CurDAG));
      DEBUG(cerr << "\nOp1 = ");
      DEBUG(Op.getOperand(1).Val->dump(CurDAG));
      DEBUG(cerr << "\n");

      if (Op1.getOpcode() == ISD::Constant) {
        ConstantSDNode *CN = cast<ConstantSDNode>(Op1);
        Op1 = CurDAG->getTargetConstant(CN->getValue(), VT);
      }
      AddToISelQueue(Op0);
      AddToISelQueue(Op1);
      NewOpc = SPU::AIr32;
      Ops[0] = Op0;
      Ops[1] = Op1;
      n_ops = 2;
    }
  }
  
  if (n_ops > 0) {
    if (N->hasOneUse())
      return CurDAG->SelectNodeTo(N, NewOpc, OpVT, Ops, n_ops);
    else
      return CurDAG->getTargetNode(NewOpc, OpVT, Ops, n_ops);
  } else
    return SelectCode(Op);
}

/// createPPCISelDag - This pass converts a legalized DAG into a 
/// SPU-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createSPUISelDag(SPUTargetMachine &TM) {
  return new SPUDAGToDAGISel(TM);
}
