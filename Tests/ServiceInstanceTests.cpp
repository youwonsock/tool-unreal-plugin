#include "UnrealToolsServiceInstance.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

std::uint64_t AllocateServiceGenerationInOtherTranslationUnit();

struct ServiceProbe
{
    const std::uint64_t Generation = UnrealToolsRuntime::NextServiceInstanceGeneration();
    const std::uint64_t Revision = 1;
};
static int Checks = 0;
static void Check(bool Value, const char* Message)
{
    ++Checks;
    if (!Value)
        throw std::runtime_error(Message);
}

int main()
{
    try
    {
        alignas(ServiceProbe) unsigned char Storage[sizeof(ServiceProbe)];
        auto* First = new (Storage) ServiceProbe();
        const void* Address = First;
        const auto FirstGeneration = First->Generation;
        const auto FirstRevision = First->Revision;
        Check(FirstGeneration != 0, "zero remains the unobserved generation");
        First->~ServiceProbe();
        auto* Second = new (Storage) ServiceProbe();
        Check(static_cast<const void*>(Second) == Address, "placement construction reuses the exact address");
        Check(Second->Revision == FirstRevision, "recreated services may expose the same revision");
        Check(Second->Generation > FirstGeneration, "a recreated service gets a newer identity regardless of address");
        auto Previous = Second->Generation;
        Second->~ServiceProbe();
        bool Increasing = true;
        for (int Index = 0; Index < 128; ++Index)
        {
            auto* Service = new (Storage) ServiceProbe();
            Increasing &= Service->Generation > Previous;
            Previous = Service->Generation;
            Service->~ServiceProbe();
        }
        Check(Increasing, "repeated reconstruction never resets the generation");
        const auto OtherUnit = AllocateServiceGenerationInOtherTranslationUnit();
        Check(OtherUnit > Previous && UnrealToolsRuntime::NextServiceInstanceGeneration() > OtherUnit,
              "translation units in the same runtime share the generation allocator");

        std::array<std::vector<std::uint64_t>, 4> Values;
        std::vector<std::thread> Workers;
        for (std::size_t Index = 0; Index < Values.size(); ++Index)
            Workers.emplace_back([&, Index]() {
                for (int Count = 0; Count < 256; ++Count)
                    Values[Index].push_back(UnrealToolsRuntime::NextServiceInstanceGeneration());
            });
        for (auto& Worker : Workers)
            Worker.join();
        bool EachWorkerIncreases = true;
        std::vector<std::uint64_t> All;
        for (const auto& Sequence : Values)
        {
            EachWorkerIncreases &= std::is_sorted(Sequence.begin(), Sequence.end());
            All.insert(All.end(), Sequence.begin(), Sequence.end());
        }
        Check(EachWorkerIncreases, "each constructor sequence stays monotonic under concurrent allocation");
        std::sort(All.begin(), All.end());
        Check(All.size() == 1024 && std::adjacent_find(All.begin(), All.end()) == All.end(),
              "concurrent allocations have distinct generations");
        Check(UnrealToolsRuntime::NextServiceInstanceGeneration() > All.back(),
              "the counter continues after worker lifetimes end");
        std::cout << "Service instance checks: " << Checks << " passed\n";
    }
    catch (const std::exception& Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
