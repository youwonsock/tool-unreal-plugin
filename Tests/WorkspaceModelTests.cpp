#include "UnrealToolsWorkspace.h"
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

struct Payload
{
    std::map<std::string, int> Values;
    std::shared_ptr<std::vector<int>> Nested = std::make_shared<std::vector<int>>(3, 7);
    int& operator[](const std::string& Key)
    {
        return Values[Key];
    }
    int at(const std::string& Key) const
    {
        return Values.at(Key);
    }
};
using Options = std::shared_ptr<Payload>;
struct Copy
{
    static inline int Copies = 0;
    static Options Clone(const Options& Value)
    {
        ++Copies;
        auto Result = std::make_shared<Payload>(*Value);
        Result->Nested = std::make_shared<std::vector<int>>(*Value->Nested);
        return Result;
    }
};
using Model = UnrealToolsWorkspace::TModel<std::string, Options, Copy>;
static int Checks = 0;
static void Check(bool Value, const char* Message)
{
    ++Checks;
    if (!Value)
        throw std::runtime_error(Message);
}
static int Resolution(const Model& State, const std::optional<std::string>& Key)
{
    return State.Find(Key)->Value->at("resolution");
}
int main()
{
    try
    {
        auto Initial = std::make_shared<Payload>();
        Initial->Values = {{"resolution", 1024}};
        auto Session = std::make_shared<Model>(Initial, "default");
        auto& State = *Session;
        (*Initial)["resolution"] = 9;
        Check(Resolution(State, {}) == 1024, "constructor owns defaults");
        Check(State.Add("A") && State.Add("B") && State.Add("C"), "add rows");
        State.Select("A", true);
        State.Select("C", true);
        State.Edit("A");
        const auto Before = State.Revision();
        const auto Count = Copy::Copies;
        State.Add("A");
        State.Edit("A");
        State.Rows();
        State.Token();
        State.Find("A");
        Check(State.Revision() == Before && Count == Copy::Copies,
              "reads, duplicate add and repeated selection are inert");
        auto Defaults = State.CopyOptions({});
        (*Defaults)["resolution"] = 2048;
        State.Replace(State.TokenFor({}), Defaults, "new", false);
        Check(Resolution(State, "A") == 1024 && Resolution(State, "B") == 1024, "defaults never propagate to old rows");
        State.Add("D");
        Check(Resolution(State, "D") == 2048, "new rows copy defaults");
        auto Value = State.CopyOptions("A");
        (*Value)["resolution"] = 512;
        State.Replace(State.Token(), Value, "row", true);
        State.ApplySelected(State.Token());
        (*Value)["resolution"] = 1;
        auto NestedEdit = State.CopyOptions("A");
        (*NestedEdit->Nested)[0] = 999;
        State.Replace(State.Token(), NestedEdit, "row", true);
        Check((*State.Find("C")->Value->Nested)[0] == 7, "nested payload is not shared between rows");
        Check(Resolution(State, "A") == 512 && Resolution(State, "C") == 512 && Resolution(State, "B") == 1024,
              "bulk clones selected rows only");
        State.Move("A", 2);
        Check(State.Editing() == std::optional<std::string>("A"), "reorder tracks key");
        const auto Stale = State.Token();
        State.Edit("B");
        Check(!State.Replace(Stale, Value, "bad", true), "old scope token rejected");
        State.Validate(State.TokenFor({}), false);
        Check(!State.Add("E") && !State.Reset("A"), "invalid defaults block creation/reset");
        Model::FSnapshot Snapshot;
        Check(State.Snapshot(Snapshot), "invalid defaults do not block independent rows");
        State.Validate(State.TokenFor("A"), false);
        Check(!State.Snapshot(Snapshot) && Snapshot.empty(), "bad row blocks entire snapshot without partial result");
        State.SelectAll(false);
        State.Select("A", true);
        State.ApplySelected(State.Token());
        Check(State.Snapshot(Snapshot), "explicit bulk clears destination error");
        const auto Frozen = Snapshot.front().second->at("resolution");
        const auto NestedFrozen = Snapshot.front().second->Nested->at(0);
        auto Changed = State.CopyOptions(Snapshot.front().first);
        (*Changed)["resolution"] = 99;
        (*Changed->Nested)[0] = 123;
        State.Replace(State.TokenFor(Snapshot.front().first), Changed, "row", true);
        Check(Snapshot.front().second->Nested->at(0) == NestedFrozen, "nested snapshot is detached");
        Check(Snapshot.front().second->at("resolution") == Frozen, "execution snapshot is detached");
        auto Old = State.TokenFor("A");
        State.Remove("A");
        State.Validate(State.TokenFor({}), true);
        State.Add("A");
        Check(!State.Replace(Old, Value, "bad", true), "remove/readd never reuses generation");
        State.Edit("A");
        Old = State.Token();
        State.Select("A", true);
        State.ApplySelected(State.TokenFor({}));
        Check(!State.IsCurrent(Old), "destructive bulk invalidates delayed focus event");
        State.SelectAll(false);
        const auto NoSelection = State.Revision();
        State.ApplySelected(State.TokenFor({}));
        Check(State.Revision() == NoSelection, "empty bulk is inert");
        State.Validate(State.TokenFor({}), false);
        const auto InvalidDefaultRevision = State.Revision();
        Check(State.ApplySelected(State.TokenFor({})) && State.Revision() == InvalidDefaultRevision,
              "empty bulk ignores an invalid source");
        State.Validate(State.TokenFor({}), true);
        State.Edit("B");
        State.Remove("B");
        Check(State.Editing() && *State.Editing() == "D", "remove active selects next row");
        auto Reopened = Session;
        Session.reset();
        Check(Reopened->Rows().size() == 3, "view close/reopen retains session state");
        Model Fresh(Initial, "default");
        Check(Fresh.Rows().empty() && !Fresh.Editing(), "new session starts empty");
        State.Edit("A");
        State.Remove("A");
        Check(State.Editing() && *State.Editing() == "D", "last active row falls back to previous");
        const auto OldDefault = State.TokenFor({});
        State.Clear();
        Check(!State.IsCurrent(OldDefault), "clear invalidates default input generation");
        Check(!State.Editing() && State.Rows().empty(), "clear restores default editing");
        std::cout << "Workspace checks: " << Checks << " passed\n";
    }
    catch (const std::exception& Error)
    {
        std::cerr << Error.what() << '\n';
        return 1;
    }
}
