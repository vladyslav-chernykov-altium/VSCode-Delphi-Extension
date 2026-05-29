program records;

// Records / classes / enums fixture for the rsm2pdb TPI struct
// synthesis work (Step 11b).
//
// Goals:
//   - Give the RSM RE work a clean, minimal program that exercises
//     every "non-primitive aggregate" shape we care about: plain
//     record, mixed-type record, nested record, packed record,
//     simple class, class with inheritance + virtuals, enum,
//     set-of-enum.
//   - Each aggregate is used as BOTH a global variable AND a local
//     variable, so the RSM type-table entries get referenced from
//     two distinct contexts -- helpful for spotting whether the
//     encoding differs between the global and the local code paths.
//   - All fields have distinct, easily-recognisable values so when
//     `dt` / `gdb p` finally renders the struct, "did we get the
//     offsets right?" is obvious by eye.
//
// One statement per line so every probe gets its own .map row.
//
// Probe point: ProbeAll() runs at startup, fills every aggregate,
// then calls a no-op SetBreakHere() so cdb / gdb can break with
// every local still live.

{$APPTYPE CONSOLE}
{$D+}
{$L+}
{$Y+}
{$O-}
{$INLINE OFF}
{$W+}

uses
  System.SysUtils;

type
  // --- Plain record, all-Integer ---
  TPoint = record
    X: Integer;
    Y: Integer;
  end;

  // --- Mixed-type record (int / float / bool / string / char) ---
  TPerson = record
    Age:     Integer;
    Salary:  Double;
    Active:  Boolean;
    Name:    string;
    Grade:   Char;
  end;

  // --- Nested record (record-of-records) ---
  TBox = record
    TopLeft:     TPoint;
    BottomRight: TPoint;
    Label_:      string;
  end;

  // --- Packed record (no padding -- fields tightly laid out) ---
  TPacked = packed record
    A: Byte;
    B: Word;
    C: Cardinal;
    D: Byte;
  end;

  // --- Enum ---
  TColor = (clRed, clGreen, clBlue, clYellow);

  // --- Set of enum ---
  TColors = set of TColor;

  // --- Simple class with fields + a virtual method ---
  TShape = class
  protected
    fName:  string;
    fArea:  Double;
  public
    constructor Create(const AName: string; AArea: Double);
    function Describe: string; virtual;
  end;

  // --- Derived class, adds a field + overrides ---
  TCircle = class(TShape)
  private
    fRadius: Double;
  public
    constructor Create(AArea: Double; ARadius: Double);
    function Describe: string; override;
  end;

  // --- Class with composite-typed field (clears risk R4) ---
  // TBag holds a TPoint by value -- this is the case the current
  // fixture doesn't cover: class field whose type is a user
  // record, not a primitive.
  TBag = class
  public
    fPos:   TPoint;
    fSlot:  TPoint;
    fTag:   Integer;
  end;

  // --- Big record forcing offset > 127 (clears risk R6) ---
  // 40 Integer fields = 160 bytes; the last few sit at offsets
  // 128 .. 156, well past u8 (127). If RSM's `<offset_x2>` byte
  // ran out and switched to a 2-byte form, we'll see it here.
  TBig = record
    F00, F01, F02, F03, F04, F05, F06, F07, F08, F09: Integer;
    F10, F11, F12, F13, F14, F15, F16, F17, F18, F19: Integer;
    F20, F21, F22, F23, F24, F25, F26, F27, F28, F29: Integer;
    F30, F31, F32, F33, F34, F35, F36, F37, F38, F39: Integer;
  end;

// ---------------- globals ----------------

var
  GPoint:   TPoint;
  GPerson:  TPerson;
  GBox:     TBox;
  GPacked:  TPacked;
  GColor:   TColor;
  GColors:  TColors;
  GShape:   TShape;
  GCircle:  TCircle;
  GBag:     TBag;
  GBig:     TBig;

// ---------------- methods ----------------

constructor TShape.Create(const AName: string; AArea: Double);
begin
  fName := AName;
  fArea := AArea;
end;

function TShape.Describe: string;
Type
    TSaveFormat = (eASCII, eBinary3, eBinary4, eBinary5, eBinary6, eBinaryPCBWorks, eBinaryCS, eBinaryCM, eExternal);
var
    r : set of TSaveFormat;
begin
  r := { eASCII, eBinaryCS };
  Result := fName + ' (base)';
end;

constructor TCircle.Create(AArea: Double; ARadius: Double);
begin
  inherited Create('Circle', AArea);
  fRadius := ARadius;
end;

function TCircle.Describe: string;
begin
  Result := fName + '/' + FloatToStr(fRadius);
end;

// ---------------- breakpoint anchor ----------------

procedure SetBreakHere;
begin
  // no-op; gives the debugger a stable spot to break with every
  // local in ProbeAll still in scope.
  Sleep(0);
end;

// ---------------- probe ----------------

procedure ProbeAll;
var
  lPoint:  TPoint;
  lPerson: TPerson;
  lBox:    TBox;
  lPacked: TPacked;
  lColor:  TColor;
  lColors: TColors;
  lShape:  TShape;
  lCircle: TCircle;
  lBag:    TBag;
  lBig:    TBig;
begin
  // --- TPoint ---
  lPoint.X := 11;
  lPoint.Y := 22;

  // --- TPerson ---
  lPerson.Age    := 33;
  lPerson.Salary := 4444.5;
  lPerson.Active := True;
  lPerson.Name   := 'Alice';
  lPerson.Grade  := 'A';

  // --- TBox ---
  lBox.TopLeft.X     := 100;
  lBox.TopLeft.Y     := 101;
  lBox.BottomRight.X := 200;
  lBox.BottomRight.Y := 202;
  lBox.Label_        := 'BoxLabel';

  // --- TPacked ---
  lPacked.A := $AA;
  lPacked.B := $BBCC;
  lPacked.C := $DEADBEEF;
  lPacked.D := $11;

  // --- enum + set ---
  lColor  := clGreen;
  lColors := [clRed, clBlue];

  // --- classes ---
  lShape  := TShape.Create('PlainShape', 10.5);
  lCircle := TCircle.Create(78.5, 5.0);

  // --- TBag (class with composite field, clears R4) ---
  lBag := TBag.Create;
  lBag.fPos.X  := 1001;
  lBag.fPos.Y  := 1002;
  lBag.fSlot.X := 2001;
  lBag.fSlot.Y := 2002;
  lBag.fTag    := 7777;

  // --- TBig (big record forcing offset > 127, clears R6) ---
  lBig.F00 := 100;
  lBig.F19 := 119;
  lBig.F32 := 132;
  lBig.F39 := 139;

  // --- mirror into globals so we can also test global struct view ---
  GPoint  := lPoint;
  GPerson := lPerson;
  GBox    := lBox;
  GPacked := lPacked;
  GColor  := lColor;
  GColors := lColors;
  GShape  := lShape;
  GCircle := lCircle;
  GBag    := lBag;
  GBig    := lBig;

  Writeln('locals + globals filled; ready to inspect.');
  SetBreakHere;
  Writeln('done');

  lShape.Free;
  lCircle.Free;
  lBag.Free;
end;

begin
  try
    ProbeAll;
  except
    on E: Exception do
      Writeln(E.ClassName, ': ', E.Message);
  end;
end.
