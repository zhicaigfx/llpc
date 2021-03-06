/*
 ***********************************************************************************************************************
 *
 *  Copyright (c) 2019 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in all
 *  copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *  SOFTWARE.
 *
 **********************************************************************************************************************/
/**
 ***********************************************************************************************************************
 * @file  llpcBuilderReplayer.cpp
 * @brief LLPC source file: BuilderReplayer pass
 ***********************************************************************************************************************
 */
#include "llpcBuilderRecorder.h"
#include "llpcInternal.h"

#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "llpc-builder-replayer"

using namespace Llpc;
using namespace llvm;

namespace
{

// =====================================================================================================================
// Pass to replay Builder calls recorded by BuilderRecorder
class BuilderReplayer final : public ModulePass
{
public:
    BuilderReplayer() : ModulePass(ID) {}
    BuilderReplayer(Builder* pBuilder) :
        ModulePass(ID),
        m_pBuilder(pBuilder)
    {
        initializeBuilderReplayerPass(*PassRegistry::getPassRegistry());
    }

    bool runOnModule(Module& module) override;

    // -----------------------------------------------------------------------------------------------------------------

    static char ID;

private:
    LLPC_DISALLOW_COPY_AND_ASSIGN(BuilderReplayer);

    void ReplayCall(uint32_t opcode, CallInst* pCall);
    void CheckCallAndReplay(Value* pValue);
    Value* ProcessCall(uint32_t opcode, CallInst* pCall);

    // The LLPC builder that the builder calls are being replayed on.
    std::unique_ptr<Builder> m_pBuilder;
};

} // anonymous

char BuilderReplayer::ID = 0;

// =====================================================================================================================
// Create BuilderReplayer pass
ModulePass* Llpc::CreateBuilderReplayer(
    Builder* pBuilder)    // [in] Builder to replay Builder calls on. The BuilderReplayer takes ownership of this.
{
    return new BuilderReplayer(pBuilder);
}

// =====================================================================================================================
// Run the BuilderReplayer pass on a module
bool BuilderReplayer::runOnModule(
    Module& module)   // [in] Module to run this pass on
{
    LLVM_DEBUG(dbgs() << "Running the pass of replaying LLPC builder calls\n");

    bool changed = false;

    SmallVector<Function*, 8> funcsToRemove;

    for (auto& func : module)
    {
        // Skip non-declarations that are definitely not LLPC intrinsics.
        if (func.isDeclaration() == false)
        {
            continue;
        }

        const MDNode* const pFuncMeta = func.getMetadata(BuilderCallMetadataName);

        // Skip builder calls that do not have the correct metadata to identify the opcode.
        if (pFuncMeta == nullptr)
        {
            // If the function had the llpc builder call prefix, it means the metadata was not encoded correctly.
            LLPC_ASSERT(func.getName().startswith(BuilderCallPrefix) == false);
            continue;
        }

        const ConstantAsMetadata* const pMetaConst = cast<ConstantAsMetadata>(pFuncMeta->getOperand(0));
        uint32_t opcode = cast<ConstantInt>(pMetaConst->getValue())->getZExtValue();

        // If we got here we are definitely changing the module.
        changed = true;

        SmallVector<CallInst*, 8> callsToRemove;

        while (func.use_empty() == false)
        {
            CallInst* const pCall = dyn_cast<CallInst>(func.use_begin()->getUser());

            // Replay the call into BuilderImpl.
            ReplayCall(opcode, pCall);
        }

        funcsToRemove.push_back(&func);
    }

    for (Function* const pFunc : funcsToRemove)
    {
        pFunc->eraseFromParent();
    }

    return changed;
}

// =====================================================================================================================
// Replay a recorded builder call.
void BuilderReplayer::ReplayCall(
    uint32_t  opcode,   // The builder call opcode
    CallInst* pCall)    // [in] The builder call to process
{
    // Set the insert point on the Builder. Also sets debug location to that of pCall.
    m_pBuilder->SetInsertPoint(pCall);

    LLVM_DEBUG(dbgs() << "Replaying " << *pCall << "\n");
    Value* pNewValue = ProcessCall(opcode, pCall);

    // Replace uses of the call with the new value, take the name, remove the old call.
    if (pNewValue != nullptr)
    {
        LLVM_DEBUG(dbgs() << "  replacing with: " << *pNewValue << "\n");
        pCall->replaceAllUsesWith(pNewValue);
        if (auto pNewInst = dyn_cast<Instruction>(pNewValue))
        {
            pNewInst->takeName(pCall);
        }
    }
    pCall->eraseFromParent();
}

// =====================================================================================================================
// If the passed value is a recorded builder call, replay it now.
// This is used in the waterfall loop workaround for not knowing the replay order.
void BuilderReplayer::CheckCallAndReplay(
    Value* pValue)    // [in] Value that might be a recorded call
{
    if (auto pCall = dyn_cast<CallInst>(pValue))
    {
        if (auto pFunc = pCall->getCalledFunction())
        {
            if (pFunc->getName().startswith(BuilderCallPrefix))
            {
                uint32_t opcode = cast<ConstantInt>(cast<ConstantAsMetadata>(
                                      pFunc->getMetadata(BuilderCallMetadataName)->getOperand(0))
                                    ->getValue())->getZExtValue();

                ReplayCall(opcode, pCall);
            }
        }
    }
}

