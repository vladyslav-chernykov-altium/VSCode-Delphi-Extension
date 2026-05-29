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
  public
    constructor Create(const AName: string;
                       const APos: TPoint;
                       const ASize: TSize;
                       AColor: TColor);
    property Name: string read fName;
    property Tag:  Integer read fTag write fTag;
  end;

implementation

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

end.
