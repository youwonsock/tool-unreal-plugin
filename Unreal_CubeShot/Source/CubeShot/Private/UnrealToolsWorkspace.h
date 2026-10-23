#pragma once

// This state machine is also compiled by Scripts/test_workspace.py without UE.
#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace UnrealToolsWorkspace
{
template <class Key, class Options, class Policy> class TModel
{
public:
    struct FToken
    {
        std::optional<Key> Scope;
        std::uint64_t Generation = 0;
    };
    struct FRow
    {
        Key Path;
        Options Value;
        Key Profile;
        bool Selected = false, Custom = false, Valid = true;
        std::uint64_t Generation = 0;
    };
    using FSnapshot = std::vector<std::pair<Key, Options>>;

    TModel(const Options& Defaults, const Key& Profile)
        : Default{Key{}, Policy::Clone(Defaults), Profile, false, false, true, Next()}
    {
    }
    const std::vector<FRow>& Rows() const
    {
        return Items;
    }
    const std::optional<Key>& Editing() const
    {
        return Active;
    }
    std::uint64_t Revision() const
    {
        return Version;
    }
    FToken Token() const
    {
        return TokenFor(Active);
    }
    FToken TokenFor(const std::optional<Key>& Scope) const
    {
        const auto* Row = Find(Scope);
        return {Scope, Row ? Row->Generation : 0};
    }
    bool IsCurrent(const FToken& Token) const
    {
        const auto* Row = Find(Token.Scope);
        return Row && Row->Generation == Token.Generation;
    }
    const FRow* Find(const std::optional<Key>& Scope) const
    {
        if (!Scope)
            return &Default;
        const auto It = std::find_if(Items.begin(), Items.end(), [&](const auto& Row) { return Row.Path == *Scope; });
        return It == Items.end() ? nullptr : &*It;
    }
    Options CopyOptions(const std::optional<Key>& Scope) const
    {
        const auto* Row = Find(Scope);
        return Row ? Policy::Clone(Row->Value) : Options{};
    }
    bool Add(const Key& Path)
    {
        if (Find(Path))
            return true; // A duplicate never changes selection/options/order.
        if (!Default.Valid)
            return false;
        Items.push_back({Path, Policy::Clone(Default.Value), Default.Profile, false, false, true, Next()});
        ++Version;
        return true;
    }
    bool Edit(const std::optional<Key>& Scope)
    {
        if (!Find(Scope))
            return false;
        if (Active == Scope)
            return true;
        Mutable(Active)->Generation = Next();
        Active = Scope;
        Mutable(Active)->Generation = Next();
        ++Version;
        return true;
    }
    bool Replace(const FToken& Token, const Options& Value, const Key& Profile, bool Custom, bool Invalidate = false)
    {
        if (!IsCurrent(Token))
            return false;
        auto* Row = Mutable(Token.Scope);
        Row->Value = Policy::Clone(Value);
        Row->Profile = Profile;
        Row->Custom = Custom;
        Row->Valid = true;
        if (Invalidate)
            Row->Generation = Next();
        ++Version;
        return true;
    }
    bool Validate(const FToken& Token, bool Valid)
    {
        if (!IsCurrent(Token))
            return false;
        auto* Row = Mutable(Token.Scope);
        if (Row->Valid != Valid)
        {
            Row->Valid = Valid;
            ++Version;
        }
        return true;
    }
    void ProfileIdentity(const std::optional<Key>& Scope, const Key& Profile)
    {
        if (auto* Row = Mutable(Scope); Row && !(Row->Profile == Profile))
        {
            Row->Profile = Profile;
            ++Version;
        }
    }
    void Select(const Key& Path, bool Selected)
    {
        if (auto* Row = Mutable(Path); Row && Row->Selected != Selected)
        {
            Row->Selected = Selected;
            ++Version;
        }
    }
    void SelectAll(bool Selected)
    {
        for (auto& Row : Items)
            Select(Row.Path, Selected);
    }
    bool ApplySelected(const FToken& Source)
    {
        if (std::none_of(Items.begin(), Items.end(), [](const auto& Row) { return Row.Selected; }))
            return true;
        if (!IsCurrent(Source) || !Find(Source.Scope)->Valid)
            return false;
        const auto Value = CopyOptions(Source.Scope);
        const auto Profile = Find(Source.Scope)->Profile;
        for (auto& Row : Items)
            if (Row.Selected)
            {
                Row.Value = Policy::Clone(Value);
                Row.Profile = Profile;
                Row.Valid = true;
                Row.Custom = true;
                Row.Generation = Next();
                ++Version;
            }
        return true;
    }
    bool Reset(const Key& Path)
    {
        if (!Default.Valid || !Find(Path))
            return false;
        return Replace(TokenFor(Path), Default.Value, Default.Profile, false, true);
    }
    void Move(const Key& Path, int Delta)
    {
        for (std::size_t I = 0; I < Items.size(); ++I)
            if (Items[I].Path == Path)
            {
                const auto To = static_cast<long long>(I) + Delta;
                if (To >= 0 && To < static_cast<long long>(Items.size()))
                {
                    std::swap(Items[I], Items[static_cast<std::size_t>(To)]);
                    ++Version;
                }
                return;
            }
    }
    void Remove(const Key& Path)
    {
        const auto It = std::find_if(Items.begin(), Items.end(), [&](const auto& Row) { return Row.Path == Path; });
        if (It == Items.end())
            return;
        const auto Index = static_cast<std::size_t>(It - Items.begin());
        const bool WasActive = Active && *Active == Path;
        Items.erase(It);
        ++Version;
        if (WasActive)
        {
            Active = Items.empty() ? std::optional<Key>{}
                                   : std::optional<Key>{Items[std::min(Index, Items.size() - 1)].Path};
            Mutable(Active)->Generation = Next();
        }
    }
    void RemoveSelected()
    {
        std::vector<Key> Paths;
        for (const auto& Row : Items)
            if (Row.Selected)
                Paths.push_back(Row.Path);
        for (const auto& Path : Paths)
            Remove(Path);
    }
    void Clear()
    {
        Items.clear();
        Active.reset();
        Default.Generation = Next();
        ++Version;
    }
    bool Snapshot(FSnapshot& Out) const
    {
        Out.clear();
        if (Items.empty())
            return false;
        for (const auto& Row : Items)
            if (!Row.Valid)
                return false;
        for (const auto& Row : Items)
            Out.emplace_back(Row.Path, Policy::Clone(Row.Value));
        return true;
    }

private:
    std::uint64_t Next()
    {
        return ++Generation;
    }
    FRow* Mutable(const std::optional<Key>& Scope)
    {
        return const_cast<FRow*>(Find(Scope));
    }
    std::uint64_t Generation = 0, Version = 0;
    FRow Default;
    std::vector<FRow> Items;
    std::optional<Key> Active;
};
} // namespace UnrealToolsWorkspace
