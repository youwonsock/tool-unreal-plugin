#include "UnrealToolsServiceInstance.h"

std::uint64_t AllocateServiceGenerationInOtherTranslationUnit()
{
    return UnrealToolsRuntime::NextServiceInstanceGeneration();
}
