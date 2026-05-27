program locals;

// Systematic coverage of Delphi function shapes for RSM proc-record
// reverse engineering. Each shape has predictable param + local names
// (pA, pB, ...; lA, lB, ...) so we can pin every record back to its
// declaration when reading the RSM hex.
//
// Every body is formatted one statement per line and begin/end on
// their own lines so each proc gets a maximal set of distinct line
// entries -- helps when reverse-engineering the RSM offset encoding
// (more line records to anchor procs by, less ambiguity).

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}
{$W+}   // always emit frame pointer prologue ({$STACKFRAMES ON})

uses
  System.SysUtils;

// ---------- 1. Global procedures / functions (no Self) ----------

procedure GlobalProc0;
var
  lA: Integer;
begin
  lA := 1;
  Writeln('GP0 ', lA);
end;

procedure GlobalProc1(pA: Integer);
var
  lA: Integer;
begin
  lA := pA + 1;
  Writeln('GP1 ', lA);
end;

procedure GlobalProc2(pA, pB: Integer);
var
  lA, lB: Integer;
begin
  lA := pA;
  lB := pB;
  Writeln('GP2 ', lA, ' ', lB);
end;

procedure GlobalProc3(pA, pB, pC: Integer);
var
  lA, lB, lC: Integer;
begin
  lA := pA;
  lB := pB;
  lC := pC;
  Writeln('GP3 ', lA, ' ', lB, ' ', lC);
end;

procedure GlobalProc4(pA, pB, pC, pD: Integer);
var
  lA, lB, lC, lD: Integer;
begin
  lA := pA;
  lB := pB;
  lC := pC;
  lD := pD;
  Writeln('GP4 ', lA, ' ', lB, ' ', lC, ' ', lD);
end;

procedure GlobalProc10(
  pA, pB, pC, pD, pE, pF, pG, pH, pI, pJ: Integer);
var
  lA, lB: Integer;
begin
  lA := pA + pJ;
  lB := pB + pI;
  Writeln('GP10 ', lA, ' ', lB);
end;

function GlobalFunc0: Integer;
var
  lA: Integer;
begin
  lA := 99;
  Result := lA;
end;

function GlobalFunc1(pA: Integer): Integer;
var
  lA: Integer;
begin
  lA := pA * 2;
  Result := lA;
end;

function GlobalFunc3(pA, pB, pC: Integer): Integer;
var
  lA: Integer;
begin
  lA := pA + pB + pC;
  Result := lA;
end;

// ---------- 1b. Nested function (function inside function) ----------

function NestedOuter(pA: Integer): Integer;
var
  lOuter: Integer;

  // Inner sees pA / lOuter from its parent's frame (Delphi closure).
  function NestedInner(pB: Integer): Integer;
  var
    lInner: Integer;
  begin
    lInner := pB + lOuter;     // touches outer's local
    Result := lInner + pA;     // touches outer's param
  end;

begin
  lOuter := pA * 10;
  Result := NestedInner(pA + 1);
end;

// ---------- 1c. Anonymous methods (Delphi closures / lambdas) ----------

type
  TIntFunc = reference to function(pA: Integer): Integer;

function MakeLambda(pCapture: Integer): TIntFunc;
begin
  // Captures pCapture in an implicit interface-backed frame.
  Result := function(pA: Integer): Integer
            var
              lA: Integer;
            begin
              lA := pA + pCapture;
              Result := lA;
            end;
end;

// ---------- 1d. Param modes: var / const / out + by-value String ----------
//
// Each modifier produces a different RSM encoding (byref flag, possibly
// different stack_offset semantics, "out" zero-init prologue, etc.).
// Mix integer (4-byte) and string (managed reference) so we can see
// how the encoding differs by value-category as well as by mode.

procedure WithVarParam(var pVar: Integer);
var
  lA: Integer;
begin
  lA := pVar;
  pVar := lA + 1;
  Writeln('VAR ', pVar);
end;

procedure WithConstParam(const pConst: Integer);
var
  lA: Integer;
begin
  lA := pConst;
  Writeln('CONST ', lA);
end;

procedure WithOutParam(out pOut: Integer);
var
  lA: Integer;
begin
  lA := 42;
  pOut := lA;
  Writeln('OUT ', pOut);
end;

procedure WithStringByValue(pS: string);
var
  lS: string;
begin
  lS := pS + '!';
  Writeln('SVAL ', lS);
end;

procedure WithStringByVar(var pS: string);
var
  lS: string;
begin
  lS := pS;
  pS := pS + '?';
  Writeln('SVAR ', lS);
end;

procedure WithStringByConst(const pS: string);
var
  lS: string;
begin
  lS := pS + '#';
  Writeln('SCONST ', lS);
end;

procedure WithMixedParams(
  pInt: Integer; var pVarInt: Integer;
  const pConstStr: string; out pOutInt: Integer);
var
  lA: Integer;
begin
  lA := pInt + pVarInt;
  pVarInt := pVarInt + 1;
  pOutInt := lA;
  Writeln('MIX ', lA, ' ', pConstStr);
end;

// ---------- 2. Class methods (implicit Self) ----------

