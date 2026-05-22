unit Geometry;

interface

type
  TPoint = record
    X: Integer;
    Y: Integer;
  end;

function Add(A, B: Integer): Integer;
function DistanceSq(const P1, P2: TPoint): Integer;

implementation

function Add(A, B: Integer): Integer;
var
  Tmp: Integer;
begin
  Tmp := A + B;
  Result := Tmp;
end;

function DistanceSq(const P1, P2: TPoint): Integer;
var
  Dx, Dy: Integer;
begin
  Dx := P1.X - P2.X;
  Dy := P1.Y - P2.Y;
  Result := Dx * Dx + Dy * Dy;
end;

end.
