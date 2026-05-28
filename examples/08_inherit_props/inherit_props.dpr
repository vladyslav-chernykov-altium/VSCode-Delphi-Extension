program inherit_props;

// Fixture for verifying how rsm2pdb handles:
//   (a) MULTI-LEVEL class inheritance (TAnimal -> TMammal -> TDog)
//       -- does the LF_BCLASS chain walk all three levels in cdb /
//       Watch?  Are inherited fields reachable via deep
//       (grand-grand-parent) access?
//   (b) Pascal `property` declarations -- both field-backed
//       (read X write X) and getter/setter-backed
//       (read GetX write SetX) variants, plus read-only.  RSM /
//       PDB historically don't carry property metadata directly,
//       so the expected debugger view is: properties are
//       invisible, only the backing fields show in Watch. This
//       fixture lets us confirm that and decide whether to add
//       NatVis or synthetic accessor emission later.
//
// One statement per source line so every probe gets its own .map
// row. SetBreakHere() is the inspection anchor.

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
  // --- Level 1 ---
  TAnimal = class
  protected
    fName:   string;
    fAge:    Integer;
  public
    constructor Create(const AName: string; AAge: Integer);
    function Describe: string; virtual;
    // Field-backed properties (read X write X).
    property Name: string  read fName write fName;
    property Age:  Integer read fAge  write fAge;
  end;

  // --- Level 2 ---
  TMammal = class(TAnimal)
  protected
    fFurColor: string;
  public
    constructor Create(const AName: string; AAge: Integer;
                       const AFurColor: string);
    function Describe: string; override;
    property FurColor: string read fFurColor write fFurColor;
  end;

  // --- Level 3 ---
  TDog = class(TMammal)
  private
    fBreed:     string;
    fBarkCount: Integer;
    function  GetBarkCount: Integer;
    procedure SetBarkCount(Value: Integer);
  public
    constructor Create(const AName: string; AAge: Integer;
                       const AFurColor, ABreed: string);
    function Describe: string; override;
    // Read-only property (no `write`).
    property Breed: string read fBreed;
    // Getter/setter-backed property (no direct field exposure
    // through the property syntax -- but fBarkCount IS a real
    // field that the debugger should still see).
    property BarkCount: Integer read GetBarkCount write SetBarkCount;
  end;

// ---------------- methods ----------------

constructor TAnimal.Create(const AName: string; AAge: Integer);
begin
  fName := AName;
  fAge  := AAge;
end;

function TAnimal.Describe: string;
begin
  Result := 'Animal(' + fName + ')';
end;

constructor TMammal.Create(const AName: string; AAge: Integer;
                           const AFurColor: string);
begin
  inherited Create(AName, AAge);
  fFurColor := AFurColor;
end;

function TMammal.Describe: string;
begin
  Result := 'Mammal(' + fName + ', fur=' + fFurColor + ')';
end;

constructor TDog.Create(const AName: string; AAge: Integer;
                        const AFurColor, ABreed: string);
begin
  inherited Create(AName, AAge, AFurColor);
  fBreed     := ABreed;
  fBarkCount := 0;
end;

function TDog.Describe: string;
begin
  Result := 'Dog(' + fName + ', ' + fBreed + ')';
end;

function TDog.GetBarkCount: Integer;
begin
  Result := fBarkCount;
end;

procedure TDog.SetBarkCount(Value: Integer);
begin
  fBarkCount := Value;
end;

// ---------------- breakpoint anchor ----------------

procedure SetBreakHere;
begin
  Sleep(0);
end;

// ---------------- probe ----------------

procedure ProbeAll;
var
  lAnimal: TAnimal;
  lMammal: TMammal;
  lDog:    TDog;
begin
  lAnimal := TAnimal.Create('Generic', 1);
  lMammal := TMammal.Create('Whiskers', 3, 'gray');
  lDog    := TDog.Create('Rex', 5, 'brown', 'Labrador');

  // Touch the property setters so the bark count isn't 0.
  lDog.BarkCount := 42;

  // Touch via property syntax (these become getter/field reads
  // at runtime; the debugger only sees the backing fields).
  lAnimal.Name := 'Updated';
  lAnimal.Age  := 99;

  Writeln('animals created; bark=', lDog.BarkCount);
  SetBreakHere;
  Writeln('done');

  lAnimal.Free;
  lMammal.Free;
  lDog.Free;
end;

begin
  try
    ProbeAll;
  except
    on E: Exception do
      Writeln(E.ClassName, ': ', E.Message);
  end;
end.