type
  TBase = class
  public
    mField: Integer;
    procedure RegProc0;
    procedure RegProc1(pA: Integer);
    procedure RegProc2(pA, pB: Integer);
    procedure RegProc3(pA, pB, pC: Integer);
    procedure RegProc10(pA, pB, pC, pD, pE, pF, pG, pH, pI, pJ: Integer);
    function  RegFunc1(pA: Integer): Integer;
    function  RegFunc3(pA, pB, pC: Integer): Integer;
    procedure VirtProc1(pA: Integer); virtual;
    function  VirtFunc1(pA: Integer): Integer; virtual;
    procedure OverloadedM(pA: Integer);                 overload;
    procedure OverloadedM(pA, pB: Integer);             overload;
    procedure OverloadedM(pA, pB, pC: Integer);         overload;
  end;

  TDerived = class(TBase)
  public
    mDerivedField: Integer;
    procedure VirtProc1(pA: Integer); override;
    function  VirtFunc1(pA: Integer): Integer; override;
  end;

procedure TBase.RegProc0;
var
  lA: Integer;
begin
  lA := mField;
  Writeln('TB.RP0 ', lA);
end;

procedure TBase.RegProc1(pA: Integer);
var
  lA: Integer;
begin
  lA := pA + mField;
  Writeln('TB.RP1 ', lA);
end;

procedure TBase.RegProc2(pA, pB: Integer);
var
  lA, lB: Integer;
begin
  lA := pA;
  lB := pB + mField;
  Writeln('TB.RP2 ', lA, ' ', lB);
end;

procedure TBase.RegProc3(pA, pB, pC: Integer);
var
  lA, lB, lC: Integer;
begin
  lA := pA;
  lB := pB;
  lC := pC + mField;
  Writeln('TB.RP3 ', lA, ' ', lB, ' ', lC);
end;

procedure TBase.RegProc10(
  pA, pB, pC, pD, pE, pF, pG, pH, pI, pJ: Integer);
var
  lA, lB: Integer;
begin
  lA := pA + pJ;
  lB := pB + mField;
  Writeln('TB.RP10 ', lA, ' ', lB);
end;

function TBase.RegFunc1(pA: Integer): Integer;
var
  lA: Integer;
begin
  lA := pA;
  Result := lA + mField;
end;

function TBase.RegFunc3(pA, pB, pC: Integer): Integer;
var
  lA: Integer;
begin
  lA := pA + pB + pC;
  Result := lA + mField;
end;

procedure TBase.VirtProc1(pA: Integer);
var
  lA: Integer;
begin
  lA := pA + 1;
  Writeln('TB.VP1 ', lA);
end;

function TBase.VirtFunc1(pA: Integer): Integer;
var
  lA: Integer;
begin
  lA := pA + 10;
  Result := lA;
end;

procedure TBase.OverloadedM(pA: Integer);
var
  lA: Integer;
begin
  lA := pA;
  Writeln('TB.O1 ', lA);
end;

procedure TBase.OverloadedM(pA, pB: Integer);
var
  lA, lB: Integer;
begin
  lA := pA;
  lB := pB;
  Writeln('TB.O2 ', lA, ' ', lB);
end;

procedure TBase.OverloadedM(pA, pB, pC: Integer);
var
  lA, lB, lC: Integer;
begin
  lA := pA;
  lB := pB;
  lC := pC;
  Writeln('TB.O3 ', lA, ' ', lB, ' ', lC);
end;

procedure TDerived.VirtProc1(pA: Integer);
var
  lA: Integer;
begin
  lA := pA + 100;
  Writeln('TD.VP1 ', lA);
end;

function TDerived.VirtFunc1(pA: Integer): Integer;
var
  lA: Integer;
begin
  lA := pA + 1000;
  Result := lA;
end;

// ---------- 3. Driver: invoke each shape ----------

var
  base:    TBase;
  derived: TDerived;
  alias:   TBase;
  vInt:    Integer;
  vOut:    Integer;
  vStr:    string;
begin
  GlobalProc0;
  GlobalProc1(1);
  GlobalProc2(1, 2);
  GlobalProc3(1, 2, 3);
  GlobalProc4(1, 2, 3, 4);
  GlobalProc10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
  Writeln('GF0=', GlobalFunc0);
  Writeln('GF1=', GlobalFunc1(5));
  Writeln('GF3=', GlobalFunc3(1, 2, 3));
  Writeln('NESTED=', NestedOuter(7));
  Writeln('LAMBDA=', MakeLambda(100)(5));

  // Param-mode exercises.
  vInt := 10;
  vOut := 0;
  vStr := 'world';
  WithVarParam(vInt);
  WithConstParam(20);
  WithOutParam(vOut);
  WithStringByValue('hello');
  WithStringByVar(vStr);
  WithStringByConst('frozen');
  WithMixedParams(1, vInt, 'const-str', vOut);

  base := TBase.Create;
  try
    base.mField := 42;
    base.RegProc0;
    base.RegProc1(1);
    base.RegProc2(1, 2);
    base.RegProc3(1, 2, 3);
    base.RegProc10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
    Writeln('TB.RF1=', base.RegFunc1(5));
    Writeln('TB.RF3=', base.RegFunc3(1, 2, 3));
    base.VirtProc1(7);
    Writeln('TB.VF1=', base.VirtFunc1(7));
    base.OverloadedM(1);
    base.OverloadedM(1, 2);
    base.OverloadedM(1, 2, 3);
  finally
    base.Free;
  end;

  derived := TDerived.Create;
  try
    derived.mField := 100;
    derived.mDerivedField := 200;
    alias := derived;
    alias.VirtProc1(7);                   // dispatches to TDerived
    Writeln('TD.VF1=', alias.VirtFunc1(7));
  finally
    derived.Free;
  end;
end.
