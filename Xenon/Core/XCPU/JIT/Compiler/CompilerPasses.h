#pragma once

#include "Core/XCPU/JIT/Compiler/Passes/ConditionalGroupPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/ConditionalGroupSubpass.h"
#include "Core/XCPU/JIT/Compiler/Passes/ConstantPropagationPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/ContextPromotionPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/ControlFlowAnalysisPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/DataFlowAnalysisPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/DeadCodeEliminationPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/FinalizationPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/SimplificationPass.h"
#include "Core/XCPU/JIT/Compiler/Passes/ValueReductionPass.h"
