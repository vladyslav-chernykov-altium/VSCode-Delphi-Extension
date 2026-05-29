unit Items;

// Cross-unit fixture: SINGLE-CONSUMER unit.  Declares TItem class
// whose fields reference TPoint / TColor / TSize from Shapes.pas.
// Used by Layout.pas and cross_unit.dpr as a field / local type
// respectively, so we get a 3-level reference chain on TItem ->
// (TPoint, TColor, TSize) -> primitives.

interface

uses
  Shapes;

type
  TItem = class
  private
    fName:  string;
    fPos:   TPoint;
    fSize:  TSize;
    fColor: TColor;
    fTag:   Integer;
    function  GetDescription: string;
    procedure SetTag(V: Integer);
  public
    constructor Create(const AName: string;
                       const APos: TPoint;
                       const ASize: TSize;
                       AColor: TColor);
    // Field-backed read-only.
    property Name:   string  read fName;
    // Field-backed read/write -- record type (Shapes.TPoint).
    property Pos:    TPoint  read fPos   write fPos;
    // Field-backed read/write -- record type (Shapes.TSize).
    property Size:   TSize   read fSize  write fSize;
    // Field-backed read/write -- enum type (Shapes.TColor).
    property Color:  TColor  read fColor write fColor;
    // Field-backed read/write -- primitive.
    property Tag:    Integer read fTag   write fTag;
    // Method-backed read-only -- computed (concatenates other fields).
    property Description: string read GetDescription;
    // Mixed: field-read, method-write (for validation-style setters).
    property Tag2:   Integer read fTag   write SetTag;
  end;

implementation

uses
  System.SysUtils;

constructor TItem.Create(const AName: string;
                         const APos: TPoint;
                         const ASize: TSize;
                         AColor: TColor);
begin
  fName  := AName;
  fPos   := APos;
  fSize  := ASize;
  fColor := AColor;
  fTag   := 0;
end;

function TItem.GetDescription: string;
begin
  Result := fName + '@(' + IntToStr(fPos.X) + ',' + IntToStr(fPos.Y) + ')';
end;

procedure TItem.SetTag(V: Integer);
begin
  // Setter clamps to non-negative for testing.
  if V < 0 then
    fTag := 0
  else
    fTag := V;
end;

end.
