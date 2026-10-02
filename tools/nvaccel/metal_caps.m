#import <objc/runtime.h>
#import <Metal/Metal.h>
int main(){ @autoreleasepool {
 id<MTLDevice> d=MTLCreateSystemDefaultDevice();
 struct {MTLGPUFamily f; const char*n;} F[]={{MTLGPUFamilyApple1,"Apple1"},{MTLGPUFamilyApple7,"Apple7"},{MTLGPUFamilyApple9,"Apple9"},{MTLGPUFamilyMac2,"Mac2"},{MTLGPUFamilyCommon1,"Common1"},{MTLGPUFamilyCommon2,"Common2"},{MTLGPUFamilyCommon3,"Common3"},{MTLGPUFamilyMetal3,"Metal3"},{(MTLGPUFamily)5002,"Metal4"}};
 for(int i=0;i<9;i++) printf("%s=%d ",F[i].n,[d supportsFamily:F[i].f]); printf("\n");
 printf("raytracing=%d fnptr=%d argbuf=%lu rwtex=%lu bcTex=%d 32bitMSAA=%d maxBufLen=%lu recommendedWS=%llu hasUnified=%d loc=%lu\n",
  d.supportsRaytracing,d.supportsFunctionPointers,(unsigned long)d.argumentBuffersSupport,(unsigned long)d.readWriteTextureSupport,
  d.supportsBCTextureCompression,d.supports32BitMSAA,(unsigned long)d.maxBufferLength,d.recommendedMaxWorkingSetSize,d.hasUnifiedMemory,(unsigned long)d.location);
 MTLIndirectCommandBufferDescriptor *icd=[MTLIndirectCommandBufferDescriptor new]; icd.commandTypes=MTLIndirectCommandTypeConcurrentDispatch; icd.maxKernelBufferBindCount=4;
 printf("icb=%p\n",[d newIndirectCommandBufferWithDescriptor:icd maxCommandCount:4 options:0]);
 MTLArgumentDescriptor *ad=[MTLArgumentDescriptor argumentDescriptor]; ad.dataType=MTLDataTypePointer;
 id ae=[d newArgumentEncoderWithArguments:@[ad]]; printf("argenc=%s encodedLength=%lu\n", ae?class_getName([ae class]):"nil", ae?(unsigned long)[ae encodedLength]:0);
}}