// =====================================================================================================================
// Process one recorder builder call.
// Returns the replacement value, or nullptr in the case that we do not want the caller to replace uses of
// pCall with the new value.
Value* BuilderReplayer::ProcessCall(
    uint32_t  opcode,   // The builder call opcode
    CallInst* pCall)    // [in] The builder call to process
{
    // Get the args.
    auto args = ArrayRef<Use>(&pCall->getOperandList()[0], pCall->getNumArgOperands());

    switch (opcode)
    {
    case BuilderRecorder::Opcode::Nop:
    default:
        {
            LLPC_NEVER_CALLED();
            return nullptr;
        }

    // Replayer implementations of BuilderImplDesc methods
    case BuilderRecorder::Opcode::DescWaterfallLoop:
    case BuilderRecorder::Opcode::DescWaterfallStoreLoop:
        {
            SmallVector<uint32_t, 2> operandIdxs;
            for (Value* pOperand : args)
            {
                if (auto pConstOperand = dyn_cast<ConstantInt>(pOperand))
                {
                    operandIdxs.push_back(pConstOperand->getZExtValue());
                }
            }

            Instruction* pNonUniformInst = nullptr;
            if (opcode == BuilderRecorder::Opcode::DescWaterfallLoop)
            {
                pNonUniformInst = cast<Instruction>(args[0]);
            }
            else
            {
                // This is the special case that we want to waterfall a store op with no result.
                // The llpc.call.waterfall.store.loop intercepts (one of) the non-uniform descriptor
                // input(s) to the store. Use that interception to find the store, and remove the
                // interception.
                Use& useInNonUniformInst = *pCall->use_begin();
                pNonUniformInst = cast<Instruction>(useInNonUniformInst.getUser());
                useInNonUniformInst = args[0];
            }

            // BuilderImpl::CreateWaterfallLoop looks back at each descriptor input to the op to find
            // the non-uniform index. It does not know about BuilderRecorder/BuilderReplayer, so here
            // we must work around the unknown order of replaying by finding any recorded descriptor
            // load and replay it first.
            for (uint32_t operandIdx : operandIdxs)
            {
                Value* pInput = cast<Instruction>(args[0])->getOperand(operandIdx);
                while (auto pGep = dyn_cast<GetElementPtrInst>(pInput))
                {
                    pInput = pGep->getOperand(0);
                }
                CheckCallAndReplay(pInput);
            }

            // Create the waterfall loop.
            auto pWaterfallLoop = m_pBuilder->CreateWaterfallLoop(pNonUniformInst, operandIdxs);

            if (opcode == BuilderRecorder::Opcode::DescWaterfallLoop)
            {
                return pWaterfallLoop;
            }

            // For the store op case, avoid using the replaceAllUsesWith in the caller.
            pWaterfallLoop->takeName(pCall);
            return nullptr;
        }

    case BuilderRecorder::Opcode::DescLoadBuffer:
        {
            return m_pBuilder->CreateLoadBufferDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue(),  // isNonUniform
                  isa<PointerType>(pCall->getType()) ?
                      pCall->getType()->getPointerElementType() :
                      nullptr);                                // pPointeeTy
        }

    case BuilderRecorder::Opcode::DescLoadSampler:
        {
            return m_pBuilder->CreateLoadSamplerDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::DescLoadResource:
        {
            return m_pBuilder->CreateLoadResourceDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::DescLoadTexelBuffer:
        {
            return m_pBuilder->CreateLoadTexelBufferDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::DescLoadFmask:
        {
            return m_pBuilder->CreateLoadFmaskDesc(
                  cast<ConstantInt>(args[0])->getZExtValue(),  // descSet
                  cast<ConstantInt>(args[1])->getZExtValue(),  // binding
                  args[2],                                     // pDescIndex
                  cast<ConstantInt>(args[3])->getZExtValue()); // isNonUniform
        }

    case BuilderRecorder::Opcode::DescLoadSpillTablePtr:
        {
            return m_pBuilder->CreateLoadSpillTablePtr(
                  pCall->getType()->getPointerElementType());  // pSpillTableTy
        }

    // Replayer implementations of BuilderImplMisc methods
    case BuilderRecorder::Opcode::MiscKill:
        {
            return m_pBuilder->CreateKill();
        }
    case BuilderRecorder::Opcode::MiscReadClock:
        {
            bool realtime = (cast<ConstantInt>(args[0])->getZExtValue() != 0);
            return m_pBuilder->CreateReadClock(realtime);
        }
    }
}

// =====================================================================================================================
// Initializes the pass
INITIALIZE_PASS(BuilderReplayer, DEBUG_TYPE, "Replay LLPC builder calls", false, false)
