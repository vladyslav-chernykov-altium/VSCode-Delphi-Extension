program iface;

// Interface-dispatch fixture for the rsm2pdb debugger UX work.
//
// Goals:
//   - Show every common Delphi calling pattern that goes through
//     IInterface, so we can pin down which RTL helpers get inserted
//     in each (System._IntfCopy / @IntfAddRef / @IntfClear /
//     @IntfWeakRefToStrongRef / TInterfacedObject.{_AddRef,_Release})
//     and decide which to auto-skip in the VSCode extension.
//   - Stress the "cast a raw class pointer to an interface and call"
//     idiom (no AddRef path) so users debugging legacy code that
//     bypasses refcounting see the expected step-into behaviour.
//
// Body formatting is one statement per source line so every call
// gets its own line number -- breakpoints + RSM line-table debug
// are easier when no two calls share a line.

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}
{$W+}    // always emit frame-pointer prologue

uses
  System.SysUtils;

type
  IMyInterface = interface
    ['{6E5A0BE8-1B8C-4E9F-9F4F-7D7A0B5C0001}']
    function  MyFunction(x: Integer): Integer;
    procedure MyMethod;
  end;

  TMyClass = class(TInterfacedObject, IMyInterface)
  private
    fTag: Integer;
  public
    constructor Create(aTag: Integer);
    function  MyFunction(x: Integer): Integer;
    procedure MyMethod;
  end;

constructor TMyClass.Create(aTag: Integer);
begin
  inherited Create;
  fTag := aTag;
end;

function TMyClass.MyFunction(x: Integer): Integer;
var
  lTag: Integer;
begin
  lTag := fTag;
  Result := lTag + x;
end;

procedure TMyClass.MyMethod;
var
  lTag: Integer;
begin
  lTag := fTag;
  Writeln('TMyClass.MyMethod tag=', lTag);
end;

// ----------------------------------------------------------------
//  1. Direct class call -- bypasses IMyInterface entirely. No
//     refcount activity at all. The fastest path; the stepping
//     experience is "just" virtual dispatch through TMyClass's vmt.
// ----------------------------------------------------------------
procedure CallDirect(obj: TMyClass);
var
  r: Integer;
begin
  obj.MyMethod;
  r := obj.MyFunction(10);
  Writeln('direct          MyFunction = ', r);
end;

// ----------------------------------------------------------------
//  2. Typed `IMyInterface` parameter. Caller assigned obj -> intf,
//     compiler emitted _IntfCopy (= AddRef) at the assign and
//     _IntfClear (= Release) at scope-exit of the assignment site.
//     Inside this proc the param itself is already a strong ref;
//     each method call goes through the interface vtable and the
//     adjuster thunk that converts interface-ptr back to object-ptr.
// ----------------------------------------------------------------
procedure CallViaInterface(intf: IMyInterface);
var
  r: Integer;
begin
  intf.MyMethod;
  r := intf.MyFunction(20);
  Writeln('iface param     MyFunction = ', r);
end;

// ----------------------------------------------------------------
//  3. Raw `Pointer` cast to IMyInterface. Delphi treats
//     `IMyInterface(P)` where P is `Pointer` as a pure type-cast --
//     it reinterprets the bytes WITHOUT touching reference counts.
//     The user is responsible for keeping the underlying object
//     alive. Use case: legacy code that stores interface as raw
//     pointer in maps / arrays of TObject etc.
//
//     Expected disasm: `mov rcx, [p]; mov rax, [rcx]; call [rax+N]`.
//     Step-into should land directly in the adjuster thunk (which
//     forwards to TMyClass.MyMethod) -- no AddRef/Release dance.
// ----------------------------------------------------------------
procedure CallViaPointerCast(p: Pointer);
var
  r: Integer;
begin
  IMyInterface(p).MyMethod;
  r := IMyInterface(p).MyFunction(30);
  Writeln('Pointer cast    MyFunction = ', r);
end;

// ----------------------------------------------------------------
//  4. Class-reference cast to IMyInterface. Delphi compiler
//     inserts a runtime call to fetch the IInterface table for the
//     class (via GetInterface / @IntfCast / similar) and DOES bump
//     refcount on the resulting interface. This is the most
//     expensive path of the four.
// ----------------------------------------------------------------
procedure CallViaClassCast(obj: TMyClass);
var
  intf: IMyInterface;
  r:    Integer;
begin
  intf := obj as IMyInterface;
  intf.MyMethod;
  r := intf.MyFunction(40);
  Writeln('class-as-iface  MyFunction = ', r);
end;

// ----------------------------------------------------------------
//  5. Repeated bare cast inside a single statement. Each
//     `IMyInterface(p).Method` is a separate cast -- Delphi does
//     NOT cache the result. Useful for comparing how the auto-skip
//     handles two interface calls on one statement.
// ----------------------------------------------------------------
procedure CallChainedPointer(p: Pointer);
begin
  IMyInterface(p).MyMethod;
  Writeln('chained 1 MyFunc = ', IMyInterface(p).MyFunction(50));
  Writeln('chained 2 MyFunc = ', IMyInterface(p).MyFunction(60));
end;

// ---------- Driver: invoke each shape ----------
//
// Lifetime: `obj` is held alive by the `intf` interface reference.
// We deliberately do NOT call `obj.Free` -- TInterfacedObject's
// refcount hits zero when `intf` goes out of scope at end-of-program
// and the object is destroyed automatically. Doing both would double-
// free.
var
  obj:  TMyClass;
  intf: IMyInterface;
  ptr:  Pointer;
begin
  obj  := TMyClass.Create(100);
  intf := obj;                    // assign -> _IntfCopy / AddRef
  ptr  := Pointer(intf);          // raw alias, NO AddRef

  Writeln('--- 1 direct ---');
  CallDirect(obj);

  Writeln('--- 2 iface param ---');
  CallViaInterface(intf);

  Writeln('--- 3 Pointer cast ---');
  CallViaPointerCast(ptr);

  Writeln('--- 4 class-as-iface ---');
  CallViaClassCast(obj);

  Writeln('--- 5 chained Pointer casts ---');
  CallChainedPointer(ptr);

  // intf goes out of scope here -> Release -> Destroy -> done.
end.
