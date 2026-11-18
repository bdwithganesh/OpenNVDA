#!/usr/bin/env python3
"""Static contract check for the native Metal 4 integration.

Selector names come from the macOS 26 Metal.framework Objective-C metadata
retained at research/apple-m5-pro-20261103/metal-cache-objc.txt (the public
MTL4* protocols). The driver is not allowed to expose a feature-query bit or a
family that has no matching entry point, and every entry point an application
can reach through the public protocol must be present (implemented, forwarded
to the Metal 3 encoder, or an explicit documented refusal).

This is a source contract, not a Metal runtime test.
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
M4 = (ROOT / "drivers/NVMTLDriver/NVMTL4.m").read_text(encoding="utf-8")
M4H = (ROOT / "drivers/NVMTLDriver/NVMTL4.h").read_text(encoding="utf-8")
DEV = (ROOT / "drivers/NVMTLDriver/NVMTLDevice.m").read_text(encoding="utf-8")
GATE = (ROOT / "drivers/NVMTLDriver/NVMTLFamilyGate.h").read_text(encoding="utf-8")

DEVICE_SELECTORS = [
    "newCommandAllocator",
    "newCommandAllocatorWithDescriptor:error:",
    "newMTL4CommandQueue",
    "newMTL4CommandQueueWithDescriptor:error:",
    "newCommandBuffer",
    "newArgumentTableWithDescriptor:error:",
    "newCompilerWithDescriptor:error:",
    "supportsMTL4CommandQueue",
    "supportsMTL4CommandAllocator",
    "supportsMTL4ComputeCommandEncoder",
    "supportsMTL4RenderCommandEncoder",
    "supportsMTL4Compiler",
    "supportsMTL4Counters",
]

COMMAND_BUFFER_SELECTORS = [
    "beginCommandBufferWithAllocator:",
    "beginCommandBufferWithAllocator:options:",
    "computeCommandEncoder",
    "renderCommandEncoderWithDescriptor:",
    "renderCommandEncoderWithDescriptor:options:",
    "machineLearningCommandEncoder",
    "endCommandBuffer",
    "resolveCounterHeap:withRange:intoBuffer:waitFence:updateFence:",
]

QUEUE_SELECTORS = [
    "commit:count:",
    "commit:count:options:",
    "copyBufferMappingsFromBuffer:toBuffer:operations:count:",
    "updateBufferMappings:heap:operations:count:",
    "copyTextureMappingsFromTexture:toTexture:operations:count:",
    "updateTextureMappings:heap:operations:count:",
]

COMPILER_SELECTORS = [
    "newLibraryWithDescriptor:error:",
    "newLibraryWithDescriptor:completionHandler:",
    "newComputePipelineStateWithDescriptor:compilerTaskOptions:error:",
    "newComputePipelineStateWithDescriptor:compilerTaskOptions:completionHandler:",
    "newRenderPipelineStateWithDescriptor:compilerTaskOptions:error:",
    "newRenderPipelineStateWithDescriptor:compilerTaskOptions:completionHandler:",
    "newComputePipelineStateWithDescriptor:dynamicLinkingDescriptor:compilerTaskOptions:completionHandler:",
    "newRenderPipelineStateWithDescriptor:dynamicLinkingDescriptor:compilerTaskOptions:completionHandler:",
    "newRenderPipelineStateWithDescriptor:dynamicLinkingDescriptor:compilerTaskOptions:error:",
    "newRenderPipelineStateBySpecializationWithDescriptor:pipeline:completionHandler:",
    "newMachineLearningPipelineStateWithDescriptor:error:",
    "pipelineDataSetSerializer",
]

ENCODER_SELECTORS = [
    "commandBuffer",
    "setLabel:",
    "pushDebugGroup:",
    "popDebugGroup",
    "insertDebugSignpost:",
    "updateFence:afterEncoderStages:",
    "waitForFence:beforeEncoderStages:",
    "barrierAfterEncoderStages:beforeEncoderStages:visibilityOptions:",
    "barrierAfterQueueStages:beforeStages:visibilityOptions:",
    "barrierAfterStages:beforeQueueStages:visibilityOptions:",
    "setArgumentTable:",
    "setArgumentTable:atStages:",
    "dispatchThreadsWithIndirectBuffer:",
    "dispatchThreadgroupsWithIndirectBuffer:threadsPerThreadgroup:",
    "drawMeshThreadgroups:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
    "drawMeshThreads:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
    "drawMeshThreadgroupsWithIndirectBuffer:threadsPerObjectThreadgroup:threadsPerMeshThreadgroup:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferLength:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferLength:instanceCount:",
    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferLength:instanceCount:baseVertex:baseInstance:",
    "drawIndexedPrimitives:indexType:indexBuffer:indexBufferLength:indirectBuffer:",
    "drawPrimitives:indirectBuffer:",
    "executeCommandsInBuffer:indirectBuffer:",
    "presentDrawable:",
    "tileWidth",
    "tileHeight",
    "setColorStoreAction:atIndex:",
    "setDepthStoreAction:",
    "setStencilStoreAction:",
    "setViewports:count:",
    "setScissorRects:count:",
    "setAddress:attributeStride:atIndex:",
    "optimizeContentsForCPUAccess:",
    "optimizeContentsForCPUAccess:slice:level:",
    "optimizeContentsForGPUAccess:",
    "optimizeContentsForGPUAccess:slice:level:",
    "copyAccelerationStructure:toAccelerationStructure:",
    "copyAndCompactAccelerationStructure:toAccelerationStructure:",
    "buildAccelerationStructure:descriptor:scratchBuffer:",
    "refitAccelerationStructure:descriptor:destination:scratchBuffer:",
    "refitAccelerationStructure:descriptor:destination:scratchBuffer:options:",
    "writeCompactedAccelerationStructureSize:toBuffer:",
]

CONFORMANCES = [
    ("NVMTL4Allocator", "MTL4CommandAllocator"),
    ("NVMTL4ArgumentTable", "MTL4ArgumentTable"),
    ("NVMTL4CommandQueue", "MTL4CommandQueue"),
    ("NVMTL4CommandBuffer", "MTL4CommandBuffer"),
    ("NVMTL4Compiler", "MTL4Compiler"),
    ("NVMTL4CompilerTask", "MTL4CompilerTask"),
    ("NVMTL4CommitFeedback", "MTL4CommitFeedback"),
    ("NVMTL4ComputeEncoder", "MTL4ComputeCommandEncoder"),
    ("NVMTL4RenderEncoder", "MTL4RenderCommandEncoder"),
]


def has_selector(selector, text):
    """True when all colon-separated selector keywords occur in order.

    Objective-C writes a selector as `foo:(Type)a bar:(Type)b`; the literal
    selector `foo:bar:` never appears contiguously in source. Allow parameter
    declarations between the keywords.
    """
    parts = selector.split(":")[:-1] if selector.endswith(":") else [selector]
    if len(parts) == 1:
        return re.search(r"\b" + re.escape(parts[0]) + r"\b", text) is not None
    pattern = r"\b" + re.escape(parts[0]) + r":"
    for part in parts[1:]:
        pattern += r".*?" + re.escape(part) + r":"
    return re.search(pattern, text, re.S) is not None


def missing(selectors, *texts):
    haystack = "\n".join(texts)
    return [s for s in selectors if not has_selector(s, haystack)]


class MTL4ContractTest(unittest.TestCase):
    def test_device_entry_points(self):
        self.assertEqual([], missing(DEVICE_SELECTORS, M4),
                         "MTL4 device entry points missing")

    def test_command_buffer_and_queue(self):
        self.assertEqual([], missing(COMMAND_BUFFER_SELECTORS, M4H, M4),
                         "MTL4 command-buffer entry points missing")
        self.assertEqual([], missing(QUEUE_SELECTORS, M4H, M4),
                         "MTL4 queue entry points missing")

    def test_compiler_surface(self):
        self.assertEqual([], missing(COMPILER_SELECTORS, M4H, M4),
                         "MTL4 compiler entry points missing")

    def test_encoder_surface(self):
        self.assertEqual([], missing(ENCODER_SELECTORS, M4H, M4),
                         "MTL4 encoder entry points missing")

    def test_runtime_conformances(self):
        absent = []
        for cls, proto in CONFORMANCES:
            if f'"{cls}"' not in DEV:
                absent.append(cls)
            if f'"{proto}"' not in DEV:
                absent.append(proto)
        self.assertEqual([], absent, "runtime protocol conformance entries missing")

    def test_validation_and_barriers(self):
        # 30.01/30.02 and 06.02: MTL4 must not submit an obviously invalid
        # size/index range and must lower barriers to the native encoder.
        for token in ("nvM4SizeUsable", "nvM4IndexCountFits", "nvM4IndexRangeFits",
                      "memoryBarrierWithScope", "nvGeneration",
                      "nvM4BadDescriptorError", "cachedLibraryOptions",
                      "lastCommittedCommandBufferGeneration"):
            self.assertIn(token, M4, f"missing MTL4 validation/barrier token {token}")
        # The mesh descriptor's colorAttachments object is not an NSArray.
        self.assertNotIn("colors.count", M4)
        self.assertIn("objectAtIndexedSubscript:", M4)
        # Render-pass MSAA state must survive the MTL4 -> Metal 3 mapping.
        self.assertIn("defaultColorSampleCount", M4)

    def test_capability_record(self):
        self.assertIn("nvCapabilityRecord", DEV)
        self.assertIn("supportsMTL4CommandQueue", DEV)

    def test_gate_is_central(self):
        # supportsFamily: must consult the shared policy, not featureProfile.
        self.assertTrue(has_selector("nvMTLFamilyAllowed", DEV))
        self.assertTrue(has_selector("nvMTLGateFamilyInputs", DEV))
        self.assertIn("NVMTLFamilyMetal3", GATE)
        self.assertIn("NVMTLFamilyMetal4", GATE)
        # Apple*/Mac1 are rejected before the base class is consulted.
        self.assertIn("NVMTLFamilyApple1", DEV)
        self.assertIn("NVMTLFamilyMac1", DEV)
        # A per-process object-graph flag alone must not be the family gate.
        self.assertIn("metal4Family", GATE)
        # No direct getenv() bypass in the MTL4 device methods.
        self.assertNotIn('getenv("NVMTL_METAL4")', M4)

    def test_metal4_selector_typos_are_not_the_only_form(self):
        # The macOS 26 contract has the error: variants. The older one-argument
        # forms may stay as compatibility aliases, but must not replace them.
        self.assertTrue(has_selector("newMTL4CommandQueueWithDescriptor:error:", M4 + M4H))
        self.assertTrue(has_selector("newCommandAllocatorWithDescriptor:error:", M4 + M4H))


if __name__ == "__main__":
    unittest.main()
